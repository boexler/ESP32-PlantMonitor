/**
 * @file pm_adc.c
 * @brief ESP32-C6 oneshot ADC for XIAO breakout GPIOs D0–D2 plus MTMS/MTDI/MTCK (GPIO4–6).
 */

#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"

#include "pm_adc.h"

static const char TAG[] = "pm_adc";

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/** Seeed XIAO ESP32C6: six ADC-capable pads per pin map (GPIO0,1,2,4,5,6). */
static const int s_adc_gpios[PM_MOISTURE_CHANNEL_COUNT] = {
    GPIO_NUM_0,
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_4,
    GPIO_NUM_5,
    GPIO_NUM_6,
};

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_channel_t s_ch[PM_MOISTURE_CHANNEL_COUNT];

esp_err_t pm_adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init, &s_adc), TAG, "new adc unit");

    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        adc_channel_t ch;
        adc_unit_t u;
        esp_err_t ig = adc_oneshot_io_to_channel((int)s_adc_gpios[i], &u, &ch);
        if (ig != ESP_OK) {
            ESP_LOGE(TAG, "GPIO %d is not an ADC pin", s_adc_gpios[i]);
            adc_oneshot_del_unit(s_adc);
            s_adc = NULL;
            return ig;
        }
        if (u != ADC_UNIT_1) {
            ESP_LOGW(TAG, "Unexpected ADC unit %d for GPIO %d", (int)u, s_adc_gpios[i]);
        }
        s_ch[i] = ch;

        adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = ADC_ATTEN_DB_12,
        };
        ESP_RETURN_ON_ERROR(
            adc_oneshot_config_channel(s_adc, s_ch[i], &chan_cfg), TAG, "config ch");
    }

    return ESP_OK;
}

void pm_adc_deinit(void)
{
    if (s_adc != NULL) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
}

esp_err_t pm_adc_read_averaged(int sample_count, int raw_out[PM_MOISTURE_CHANNEL_COUNT])
{
    if (s_adc == NULL || raw_out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (sample_count < 1) {
        sample_count = 1;
    }

    for (int i = 0; i < PM_MOISTURE_CHANNEL_COUNT; i++) {
        int32_t acc = 0;
        for (int s = 0; s < sample_count; s++) {
            int v = 0;
            esp_err_t e = adc_oneshot_read(s_adc, s_ch[i], &v);
            if (e != ESP_OK) {
                return e;
            }
            acc += v;
        }
        raw_out[i] = (int)(acc / sample_count);
    }

    return ESP_OK;
}
