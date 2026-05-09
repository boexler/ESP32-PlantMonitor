/**
 * @file pm_wifi.c
 * @brief WiFi station bootstrap (ESP-IDF event loop pattern).
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "lwip/err.h"

#include "pm_log.h"
#include "pm_wifi.h"

#define PM_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define PM_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define PM_ESP_MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#endif

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char TAG[] = "pm_wifi";

/** RF switch (FM8625H): GPIO3 enable low, GPIO14 high selects U.FL per Seeed wiki. */
#define PM_XIAO_C6_ANT_GPIO_ENABLE GPIO_NUM_3
#define PM_XIAO_C6_ANT_GPIO_SELECT GPIO_NUM_14

static int s_retry_num = 0;

/**
 * @brief Drive Seeed XIAO ESP32-C6 antenna switch from menuconfig (external U.FL vs internal PCB).
 *
 * Must run after esp_wifi_init() and before esp_wifi_start(). Levels match Seeed Wi-Fi usage wiki.
 */
static void apply_xiao_c6_antenna_from_config(void)
{
#if CONFIG_PM_WIFI_ANT_EXTERNAL
    gpio_reset_pin(PM_XIAO_C6_ANT_GPIO_ENABLE);
    gpio_reset_pin(PM_XIAO_C6_ANT_GPIO_SELECT);
    gpio_set_direction(PM_XIAO_C6_ANT_GPIO_ENABLE, GPIO_MODE_OUTPUT);
    gpio_set_direction(PM_XIAO_C6_ANT_GPIO_SELECT, GPIO_MODE_OUTPUT);
    gpio_set_level(PM_XIAO_C6_ANT_GPIO_ENABLE, 0);
    gpio_set_level(PM_XIAO_C6_ANT_GPIO_SELECT, 1);
    ESP_LOGI(TAG, "Wi-Fi antenna: external (U.FL), GPIO%d=0 GPIO%d=1",
             (int)PM_XIAO_C6_ANT_GPIO_ENABLE,
             (int)PM_XIAO_C6_ANT_GPIO_SELECT);
#elif CONFIG_PM_WIFI_ANT_INTERNAL
    gpio_reset_pin(PM_XIAO_C6_ANT_GPIO_ENABLE);
    gpio_reset_pin(PM_XIAO_C6_ANT_GPIO_SELECT);
    gpio_set_direction(PM_XIAO_C6_ANT_GPIO_ENABLE, GPIO_MODE_INPUT);
    gpio_set_direction(PM_XIAO_C6_ANT_GPIO_SELECT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PM_XIAO_C6_ANT_GPIO_ENABLE, GPIO_FLOATING);
    gpio_set_pull_mode(PM_XIAO_C6_ANT_GPIO_SELECT, GPIO_FLOATING);
    ESP_LOGI(TAG, "Wi-Fi antenna: internal (PCB), GPIO%d/GPIO%d floating input",
             (int)PM_XIAO_C6_ANT_GPIO_ENABLE,
             (int)PM_XIAO_C6_ANT_GPIO_SELECT);
#endif
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < PM_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry connect to AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "connect to AP failed");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(wifiIpAddress, sizeof(wifiIpAddress), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta_once(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    apply_xiao_c6_antenna_from_config();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid,
            PM_ESP_WIFI_SSID,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password,
            PM_ESP_WIFI_PASS,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "failed SSID:%s", PM_ESP_WIFI_SSID);
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "unexpected wait bits");
    return ESP_FAIL;
}

void wifi_init(void)
{
    ESP_LOGI(TAG, "STA mode init");
    ESP_ERROR_CHECK(wifi_init_sta_once());
}
