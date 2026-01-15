"""WebSocket API for Intercom Native integration."""

import asyncio
import base64
import logging
from typing import Any, Dict, Optional

# Audio queue config
AUDIO_QUEUE_SIZE = 8  # Max pending audio chunks - drop old if full

import voluptuous as vol

from homeassistant.components import websocket_api
from homeassistant.core import HomeAssistant, callback

from .const import (
    DOMAIN,
    INTERCOM_PORT,
)
from .tcp_client import IntercomTcpClient

_LOGGER = logging.getLogger(__name__)

# WebSocket command types
WS_TYPE_START = f"{DOMAIN}/start"
WS_TYPE_STOP = f"{DOMAIN}/stop"
WS_TYPE_AUDIO = f"{DOMAIN}/audio"
WS_TYPE_LIST = f"{DOMAIN}/list_devices"

# Active sessions: device_id -> IntercomSession
_sessions: Dict[str, "IntercomSession"] = {}


class IntercomSession:
    """Manages a single intercom session between browser and ESP."""

    def __init__(
        self,
        hass: HomeAssistant,
        device_id: str,
        host: str,
    ):
        """Initialize session."""
        self.hass = hass
        self.device_id = device_id
        self.host = host

        self._tcp_client: Optional[IntercomTcpClient] = None
        self._active = False
        self._tx_queue: asyncio.Queue = asyncio.Queue(maxsize=AUDIO_QUEUE_SIZE)
        self._tx_task: Optional[asyncio.Task] = None

    async def start(self) -> bool:
        """Start the intercom session."""
        if self._active:
            return True

        session = self

        def on_audio(data: bytes) -> None:
            """Handle audio from ESP - fire event to browser."""
            if not session._active:
                return
            session.hass.bus.async_fire(
                "intercom_audio",
                {
                    "device_id": session.device_id,
                    "audio": base64.b64encode(data).decode("ascii"),
                }
            )

        def on_disconnected() -> None:
            session._active = False

        self._tcp_client = IntercomTcpClient(
            host=self.host,
            port=INTERCOM_PORT,
            on_audio=on_audio,
            on_disconnected=on_disconnected,
        )

        if await self._tcp_client.connect():
            if await self._tcp_client.start_stream():
                self._active = True
                self._tx_task = asyncio.create_task(self._tx_sender())
                return True
            await self._tcp_client.disconnect()

        return False

    async def stop(self) -> None:
        """Stop the intercom session."""
        self._active = False

        if self._tx_task:
            self._tx_task.cancel()
            try:
                await asyncio.wait_for(self._tx_task, timeout=1.0)
            except (asyncio.CancelledError, asyncio.TimeoutError):
                pass
            self._tx_task = None

        if self._tcp_client:
            await self._tcp_client.stop_stream()
            await self._tcp_client.disconnect()
            self._tcp_client = None

    async def _tx_sender(self) -> None:
        """Single task that sends audio from queue to TCP."""
        try:
            while self._active and self._tcp_client:
                data = await self._tx_queue.get()
                await self._tcp_client.send_audio(data)
        except asyncio.CancelledError:
            pass

    def queue_audio(self, data: bytes) -> None:
        """Queue audio for sending - drops if full (non-blocking)."""
        if not self._active:
            return

        try:
            self._tx_queue.put_nowait(data)
        except asyncio.QueueFull:
            pass  # Drop silently - low latency > perfect audio


def async_register_websocket_api(hass: HomeAssistant) -> None:
    """Register WebSocket API commands."""
    websocket_api.async_register_command(hass, websocket_start)
    websocket_api.async_register_command(hass, websocket_stop)
    websocket_api.async_register_command(hass, websocket_audio)
    websocket_api.async_register_command(hass, websocket_list_devices)


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_START,
        vol.Required("device_id"): str,
        vol.Required("host"): str,
    }
)
@websocket_api.async_response
async def websocket_start(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Start intercom session."""
    device_id = msg["device_id"]
    host = msg["host"]
    msg_id = msg["id"]

    _LOGGER.info("Start request: device=%s host=%s", device_id, host)

    try:
        # Stop existing session if any
        if device_id in _sessions:
            old_session = _sessions.pop(device_id)
            await old_session.stop()

        session = IntercomSession(hass=hass, device_id=device_id, host=host)
        if await session.start():
            _sessions[device_id] = session
            _LOGGER.info("Session started: %s", device_id)
            connection.send_result(msg_id, {"success": True})
        else:
            _LOGGER.error("Session failed: %s", device_id)
            connection.send_error(msg_id, "connection_failed", f"Failed to connect to {host}")
    except Exception as err:
        _LOGGER.exception("Start exception: %s", err)
        connection.send_error(msg_id, "exception", str(err))


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_STOP,
        vol.Required("device_id"): str,
    }
)
@websocket_api.async_response
async def websocket_stop(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Stop intercom session."""
    device_id = msg["device_id"]
    msg_id = msg["id"]

    session = _sessions.pop(device_id, None)
    if session:
        await session.stop()
        _LOGGER.info("Session stopped: %s", device_id)

    connection.send_result(msg_id, {"success": True})


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_AUDIO,
        vol.Required("device_id"): str,
        vol.Required("audio"): str,  # base64 encoded audio
    }
)
@callback
def websocket_audio(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Handle audio from browser (JSON with base64) - non-blocking."""
    device_id = msg["device_id"]
    audio_b64 = msg["audio"]

    session = _sessions.get(device_id)
    if not session or not session._active:
        return

    try:
        session.queue_audio(base64.b64decode(audio_b64))
    except Exception:
        pass


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_LIST,
    }
)
@callback
def websocket_list_devices(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """List devices with intercom capability."""
    from homeassistant.helpers import entity_registry as er
    from homeassistant.helpers import device_registry as dr

    devices = []
    entity_registry = er.async_get(hass)
    device_registry = dr.async_get(hass)

    for entity in entity_registry.entities.values():
        if entity.domain == "switch" and entity.entity_id.endswith("_intercom"):
            device = device_registry.async_get(entity.device_id)
            if device:
                devices.append(
                    {
                        "device_id": entity.device_id,
                        "name": device.name,
                        "entity_id": entity.entity_id,
                    }
                )

    connection.send_result(msg["id"], {"devices": devices})
