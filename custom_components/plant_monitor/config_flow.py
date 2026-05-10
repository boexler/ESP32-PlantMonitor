"""Config flow: link Home Assistant MQTT to a firmware device by topic + MAC."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.components import mqtt
from homeassistant.config_entries import ConfigEntry, OptionsFlow
from homeassistant.data_entry_flow import FlowResult
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.dispatcher import async_dispatcher_send

from .const import (
    CONF_MAC_ADDRESS,
    CONF_TOPIC_PREFIX,
    CONF_OPT_NOTIFY_DRY_PERSISTENT,
    CONF_OPT_CHANNEL_NAMES,
    DEFAULT_DRY_THRESHOLD,
    DEFAULT_TOPIC_PREFIX,
    DOMAIN,
    dispatcher_signal_update,
    normalize_mac,
)


def _mqtt_ready(hass) -> bool:
    """Return True if MQTT broker is reachable."""

    return mqtt.is_connected(hass)


class PlantMonitorOptionsFlow(OptionsFlow):
    """Configure optional dry-alarm notifications."""

    def __init__(self, entry: ConfigEntry) -> None:
        """Pass the config entry owning these options."""

        self._entry = entry

    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> FlowResult:
        """Show or submit the notification checkbox."""

        if user_input is not None:
            merged = {
                **dict(self._entry.options or {}),
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

        schema = vol.Schema(
            {
                vol.Required(
                    CONF_OPT_NOTIFY_DRY_PERSISTENT,
                    default=bool((self._entry.options or {}).get(CONF_OPT_NOTIFY_DRY_PERSISTENT, False)),
                ): cv.boolean,
            },
        )

        return self.async_show_form(step_id="init", data_schema=schema)


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
                    CONF_OPT_NOTIFY_DRY_PERSISTENT: False,
                    CONF_OPT_CHANNEL_NAMES: {},
                }

                return self.async_create_entry(title=title, data=data, options=options)

        schema = vol.Schema(
            {
                vol.Required(CONF_TOPIC_PREFIX, default=DEFAULT_TOPIC_PREFIX): str,
                vol.Required(CONF_MAC_ADDRESS): str,
                vol.Optional("friendly_name"): str,
            },
        )

        return self.async_show_form(step_id="user", data_schema=schema, errors=errors)
