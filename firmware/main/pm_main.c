/**
 * @file pm_main.c
 * @brief Soil moisture sampler for XIAO ESP32-C6: WiFi → config → MQTT, then deep sleep or continuous debug loop.
 */

#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include <time.h>

#include "pm_wifi.h"
#include "pm_log.h"
#include "pm_config.h"
#include "pm_adc.h"
#include "pm_mqtt.h"

static const char TAG[] = "plant_monitor";

/** Retries for one-shot MQTT cycle before giving up (deep sleep still follows). */
#define PM_MQTT_ONESHOT_RETRY_MAX 3

static void sync_time_via_sntp(void)
{
    ESP_LOGI(TAG, "Timezone TZ=%s", CONFIG_ESP_NTP_TZ);
    setenv("TZ", CONFIG_ESP_NTP_TZ, 1);
    tzset();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    ESP_LOGI(TAG, "SNTP server %s", CONFIG_ESP_NTP_SERVER);
    esp_sntp_setservername(0, CONFIG_ESP_NTP_SERVER);

    int total_retries = 0;
    const int total_max_retries = 3;

    do {
        esp_sntp_init();
        int retry = 0;
        const int retry_count = 10;
        while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
            ESP_LOGI(TAG, "Waiting for SNTP (%d/%d)", retry, retry_count);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (retry == retry_count) {
            total_retries++;
            esp_sntp_stop();
        } else {
            break;
        }
    } while (total_retries < total_max_retries);

    if (total_retries >= total_max_retries) {
        ESP_LOGW(TAG, "SNTP failed after retries — continuing anyway");
        return;
    }

    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char buf[64];
    strftime(buf, sizeof(buf), "%c", &tm_info);
    ESP_LOGI(TAG, "Local time %s", buf);
}

