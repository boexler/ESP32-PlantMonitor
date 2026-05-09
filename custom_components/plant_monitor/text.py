"""Text platform: per-channel editable display names stored inside config-entry options."""

from __future__ import annotations

from typing import Any

from homeassistant.components.text import TextEntity, TextMode
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity import EntityCategory
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from .const import (
    CONF_MAC_ADDRESS,
    CONF_OPT_CHANNEL_NAMES,
    DOMAIN,
    dispatcher_signal_update,
)
from .helpers import coerce_channel_names


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Expose six persisted text widgets for customizing channel captions."""

    runtime = hass.data[DOMAIN][entry.entry_id]

    async_add_entities([PlantChannelCaptionText(hass, entry, runtime, ch) for ch in range(6)])


class PlantChannelCaptionText(TextEntity):
    """Short user-authored label surfaced by sensors and dashboards."""

    _attr_has_entity_name = False
    _attr_mode = TextMode.TEXT
    _attr_native_max = 80
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, runtime: Any, channel: int) -> None:
        """Wire one ESP channel index."""

        self.hass = hass
        self._entry = entry
        self._runtime = runtime
        self._channel = channel

        mac = entry.data[CONF_MAC_ADDRESS]

        self._attr_unique_id = f"{DOMAIN}_{mac}_channel_caption_{channel}"

    @property
    def name(self) -> str | None:
        """Static tile title so each row is discoverable in the config section."""

        idx = self._channel + 1

        return f"Moisture channel {idx} display name"

    @property
    def available(self) -> bool:
        """Hide editors for channels the firmware disabled."""

        if self._channel >= len(self._runtime.channel_active):
            return False

        return bool(self._runtime.channel_active[self._channel])

    @property
    def native_value(self) -> str | None:
        """Return the stored caption string."""

        names = coerce_channel_names((self._entry.options or {}).get(CONF_OPT_CHANNEL_NAMES))

        return names.get(str(self._channel), "")

    async def async_set_value(self, value: str) -> None:
        """Persist caption edits."""

        trimmed = "" if value is None else value.strip()

        opts = dict(self._entry.options or {})

        names = coerce_channel_names(opts.get(CONF_OPT_CHANNEL_NAMES))

        names[str(self._channel)] = trimmed

        opts[CONF_OPT_CHANNEL_NAMES] = names

        self.hass.config_entries.async_update_entry(self._entry, options=opts)

        self.async_write_ha_state()

        self._runtime.dispatch_refresh()

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
        """Refresh when MQTT metadata toggles."""

        await super().async_added_to_hass()

        sig = dispatcher_signal_update(self._entry.entry_id)

        @callback
        def _refresh() -> None:
            self.schedule_update_ha_state(False)

        self.async_on_remove(async_dispatcher_connect(self.hass, sig, _refresh))
