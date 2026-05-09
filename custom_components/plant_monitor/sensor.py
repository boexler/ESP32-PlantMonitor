"""Sensor platform: soil %, textual zones, and probe connection state."""

from __future__ import annotations

from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    CONF_MAC_ADDRESS,
    CONF_OPT_MOISTURE_ZONES,
    DOMAIN,
    PROBE_NO_SENSOR,
    PROBE_OK,
    PROBE_UNKNOWN,
    coerce_moisture_zones,
    default_moisture_zones,
    dispatcher_signal_update,
    resolve_moisture_zone,
)
from .helpers import channel_custom_label


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Create qualitative zone, numeric %, and probe-state sensors."""

    runtime = hass.data[DOMAIN][entry.entry_id]

    ents: list[SensorEntity] = []

    for ch in range(6):

        ents.append(PlantMoistureZoneSensor(hass, entry, runtime, ch))

        ents.append(PlantMoisturePercentSensor(hass, entry, runtime, ch))

        ents.append(PlantSoilProbeSensor(hass, entry, runtime, ch))

    async_add_entities(ents)


class PlantMoistureZoneSensor(SensorEntity):
    """Human-readable moisture band (configured globally per integration options)."""

    _attr_has_entity_name = False

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, runtime: Any, channel: int) -> None:
        """Bind an ESP32 ADC channel index (0-based) to MQTT runtime cache."""

        self.hass = hass
        self._entry = entry
        self._runtime = runtime
        self._channel = channel

        mac = entry.data[CONF_MAC_ADDRESS]

        self._attr_unique_id = f"{DOMAIN}_{mac}_moisture_zone_{channel}"

    def _effective_zones(self) -> list[dict[str, Any]]:
        """Validated zone table from entry options."""

        zones = coerce_moisture_zones((self._entry.options or {}).get(CONF_OPT_MOISTURE_ZONES))

        return zones if zones is not None else default_moisture_zones()

    def _readable_label(self) -> str:
        """Display name prioritizing optional user-authored channel naming."""

        return channel_custom_label(self._entry.options or {}, self._channel) or ""

    @property
    def name(self) -> str | None:
        """Include optional plant label alongside the qualitative zone wording."""

        custom = self._readable_label()

        ch = self._channel + 1

        if custom:

            return f"{custom} soil moisture zone"

        return f"Moisture channel {ch} soil zone"

    @property
    def available(self) -> bool:
        """Active channel with at least one valid moisture reading cached."""

        if self._channel >= len(self._runtime.channel_active):

            return False

        if not self._runtime.channel_active[self._channel]:
            return False

        return self._channel in self._runtime.moisture

    @property
    def native_value(self) -> str | None:
        """Current zone description string."""

        if not self.available:
            return None

        pct = float(self._runtime.moisture[self._channel])

        zones = self._effective_zones()

        _idx, row = resolve_moisture_zone(pct, zones)

        return str(row["label"])

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        """Numeric context for dashboards."""

        zones = self._effective_zones()

        attrs: dict[str, Any] = {"channel": self._channel + 1}

        if not self.available:

            return attrs

        pct = float(self._runtime.moisture[self._channel])

        zone_idx, row = resolve_moisture_zone(pct, zones)

        attrs.update(
            moisture_percent=round(pct, 3),
            zone_index=zone_idx,
            color_hint=row.get("color_hint", ""),
            zone_min=float(row["min"]),
            zone_max=float(row["max"]),
            placement_name=self._readable_label(),
        )

        return attrs

    @property
    def device_info(self) -> DeviceInfo:
        """Group under the unified plant monitor appliance."""

        return DeviceInfo(
            identifiers={(DOMAIN, self._entry.data[CONF_MAC_ADDRESS])},
            name=self._entry.title,
            manufacturer="Seeed Studio",
            model="XIAO ESP32-C6",
        )

    async def async_added_to_hass(self) -> None:
        """Subscribe to MQTT refresh dispatcher bursts."""

        await super().async_added_to_hass()

        sig = dispatcher_signal_update(self._entry.entry_id)

        @callback
        def _refresh() -> None:
            self.schedule_update_ha_state(False)

        self.async_on_remove(async_dispatcher_connect(self.hass, sig, _refresh))


class PlantMoisturePercentSensor(SensorEntity):
    """Numeric soil moisture derived from MQTT (excluding firmware -1)."""

    _attr_native_unit_of_measurement = "%"

    _attr_suggested_display_precision = 1

    _attr_state_class = SensorStateClass.MEASUREMENT

    _attr_has_entity_name = False

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, runtime: Any, channel: int) -> None:
        """Bind one ADC channel."""

        self.hass = hass

        self._entry = entry

        self._runtime = runtime

        self._channel = channel

        mac = entry.data[CONF_MAC_ADDRESS]

        self._attr_unique_id = f"{DOMAIN}_{mac}_moisture_pct_{channel}"

    @property

    def name(self) -> str | None:

        custom = channel_custom_label(self._entry.options or {}, self._channel) or ""

        ch = self._channel + 1

        if custom:

            return f"{custom} soil moisture"

        return f"Moisture channel {ch} soil %"

    @property

    def available(self) -> bool:

        if self._channel >= len(self._runtime.channel_active):

            return False

        if not self._runtime.channel_active[self._channel]:
            return False

        return self._runtime.probe_status.get(self._channel) == PROBE_OK and self._channel in self._runtime.moisture

    @property

    def native_value(self) -> float | None:

        if not self.available:
            return None

        return float(self._runtime.moisture[self._channel])

    @property

    def device_info(self) -> DeviceInfo:

        return DeviceInfo(
            identifiers={(DOMAIN, self._entry.data[CONF_MAC_ADDRESS])},
            name=self._entry.title,
            manufacturer="Seeed Studio",

            model="XIAO ESP32-C6",
        )

    async def async_added_to_hass(self) -> None:

        await super().async_added_to_hass()

        sig = dispatcher_signal_update(self._entry.entry_id)

        @callback

        def _refresh() -> None:

            self.schedule_update_ha_state(False)

        self.async_on_remove(async_dispatcher_connect(self.hass, sig, _refresh))


class PlantSoilProbeSensor(SensorEntity):
    """Enum: unknown vs no probe (firmware -1) vs measuring."""

    _attr_has_entity_name = False

    _attr_device_class = SensorDeviceClass.ENUM

    _attr_translation_key = "soil_probe"

    _attr_options = [PROBE_UNKNOWN, PROBE_NO_SENSOR, PROBE_OK]

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, runtime: Any, channel: int) -> None:

        self.hass = hass

        self._entry = entry

        self._runtime = runtime

        self._channel = channel

        mac = entry.data[CONF_MAC_ADDRESS]

        self._attr_unique_id = f"{DOMAIN}_{mac}_soil_probe_{channel}"

    @property

    def name(self) -> str | None:

        idx = self._channel + 1

        custom = channel_custom_label(self._entry.options or {}, self._channel) or ""

        if custom:

            return f"{custom} sensor connection"

        return f"Moisture channel {idx} probe"

    @property

    def available(self) -> bool:

        if self._channel >= len(self._runtime.channel_active):

            return False

        return bool(self._runtime.channel_active[self._channel])

    @property

    def native_value(self) -> str | None:

        if not self.available:
            return None

        return str(self._runtime.probe_status.get(self._channel, PROBE_UNKNOWN))

    @property

    def device_info(self) -> DeviceInfo:

        return DeviceInfo(
            identifiers={(DOMAIN, self._entry.data[CONF_MAC_ADDRESS])},
            name=self._entry.title,
            manufacturer="Seeed Studio",
            model="XIAO ESP32-C6",
        )

    async def async_added_to_hass(self) -> None:

        await super().async_added_to_hass()

        sig = dispatcher_signal_update(self._entry.entry_id)

        @callback

        def _refresh() -> None:

            self.schedule_update_ha_state(False)

        self.async_on_remove(async_dispatcher_connect(self.hass, sig, _refresh))

