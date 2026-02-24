"""Intercom Native integration for Home Assistant.

This integration provides TCP-based audio streaming between browser and ESP32.
Simple mode: Browser ↔ HA ↔ ESP (port 6054)
Full mode: HA detects ESP going to "Outgoing" state and auto-starts bridge

Unlike WebRTC/go2rtc approaches, this uses simple TCP protocols
which are more reliable across NAT/firewall scenarios.
"""

import logging

import voluptuous as vol

from homeassistant.core import HomeAssistant, CoreState, Event
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform, EVENT_HOMEASSISTANT_STARTED
from homeassistant.helpers import config_validation as cv
from homeassistant.helpers.discovery import async_load_platform

from .const import DOMAIN
from .websocket_api import async_register_websocket_api

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [Platform.SENSOR]


async def _async_setup_shared(hass: HomeAssistant, config: dict | None = None) -> None:
    """Shared setup logic for both YAML and config entry."""
    if hass.data.get(DOMAIN, {}).get("initialized"):
        return  # Already set up (e.g. YAML + config entry both present)

    hass.data.setdefault(DOMAIN, {})
    hass.data[DOMAIN]["initialized"] = True

    # Register WebSocket API commands
    async_register_websocket_api(hass)

    # Load sensor platform
    hass.async_create_task(
        async_load_platform(hass, Platform.SENSOR, DOMAIN, {}, config or {})
    )

    # Register frontend (Lovelace card auto-served from integration)
    async def _register_frontend(_event: Event | None = None) -> None:
        from .frontend import JSModuleRegistration
        registration = JSModuleRegistration(hass)
        await registration.async_register()

    if hass.state == CoreState.running:
        await _register_frontend(None)
    else:
        hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STARTED, _register_frontend)

    _LOGGER.info("Intercom Native integration loaded (simple + full mode auto-bridge)")


async def async_setup(hass: HomeAssistant, config: dict) -> bool:
    """Set up Intercom Native from configuration.yaml (legacy support)."""
    await _async_setup_shared(hass, config)
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up Intercom Native from a config entry (UI setup)."""
    await _async_setup_shared(hass)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    return True
