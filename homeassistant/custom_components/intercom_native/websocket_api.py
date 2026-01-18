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
WS_TYPE_BRIDGE = f"{DOMAIN}/bridge"
WS_TYPE_BRIDGE_STOP = f"{DOMAIN}/bridge_stop"

# Active sessions: device_id -> IntercomSession
_sessions: Dict[str, "IntercomSession"] = {}

# Active bridges: bridge_id -> BridgeSession
_bridges: Dict[str, "BridgeSession"] = {}


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


class BridgeSession:
    """Manages audio bridge between two ESP devices (PTMP mode)."""

    def __init__(
        self,
        hass: HomeAssistant,
        bridge_id: str,
        source_device_id: str,
        source_host: str,
        dest_device_id: str,
        dest_host: str,
    ):
        """Initialize bridge session."""
        self.hass = hass
        self.bridge_id = bridge_id
        self.source_device_id = source_device_id
        self.source_host = source_host
        self.dest_device_id = dest_device_id
        self.dest_host = dest_host

        self._source_client: Optional[IntercomTcpClient] = None
        self._dest_client: Optional[IntercomTcpClient] = None
        self._active = False

    async def start(self) -> str:
        """Start the bridge session.

        Returns:
            "connected" - Both ESPs connected, bridge active
            "error" - Connection failed
        """
        if self._active:
            return "connected"

        bridge = self

        # Audio from source -> dest
        def on_source_audio(data: bytes) -> None:
            if not bridge._active or not bridge._dest_client:
                return
            asyncio.create_task(bridge._dest_client.send_audio(data))

        # Audio from dest -> source
        def on_dest_audio(data: bytes) -> None:
            if not bridge._active or not bridge._source_client:
                return
            asyncio.create_task(bridge._source_client.send_audio(data))

        def on_source_disconnected() -> None:
            _LOGGER.debug("Bridge source disconnected: %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge.hass.bus.async_fire(
                "intercom_bridge_state",
                {
                    "bridge_id": bridge.bridge_id,
                    "source_device_id": bridge.source_device_id,
                    "dest_device_id": bridge.dest_device_id,
                    "state": "disconnected",
                }
            )

        def on_dest_disconnected() -> None:
            _LOGGER.debug("Bridge dest disconnected: %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge.hass.bus.async_fire(
                "intercom_bridge_state",
                {
                    "bridge_id": bridge.bridge_id,
                    "source_device_id": bridge.source_device_id,
                    "dest_device_id": bridge.dest_device_id,
                    "state": "disconnected",
                }
            )

        def on_source_answered() -> None:
            _LOGGER.debug("Bridge source answered: %s", bridge.bridge_id)
            bridge._check_both_connected()

        def on_dest_answered() -> None:
            _LOGGER.debug("Bridge dest answered: %s", bridge.bridge_id)
            bridge._check_both_connected()

        def on_source_stop() -> None:
            _LOGGER.info("Bridge source sent STOP (hangup): %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge.hass.bus.async_fire(
                "intercom_bridge_state",
                {
                    "bridge_id": bridge.bridge_id,
                    "source_device_id": bridge.source_device_id,
                    "dest_device_id": bridge.dest_device_id,
                    "state": "disconnected",
                }
            )

        def on_dest_stop() -> None:
            _LOGGER.info("Bridge dest sent STOP (hangup): %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge.hass.bus.async_fire(
                "intercom_bridge_state",
                {
                    "bridge_id": bridge.bridge_id,
                    "source_device_id": bridge.source_device_id,
                    "dest_device_id": bridge.dest_device_id,
                    "state": "disconnected",
                }
            )

        # Create TCP clients for both ESPs
        self._source_client = IntercomTcpClient(
            host=self.source_host,
            port=INTERCOM_PORT,
            on_audio=on_source_audio,
            on_disconnected=on_source_disconnected,
            on_ringing=lambda: None,
            on_answered=on_source_answered,
            on_stop_received=on_source_stop,
        )

        self._dest_client = IntercomTcpClient(
            host=self.dest_host,
            port=INTERCOM_PORT,
            on_audio=on_dest_audio,
            on_disconnected=on_dest_disconnected,
            on_ringing=lambda: None,
            on_answered=on_dest_answered,
            on_stop_received=on_dest_stop,
        )

        # Connect to both ESPs
        source_connected = await self._source_client.connect()
        if not source_connected:
            _LOGGER.error("Bridge: failed to connect to source %s", self.source_host)
            return "error"

        dest_connected = await self._dest_client.connect()
        if not dest_connected:
            _LOGGER.error("Bridge: failed to connect to dest %s", self.dest_host)
            await self._source_client.disconnect()
            return "error"

        # Start streaming on both
        source_result = await self._source_client.start_stream()
        dest_result = await self._dest_client.start_stream()

        if source_result == "error" or dest_result == "error":
            _LOGGER.error("Bridge: failed to start stream")
            await self._source_client.disconnect()
            await self._dest_client.disconnect()
            return "error"

        self._active = True
        _LOGGER.info("Bridge started: %s <-> %s", self.source_host, self.dest_host)

        self.hass.bus.async_fire(
            "intercom_bridge_state",
            {
                "bridge_id": self.bridge_id,
                "source_device_id": self.source_device_id,
                "dest_device_id": self.dest_device_id,
                "state": "connected",
            }
        )

        return "connected"

    def _check_both_connected(self) -> None:
        """Check if both ESPs are streaming and fire connected event."""
        # This is called when either side answers
        # For now we assume connected after start_stream succeeds
        pass

    async def stop(self) -> None:
        """Stop the bridge session."""
        self._active = False

        if self._source_client:
            await self._source_client.stop_stream()
            await self._source_client.disconnect()
            self._source_client = None

        if self._dest_client:
            await self._dest_client.stop_stream()
            await self._dest_client.disconnect()
            self._dest_client = None

        _LOGGER.debug("Bridge stopped: %s", self.bridge_id)


