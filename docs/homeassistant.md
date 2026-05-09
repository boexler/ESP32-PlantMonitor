# Home Assistant integration

Integrate the ESP32 MQTT bridge with HA in three layers:

1. **MQTT broker** — add-on (`Mosquitto`) or external broker the firmware reaches.
2. **Built-in MQTT integration** — consumes Home Assistant MQTT discovery payloads (moisture % sensors automatically).
3. **This custom integration** (`plant_monitor`) — subscribes to the same MQTT topic tree so you keep **six dry thresholds** and **six dry-alarm binaries** bundled on the merged device registry entry.

**With HACS:** add this Git repository as a **Custom repository** (category **Integration**), install **Plant Monitor (ESP32 MQTT)** from HACS, restart Home Assistant, then add the integration from the UI. The root [`hacs.json`](../hacs.json) is used by HACS for metadata.

**Manual:** copy [`custom_components/plant_monitor`](../custom_components/plant_monitor/) into `config/custom_components/plant_monitor/`, restart, then *Settings → Devices & services → Add integration → Plant monitor*. Ensure MQTT connects before adding the helper.

Required fields mirror the firmware `menuconfig` defaults:

| Config entry field | Matches firmware |
|--------------------|----------------|
| MQTT topic prefix | `CONFIG_PM_MQTT_TOPIC_PREFIX` (default `plant_monitor`) |
| MAC address | Twelve lowercase hex nibbles Wi-Fi STA MAC (**no separators**, same suffix as MQTT topics logged on boot) |

## Hosted device registry (`www/` example)

Expose the firmware `CONFIG_ESP_CONFIG_URL` over HTTP (`/local/`). Example file `configuration/www/plant_monitor_devices.json`:

```json
[
  {
    "mac": "d4:f9:8d:aa:bb:cc",
    "name": "Living shelf",
    "channels": [1, 1, 1, 0, 0, 0]
  }
]
```

- `channels` accepts six booleans or `1`/`0` (`true/false`).
- Omitting sensors **removes MQTT discovery payloads** during the next firmware wake (`false` ⇒ entity never announced).
- The integration listens to **`{prefix}/{mac}/meta/channels`** to mirror active bits for threshold + alarm helpers.

### Automation idea (critical dry state)

Expose a notifier when an alarm toggles:

```yaml
alias: Soil dry alert
trigger:
  - platform: state
    entity_id: binary_sensor.PLANT_REPLACE_channel_1_dry
    from: 'off'
    to: 'on'
action:
  - service: notify.mobile_app_REPLACE
    data:
      message: "Shelf plant needs water."
```

Rename entity IDs inside Home Assistant as needed.

## Notifications vs database hacks

Prefer **MQTT state**, **integrations**, and **`notify` services**. Do not write Recorder/SQL manually; Recorder still logs legitimate entity updates automatically.

## Topic cheat sheet (`{prefix}` default `plant_monitor`, `{mac}` compact STA MAC)

| Topic | QoS retain | Payload |
|-------|------------|---------|
| `{prefix}/{mac}/availability` | QoS 1 retain | `online`/`offline` (LWT) |
| `{prefix}/{mac}/meta/channels` | retain | JSON `{"active":[bool×6]}` |
| `{prefix}/{mac}/meta/device` | retain | JSON `{"name":"…"}` |
| `{prefix}/{mac}/moisture/ch{N}` | no retain | Moisture `%` ASCII float |
| `homeassistant/sensor/{prefix}_{mac}_m{N}/config` | retain | MQTT discovery JSON |

## Troubleshooting

- **MQTT shows offline between samples** — expected: firmware sleeps and LWT publishes `offline` until next wake publishes `online` again.
- **Integration cannot be added** — finish HA MQTT broker configuration first (`mqtt.is_connected`).
- **No MQTT entities** — ensure discovery is enabled on the MQTT integration and the broker retained messages are permitted.
