/**
 * @file pm_adc.h
 * @brief ADC oneshot helpers for ESP32-C6 (Seeed XIAO default GPIO routing).
 */

#ifndef PM_ADC_H
#define PM_ADC_H

#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifndef PM_MOISTURE_CHANNEL_COUNT
#define PM_MOISTURE_CHANNEL_COUNT 6
#endif

/**
 * @brief Initialize ADC unit and configure all moisture channels.
 */
esp_err_t pm_adc_init(void);

void pm_adc_deinit(void);

/**
 * @brief Average several raw reads per channel (12-bit scaled per IDF attenuation choice).
 */
esp_err_t pm_adc_read_averaged(int sample_count, int raw_out[PM_MOISTURE_CHANNEL_COUNT]);

#endif
