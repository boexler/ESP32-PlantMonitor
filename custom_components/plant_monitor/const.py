"""Constants for Plant monitor MQTT companion integration."""

from __future__ import annotations

import copy
from typing import Any

from homeassistant.const import Platform

DOMAIN = "plant_monitor"

DEFAULT_TOPIC_PREFIX = "plant_monitor"

CONF_TOPIC_PREFIX = "topic_prefix"
CONF_MAC_ADDRESS = "mac_address"

CONF_OPT_MOISTURE_ZONES = "moisture_zones"
CONF_OPT_NOTIFY_DRY_PERSISTENT = "notify_dry_persistent"
CONF_OPT_CHANNEL_NAMES = "channel_names"

EVENT_SOIL_MOISTURE_LOW = "plant_monitor_soil_low"

DEFAULT_DRY_THRESHOLD = 35.0

# Global moisture zones: half-open [min,max) except the last row, which includes max (100%).
DEFAULT_MOISTURE_ZONES: list[dict[str, Any]] = [
    {"min": 0, "max": 20, "label": "sehr trocken", "color_hint": "red"},
    {"min": 20, "max": 40, "label": "trocken", "color_hint": "orange"},
    {"min": 40, "max": 70, "label": "optimal", "color_hint": "green"},
    {"min": 70, "max": 85, "label": "feucht", "color_hint": "teal"},
    {"min": 85, "max": 100, "label": "zu nass", "color_hint": "blue"},
]

PLATFORMS: tuple[Platform, ...] = (
    Platform.NUMBER,
    Platform.BINARY_SENSOR,
    Platform.SENSOR,
    Platform.TEXT,
)


def normalize_mac(mac: str) -> str:
    """Return lowercase compact MAC without separators."""
    return mac.lower().replace(":", "").replace("-", "").strip()


def dispatcher_signal_update(entry_id: str) -> str:
    """Home Assistant dispatcher string for entity refresh."""
    return f"{DOMAIN}_update_{entry_id}"


def coerce_moisture_zones(raw: Any) -> list[dict[str, Any]] | None:
    """Normalize moisture_zones options; returns None when invalid."""

    if not isinstance(raw, list):
        return None

    normalized: list[dict[str, Any]] = []

    for row in raw:
        if not isinstance(row, dict):
            return None

        label = row.get("label")

        color_hint = row.get("color_hint", "")

        try:
            min_v = float(row["min"])

            max_v = float(row["max"])

        except (KeyError, TypeError, ValueError):
            return None

        if not isinstance(label, str) or label.strip() == "":
            return None

        if min_v >= max_v:
            return None

        if not isinstance(color_hint, str):

            color_hint = str(color_hint) if color_hint is not None else ""

        normalized.append(
            {"min": min_v, "max": max_v, "label": label.strip(), "color_hint": color_hint.strip()},
        )

    if len(normalized) == 0:
        return None

    normalized_sorted = sorted(normalized, key=lambda z: z["min"])

    for a, b in zip(normalized_sorted, normalized_sorted[1:], strict=False):

        if a["min"] >= b["min"]:
            return None

    return normalized_sorted


def default_moisture_zones() -> list[dict[str, Any]]:
    """Deep copy of the default moisture zone table."""

    return copy.deepcopy(DEFAULT_MOISTURE_ZONES)


def resolve_moisture_zone(pct: float, zones: list[dict[str, Any]]) -> tuple[int, dict[str, Any]]:
    """Map moisture percent (0–100) to zone index and row dict."""

    if not zones:
        return (-1, {"label": "", "color_hint": ""})

    v = pct

    for idx, zone in enumerate(zones):
        lo = float(zone["min"])

        hi = float(zone["max"])

        last = idx == len(zones) - 1

        if last:
            if v >= lo and v <= hi:
                return (idx, zone)

        elif lo <= v < hi:
            return (idx, zone)

    last_row = zones[-1]

    return (len(zones) - 1, last_row)