def async_register_websocket_api(hass: HomeAssistant) -> None:
    """Register WebSocket API commands."""
    websocket_api.async_register_command(hass, websocket_start)
    websocket_api.async_register_command(hass, websocket_stop)
    websocket_api.async_register_command(hass, websocket_audio)
    websocket_api.async_register_command(hass, websocket_list_devices)
    websocket_api.async_register_command(hass, websocket_bridge)
    websocket_api.async_register_command(hass, websocket_bridge_stop)


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
@websocket_api.async_response
async def websocket_list_devices(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """List ESPHome devices with intercom capability."""
    devices = await _get_intercom_devices(hass)

    # Automatically sync contacts to all ESP devices
    await _sync_contacts_to_all_devices(hass, devices)

    connection.send_result(msg["id"], {"devices": devices})


async def _get_intercom_devices(hass: HomeAssistant) -> list:
    """Get all intercom devices with their info."""
    from homeassistant.helpers import entity_registry as er
    from homeassistant.helpers import device_registry as dr

    devices = []
    entity_registry = er.async_get(hass)
    device_registry = dr.async_get(hass)

    # Find devices that have intercom_state sensor (indicates intercom_api component)
    intercom_device_ids = set()
    for entity in entity_registry.entities.values():
        if "intercom_state" in entity.entity_id:
            intercom_device_ids.add(entity.device_id)

    # Get device info and IP for each intercom device
    for device_id in intercom_device_ids:
        device = device_registry.async_get(device_id)
        if not device:
            continue

        # Get IP from connections (ESPHome devices have IP in connections)
        ip_address = None
        for conn_type, conn_value in device.connections:
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
                if entry and entry.domain == "esphome":
                    ip_address = entry.data.get("host")
                    break

        # Only add devices with valid IP
        if ip_address:
            devices.append({
                "device_id": device_id,
                "name": device.name or esphome_id or device_id,
                "host": ip_address,
                "esphome_id": esphome_id,
            })

    return devices


async def _sync_contacts_to_all_devices(hass: HomeAssistant, devices: list) -> None:
    """Sync contacts list to all intercom ESP devices."""
    from homeassistant.helpers import entity_registry as er

    entity_registry = er.async_get(hass)

    for device in devices:
        device_id = device["device_id"]
        device_name = device["name"]

        # Build contacts list for this device: "Home Assistant" + all other devices
        contacts = ["Home Assistant"]
        for other_device in devices:
            if other_device["device_id"] != device_id:
                contacts.append(other_device["name"])

        contacts_str = ",".join(contacts)

        # Find contacts_data text entity for this device
        for entity in entity_registry.entities.values():
            if entity.device_id == device_id and "contacts" in entity.entity_id.lower():
                try:
                    # Check current value to avoid unnecessary updates
                    current_state = hass.states.get(entity.entity_id)
                    if current_state and current_state.state == contacts_str:
                        _LOGGER.debug("Contacts already synced for %s", device_name)
                        continue

                    await hass.services.async_call(
                        "text",
                        "set_value",
                        {"entity_id": entity.entity_id, "value": contacts_str},
                        blocking=True,
                    )
                    _LOGGER.info("Synced contacts to %s: %s", device_name, contacts_str)
                except Exception as err:
                    _LOGGER.warning("Failed to sync contacts to %s: %s", device_name, err)
                break


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_BRIDGE,
        vol.Required("source_device_id"): str,
        vol.Required("source_host"): str,
        vol.Optional("source_name"): str,
        vol.Required("dest_device_id"): str,
        vol.Required("dest_host"): str,
        vol.Optional("dest_name"): str,
    }
)
@websocket_api.async_response
async def websocket_bridge(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Start bridge between two ESP devices (PTMP mode)."""
    source_device_id = msg["source_device_id"]
    source_host = msg["source_host"]
    source_name = msg.get("source_name", "Intercom")
    dest_device_id = msg["dest_device_id"]
    dest_host = msg["dest_host"]
    dest_name = msg.get("dest_name", "Intercom")
    msg_id = msg["id"]

    # Create unique bridge ID
    bridge_id = f"{source_device_id}_{dest_device_id}"

    _LOGGER.info(
        "Bridge request: %s (%s) <-> %s (%s)",
        source_name, source_host,
        dest_name, dest_host
    )

    try:
        # Stop existing bridge if any
        if bridge_id in _bridges:
            old_bridge = _bridges.pop(bridge_id)
            await old_bridge.stop()

        # Also stop any P2P sessions for these devices
        for device_id in [source_device_id, dest_device_id]:
            if device_id in _sessions:
                old_session = _sessions.pop(device_id)
                await old_session.stop()

        # Set incoming_caller only on destination (callee) - source is the caller
        await _set_incoming_caller(hass, dest_device_id, source_name)

        bridge = BridgeSession(
            hass=hass,
            bridge_id=bridge_id,
            source_device_id=source_device_id,
            source_host=source_host,
            dest_device_id=dest_device_id,
            dest_host=dest_host,
        )

        result = await bridge.start()

        if result == "connected":
            _bridges[bridge_id] = bridge
            connection.send_result(msg_id, {"success": True, "bridge_id": bridge_id})
        else:
            connection.send_error(msg_id, "bridge_failed", "Failed to establish bridge")

    except Exception as err:
        _LOGGER.exception("Bridge exception: %s", err)
        connection.send_error(msg_id, "exception", str(err))


async def _set_incoming_caller(hass: HomeAssistant, device_id: str, caller_name: str) -> None:
    """Set the incoming_caller text entity on an ESP device."""
    from homeassistant.helpers import entity_registry as er

    entity_registry = er.async_get(hass)

    # Find the incoming_caller text entity for this device
    for entity in entity_registry.entities.values():
        if entity.device_id == device_id and "incoming_caller" in entity.entity_id:
            try:
                await hass.services.async_call(
                    "text",
                    "set_value",
                    {"entity_id": entity.entity_id, "value": caller_name},
                    blocking=True,  # Wait for ESP to receive the value
                )
                _LOGGER.info("Set incoming_caller on %s to %s", entity.entity_id, caller_name)
            except Exception as err:
                _LOGGER.warning("Failed to set incoming_caller: %s", err)
            break


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_BRIDGE_STOP,
        vol.Required("bridge_id"): str,
    }
)
@websocket_api.async_response
async def websocket_bridge_stop(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Stop a bridge session."""
    bridge_id = msg["bridge_id"]
    msg_id = msg["id"]

    bridge = _bridges.pop(bridge_id, None)
    if bridge:
        await bridge.stop()
        _LOGGER.debug("Bridge stopped: %s", bridge_id)

    connection.send_result(msg_id, {"success": True})
