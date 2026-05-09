# ESP32-C6 Plant Monitor

ESP-IDF firmware for Seeed Studio **XIAO ESP32-C6** plus a **Home Assistant MQTT companion** integration: six soil-moisture ADC channels, MQTT discovery metadata, host-driven per-channel masking, and configurable dry thresholds and alarms stored in Home Assistant (not direct database writes).

| Path | Purpose |
|------|---------|
| [`firmware/`](firmware/) | ESP-IDF **5.x / 6.x**, target `esp32c6`; Wi-Fi, NTP, HTTP JSON config, MQTT + Home Assistant discovery. |
| [`custom_components/plant_monitor/`](custom_components/plant_monitor/) | Six `number` thresholds + six `binary_sensor` dry alarms linked to MQTT telemetry. |
| [`docs/hardware_xiao_esp32c6.md`](docs/hardware_xiao_esp32c6.md) | MCU ↔ pin mapping for the six analog inputs. |
| [`docs/homeassistant.md`](docs/homeassistant.md) | MQTT topic contract, `www/` JSON example, integration steps. |

## Firmware quick start

1. Install [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (**5.x or 6.x**) with `esp32c6` support.
2. `cd firmware`
3. If **`set-target`** ever failed or `build` is not a clean CMake tree: delete the folder manually, then continue (PowerShell: `Remove-Item -Recurse -Force .\build` from `firmware/`).
4. **Set the MCU first** — this writes `sdkconfig` for **esp32c6**. If you skip this, CMake may default to **esp32** and the toolchain will be wrong:

   ```bash
   idf.py set-target esp32c6
   ```

5. **ESP-IDF 6+ managed components:** MQTT and cJSON are no longer bundled as `components/mqtt` and `components/json`. This repo lists `espressif/mqtt` and `espressif/cjson` in [`firmware/main/idf_component.yml`](firmware/main/idf_component.yml); the first configure/build downloads them (**network required**).  
   If CMake reports **`unknown component 'mqtt'`** or **`unknown component 'json'`** (or missing cJSON), add the managed deps once from **`firmware/`**:

   ```bash
   idf.py add-dependency "espressif/mqtt"
   idf.py add-dependency "espressif/cjson"
   ```

6. **`idf.py menuconfig`** — optional but recommended to set Wi‑Fi, broker URL, device registry URL, and **Power / sleep behaviour** (see below).  
   `menuconfig` runs CMake like `build`; it does **not** replace `set-target`.

   - **Plant Monitor (ESP32-C6)** → **Power / sleep behaviour**:
     - **Deep sleep between measurements** (default): uses *Deep sleep duration (seconds)*, then sleeps — best on battery.
     - **Continuous (debug, no deep sleep)**: loop + delay between measurements — best for USB serial debugging (no prompt during `flash`; you choose here).

7. **`idf.py build`** — optional explicit compile (or go straight to **`idf.py flash monitor`**, which builds if needed):

   ```bash
   idf.py build
   idf.py -p PORT flash monitor
   ```

The serial log prints the **compact Wi-Fi MAC** used when adding the Home Assistant integration.

### Firmware build troubleshooting

- **`fullclean` / `set-target` refuses** (“doesn't seem to be a CMake build directory”): delete **`firmware/build` manually**, then run `idf.py set-target esp32c6` again.
- **Wrong chip in the log** (`esp32` vs `esp32c6`, or wrong compiler triplet): you ran `menuconfig` / `build` **before** `idf.py set-target esp32c6`, or `sdkconfig` is stale — delete `build`, run `set-target`, rebuild.

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
