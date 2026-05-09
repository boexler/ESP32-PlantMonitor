"""Config flow: link Home Assistant MQTT to a firmware device by topic + MAC."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry, OptionsFlow
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.dispatcher import async_dispatcher_send

from .const import (
    CONF_MAC_ADDRESS,
    CONF_TOPIC_PREFIX,
    CONF_OPT_MOISTURE_ZONES,
    CONF_OPT_NOTIFY_DRY_PERSISTENT,
    CONF_OPT_CHANNEL_NAMES,
    DEFAULT_DRY_THRESHOLD,
    DEFAULT_TOPIC_PREFIX,
    DOMAIN,
    coerce_moisture_zones,
    default_moisture_zones,
    dispatcher_signal_update,
    normalize_mac,
)


def _mqtt_ready(hass) -> bool:
    """Return True if MQTT broker is reachable."""

    return mqtt.is_connected(hass)


def _effective_moisture_zones(opts: dict[str, Any]) -> list[dict[str, Any]]:
    """Resolved moisture_zones list with defaults."""

    zones = coerce_moisture_zones(opts.get(CONF_OPT_MOISTURE_ZONES))

    return zones if zones is not None else default_moisture_zones()


class PlantMonitorOptionsFlow(OptionsFlow):
    """Configure global moisture zone labels/ranges and dry notify behavior."""

    def __init__(self, entry: ConfigEntry) -> None:
        """Pass the config entry owning these options."""

        self._entry = entry

    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> FlowResult:
        """Show or submit the zones + notification checkbox."""

        errs: dict[str, str] = {}

        zones = _effective_moisture_zones(dict(self._entry.options or {}))

        if user_input is not None:
            built: list[dict[str, Any]] = []

            for i in range(len(zones)):
                built.append(
                    {
                        "min": float(user_input[f"zone_{i}_min"]),
                        "max": float(user_input[f"zone_{i}_max"]),
                        "label": str(user_input[f"zone_{i}_label"]).strip(),
                        "color_hint": str(user_input.get(f"zone_{i}_color", "") or "").strip(),
                    },
                )

            coerced = coerce_moisture_zones(built)

            if coerced is None:
                errs["base"] = "invalid_zones"
            else:

                merged = {
                    **dict(self._entry.options or {}),

                    CONF_OPT_MOISTURE_ZONES: coerced,

                    CONF_OPT_NOTIFY_DRY_PERSISTENT: bool(
                        user_input.get(CONF_OPT_NOTIFY_DRY_PERSISTENT, False),
                    ),
                }

                self.hass.config_entries.async_update_entry(
                    self._entry,
                    options=merged,
                )
                async_dispatcher_send(
                    self.hass,
                    dispatcher_signal_update(self._entry.entry_id),
                )

                return self.async_create_entry(title="", data={})

        schema_kv: dict[Any, Any] = {
            vol.Required(
                CONF_OPT_NOTIFY_DRY_PERSISTENT,
                default=bool((self._entry.options or {}).get(CONF_OPT_NOTIFY_DRY_PERSISTENT, False)),
            ): cv.boolean,
        }

        for i, z in enumerate(zones):
            schema_kv[vol.Required(f"zone_{i}_min", default=z["min"])] = vol.Coerce(float)

            schema_kv[vol.Required(f"zone_{i}_max", default=z["max"])] = vol.Coerce(float)

            schema_kv[vol.Required(f"zone_{i}_label", default=z["label"])] = str

            schema_kv[vol.Optional(f"zone_{i}_color", default=z.get("color_hint", ""))] = str

        return self.async_show_form(
            step_id="init",
            data_schema=vol.Schema(schema_kv),

            errors=errs,
        )


class PlantMonitorFlowHandler(config_entries.ConfigFlow, domain=DOMAIN):
    """Manual pairing using MQTT topic prefix and compact Wi-Fi MAC."""

    VERSION = 1

    @staticmethod
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:

        """Options flow hook for integrations UI."""

        return PlantMonitorOptionsFlow(config_entry)

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> FlowResult:
        """Ask for MQTT parameters."""

        errors: dict[str, str] = {}

        if user_input is not None:
            prefix = user_input[CONF_TOPIC_PREFIX].strip().strip("/")

            mac_compact = normalize_mac(user_input[CONF_MAC_ADDRESS])

            if len(mac_compact) != 12:

                errors["base"] = "bad_mac"

            elif not mac_compact.isalnum():
                errors["base"] = "bad_mac"

            elif not _mqtt_ready(self.hass):
                errors["base"] = "mqtt_not_connected"
            else:
                await self.async_set_unique_id(mac_compact)

                self._abort_if_unique_id_configured(
                    updates={
                        CONF_TOPIC_PREFIX: prefix,

                        CONF_MAC_ADDRESS: mac_compact,
                    },

                )

                data = {

                    CONF_TOPIC_PREFIX: prefix,

                    CONF_MAC_ADDRESS: mac_compact,
                }

                fname = user_input.get("friendly_name")

                title = (
                    fname.strip()
                    if isinstance(fname, str) and fname.strip()
                    else mac_compact
                )

                options = {
                    "thresholds": {str(i): DEFAULT_DRY_THRESHOLD for i in range(6)},
                    CONF_OPT_MOISTURE_ZONES: default_moisture_zones(),
                    CONF_OPT_NOTIFY_DRY_PERSISTENT: False,
                    CONF_OPT_CHANNEL_NAMES: {},
                }

                return self.async_create_entry(title=title, data=data, options=options)

        schema = vol.Schema(
            {
                vol.Required(CONF_TOPIC_PREFIX, default=DEFAULT_TOPIC_PREFIX): str,
                vol.Required(CONF_MAC_ADDRESS): str,
                vol.Optional("friendly_name"): str,
            }
        )

        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)
