Seeed Studio XIAO ESP32-C6 exposes **six MCU ADC-capable nets** wired as:

| User channel | Physical pin label | MCU GPIO |
|-------------|---------------------|----------|
| 1 | **D0** | GPIO0 |
| 2 | **D1** | GPIO1 |
| 3 | **D2** | GPIO2 |
| 4 | **MTMS** test pad | GPIO4 |
| 5 | **MTDI** test pad | GPIO5 |
| 6 | **MTCK** test pad | GPIO6 |

Source: [Seeed Wiki – XIAO ESP32C6 pin map](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/).

Firmware maps **software indices 0…5 → GPIO0,1,2,4,5,6**.

### Notes

- **JTAG pads** GPIO4–6 double as ADC and debug; avoid holding them in incompatible states during measurement.
- **RF switch pins** GPIO3/GPIO14 control the onboard FM8625H (external U.FL vs internal ceramic). They are unrelated to moisture channels. In firmware, choose **Plant Monitor → Wi-Fi antenna (XIAO ESP32-C6)** in `idf.py menuconfig`; **default is external (U.FL)**. Internal mode sets both pins to high-impedance inputs per Seeed’s guidance.
- **MQTT** publishes averaged **raw ADC** counts (and `-1` when the firmware detects no probe); dry alarms use a **raw threshold** in Home Assistant (higher counts usually mean drier soil on typical capacitive probes).
- **Attenuation** uses IDF maximum span (`ADC_ATTEN_DB_12`) so mixed probe wiring still fits inside the SAR range — adjust attenuation/bit width only if probes rail.
- **Sampling** averages `CONFIG_PM_ADC_SAMPLE_COUNT` reads per wake.
