/**
 * @file pm_config.c
 * @brief Fetch device registry JSON over HTTP and match station MAC.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "pm_config.h"

static const char TAG[] = "pm_config";
#define MAX_HTTP_BODY 8192

static char s_body[MAX_HTTP_BODY];
static size_t s_body_len;

static void http_reset_buffer(void)
{
    s_body_len = 0;
    memset(s_body, 0, sizeof(s_body));
}

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len <= 0 || esp_http_client_is_chunked_response(evt->client)) {
            break;
        }
        if (s_body_len + (size_t)evt->data_len >= sizeof(s_body) - 1) {
            ESP_LOGE(TAG, "HTTP body overflow");
            return ESP_FAIL;
        }
        memcpy(s_body + s_body_len, evt->data, (size_t)evt->data_len);
        s_body_len += (size_t)evt->data_len;
        s_body[s_body_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

/** True if @p url uses HTTPS (needs TLS verification options for mbedTLS). */
static bool url_is_https(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8) == 0;
}

static void mac_to_string(const uint8_t mac[6], char *out18)
{
    snprintf(out18,
             18,
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static bool mac_match_string(const uint8_t mac[6], const char *str)
{
    char want[18];
    mac_to_string(mac, want);
    if (str == NULL || strlen(str) < 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        char a = (char)tolower((unsigned char)want[i]);
        char b = (char)tolower((unsigned char)str[i]);
        if (a != b) {
            return false;
        }
    }
    return true;
}

static void set_default_all_active(pm_station_config_t *out, const char *fallback_name)
{
    out->device_name = strdup(fallback_name != NULL ? fallback_name : "plant_monitor");
    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        out->channel_active[i] = true;
    }
}

static bool apply_device_object(const cJSON *obj,
                                const uint8_t mac[6],
                                pm_station_config_t *out)
{
    const cJSON *jmac = cJSON_GetObjectItem(obj, "mac");
    if (!cJSON_IsString(jmac) || jmac->valuestring == NULL) {
        return false;
    }
    if (!mac_match_string(mac, jmac->valuestring)) {
        return false;
    }

    const cJSON *jname = cJSON_GetObjectItem(obj, "name");
    const char *name = (cJSON_IsString(jname) && jname->valuestring != NULL)
                           ? jname->valuestring
                           : "plant_monitor";

    out->device_name = strdup(name);
    if (out->device_name == NULL) {
        return false;
    }

    const cJSON *jch = cJSON_GetObjectItem(obj, "channels");
    if (cJSON_IsArray(jch) && cJSON_GetArraySize(jch) >= PM_MOISTURE_CHANNEL_COUNT) {
        for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
            const cJSON *it = cJSON_GetArrayItem(jch, i);
            if (cJSON_IsBool(it)) {
                out->channel_active[i] = cJSON_IsTrue(it);
            } else if (cJSON_IsNumber(it)) {
                out->channel_active[i] = (it->valueint != 0);
            } else {
                out->channel_active[i] = true;
            }
        }
    } else {
        for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
            out->channel_active[i] = true;
        }
    }
    return true;
}

void pm_station_config_release(pm_station_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    free(cfg->device_name);
    cfg->device_name = NULL;
}

esp_err_t pm_config_load_from_url(const char *config_url, pm_station_config_t *out)
{
    if (config_url == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_err != ESP_OK) {
        return mac_err;
    }

    char macstr[18];
    mac_to_string(mac, macstr);

    http_reset_buffer();

    esp_http_client_config_t http_cfg = {
        .url = config_url,
        .event_handler = http_event,
        .timeout_ms = 15000,
    };

    /* ESP-IDF 6 mbedTLS rejects HTTPS without crt bundle, PEM cert, or explicit skip-verify. */
    if (url_is_https(config_url)) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || code < 200 || code >= 300) {
        ESP_LOGW(TAG, "HTTP err=%s code=%d → defaults", esp_err_to_name(err), code);
        set_default_all_active(out, macstr);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(s_body);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON parse error → defaults");
        set_default_all_active(out, macstr);
        return ESP_OK;
    }

    bool found = false;
    cJSON *arr = NULL;
    if (cJSON_IsArray(root)) {
        arr = root;
    } else {
        const cJSON *jd = cJSON_GetObjectItem(root, "devices");
        if (cJSON_IsArray(jd)) {
            arr = (cJSON *)jd;
        }
    }

    if (arr != NULL) {
        int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; i++) {
            const cJSON *obj = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(obj)) {
                continue;
            }
            if (apply_device_object(obj, mac, out)) {
                found = true;
                break;
            }
        }
    }

    cJSON_Delete(root);

    if (!found) {
        ESP_LOGW(TAG, "No entry for %s → all channels active", macstr);
        pm_station_config_release(out);
        memset(out, 0, sizeof(*out));
        set_default_all_active(out, macstr);
    }

    ESP_LOGI(TAG, "Configured device name \"%s\"", out->device_name != NULL ? out->device_name : "");
    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        ESP_LOGI(TAG, "  channel %d active=%d", i, (int)out->channel_active[i]);
    }

    return ESP_OK;
}
