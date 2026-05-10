# Home Assistant integration

Integrate the ESP32 MQTT bridge with HA in three layers:

1. **MQTT broker** — add-on (`Mosquitto`) or external broker the firmware reaches.
2. **Built-in MQTT integration** — consumes Home Assistant MQTT discovery payloads (**numeric raw soil ADC sensors** automatically).
3. **This custom integration** (`plant_monitor`) — caches the MQTT stream so **per-channel display names**, numeric **dry thresholds (raw ADC)** , `binary_sensor` dry alarms, and optional HA notifications bundle on **one merged device**.

**Dry threshold:** each channel has a **raw ADC** threshold (0–4095, default 2400). The `binary_sensor` **PROBLEM** state is **on** when the live raw reading is **above** that value (typical capacitive probes: higher count ⇔ drier soil). Adjust per plant in the device page.

**Upgrade note:** If you previously used **0–100 %** MQTT payloads and zone bands, flash the new firmware and restart Home Assistant. MQTT discovery uses new `unique_id` values (`…_soil_raw_…`); older `%` entities may become orphaned. Re-check each channel **dry threshold** in raw units. Optional **Configure** no longer edits moisture zones—only dry-alarm notifications.

**With HACS:** add this Git repository as a **Custom repository** (category **Integration**), install **Plant Monitor (ESP32 MQTT)** from HACS, restart Home Assistant, then add the integration from the UI. The root [`hacs.json`](../hacs.json) is used by HACS for metadata.

**Manual:** copy [`custom_components/plant_monitor`](../custom_components/plant_monitor/) into `config/custom_components/plant_monitor/`, restart, then *Settings → Devices & services → Add integration → Plant monitor*. Ensure MQTT connects before adding the helper.

Required fields mirror the firmware `menuconfig` defaults:

| Config entry field | Matches firmware |
|--------------------|----------------|
| MQTT topic prefix | `CONFIG_PM_MQTT_TOPIC_PREFIX` (default `plant_monitor`) |
| MAC address | Twelve lowercase hex nibbles Wi-Fi STA MAC (**no separators**, same suffix as MQTT topics logged on boot) |

## Entities and events

After pairing the device entry:

| Entity type | Purpose |
|-------------|---------|
| MQTT **`sensor`** (discovered) | Raw soil **ADC** as published (integer string; **`-1`** when no probe). Unitless. You can hide duplicates in favor of integration sensors. |
| `sensor.*_moisture_raw_*` | Companion **raw** value from the same MQTT stream: **unavailable** unless a valid 0–4095 reading was received (firmware **-1** / invalid ⇒ *no sensor* in probe enum). |
| `sensor.*_soil_probe_*` | **Enum** (`unknown` / `no_sensor` / `ok`): `no_sensor` when the payload is **&lt; 0**, **&gt; 4095**, or non-finite (typically **-1** = no capacitive probe connected). |
| `text.*_channel_caption_*` | Per-channel captions you edit in HA; they decorate sensor titles and event `placement_name`. |
| `number.*_dry_threshold_*` | Dry threshold per channel (**raw ADC**). |
| `binary_sensor.*_dry` | **PROBLEM** when raw ADC **&gt;** channel threshold — only when a valid reading is cached. |

Integration **Configure** (options flow): optional **persistent notifications** when a dry alarm turns **on**.

Whenever a dry helper flips **off → on**, Home Assistant emits a bus event:

- **Event:** `plant_monitor_soil_low`
- **Fields:** `mac`, `channel` (index 0–5), `channel_ui`, `entity_id`, `moisture_raw`, `dry_threshold_raw`, `placement_name`.

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
        raw ADC {{ trigger.event.data.moisture_raw }}
        threshold {{ trigger.event.data.dry_threshold_raw }}
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

### Lovelace (raw trend)

Use **`sensor.<device>_moisture_raw_X`** or the discovered MQTT sensor. Example stat or history card—set min/max from your own calibration (typical window is inside 0–4095):

```yaml
type: sensor
entity: sensor.PLANT_REPLACE_moisture_raw_0
graph: line
```

Some HA dashboards also support custom gauge cards if you need color bands for raw ranges.

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
- The integration listens to **`{prefix}/{mac}/meta/channels`** to mirror active bits for thresholds, alarms, and text editors.

## Notifications vs database hacks

Prefer **MQTT state**, **integrations**, and **`notify` services**. Do not write Recorder/SQL manually; Recorder still logs legitimate entity updates automatically.

## Topic cheat sheet (`{prefix}` default `plant_monitor`, `{mac}` compact STA MAC)

| Topic | QoS retain | Payload |
|-------|------------|---------|
| `{prefix}/{mac}/availability` | QoS 1 retain | `online`/`offline` (LWT) |
| `{prefix}/{mac}/meta/channels` | retain | JSON `{"active":[bool×6]}` |
| `{prefix}/{mac}/meta/device` | retain | JSON `{"name":"…"}` |
| `{prefix}/{mac}/moisture/ch{N}` (`N`=`0…5`) | no retain | **Raw ADC** ASCII integer (**`-1`** when no probe / invalid window — use integration **`_soil_probe_`** / **`_moisture_raw_`** for UI). |
| `homeassistant/sensor/{prefix}_{mac}_m{N}/config` | retain | MQTT discovery JSON (`unique_id` … `soil_raw` …, no `%` unit) |

## Troubleshooting

- **MQTT shows offline between samples** — expected: firmware sleeps and LWT publishes `offline` until next wake publishes `online` again.
- **Integration cannot be added** — finish HA MQTT broker configuration first (`mqtt.is_connected`).
- **No MQTT entities** — ensure discovery is enabled on the MQTT integration and the broker retained messages are permitted.
- **No dry event** — thresholds must be crossed (`binary_sensor … dry` rises to `on`); captions are optional helpers only.
- **MQTT echoes −1** — MQTT discovery forwards raw payloads; **`_moisture_raw_`** stays unavailable and **`_soil_probe_`** reports `no_sensor` (localized, e.g. German *Kein Sensor angeschlossen*).
- **Stale `%` entities after upgrade** — delete orphaned entities or reload MQTT discovery; thresholds must be set in **raw ADC** after migrating from older firmware.
