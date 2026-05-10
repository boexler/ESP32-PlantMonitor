"""Binary sensors: dry soil when raw ADC is above configured threshold."""

from __future__ import annotations

from typing import Any

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import CONF_MAC_ADDRESS, DEFAULT_DRY_THRESHOLD, DOMAIN, dispatcher_signal_update


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    runtime = hass.data[DOMAIN][entry.entry_id]
    async_add_entities([PlantDryAlarm(hass, entry, runtime, ch) for ch in range(6)])


class PlantDryAlarm(BinarySensorEntity):
    """On when the live raw ADC reading exceeds the configured dry threshold."""

    _attr_has_entity_name = True
    _attr_device_class = BinarySensorDeviceClass.PROBLEM

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, runtime: Any, channel: int) -> None:
        self.hass = hass
        self._entry = entry
        self._runtime = runtime
        self._channel = channel

        mac = entry.data[CONF_MAC_ADDRESS]
        self._attr_unique_id = f"{DOMAIN}_{mac}_dry_alarm_{channel}"
        self._attr_name = f"Moisture channel {channel + 1} dry"

    @property
    def available(self) -> bool:
        if self._channel >= len(self._runtime.channel_active):
            return False
        if not self._runtime.channel_active[self._channel]:
            return False
        return self._channel in self._runtime.moisture

    @property
    def is_on(self) -> bool | None:
        if not self.available:
            return None
        mo = self._runtime.moisture.get(self._channel)
        if mo is None:
            return None

        opts = dict(self._entry.options or {})
        thresholds = opts.get("thresholds", {})
        th = float(thresholds.get(str(self._channel), DEFAULT_DRY_THRESHOLD))
        return mo > th

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
