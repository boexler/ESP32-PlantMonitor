"""Constants for Plant monitor MQTT companion integration."""

from __future__ import annotations

from homeassistant.const import Platform

DOMAIN = "plant_monitor"

DEFAULT_TOPIC_PREFIX = "plant_monitor"

CONF_TOPIC_PREFIX = "topic_prefix"
CONF_MAC_ADDRESS = "mac_address"

CONF_OPT_NOTIFY_DRY_PERSISTENT = "notify_dry_persistent"
CONF_OPT_CHANNEL_NAMES = "channel_names"

EVENT_SOIL_MOISTURE_LOW = "plant_monitor_soil_low"

# Firmware publishes -1 when no probe or invalid raw window.
PROBE_UNKNOWN = "unknown"
PROBE_NO_SENSOR = "no_sensor"
PROBE_OK = "ok"

# Default dry alarm threshold (raw ADC); higher raw = drier on typical capacitive probes.
DEFAULT_DRY_THRESHOLD = 2400.0

ADC_RAW_MAX = 4095.0

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
