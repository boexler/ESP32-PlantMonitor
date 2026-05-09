"""Constants for Plant monitor MQTT companion integration."""

from __future__ import annotations

from homeassistant.const import Platform

DOMAIN = "plant_monitor"

DEFAULT_TOPIC_PREFIX = "plant_monitor"

CONF_TOPIC_PREFIX = "topic_prefix"
CONF_MAC_ADDRESS = "mac_address"

DEFAULT_DRY_THRESHOLD = 35.0

PLATFORMS: tuple[Platform, ...] = (Platform.NUMBER, Platform.BINARY_SENSOR)


def normalize_mac(mac: str) -> str:
    """Return lowercase compact MAC without separators."""
    return mac.lower().replace(":", "").replace("-", "").strip()


def dispatcher_signal_update(entry_id: str) -> str:
    """Home Assistant dispatcher string for entity refresh."""
    return f"{DOMAIN}_update_{entry_id}"
