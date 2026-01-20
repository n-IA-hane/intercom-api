"""Sensor platform for Intercom Native integration."""
import logging
from typing import Any

from homeassistant.components.sensor import SensorEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.event import async_track_state_change_event
from homeassistant.const import EVENT_STATE_CHANGED

from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)


async def async_setup_platform(
    hass: HomeAssistant,
    config: dict,
    async_add_entities: AddEntitiesCallback,
    discovery_info: dict | None = None,
) -> None:
    """Set up the sensor platform."""
    sensor = IntercomActiveDevicesSensor(hass)
    async_add_entities([sensor], True)

    # Store reference for updates
    hass.data[DOMAIN]["active_devices_sensor"] = sensor


class IntercomActiveDevicesSensor(SensorEntity):
    """Sensor that tracks active intercom devices."""

    _attr_name = "Intercom Active Devices"
    _attr_unique_id = "intercom_native_active_devices"
    _attr_icon = "mdi:phone-voip"

    def __init__(self, hass: HomeAssistant) -> None:
        """Initialize the sensor."""
        self.hass = hass
        self._attr_native_value = "Home Assistant"
        self._tracked_entities: set[str] = set()
        self._unsubscribe = None

    async def async_added_to_hass(self) -> None:
        """Run when entity is added to hass."""
        # Initial scan for intercom devices
        await self._update_active_devices()

        # Subscribe to all state changes to detect intercom devices going online/offline
        @callback
        def state_change_listener(event):
            """Handle state change events."""
            entity_id = event.data.get("entity_id", "")
            new_state = event.data.get("new_state")
            old_state = event.data.get("old_state")

            # Only care about intercom_state sensors
            if "intercom_state" not in entity_id:
                return

            # Check if availability changed
            old_available = old_state is not None and old_state.state != "unavailable"
            new_available = new_state is not None and new_state.state != "unavailable"

            if old_available != new_available:
                _LOGGER.info("Intercom device availability changed: %s (available=%s)", entity_id, new_available)
                self.hass.async_create_task(self._update_active_devices())

        self._unsubscribe = self.hass.bus.async_listen(EVENT_STATE_CHANGED, state_change_listener)

    async def async_will_remove_from_hass(self) -> None:
        """Run when entity is removed from hass."""
        if self._unsubscribe:
            self._unsubscribe()

    async def _update_active_devices(self) -> None:
        """Update the list of active intercom devices."""
        from homeassistant.helpers import device_registry as dr
        from homeassistant.helpers import entity_registry as er

        entity_registry = er.async_get(self.hass)
        device_registry = dr.async_get(self.hass)

        names: set[str] = set()  # Use set for deduplication

        # Find all intercom_state entities and check if they're available
        for entity in entity_registry.entities.values():
            if "intercom_state" not in entity.entity_id:
                continue

            # Check if entity is available
            state = self.hass.states.get(entity.entity_id)
            if state is None or state.state == "unavailable":
                continue

            # Get device name
            if entity.device_id:
                device = device_registry.async_get(entity.device_id)
                if device and device.name:
                    names.add(device.name.strip())

        # Sort for stable order, prepend Home Assistant
        active_devices = ["Home Assistant"] + sorted(names, key=str.casefold)

        # Update sensor value
        new_value = ",".join(active_devices)
        if new_value != self._attr_native_value:
            self._attr_native_value = new_value
            _LOGGER.info("Active intercom devices updated: %s", new_value)
            self.async_write_ha_state()

    async def async_update(self) -> None:
        """Update the sensor."""
        await self._update_active_devices()
