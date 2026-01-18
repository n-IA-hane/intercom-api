"""Intercom Native integration for Home Assistant.

This integration provides TCP-based audio streaming between browser and ESP32.
Simple P2P mode: Browser ↔ HA ↔ ESP (port 6054)

Unlike WebRTC/go2rtc approaches, this uses simple TCP protocols
which are more reliable across NAT/firewall scenarios.
"""

import logging

from homeassistant.core import HomeAssistant
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform

from .const import DOMAIN
from .websocket_api import async_register_websocket_api

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = []


async def async_setup(hass: HomeAssistant, config: dict) -> bool:
    """Set up Intercom Native from configuration.yaml."""
    hass.data.setdefault(DOMAIN, {})

    # Register WebSocket API commands
    async_register_websocket_api(hass)

    _LOGGER.info("Intercom Native integration loaded (P2P mode)")
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
