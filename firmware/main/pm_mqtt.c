/**
 * @file pm_mqtt.c
 * @brief One-shot MQTT publish: Home Assistant discovery, metadata, moisture states.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "sdkconfig.h"

#include "pm_mqtt.h"

static const char TAG[] = "pm_mqtt";

#define MQTT_EV_CONNECTED BIT0
#define MQTT_EV_FAILED BIT1

typedef struct {
    EventGroupHandle_t eg;
    esp_mqtt_client_handle_t client;
} mqtt_wait_t;

#ifndef PM_FW_VERSION
#define PM_FW_VERSION "1.0.0"
#endif

static void mqtt_event(void *handler_args,
                       esp_event_base_t base,
                       int32_t event_id,
                       void *event_data)
{
    mqtt_wait_t *ctx = (mqtt_wait_t *)handler_args;
    (void)base;
    (void)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        xEventGroupSetBits(ctx->eg, MQTT_EV_CONNECTED);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        xEventGroupSetBits(ctx->eg, MQTT_EV_FAILED);
        break;
    default:
        break;
    }
}

static int topic_base(char *buf, size_t len, const char *mac)
{
    return snprintf(buf, len, "%s/%s", CONFIG_PM_MQTT_TOPIC_PREFIX, mac);
}

static int topic_suffix(char *buf, size_t len, const char *mac, const char *suffix)
{
    char base[128];
    topic_base(base, sizeof(base), mac);
    return snprintf(buf, len, "%s/%s", base, suffix);
}

static esp_err_t publish_retained(esp_mqtt_client_handle_t c, const char *topic, const char *payload)
{
    int mid = esp_mqtt_client_publish(c, topic, payload, 0, 1, 1);
    return (mid >= 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t publish_state(esp_mqtt_client_handle_t c, const char *topic, const char *payload)
{
    int mid = esp_mqtt_client_publish(c, topic, payload, 0, 0, 0);
    return (mid >= 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Home Assistant MQTT discovery payload for one moisture sensor.
 */
