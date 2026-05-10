"""Per-channel dry thresholds stored in the config entry options."""

from __future__ import annotations

from typing import Any

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import CONF_MAC_ADDRESS, DEFAULT_DRY_THRESHOLD, DOMAIN, dispatcher_signal_update


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Create six threshold entities."""

    runtime = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([PlantDryThreshold(hass, entry, runtime, ch) for ch in range(6)])


class PlantDryThreshold(NumberEntity):
    """Dry-soil alarm threshold as raw ADC (problem when reading is above this)."""

    _attr_has_entity_name = True
    _attr_native_min_value = 0
    _attr_native_max_value = 4095
    _attr_native_step = 1
    _attr_mode = NumberMode.BOX
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, runtime: Any, channel: int) -> None:
        self.hass = hass
        self._entry = entry
        self._runtime = runtime
        self._channel = channel

        mac = entry.data[CONF_MAC_ADDRESS]
        self._attr_unique_id = f"{DOMAIN}_{mac}_dry_threshold_{channel}"
        self._attr_name = f"Moisture channel {channel + 1} dry threshold"

    @property
    def available(self) -> bool:
        if self._channel >= len(self._runtime.channel_active):
            return False
        return bool(self._runtime.channel_active[self._channel])

    @property
    def native_value(self) -> float:
        opts = dict(self._entry.options or {})
        thresholds = opts.get("thresholds", {})
        return float(thresholds.get(str(self._channel), DEFAULT_DRY_THRESHOLD))

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

    async def async_set_native_value(self, value: float) -> None:
        opts = dict(self._entry.options or {})
        thresholds = dict(opts.get("thresholds", {}))
        thresholds[str(self._channel)] = float(value)
        self.hass.config_entries.async_update_entry(
            self._entry, options={**opts, "thresholds": thresholds}
        )
        self.async_write_ha_state()
