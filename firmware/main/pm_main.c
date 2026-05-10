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
#include "esp_system.h"
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

/**
 * @brief Log how the chip last exited reset (power-on, RST pin, deep sleep wake, etc.).
 */
static void log_reset_reason(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    const char *name;

    switch (r) {
    case ESP_RST_UNKNOWN:
        name = "UNKNOWN";
        break;
    case ESP_RST_POWERON:
        name = "POWERON";
        break;
    case ESP_RST_EXT:
        name = "EXT (RST button / reset pin)";
        break;
    case ESP_RST_SW:
        name = "SW (esp_restart)";
        break;
    case ESP_RST_PANIC:
        name = "PANIC";
        break;
    case ESP_RST_INT_WDT:
        name = "INT_WDT";
        break;
    case ESP_RST_TASK_WDT:
        name = "TASK_WDT";
        break;
    case ESP_RST_WDT:
        name = "WDT";
        break;
    case ESP_RST_DEEPSLEEP:
        name = "DEEPSLEEP";
        break;
    case ESP_RST_BROWNOUT:
        name = "BROWNOUT";
        break;
    case ESP_RST_SDIO:
        name = "SDIO";
        break;
    case ESP_RST_USB:
        name = "USB";
        break;
    case ESP_RST_JTAG:
        name = "JTAG";
        break;
    case ESP_RST_EFUSE:
        name = "EFUSE";
        break;
    case ESP_RST_PWR_GLITCH:
        name = "PWR_GLITCH";
        break;
    case ESP_RST_CPU_LOCKUP:
        name = "CPU_LOCKUP";
        break;
    default:
        name = "OTHER";
        break;
    }

    ESP_LOGI(TAG, "Reset reason: %s (%d)", name, (int)r);
}

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

/**
 * @brief Warn once at boot if soil ADC reference ordering is inconsistent (disconnect < water < moist < dry < air <
 *        spike).
 */
static void warn_if_soil_calib_order_invalid(void)
{
    int dc = CONFIG_PM_SOIL_ADC_DISCONNECT_MAX;
    int w = CONFIG_PM_SOIL_ADC_WATER;
    int moist = CONFIG_PM_SOIL_ADC_ILEX_VERY_WET;
    int dryp = CONFIG_PM_SOIL_ADC_VERY_DRY;
    int a = CONFIG_PM_SOIL_ADC_AIR;
    int sp = CONFIG_PM_SOIL_ADC_SPIKE_MIN;
    if (!(dc < w && w < moist && moist < dryp && dryp < a && a < sp)) {
        ESP_LOGW(TAG,
                 "Soil ADC Kconfig ordering should be: disconnect < water < moist_plant < dry_plant < air < spike "
                 "(got %d, %d, %d, %d, %d, %d)",
                 dc,
                 w,
                 moist,
                 dryp,
                 a,
                 sp);
    }
    if (w >= a) {
        ESP_LOGW(TAG, "Soil ADC: water reference must be less than air reference (got water=%d air=%d)", w, a);
    }
}

/**
 * @brief Map averaged raw ADC to moisture 0–100%, or -1 if no sensor (disconnected or spike).
 *
 * Uses water and air references from sdkconfig; clamps to [0, 100] when connected.
 */
static float raw_to_moisture_pct(int raw)
{
    if (raw < CONFIG_PM_SOIL_ADC_DISCONNECT_MAX) {
        return -1.f;
    }
    if (raw > CONFIG_PM_SOIL_ADC_SPIKE_MIN) {
        return -1.f;
    }
    const int air = CONFIG_PM_SOIL_ADC_AIR;
    const int water = CONFIG_PM_SOIL_ADC_WATER;
    const int span = air - water;
    if (span <= 0) {
        return -1.f;
    }
    float pct = 100.f * (float)(air - raw) / (float)span;
    if (pct < 0.f) {
        pct = 0.f;
    } else if (pct > 100.f) {
        pct = 100.f;
    }
    return pct;
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
        if (moisture_out[i] < 0.f) {
            if (raw[i] > CONFIG_PM_SOIL_ADC_SPIKE_MIN) {
                ESP_LOGW(TAG, "CH%d: no sensor / spike (raw=%d)", i, raw[i]);
            } else {
                ESP_LOGW(TAG, "CH%d: no sensor / disconnected (raw=%d)", i, raw[i]);
            }
        } else {
            ESP_LOGI(TAG, "CH%d raw=%d moisture=%.1f%%", i, raw[i], (double)moisture_out[i]);
        }
    }
    return ESP_OK;
}

