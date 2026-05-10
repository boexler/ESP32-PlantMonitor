"""Plant monitor MQTT bridge — runtime data and discovery subscriptions."""

from __future__ import annotations


import asyncio
import json
import logging
import math
from dataclasses import dataclass, field
from typing import Any, Callable

from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback, Event
from homeassistant.exceptions import ConfigEntryNotReady
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.event import async_track_state_change_event

from .const import (
    ADC_RAW_MAX,
    CONF_MAC_ADDRESS,
    CONF_OPT_NOTIFY_DRY_PERSISTENT,
    CONF_TOPIC_PREFIX,
    DEFAULT_DRY_THRESHOLD,
    DOMAIN,
    EVENT_SOIL_MOISTURE_LOW,
    PLATFORMS,
    PROBE_NO_SENSOR,
    PROBE_OK,
    dispatcher_signal_update,
)
from .helpers import channel_custom_label, ensure_plant_monitor_options

_LOGGER = logging.getLogger(__package__)

_DRY_UID_MARK = "_dry_alarm_"


@dataclass
class PlantRuntime:
    """Holds MQTT-derived state for one ESP32 module."""

    hass: HomeAssistant
    entry: ConfigEntry
    topic_prefix: str
    mac: str
    moisture: dict[int, float] = field(default_factory=dict)
    probe_status: dict[int, str] = field(default_factory=dict)
    channel_active: list[bool] = field(default_factory=lambda: [True] * 6)
    mqtt_unsubs: list[Callable[[], Any]] = field(default_factory=list)

    @property
    def base_topic(self) -> str:
        """Root MQTT topic `{prefix}/{compact_mac}`."""

        return f"{self.topic_prefix}/{self.mac}"

    def dispatch_refresh(self) -> None:
        """Notify platform entities that derived state may have changed."""

        async_dispatcher_send(self.hass, dispatcher_signal_update(self.entry.entry_id))


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Subscribe to MQTT topics then load platforms."""

    if not mqtt.is_connected(hass):
        raise ConfigEntryNotReady("MQTT broker is not connected")

    merged_opts, patched = ensure_plant_monitor_options(entry.options or {})
    if patched:
        hass.config_entries.async_update_entry(entry, options=merged_opts)

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

    entry.async_on_unload(entry.add_update_listener(_async_reload_entry_options))

    @callback

    def meta_channels(msg: mqtt.ReceiveMessage) -> None:

        try:

            payload = json.loads(msg.payload)

            active = payload.get("active")

            if isinstance(active, list) and len(active) >= 6:

                runtime.channel_active = [bool(x) for x in active[:6]]

                for idx in range(6):

                    if not runtime.channel_active[idx]:

                        runtime.moisture.pop(idx, None)

                        runtime.probe_status.pop(idx, None)

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

        if ch < 0 or ch >= 6:

            return

        if ch < len(runtime.channel_active) and not runtime.channel_active[ch]:

            return

        try:

            val = float(msg.payload)

        except (TypeError, ValueError):

            return

        if (
            not math.isfinite(val)
            or val < 0.0
            or val > ADC_RAW_MAX
        ):
            runtime.moisture.pop(ch, None)

            runtime.probe_status[ch] = PROBE_NO_SENSOR

        else:

            runtime.moisture[ch] = val

            runtime.probe_status[ch] = PROBE_OK

        runtime.dispatch_refresh()

    u1 = await mqtt.async_subscribe(hass, f"{base}/meta/channels", meta_channels, 0)

    u2 = await mqtt.async_subscribe(hass, f"{base}/moisture/#", moisture, 0)

    runtime.mqtt_unsubs.extend([u1, u2])

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)

    hass.async_create_task(_schedule_dry_watchers(runtime))

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Remove entities and MQTT callbacks."""

    unload_ok = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)

    if unload_ok:

        runtime: PlantRuntime = hass.data[DOMAIN].pop(entry.entry_id)

        for unsub in runtime.mqtt_unsubs:

            unsub()

    return unload_ok


