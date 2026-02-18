"""Frontend registration for Intercom Native — auto-serves the Lovelace card."""

import logging
from pathlib import Path

from homeassistant.components.http import StaticPathConfig
from homeassistant.core import HomeAssistant
from homeassistant.helpers.event import async_call_later

from ..const import DOMAIN, URL_BASE, INTEGRATION_VERSION

_LOGGER = logging.getLogger(__name__)

CARD_JS = "intercom-card.js"
FRONTEND_DIR = Path(__file__).parent


class JSModuleRegistration:
    """Registers the Lovelace card JS module in Home Assistant."""

    def __init__(self, hass: HomeAssistant) -> None:
        self.hass = hass
        self.lovelace = hass.data.get("lovelace")

    async def async_register(self) -> None:
        """Register static path and Lovelace resource."""
        await self._async_register_path()
        if self.lovelace and hasattr(self.lovelace, "mode") and self.lovelace.mode == "storage":
            await self._async_wait_for_lovelace_resources()

    async def _async_register_path(self) -> None:
        """Register the static HTTP path for frontend files."""
        try:
            await self.hass.http.async_register_static_paths(
                [StaticPathConfig(URL_BASE, str(FRONTEND_DIR), True)]
            )
            _LOGGER.debug("Registered static path: %s -> %s", URL_BASE, FRONTEND_DIR)
        except RuntimeError:
            _LOGGER.debug("Static path already registered: %s", URL_BASE)

    async def _async_wait_for_lovelace_resources(self) -> None:
        """Wait for Lovelace resources to load, then register the card."""
        async def _check_loaded(_now=None) -> None:
            resources = self.lovelace.resources
            if resources.loaded:
                await self._async_register_card(resources)
            else:
                _LOGGER.debug("Lovelace resources not loaded yet, retrying in 5s")
                async_call_later(self.hass, 5, _check_loaded)
        await _check_loaded()

    async def _async_register_card(self, resources) -> None:
        """Create or update the Lovelace resource entry for the card."""
        url = f"{URL_BASE}/{CARD_JS}"
        url_versioned = f"{url}?v={INTEGRATION_VERSION}"

        for item in resources.async_items():
            item_url = item.get("url", "")
            if item_url.split("?")[0] == url:
                # Already registered — update version if needed
                if not item_url.endswith(INTEGRATION_VERSION):
                    _LOGGER.info("Updating Intercom card to v%s", INTEGRATION_VERSION)
                    await resources.async_update_item(
                        item["id"], {"res_type": "module", "url": url_versioned}
                    )
                return

        # Not registered yet — create
        _LOGGER.info("Registering Intercom card v%s", INTEGRATION_VERSION)
        await resources.async_create_item({"res_type": "module", "url": url_versioned})
