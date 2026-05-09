"""Plant monitor MQTT bridge — runtime data and discovery subscriptions."""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field
from typing import Any, Callable

from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers.dispatcher import async_dispatcher_send

from .const import (
    CONF_MAC_ADDRESS,
    CONF_TOPIC_PREFIX,
    DOMAIN,
    PLATFORMS,
    dispatcher_signal_update,
)

_LOGGER = logging.getLogger(__package__)

@dataclass
class PlantRuntime:
    """Holds MQTT-derived state for one ESP32 module."""

    hass: HomeAssistant
    entry: ConfigEntry
    topic_prefix: str
    mac: str
    moisture: dict[int, float] = field(default_factory=dict)
    channel_active: list[bool] = field(default_factory=lambda: [True] * 6)
    mqtt_unsubs: list[Callable[[], Any]] = field(default_factory=list)

    @property
    def base_topic(self) -> str:
        return f"{self.topic_prefix}/{self.mac}"

    def dispatch_refresh(self) -> None:
        async_dispatcher_send(self.hass, dispatcher_signal_update(self.entry.entry_id))


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Subscribe to MQTT topics then load platforms."""

    if not mqtt.is_connected(hass):
        raise ConfigEntryNotReady("MQTT broker is not connected")

    topic_prefix = entry.data[CONF_TOPIC_PREFIX]
    mac = entry.data[CONF_MAC_ADDRESS]

    runtime = PlantRuntime(
        hass=hass,
        entry=entry,
        topic_prefix=topic_prefix,
        mac=mac,
    )

    hass.data.setdefault(DOMAIN, {})
    hass.data[DOMAIN][entry.entry_id] = runtime

    base = runtime.base_topic

    @callback
    def meta_channels(msg: mqtt.ReceiveMessage) -> None:
        try:
            payload = json.loads(msg.payload)
            active = payload.get("active")
            if isinstance(active, list) and len(active) >= 6:
                runtime.channel_active = [bool(x) for x in active[:6]]
        except (json.JSONDecodeError, TypeError, ValueError):
            _LOGGER.warning("Invalid JSON on %s", msg.topic)
        runtime.dispatch_refresh()

    @callback
    def moisture(msg: mqtt.ReceiveMessage) -> None:
        parts = msg.topic.split("/")
        if not parts or not parts[-1].startswith("ch"):
            return
        try:
            ch = int(parts[-1][2:])
        except ValueError:
            return
        try:
            val = float(msg.payload)
        except (TypeError, ValueError):
            return
        runtime.moisture[ch] = val
        runtime.dispatch_refresh()

    u1 = await mqtt.async_subscribe(hass, f"{base}/meta/channels", meta_channels, 0)
    u2 = await mqtt.async_subscribe(hass, f"{base}/moisture/#", moisture, 0)
    runtime.mqtt_unsubs.extend([u1, u2])

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Remove entities and MQTT callbacks."""

    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unload_ok:
        runtime: PlantRuntime = hass.data[DOMAIN].pop(entry.entry_id)
        for unsub in runtime.mqtt_unsubs:
            unsub()
    return unload_ok
