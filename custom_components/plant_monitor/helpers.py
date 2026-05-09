"""Shared helpers for config entry options."""

from __future__ import annotations

from typing import Any, Mapping

from .const import (
    CONF_OPT_CHANNEL_NAMES,
    CONF_OPT_MOISTURE_ZONES,
    CONF_OPT_NOTIFY_DRY_PERSISTENT,
    coerce_moisture_zones,
    default_moisture_zones,
)


def coerce_channel_names(raw: Any) -> dict[str, str]:
    """Normalize channel_names mapping to dict[str,str] with string keys."""

    out: dict[str, str] = {}

    if not isinstance(raw, dict):

        return out

    for key, val in raw.items():

        out[str(key)] = "" if val is None else str(val)

    return out


def ensure_plant_monitor_options(options: Mapping[str, Any]) -> tuple[dict[str, Any], bool]:
    """Return merged options dict and True when defaults were patched in."""

    opts = dict(options)

    changed = False

    if coerce_moisture_zones(opts.get(CONF_OPT_MOISTURE_ZONES)) is None:

        opts[CONF_OPT_MOISTURE_ZONES] = default_moisture_zones()

        changed = True

    if CONF_OPT_NOTIFY_DRY_PERSISTENT not in opts:

        opts[CONF_OPT_NOTIFY_DRY_PERSISTENT] = False

        changed = True

    raw_cn = opts.get(CONF_OPT_CHANNEL_NAMES)

    if CONF_OPT_CHANNEL_NAMES not in opts or not isinstance(raw_cn, dict):

        opts[CONF_OPT_CHANNEL_NAMES] = coerce_channel_names(raw_cn)

        changed = True

    else:

        norm_cn = coerce_channel_names(raw_cn)

        if norm_cn != raw_cn:

            opts[CONF_OPT_CHANNEL_NAMES] = norm_cn

            changed = True

    return opts, changed


def channel_custom_label(opts: Mapping[str, Any], channel: int) -> str | None:
    """Optional user-defined channel title; trimmed empty string yields None."""

    names = coerce_channel_names(opts.get(CONF_OPT_CHANNEL_NAMES))

    text = names.get(str(channel), "").strip()

    return text or None
