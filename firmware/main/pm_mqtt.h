/**
 * @file pm_mqtt.h
 * @brief MQTT client: Home Assistant discovery, moisture state topics, retained metadata.
 */

#ifndef PM_MQTT_H
#define PM_MQTT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifndef PM_MOISTURE_CHANNEL_COUNT
#define PM_MOISTURE_CHANNEL_COUNT 6
#endif

typedef struct {
    bool channel_active[PM_MOISTURE_CHANNEL_COUNT];
    const char *human_device_name;
} pm_mqtt_discovery_input_t;

/**
 * @brief Connects to MQTT broker, publishes discovery + meta + moisture readings, disconnects cleanly.
 *
 * @param mac_topic_id Lowercase MAC without colons (e.g. "aabbccddeeff").
 * @param moisture_pct Moisture estimate 0-100 per channel (ignored if channel inactive).
 */
esp_err_t pm_mqtt_publish_cycle(const char *mac_topic_id,
                               const pm_mqtt_discovery_input_t *disc,
                               const float moisture_pct[PM_MOISTURE_CHANNEL_COUNT]);

#endif
