"""Intercom Native integration for Home Assistant.

This integration provides:
1. TCP-based audio streaming between browser and ESP32 (port 6054)
2. Broker service for ESP↔ESP communication (port 6060)

Unlike WebRTC/go2rtc approaches, this uses simple TCP protocols
which are more reliable across NAT/firewall scenarios.
"""

import logging

from homeassistant.core import HomeAssistant
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform

from .const import DOMAIN, BROKER_PORT
from .websocket_api import async_register_websocket_api
from .broker import async_setup_broker, async_stop_broker, get_broker

_LOGGER = logging.getLogger(__name__)

PLATFORMS: list[Platform] = []

# Config keys
CONF_BROKER_ENABLED = "broker_enabled"
CONF_BROKER_PORT = "broker_port"


async def async_setup(hass: HomeAssistant, config: dict) -> bool:
    """Set up Intercom Native from configuration.yaml."""
    hass.data.setdefault(DOMAIN, {})

    # Register WebSocket API commands
    async_register_websocket_api(hass)

    # Start broker by default (can be disabled via config entry)
    if await async_setup_broker(hass, BROKER_PORT):
        _LOGGER.info("Intercom broker started on port %d", BROKER_PORT)
    else:
        _LOGGER.warning("Failed to start intercom broker")

    _LOGGER.info("Intercom Native integration loaded")
    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up Intercom Native from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    # Register WebSocket API if not already done
    async_register_websocket_api(hass)

    # Check if broker should be enabled
    broker_enabled = entry.data.get(CONF_BROKER_ENABLED, True)
    broker_port = entry.data.get(CONF_BROKER_PORT, BROKER_PORT)

    if broker_enabled and not get_broker():
        if await async_setup_broker(hass, broker_port):
            _LOGGER.info("Intercom broker started on port %d", broker_port)
        else:
            _LOGGER.warning("Failed to start intercom broker")

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    # Stop broker if it was started by this entry
    await async_stop_broker()
    return True