static void mac_to_compact_id(const uint8_t mac[6], char out_compact[13])
{
    snprintf(out_compact,
             13,
             "%02x%02x%02x%02x%02x%02x",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

/** Map raw averaged ADC to approximate moisture index (invert common resistive probes). */
static float raw_to_moisture_pct(int raw12)
{
    const float vmax = (float)((1 << 12) - 1);
    float r = (float)raw12;
    if (r < 0.f) {
        r = 0.f;
    }
    if (r > vmax) {
        r = vmax;
    }
    return 100.f - (100.f * r / vmax);
}

/**
 * @brief Sample ADC and fill moisture array; log each channel.
 */
static esp_err_t measure_channels(pm_station_config_t *cfg, float moisture_out[PM_MOISTURE_CHANNEL_COUNT])
{
    (void)cfg;
    int raw[PM_MOISTURE_CHANNEL_COUNT];
    esp_err_t err = pm_adc_read_averaged(CONFIG_PM_ADC_SAMPLE_COUNT, raw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pm_adc_read_averaged failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        moisture_out[i] = raw_to_moisture_pct(raw[i]);
        ESP_LOGI(TAG, "CH%d raw=%d approx_moisture=%.1f%%", i, raw[i], (double)moisture_out[i]);
    }
    return ESP_OK;
}

static void fill_mqtt_disc(pm_mqtt_discovery_input_t *disc, pm_station_config_t *cfg)
{
    disc->human_device_name = cfg->device_name;
    memcpy(disc->channel_active, cfg->channel_active, sizeof(disc->channel_active));
}

/**
 * @brief One shot: measure + @ref pm_mqtt_publish_cycle for deep sleep mode.
 */
static esp_err_t measure_and_mqtt_publish_oneshot(const char *compact, pm_station_config_t *cfg)
{
    float moisture[PM_MOISTURE_CHANNEL_COUNT];
    ESP_RETURN_ON_ERROR(measure_channels(cfg, moisture), TAG, "measure_channels failed");

    pm_mqtt_discovery_input_t disc;
    fill_mqtt_disc(&disc, cfg);
    return pm_mqtt_publish_cycle(compact, &disc, moisture);
}

/** Exponential backoff for deep-sleep MQTT retries: 2 s, 4 s, 8 s (capped). */
static uint32_t mqtt_backoff_deep_sleep_ms(int attempt_index_0_based)
{
    uint32_t ms = 2000u << (unsigned)attempt_index_0_based;
    const uint32_t cap = 16000u;
    return (ms > cap) ? cap : ms;
}

void app_main(void)
{
    printf("ESP32-C6 plant monitor (MQTT / Home Assistant discovery)\n");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();

    sync_time_via_sntp();
    init_logging();

    pm_station_config_t cfg = {0};
    ESP_ERROR_CHECK(pm_config_load_from_url(CONFIG_ESP_CONFIG_URL, &cfg));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char compact[13];
    mac_to_compact_id(mac, compact);
    ESP_LOGI(TAG, "WiFi STA MAC compact id %s", compact);

    ESP_ERROR_CHECK(pm_adc_init());

#if CONFIG_PM_POWER_DEEP_SLEEP
    ESP_LOGI(TAG, "Power mode: deep sleep (interval %" PRIu32 " s)", (uint32_t)CONFIG_PM_MEASURE_INTERVAL_SEC);

    esp_err_t mqtt_err = ESP_OK;
    for (int attempt = 0; attempt < PM_MQTT_ONESHOT_RETRY_MAX; attempt++) {
        mqtt_err = measure_and_mqtt_publish_oneshot(compact, &cfg);
        if (mqtt_err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG,
                 "measure/MQTT cycle failed (%s), attempt %d/%d",
                 esp_err_to_name(mqtt_err),
                 attempt + 1,
                 PM_MQTT_ONESHOT_RETRY_MAX);
        if (attempt + 1 < PM_MQTT_ONESHOT_RETRY_MAX) {
            vTaskDelay(pdMS_TO_TICKS(mqtt_backoff_deep_sleep_ms(attempt)));
        }
    }
    if (mqtt_err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT publish failed after retries; entering deep sleep anyway");
    }

    pm_adc_deinit();
    pm_station_config_release(&cfg);

    uint64_t us = (uint64_t)CONFIG_PM_MEASURE_INTERVAL_SEC * 1000000ULL;
    ESP_LOGI(TAG, "Deep sleep %" PRIu32 " s", (uint32_t)CONFIG_PM_MEASURE_INTERVAL_SEC);
    esp_deep_sleep(us);
#else
    ESP_LOGI(TAG,
             "Power mode: continuous debug (delay %" PRIu32 " ms between cycles)",
             (uint32_t)CONFIG_PM_DEBUG_LOOP_PERIOD_MS);

    pm_mqtt_discovery_input_t disc;
    fill_mqtt_disc(&disc, &cfg);

    uint32_t start_backoff_ms = 1000;
    for (;;) {
        esp_err_t se = pm_mqtt_session_start(compact, &disc);
        if (se == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG,
                 "pm_mqtt_session_start failed: %s, retry in %" PRIu32 " ms",
                 esp_err_to_name(se),
                 start_backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(start_backoff_ms));
        if (start_backoff_ms < 16000) {
            start_backoff_ms *= 2;
        }
    }

    while (1) {
        float moisture[PM_MOISTURE_CHANNEL_COUNT];
        esp_err_t me = measure_channels(&cfg, moisture);
        if (me != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_PM_DEBUG_LOOP_PERIOD_MS));
            continue;
        }

        fill_mqtt_disc(&disc, &cfg);
        esp_err_t pe = pm_mqtt_session_publish_moisture(&disc, moisture);
        if (pe != ESP_OK) {
            ESP_LOGW(TAG, "pm_mqtt_session_publish_moisture failed: %s", esp_err_to_name(pe));
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_PM_DEBUG_LOOP_PERIOD_MS));
    }
#endif
}