static void fill_mqtt_disc(pm_mqtt_discovery_input_t *disc, pm_station_config_t *cfg)
{
    disc->human_device_name = cfg->device_name;
    memcpy(disc->channel_active, cfg->channel_active, sizeof(disc->channel_active));
}

#if CONFIG_PM_POWER_DEEP_SLEEP

/** Retries for one-shot MQTT cycle before giving up (deep sleep still follows). */
#define PM_MQTT_ONESHOT_RETRY_MAX 3

/**
 * @brief Repeated ADC batches over CONFIG_PM_POST_WIFI_MEASURE_WINDOW_SEC; mean raw → moisture like measure_channels.
 *
 * Reduces post-wake transients by averaging many short bursts before MQTT.
 */
static esp_err_t measure_channels_over_window(pm_station_config_t *cfg,
                                              float moisture_out[PM_MOISTURE_CHANNEL_COUNT])
{
    (void)cfg;
    int raw_scratch[PM_MOISTURE_CHANNEL_COUNT];
    int64_t sum[PM_MOISTURE_CHANNEL_COUNT] = {0};
    uint32_t batches = 0;

    const uint32_t window_ms = (uint32_t)CONFIG_PM_POST_WIFI_MEASURE_WINDOW_SEC * 1000u;
    uint32_t period_ms = (uint32_t)CONFIG_PM_POST_WIFI_MEASURE_SAMPLE_PERIOD_MS;

    ESP_LOGI(TAG,
             "Measurement window %" PRIu32 " s (batch every %" PRIu32 " ms)",
             (uint32_t)CONFIG_PM_POST_WIFI_MEASURE_WINDOW_SEC,
             period_ms);

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(window_ms);

    while (xTaskGetTickCount() < deadline) {
        esp_err_t err = pm_adc_read_averaged(CONFIG_PM_ADC_SAMPLE_COUNT, raw_scratch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pm_adc_read_averaged failed: %s", esp_err_to_name(err));
            return err;
        }
        for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
            sum[i] += (int64_t)raw_scratch[i];
        }
        batches++;

        if (xTaskGetTickCount() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }

    if (batches == 0) {
        ESP_LOGE(TAG, "measure_channels_over_window: no batches");
        return ESP_ERR_INVALID_STATE;
    }

    int raw_mean[PM_MOISTURE_CHANNEL_COUNT];
    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        raw_mean[i] = (int)(sum[i] / (int64_t)batches);
    }

    ESP_LOGI(TAG, "Measurement window done (%" PRIu32 " batches)", batches);

    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        const int raw = raw_mean[i];
        moisture_out[i] = raw_to_moisture_pct(raw);
        if (moisture_out[i] < 0.f) {
            if (raw > CONFIG_PM_SOIL_ADC_SPIKE_MIN) {
                ESP_LOGW(TAG, "CH%d: no sensor / spike (mean raw=%d)", i, raw);
            } else {
                ESP_LOGW(TAG, "CH%d: no sensor / disconnected (mean raw=%d)", i, raw);
            }
        } else {
            ESP_LOGI(TAG, "CH%d mean raw=%d moisture=%.1f%%", i, raw, (double)moisture_out[i]);
        }
    }

    return ESP_OK;
}

/**
 * @brief One shot: measure + @ref pm_mqtt_publish_cycle for deep sleep mode.
 */
static esp_err_t measure_and_mqtt_publish_oneshot(const char *compact, pm_station_config_t *cfg)
{
    float moisture[PM_MOISTURE_CHANNEL_COUNT];
    esp_err_t me;
#if CONFIG_PM_POST_WIFI_MEASURE_WINDOW_SEC > 0
    me = measure_channels_over_window(cfg, moisture);
#else
    me = measure_channels(cfg, moisture);
#endif
    ESP_RETURN_ON_ERROR(me, TAG, "measure failed");

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

#endif /* deep sleep helpers */

void app_main(void)
{
    printf("ESP32-C6 plant monitor (MQTT / Home Assistant discovery)\n");
    log_reset_reason();

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

    warn_if_soil_calib_order_invalid();

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
