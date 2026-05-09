# ESP32-C6 Plant Monitor

ESP-IDF firmware for Seeed Studio **XIAO ESP32-C6** plus a **Home Assistant MQTT companion** integration: six soil-moisture ADC channels, MQTT discovery metadata, host-driven per-channel masking, and configurable dry thresholds and alarms stored in Home Assistant (not direct database writes).

| Path | Purpose |
|------|---------|
| [`firmware/`](firmware/) | ESP-IDF 5.x project targeting `esp32c6`; Wi-Fi, NTP, HTTP JSON config, MQTT + Home Assistant discovery. |
| [`custom_components/plant_monitor/`](custom_components/plant_monitor/) | Six `number` thresholds + six `binary_sensor` dry alarms linked to MQTT telemetry. |
| [`docs/hardware_xiao_esp32c6.md`](docs/hardware_xiao_esp32c6.md) | MCU ↔ pin mapping for the six analog inputs. |
| [`docs/homeassistant.md`](docs/homeassistant.md) | MQTT topic contract, `www/` JSON example, integration steps. |

## Firmware quick start

1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (5.x) with `esp32c6` support.
2. `cd firmware`
3. `idf.py set-target esp32c6`
4. `idf.py menuconfig`
   - **Plant Monitor (ESP32-C6)** → **Power / sleep behaviour**:
     - **Deep sleep between measurements** (default): wakes on the interval configured as *Deep sleep duration (seconds)*, then sleeps again — best for battery operation.
     - **Continuous (debug, no deep sleep)**: runs a loop measuring and publishing over MQTT without deep sleep — best for debugging over USB serial (nothing is prompted during `idf.py flash`; you choose here before building).
   - Configure Wi-Fi, MQTT broker URI, and device registry URL (for example `http://homeassistant.local/local/plant_monitor_devices.json`).
5. `idf.py -p PORT flash monitor`

The serial log prints the **compact Wi-Fi MAC** used when adding the custom integration.

## Home Assistant

### Install with HACS (custom repository)

1. In Home Assistant, open **HACS** → **Integrations** (the left tab for integrations, not "Frontend").
2. Open the menu (three dots, top right) → **Custom repositories**.
3. **Repository**: paste your Git URL, e.g. `https://github.com/boexler/ESP32-PlantMonitor` (use SSH if you prefer: `git@github.com:boexler/ESP32-PlantMonitor.git`).
4. **Category**: select **Integration**.
5. Click **Add**. HACS reads the root [`hacs.json`](hacs.json) and lists the integration.
6. Open the new **Plant Monitor (ESP32 MQTT)** card → **Download** (pick a version or the default branch).
7. **Restart Home Assistant** (Settings → System → Restart).
8. **Settings** → **Devices & services** → **Add integration** → **Plant monitor**, then enter MQTT topic prefix and device MAC (see [`docs/homeassistant.md`](docs/homeassistant.md)).

Requires the built-in **MQTT** integration to be connected to your broker before adding this integration.

### Manual install (without HACS)

Copy `custom_components/plant_monitor` into your HA `config` folder, restart, then add the **Plant monitor** integration as above. Details: [`docs/homeassistant.md`](docs/homeassistant.md).