async def _async_reload_entry_options(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Recompute textual sensors whenever integration options mutate."""

    runtime = hass.data.get(DOMAIN, {}).get(entry.entry_id)

    if runtime:

        runtime.dispatch_refresh()


async def _async_broadcast_soil_low(runtime: PlantRuntime, event: Event) -> None:
    """Emit bus event plus optional HA persistent notification."""

    hass = runtime.hass

    old_state = event.data.get("old_state")

    new_state = event.data.get("new_state")

    if old_state is None or new_state is None:

        return

    if old_state.state != "off" or new_state.state != "on":
        return

    entity_id = event.data["entity_id"]

    reg = er.async_get(hass)

    reg_entry = reg.async_get(entity_id)

    if reg_entry is None or _DRY_UID_MARK not in reg_entry.unique_id:

        return

    try:

        ch = int(reg_entry.unique_id.split(_DRY_UID_MARK)[-1])

    except ValueError:

        return

    if not 0 <= ch <= 5:

        return

    opts = dict(runtime.entry.options or {})

    thresholds = opts.get("thresholds", {})

    try:

        thresh = float(thresholds.get(str(ch), DEFAULT_DRY_THRESHOLD))

    except (TypeError, ValueError):

        thresh = float(DEFAULT_DRY_THRESHOLD)

    mo = runtime.moisture.get(ch)

    if mo is None:

        return

    raw_val = float(mo)

    placement = channel_custom_label(opts, ch)

    payload = {

        "mac": runtime.mac,
        "channel": ch,

        "channel_ui": ch + 1,

        "entity_id": entity_id,
        "moisture_raw": raw_val,
        "dry_threshold_raw": thresh,
        "placement_name": placement,
    }

    hass.bus.async_fire(EVENT_SOIL_MOISTURE_LOW, payload)

    if not opts.get(CONF_OPT_NOTIFY_DRY_PERSISTENT):
        return

    cap = placement or f"Channel {ch + 1}"

    title = "Plant monitor — dry soil alarm"

    message = (
        f"{cap}: soil raw ADC {raw_val:.0f} (dry threshold {thresh:.0f})."
    )

    nid = f"plant_monitor_low_{runtime.mac}_{ch}"

    await hass.services.async_call(
        "persistent_notification",

        "create",

        {"title": title, "message": message, "notification_id": nid},
    )


def _gather_dry_entity_ids(runtime: PlantRuntime) -> list[str]:
    """Resolve entity_ids for firmware dry-binary sensors belonging to this entry."""

    registry = er.async_get(runtime.hass)

    dry_ids: list[str] = []

    for ch in range(6):

        uid = f"{DOMAIN}_{runtime.mac}_dry_alarm_{ch}"

        eid = registry.async_get_entity_id("binary_sensor", DOMAIN, uid)

        if eid:

            dry_ids.append(eid)

    return dry_ids


def _install_dry_alarm_listener(runtime: PlantRuntime, dry_ids: list[str]) -> Callable[[], None]:
    """Register a ONE state-change watcher for crossing into the dry/problem state."""

    hass = runtime.hass

    @callback
    def _edge(event: Event) -> None:

        old_state = event.data.get("old_state")

        new_state = event.data.get("new_state")

        if old_state is None or new_state is None:

            return

        if old_state.state != "off" or new_state.state != "on":
            return

        hass.async_create_task(_async_broadcast_soil_low(runtime, event))

    return async_track_state_change_event(hass, dry_ids, _edge)


async def _schedule_dry_watchers(runtime: PlantRuntime) -> None:
    """Binary entities may finish registering milliseconds after MQTT setup."""

    hass = runtime.hass

    entry_id = runtime.entry.entry_id

    try:

        for _ in range(30):

            if hass.data.get(DOMAIN, {}).get(entry_id) is not runtime:

                return

            dry_ids = _gather_dry_entity_ids(runtime)

            if dry_ids:

                unsub = _install_dry_alarm_listener(runtime, dry_ids)

                runtime.mqtt_unsubs.append(unsub)

                return

            await asyncio.sleep(1)

        _LOGGER.debug(
            "Dry-alarm watchers not attached for %s; entities may appear later after reload",
            runtime.mac,
        )

    except asyncio.CancelledError:

        raise
