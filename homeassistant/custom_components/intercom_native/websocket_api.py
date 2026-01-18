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
WS_TYPE_LIST_TARGETS = f"{DOMAIN}/list_targets"

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
        self._ringing = False  # ESP is ringing, waiting for local answer
        self._tx_queue: asyncio.Queue = asyncio.Queue(maxsize=AUDIO_QUEUE_SIZE)
        self._tx_task: Optional[asyncio.Task] = None

    async def start(self) -> str:
        """Start the intercom session.

        Returns:
            "streaming" - Call accepted, streaming started
            "ringing" - ESP has auto_answer OFF, waiting for local answer
            "error" - Connection failed
        """
        if self._active:
            return "streaming"

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
            session._ringing = False
            # Notify browser of disconnection
            session.hass.bus.async_fire(
                "intercom_state",
                {
                    "device_id": session.device_id,
                    "state": "disconnected",
                }
            )

        def on_ringing() -> None:
            """ESP is ringing, waiting for local answer."""
            session._ringing = True
            session.hass.bus.async_fire(
                "intercom_state",
                {
                    "device_id": session.device_id,
                    "state": "ringing",
                }
            )

        def on_answered() -> None:
            """ESP answered the call, streaming started."""
            session._ringing = False
            session._active = True
            session._tx_task = asyncio.create_task(session._tx_sender())
            session.hass.bus.async_fire(
                "intercom_state",
                {
                    "device_id": session.device_id,
                    "state": "streaming",
                }
            )

        self._tcp_client = IntercomTcpClient(
            host=self.host,
            port=INTERCOM_PORT,
            on_audio=on_audio,
            on_disconnected=on_disconnected,
            on_ringing=on_ringing,
            on_answered=on_answered,
        )

        if not await self._tcp_client.connect():
            return "error"

        result = await self._tcp_client.start_stream()

        if result == "streaming":
            self._active = True
            self._tx_task = asyncio.create_task(self._tx_sender())
            return "streaming"
        elif result == "ringing":
            # Keep TCP connected, but don't start TX yet
            # TX will start when on_answered is called
            self._ringing = True
            return "ringing"
        else:
            await self._tcp_client.disconnect()
            return "error"

    async def stop(self) -> None:
        """Stop the intercom session."""
        self._active = False
        self._ringing = False

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
    websocket_api.async_register_command(hass, websocket_list_targets)


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

    _LOGGER.debug("Start request: device=%s host=%s", device_id, host)

    try:
        # Stop existing session if any
        if device_id in _sessions:
            old_session = _sessions.pop(device_id)
            await old_session.stop()

        session = IntercomSession(hass=hass, device_id=device_id, host=host)
        result = await session.start()

        if result == "streaming":
            _sessions[device_id] = session
            _LOGGER.debug("Session started (streaming): %s", device_id)
            connection.send_result(msg_id, {"success": True, "state": "streaming"})
        elif result == "ringing":
            _sessions[device_id] = session
            _LOGGER.debug("Session started (ringing): %s", device_id)
            connection.send_result(msg_id, {"success": True, "state": "ringing"})
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
        _LOGGER.debug("Session stopped: %s", device_id)

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
    """List ESPHome devices with intercom capability."""
    from homeassistant.helpers import entity_registry as er
    from homeassistant.helpers import device_registry as dr

    devices = []
    entity_registry = er.async_get(hass)
    device_registry = dr.async_get(hass)

    # Find devices that have intercom_state sensor (indicates intercom_api component)
    # Note: ESPHome text_sensor maps to HA sensor domain
    intercom_device_ids = set()
    for entity in entity_registry.entities.values():
        if "intercom_state" in entity.entity_id:
            intercom_device_ids.add(entity.device_id)
            _LOGGER.debug("Found intercom entity: %s -> device %s", entity.entity_id, entity.device_id)

    # Get device info and IP for each intercom device
    for device_id in intercom_device_ids:
        device = device_registry.async_get(device_id)
        if not device:
            _LOGGER.debug("Device not found in registry: %s", device_id)
            continue

        _LOGGER.debug("Device %s: connections=%s, identifiers=%s, config_entries=%s",
                     device_id, device.connections, device.identifiers, device.config_entries)

        # Get IP from connections (ESPHome devices have IP in connections)
        ip_address = None
        for conn_type, conn_value in device.connections:
            _LOGGER.debug("  Connection: type=%s value=%s", conn_type, conn_value)
            # ESPHome stores IP as ('network_ip', 'x.x.x.x') or similar
            if 'ip' in conn_type.lower() or conn_type == 'network_ip':
                ip_address = conn_value
                break

        # Also check identifiers for esphome
        esphome_id = None
        for domain, identifier in device.identifiers:
            if domain == "esphome":
                esphome_id = identifier
                break

        # If no IP in connections, try to get from config entries
        if not ip_address and device.config_entries:
            for entry_id in device.config_entries:
                entry = hass.config_entries.async_get_entry(entry_id)
                _LOGGER.debug("  Config entry %s: domain=%s data=%s",
                             entry_id, entry.domain if entry else None, entry.data if entry else None)
                if entry and entry.domain == "esphome":
                    # ESPHome stores host in entry data
                    ip_address = entry.data.get("host")
                    break

        _LOGGER.debug("Device %s: esphome_id=%s, ip_address=%s", device_id, esphome_id, ip_address)

        # Always add the device, even without IP (for debugging)
        devices.append({
            "device_id": device_id,
            "name": device.name or esphome_id or device_id,
            "host": ip_address,
            "esphome_id": esphome_id,
        })

    # Also add devices from broker if available
    from .broker import get_broker
    broker = get_broker()
    if broker:
        broker_devices = broker.get_connected_devices()
        for dev_id in broker_devices:
            # Normalize names for comparison (handle "Intercom Mini" vs "intercom-mini")
            def normalize(s):
                return (s or "").lower().replace(" ", "-").replace("_", "-")

            dev_id_norm = normalize(dev_id)
            already_exists = any(
                normalize(d.get("esphome_id")) == dev_id_norm or
                normalize(d.get("name")) == dev_id_norm
                for d in devices
            )

            if not already_exists:
                # Broker device - we don't have direct IP, but can be used for ESP↔ESP
                devices.append({
                    "device_id": dev_id,
                    "name": dev_id,
                    "host": None,  # No direct host - uses broker
                    "broker": True,
                })

    connection.send_result(msg["id"], {"devices": devices})


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_LIST_TARGETS,
        vol.Required("device_id"): str,
    }
)
@callback
def websocket_list_targets(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """List available call targets for a device.

    Returns:
    - Home Assistant (browser↔ESP communication)
    - Other ESPs connected to broker (for ESP↔ESP calls)
    """
    device_id = msg["device_id"]
    targets = []

    # Always include Home Assistant as a target (browser↔ESP)
    targets.append({
        "id": "home_assistant",
        "name": "Home Assistant",
        "type": "browser",
    })

    # Add other ESPs from broker (excluding self)
    from .broker import get_broker
    broker = get_broker()
    if broker:
        for other_id in broker.get_connected_devices():
            if other_id != device_id:
                targets.append({
                    "id": other_id,
                    "name": other_id,
                    "type": "esp",
                    "busy": broker.is_device_in_call(other_id),
                })

    connection.send_result(msg["id"], {"targets": targets})
