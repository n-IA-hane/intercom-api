"""Frontend registration for Intercom Native — auto-serves the Lovelace card."""

import logging
from pathlib import Path

from homeassistant.components.http import StaticPathConfig
from homeassistant.core import HomeAssistant

from ..const import URL_BASE, INTEGRATION_VERSION

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

        if self.lovelace is None:
            _LOGGER.warning("Lovelace data not available, skipping resource registration")
            return

        # HA 2026.2+ renamed .mode → .resource_mode
        mode = getattr(self.lovelace, "resource_mode",
                       getattr(self.lovelace, "mode", None))

        if mode == "storage":
            await self._async_register_resource()
        else:
            _LOGGER.debug("Lovelace mode is %s, skipping auto-registration", mode)

    async def _async_register_path(self) -> None:
        """Register the static HTTP path for frontend files."""
        try:
            await self.hass.http.async_register_static_paths(
                [StaticPathConfig(URL_BASE, str(FRONTEND_DIR), True)]
            )
            _LOGGER.debug("Registered static path: %s -> %s", URL_BASE, FRONTEND_DIR)
        except RuntimeError:
            _LOGGER.debug("Static path already registered: %s", URL_BASE)

    async def _async_register_resource(self) -> None:
        """Register or update the Lovelace resource for the card."""
        resources = self.lovelace.resources

        # Force load from storage if not loaded yet
        await resources.async_get_info()

        url = f"{URL_BASE}/{CARD_JS}"
        url_versioned = f"{url}?v={INTEGRATION_VERSION}"

        for item in resources.async_items():
            item_url = item.get("url", "")
            if item_url.split("?")[0] == url:
                # Already registered — update version if needed
                if item_url != url_versioned:
                    _LOGGER.info("Updating Intercom card to v%s", INTEGRATION_VERSION)
                    await resources.async_update_item(
                        item["id"], {"res_type": "module", "url": url_versioned}
                    )
                else:
                    _LOGGER.debug("Intercom card v%s already registered", INTEGRATION_VERSION)
                return

        # Not registered yet — create
        _LOGGER.info("Registering Intercom card v%s", INTEGRATION_VERSION)
        await resources.async_create_item({"res_type": "module", "url": url_versioned})
