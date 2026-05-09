# Home Assistant integration

Integrate the ESP32 MQTT bridge with HA in three layers:

1. **MQTT broker** — add-on (`Mosquitto`) or external broker the firmware reaches.
2. **Built-in MQTT integration** — consumes Home Assistant MQTT discovery payloads (**numeric soil moisture `%` sensors automatically**).
3. **This custom integration** (`plant_monitor`) — caches the MQTT stream so thresholds, textual moisture bands (**global zone table**), **per-channel display names**, binary dry alarms, and optional HA notifications bundle on **one merged device**.

**Zones vs dry threshold:** the qualitative band text (*Configure → Moisture zones*) is descriptive only. Each channel numeric **Dry threshold** is separate and still gates the alarm when moisture `%` is **below** that value.

**With HACS:** add this Git repository as a **Custom repository** (category **Integration**), install **Plant Monitor (ESP32 MQTT)** from HACS, restart Home Assistant, then add the integration from the UI. The root [`hacs.json`](../hacs.json) is used by HACS for metadata.

**Manual:** copy [`custom_components/plant_monitor`](../custom_components/plant_monitor/) into `config/custom_components/plant_monitor/`, restart, then *Settings → Devices & services → Add integration → Plant monitor*. Ensure MQTT connects before adding the helper.

Required fields mirror the firmware `menuconfig` defaults:

| Config entry field | Matches firmware |
|--------------------|----------------|
| MQTT topic prefix | `CONFIG_PM_MQTT_TOPIC_PREFIX` (default `plant_monitor`) |
| MAC address | Twelve lowercase hex nibbles Wi-Fi STA MAC (**no separators**, same suffix as MQTT topics logged on boot) |

## Moisture UX (zones, captions, events)

After pairing the device entry:

| Entity type | Purpose |
|-------------|---------|
| MQTT **`sensor`** (discovered) | Raw soil moisture **`%`** (best for gauges and templates). Index `m0` … `m5` in discovery aligns with **`moisture/ch0` … `/ch5`**. |
| `sensor.*_moisture_zone_*` | Text state equals the matched band label (`sehr trocken`, `optimal`, …). Attributes include `moisture_percent`, `color_hint`, and `placement_name`. |
| `text.*_channel_caption_*` | Per-channel captions you edit directly in HA; they populate `placement_name` and the qualitative sensor title when set. |
| `number.*_dry_threshold_*` | Config threshold per channel. |
| `binary_sensor.*_dry` | Goes **on** when moisture `%` `<` channel threshold (`PROBLEM`). |

Global band edges default to **half-open `[min,max)`** ranges for the first four bands and **includes 100 % on the top band**. Adjust under *Plant monitor integration → Configure*.

Whenever a dry helper flips **off → on**, Home Assistant emits a bus event:

- **Event:** `plant_monitor_soil_low`
- **Fields:** `mac`, `channel` (index 0–5), `channel_ui`, `entity_id`, `moisture_percent`, `dry_threshold_percent`, `zone_label`, `placement_name`.

Enable **persistent notifications** in the same Configure screen if you also want HA’s inbox banner (deduplicated via `notification_id` per MAC + channel).

### Automation templates

Native event trigger:

```yaml
alias: Soil dry notify (Plant monitor event)
trigger:
  - platform: event
    event_type: plant_monitor_soil_low
action:
  - service: notify.mobile_app_REPLACE_PHONE
    data:
      message: >
        {{ trigger.event.data.entity_id }}
        bei {{ trigger.event.data.moisture_percent }}%
        Schwell {{ trigger.event.data.dry_threshold_percent }}%
        Zone {{ trigger.event.data.zone_label }}
```

Binary sensor fallback (classic):

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

### Lovelace gauge (needle card)

Pick the discovered MQTT moisture sensor (`sensor.plant_monitor_…`), then match severity bands with your Configure table:

```yaml
type: gauge
entity: sensor.PLANT_REPLACE_m0   # MQTT-discovered `%` soil sensor (index = channel)
min: 0
max: 100
needle: true
severity:
  green: 40      # aligns with optimal band min (adjust to match your Configure table)
  yellow: 20
  red: 0
```

Some HA dashboards also support segmented/custom cards (`apexcharts-card`, Mushroom gauge, etc.) if you need exact color bands mirroring zone labels.


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
- The integration listens to **`{prefix}/{mac}/meta/channels`** to mirror active bits for thresholds, alarms, text editors, and qualitative sensors.

## Notifications vs database hacks

Prefer **MQTT state**, **integrations**, and **`notify` services**. Do not write Recorder/SQL manually; Recorder still logs legitimate entity updates automatically.

## Topic cheat sheet (`{prefix}` default `plant_monitor`, `{mac}` compact STA MAC)

| Topic | QoS retain | Payload |
|-------|------------|---------|
| `{prefix}/{mac}/availability` | QoS 1 retain | `online`/`offline` (LWT) |
| `{prefix}/{mac}/meta/channels` | retain | JSON `{"active":[bool×6]}` |
| `{prefix}/{mac}/meta/device` | retain | JSON `{"name":"…"}` |
| `{prefix}/{mac}/moisture/ch{N}` (`N`=`0…5`) | no retain | Soil moisture **`%`** ASCII float |
| `homeassistant/sensor/{prefix}_{mac}_m{N}/config` | retain | MQTT discovery JSON |

## Troubleshooting

- **MQTT shows offline between samples** — expected: firmware sleeps and LWT publishes `offline` until next wake publishes `online` again.
- **Integration cannot be added** — finish HA MQTT broker configuration first (`mqtt.is_connected`).
- **No MQTT entities** — ensure discovery is enabled on the MQTT integration and the broker retained messages are permitted.
- **No dry event** — thresholds must be crossed (`binary_sensor … dry` rises to `on`); captions are optional helpers only.