static esp_err_t build_discovery_json(char *out,
                                      size_t out_len,
                                      const char *mac,
                                      int ch_index,
                                      const char *device_name,
                                      const char *attr_topic_channels)
{
    char base[128];
    topic_base(base, sizeof(base), mac);

    char topic_path[160];
    snprintf(topic_path, sizeof(topic_path), "%s/moisture/ch%d", base, ch_index);

    char av_topic[160];
    snprintf(av_topic, sizeof(av_topic), "%s/availability", base);

    char uniq[144];
    snprintf(uniq,
             sizeof(uniq),
             "%s_%s_moisture_%d",
             CONFIG_PM_MQTT_TOPIC_PREFIX,
             mac,
             ch_index);

    char human[64];
    snprintf(human, sizeof(human), "Moisture %d", ch_index + 1);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "name", human);
    cJSON_AddStringToObject(root, "unique_id", uniq);
    cJSON_AddStringToObject(root, "state_topic", topic_path);
    cJSON_AddStringToObject(root, "unit_of_measurement", "%");
    cJSON_AddStringToObject(root, "state_class", "measurement");

    cJSON_AddStringToObject(root, "availability_topic", av_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");

    if (attr_topic_channels != NULL && attr_topic_channels[0] != '\0') {
        cJSON_AddStringToObject(root, "json_attributes_topic", attr_topic_channels);
    }

    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON *pair = cJSON_CreateArray();
    cJSON_AddItemToArray(pair, cJSON_CreateString("plant_monitor"));
    cJSON_AddItemToArray(pair, cJSON_CreateString(mac));
    cJSON_AddItemToArray(ids, pair);
    cJSON_AddItemToObject(dev, "identifiers", ids);
    cJSON_AddStringToObject(dev, "name", device_name != NULL ? device_name : "Plant monitor");
    cJSON_AddStringToObject(dev, "manufacturer", "Seeed Studio");
    cJSON_AddStringToObject(dev, "model", "XIAO ESP32-C6");
    cJSON_AddStringToObject(dev, "sw_version", PM_FW_VERSION);
    cJSON_AddItemToObject(root, "device", dev);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (strlen(printed) >= out_len) {
        free(printed);
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(out, printed);
    free(printed);
    return ESP_OK;
}

esp_err_t pm_mqtt_publish_cycle(const char *mac_topic_id,
                                const pm_mqtt_discovery_input_t *disc,
                                const float moisture_pct[PM_MOISTURE_CHANNEL_COUNT])
{
    if (mac_topic_id == NULL || disc == NULL || moisture_pct == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mqtt_wait_t wait = {.eg = xEventGroupCreate(), .client = NULL};
    if (wait.eg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char lwt_topic[192];
    topic_suffix(lwt_topic, sizeof(lwt_topic), mac_topic_id, "availability");

    esp_mqtt_client_config_t mq = {0};
    mq.broker.address.uri = CONFIG_PM_MQTT_BROKER_URI;
    mq.session.last_will.topic = lwt_topic;
    mq.session.last_will.msg = "offline";
    mq.session.last_will.msg_len = (int)strlen("offline");
    mq.session.last_will.qos = 1;
    mq.session.last_will.retain = 1;

    if (strlen(CONFIG_PM_MQTT_USERNAME) > 0) {
        mq.credentials.username = CONFIG_PM_MQTT_USERNAME;
        mq.credentials.authentication.password = CONFIG_PM_MQTT_PASSWORD;
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mq);
    if (!client) {
        vEventGroupDelete(wait.eg);
        return ESP_ERR_NO_MEM;
    }
    wait.client = client;

    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event, &wait);
    esp_err_t st = esp_mqtt_client_start(client);
    if (st != ESP_OK) {
        esp_mqtt_client_destroy(client);
        vEventGroupDelete(wait.eg);
        return st;
    }

    EventBits_t bits = xEventGroupWaitBits(wait.eg,
                                           MQTT_EV_CONNECTED | MQTT_EV_FAILED,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(20000));

    if ((bits & MQTT_EV_CONNECTED) == 0) {
        ESP_LOGE(TAG, "MQTT connect timeout or failure");
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        vEventGroupDelete(wait.eg);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    char base[144];
    topic_base(base, sizeof(base), mac_topic_id);

    char attr_topic_meta[176];
    topic_suffix(attr_topic_meta, sizeof(attr_topic_meta), mac_topic_id, "meta/channels");

    {
        char t[176];
        snprintf(t, sizeof(t), "%s/availability", base);
        err = publish_retained(client, t, "online");
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    {
        cJSON *mj = cJSON_CreateObject();
        cJSON *act = cJSON_CreateArray();
        for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
            cJSON_AddItemToArray(act, cJSON_CreateBool(disc->channel_active[i]));
        }
        cJSON_AddItemToObject(mj, "active", act);
        char *printed = cJSON_PrintUnformatted(mj);
        cJSON_Delete(mj);
        if (printed == NULL) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        char path[176];
        snprintf(path, sizeof(path), "%s/meta/channels", base);
        err = publish_retained(client, path, printed);
        free(printed);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    {
        cJSON *dj = cJSON_CreateObject();
        cJSON_AddStringToObject(dj, "name", disc->human_device_name != NULL ? disc->human_device_name
                                                                           : "");
        char *printed = cJSON_PrintUnformatted(dj);
        cJSON_Delete(dj);
        if (printed == NULL) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        char path[176];
        snprintf(path, sizeof(path), "%s/meta/device", base);
        err = publish_retained(client, path, printed);
        free(printed);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    char disc_line[768];
    char disc_topic[256];
    for (int ch = 0; ch < PM_MOISTURE_CHANNEL_COUNT; ch++) {
        if (!disc->channel_active[ch]) {
            continue;
        }
        err = build_discovery_json(disc_line,
                                   sizeof(disc_line),
                                   mac_topic_id,
                                   ch,
                                   disc->human_device_name,
                                   attr_topic_meta);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "discovery ch%d: %s", ch, esp_err_to_name(err));
            goto cleanup;
        }
        snprintf(disc_topic,
                 sizeof(disc_topic),
                 "%s/sensor/%s_%s_m%d/config",
                 CONFIG_PM_HA_DISCOVERY_PREFIX,
                 CONFIG_PM_MQTT_TOPIC_PREFIX,
                 mac_topic_id,
                 ch);
        err = publish_retained(client, disc_topic, disc_line);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    {
        char val[32];
        char topic[192];
        for (int ch = 0; ch < PM_MOISTURE_CHANNEL_COUNT; ch++) {
            if (!disc->channel_active[ch]) {
                continue;
            }
            snprintf(topic, sizeof(topic), "%s/moisture/ch%d", base, ch);
            snprintf(val, sizeof(val), "%.1f", (double)moisture_pct[ch]);
            err = publish_state(client, topic, val);
            if (err != ESP_OK) {
                goto cleanup;
            }
        }
    }

cleanup:
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    vEventGroupDelete(wait.eg);
    return err;
}
