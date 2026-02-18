"""Intercom Native integration for Home Assistant.

This integration provides TCP-based audio streaming between browser and ESP32.
Simple mode: Browser ↔ HA ↔ ESP (port 6054)
Full mode: HA detects ESP going to "Outgoing" state and auto-starts bridge

Unlike WebRTC/go2rtc approaches, this uses simple TCP protocols
which are more reliable across NAT/firewall scenarios.
"""

import logging

from homeassistant.core import HomeAssistant, CoreState, Event
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform, EVENT_HOMEASSISTANT_STARTED
from homeassistant.helpers.discovery import async_load_platform

from .const import DOMAIN
from .websocket_api import async_register_websocket_api

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = [Platform.SENSOR]


async def async_setup(hass: HomeAssistant, config: dict) -> bool:
    """Set up Intercom Native from configuration.yaml."""
    hass.data.setdefault(DOMAIN, {})

    # Register WebSocket API commands
    async_register_websocket_api(hass)

    # Load sensor platform (creates sensor.intercom_active_devices)
    # The sensor also listens for ESP state changes to auto-start bridges
    hass.async_create_task(
        async_load_platform(hass, Platform.SENSOR, DOMAIN, {}, config)
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
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up Intercom Native from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    # Register WebSocket API if not already done
    async_register_websocket_api(hass)

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    return True
