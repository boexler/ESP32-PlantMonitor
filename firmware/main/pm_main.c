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
 * @brief Single measurement + MQTT discovery/state publish cycle.
 */
static esp_err_t measure_and_mqtt_publish(const char *compact, pm_station_config_t *cfg)
{
    int raw[PM_MOISTURE_CHANNEL_COUNT];
    ESP_RETURN_ON_ERROR(
        pm_adc_read_averaged(CONFIG_PM_ADC_SAMPLE_COUNT, raw),
        TAG,
        "pm_adc_read_averaged failed");

    float moisture[PM_MOISTURE_CHANNEL_COUNT];
    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        moisture[i] = raw_to_moisture_pct(raw[i]);
        ESP_LOGI(TAG, "CH%d raw=%d approx_moisture=%.1f%%", i, raw[i], (double)moisture[i]);
    }

    pm_mqtt_discovery_input_t disc = {
        .human_device_name = cfg->device_name,
    };
    memcpy(disc.channel_active, cfg->channel_active, sizeof(disc.channel_active));

    return pm_mqtt_publish_cycle(compact, &disc, moisture);
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
    ESP_ERROR_CHECK(measure_and_mqtt_publish(compact, &cfg));

    pm_adc_deinit();
    pm_station_config_release(&cfg);

    uint64_t us = (uint64_t)CONFIG_PM_MEASURE_INTERVAL_SEC * 1000000ULL;
    ESP_LOGI(TAG, "Deep sleep %" PRIu32 " s", (uint32_t)CONFIG_PM_MEASURE_INTERVAL_SEC);
    esp_deep_sleep(us);
#else
    ESP_LOGI(TAG,
             "Power mode: continuous debug (delay %" PRIu32 " ms between cycles)",
             (uint32_t)CONFIG_PM_DEBUG_LOOP_PERIOD_MS);
    while (1) {
        ESP_ERROR_CHECK(measure_and_mqtt_publish(compact, &cfg));
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PM_DEBUG_LOOP_PERIOD_MS));
    }
#endif
}
