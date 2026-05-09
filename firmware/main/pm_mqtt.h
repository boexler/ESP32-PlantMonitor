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
 * @brief Opens a transport connection, publishes discovery + meta + moisture, then disconnects.
 *
 * Intended for deep-sleep wake cycles: minimal broker footprint between sleeps.
 *
 * @param mac_topic_id Lowercase MAC without colons (e.g. "aabbccddeeff").
 * @param moisture_pct Per channel: moisture 0–100 when a probe is connected, or -1 when disconnected
 *                    (raw below PM_SOIL_ADC_DISCONNECT_MAX); ignored if channel inactive.
 */
esp_err_t pm_mqtt_publish_cycle(const char *mac_topic_id,
                               const pm_mqtt_discovery_input_t *disc,
                               const float moisture_pct[PM_MOISTURE_CHANNEL_COUNT]);

/**
 * @brief Starts a long-lived MQTT session: connect once and publish bootstrap (discovery, meta, availability).
 *
 * Call @ref pm_mqtt_session_stop before @ref pm_mqtt_session_start if reconfiguring.
 *
 * @param mac_topic_id Same compact MAC string as one-shot mode.
 */
esp_err_t pm_mqtt_session_start(const char *mac_topic_id, const pm_mqtt_discovery_input_t *disc);

/**
 * @brief Publishes moisture state topics over an existing session. Reconnects and re-bootstraps if disconnected.
 *
 * @param disc Channel mask and device name (must remain valid for the call).
 * @param moisture_pct Per channel: 0–100 or -1 when no sensor; values for inactive channels are unused.
 */
esp_err_t pm_mqtt_session_publish_moisture(const pm_mqtt_discovery_input_t *disc,
                                          const float moisture_pct[PM_MOISTURE_CHANNEL_COUNT]);
/**
 * @brief Stops and destroys the persistent session.
 */
void pm_mqtt_session_stop(void);

#endif
