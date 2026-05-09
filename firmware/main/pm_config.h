/**
 * @file pm_config.h
 * @brief Loads per-device settings (name, active channels) from HTTP JSON.
 */

#ifndef PM_CONFIG_H
#define PM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_system.h"

#define PM_MOISTURE_CHANNEL_COUNT 6

typedef struct {
    char *device_name;
    bool channel_active[PM_MOISTURE_CHANNEL_COUNT];
} pm_station_config_t;

/**
 * @brief Downloads JSON from config_url and fills @p out if this device's MAC matches an entry.
 *
 * JSON format (array or object with "devices" array):
 * [
 *   {"mac":"aa:bb:cc:dd:ee:ff","name":"Living room","channels":[1,1,0,0,1,1]},
 * ]
 */
esp_err_t pm_config_load_from_url(const char *config_url, pm_station_config_t *out);

void pm_station_config_release(pm_station_config_t *cfg);

#endif
