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
    FLAG_NO_RING,
)
from .tcp_client import IntercomTcpClient

_LOGGER = logging.getLogger(__name__)

# WebSocket command types
WS_TYPE_START = f"{DOMAIN}/start"
WS_TYPE_STOP = f"{DOMAIN}/stop"
WS_TYPE_ANSWER = f"{DOMAIN}/answer"
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

        def on_stop_received() -> None:
            """ESP sent STOP (hangup from ESP side)."""
            _LOGGER.info("Session received STOP from ESP: %s", session.device_id)
            session._active = False
            session._ringing = False
            session.hass.bus.async_fire(
                "intercom_state",
                {
                    "device_id": session.device_id,
                    "state": "idle",
                }
            )

        def on_error_received(code: int) -> None:
            """ESP sent ERROR (decline/busy)."""
            _LOGGER.info("Session received ERROR from ESP (code=%d): %s", code, session.device_id)
            session._active = False
            session._ringing = False
            session.hass.bus.async_fire(
                "intercom_state",
                {
                    "device_id": session.device_id,
                    "state": "idle",
                }
            )

        self._tcp_client = IntercomTcpClient(
            host=self.host,
            port=INTERCOM_PORT,
            on_audio=on_audio,
            on_disconnected=on_disconnected,
            on_ringing=on_ringing,
            on_answered=on_answered,
            on_stop_received=on_stop_received,
            on_error_received=on_error_received,
        )

        if not await self._tcp_client.connect():
            return "error"

        # Fire "calling" state - we're calling, waiting for response
        self.hass.bus.async_fire(
            "intercom_state",
            {
                "device_id": self.device_id,
                "state": "calling",
            }
        )

        # Send HA instance name as caller (for display on ESP)
        caller_name = self.hass.config.location_name or "Home Assistant"
        result = await self._tcp_client.start_stream(caller_name=caller_name)

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

        # Fire "idle" state - call ended
        self.hass.bus.async_fire(
            "intercom_state",
            {
                "device_id": self.device_id,
                "state": "idle",
            }
        )

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

    async def answer(self) -> bool:
        """Answer a ringing call (send ANSWER to ESP).

        Returns:
            True if answer was sent, False otherwise.
        """
        if not self._ringing or not self._tcp_client:
            _LOGGER.warning("answer() called but not ringing or no client")
            return False

        result = await self._tcp_client.send_answer()
        if result:
            # on_answered callback will handle state transition
            _LOGGER.debug("ANSWER sent to ESP")
        return result

    async def answer_esp_call(self) -> str:
        """Answer an ESP-initiated call (ESP called Home Assistant).

        This is for when ESP is in OUTGOING state calling us.
        We connect and send ANSWER (not START).

        Returns:
            "streaming" - Answer sent, streaming started
            "error" - Connection failed
        """
        if self._active:
            return "streaming"

        session = self

        def on_audio(data: bytes) -> None:
            if not session._active:
                return
            session.hass.bus.async_fire(
                "intercom_audio",
                {"device_id": session.device_id, "audio": base64.b64encode(data).decode("ascii")}
            )

        def on_disconnected() -> None:
            session._active = False
            session.hass.bus.async_fire(
                "intercom_state", {"device_id": session.device_id, "state": "disconnected"}
            )

        def on_ringing() -> None:
            pass  # Not expected in this flow

        def on_answered() -> None:
            # ESP confirmed our ANSWER with PONG
            session._active = True
            session._tx_task = asyncio.create_task(session._tx_sender())
            session.hass.bus.async_fire(
                "intercom_state", {"device_id": session.device_id, "state": "streaming"}
            )

        def on_stop_received() -> None:
            _LOGGER.info("Session received STOP from ESP: %s", session.device_id)
            session._active = False
            session.hass.bus.async_fire(
                "intercom_state", {"device_id": session.device_id, "state": "idle"}
            )

        def on_error_received(code: int) -> None:
            _LOGGER.info("Session received ERROR (code=%d): %s", code, session.device_id)
            session._active = False
            session.hass.bus.async_fire(
                "intercom_state", {"device_id": session.device_id, "state": "idle"}
            )

        self._tcp_client = IntercomTcpClient(
            host=self.host,
            port=INTERCOM_PORT,
            on_audio=on_audio,
            on_disconnected=on_disconnected,
            on_ringing=on_ringing,
            on_answered=on_answered,
            on_stop_received=on_stop_received,
            on_error_received=on_error_received,
        )

        if not await self._tcp_client.connect():
            return "error"

        # Send ANSWER directly (not via send_answer which checks _ringing)
        # We're answering an ESP-initiated call - _ringing is not set because
        # this is a fresh TCP connection to an ESP that called us
        from .const import MSG_ANSWER
        result = await self._tcp_client._send_message(MSG_ANSWER)
        if result:
            # Mark that we're awaiting PONG as answer confirmation
            self._tcp_client._awaiting_answer_ack = True

            # Wait briefly for PONG from ESP
            for _ in range(50):  # 500ms max wait
                await asyncio.sleep(0.01)
                if self._active:
                    _LOGGER.debug("answer_esp_call: PONG received, streaming")
                    return "streaming"

            # Timeout - start streaming anyway (ESP might not send PONG)
            _LOGGER.warning("answer_esp_call: no PONG, assuming stream started")
            self._tcp_client._awaiting_answer_ack = False
            self._active = True
            self._tx_task = asyncio.create_task(self._tx_sender())
            return "streaming"
        else:
            await self._tcp_client.disconnect()
            return "error"

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
    """Manages audio bridge between two ESP devices (full mode).

    Uses queues + dedicated sender tasks instead of task-per-packet
    to prevent race conditions during hangup/stop.
    """

    def __init__(
        self,
        hass: HomeAssistant,
        bridge_id: str,
        source_device_id: str,
        source_host: str,
        source_name: str,
        dest_device_id: str,
        dest_host: str,
        dest_name: str,
    ):
        """Initialize bridge session."""
        self.hass = hass
        self.bridge_id = bridge_id
        self.source_device_id = source_device_id
        self.source_host = source_host
        self.source_name = source_name  # Caller name (sent to dest via TCP protocol)
        self.dest_device_id = dest_device_id
        self.dest_host = dest_host
        self.dest_name = dest_name  # Callee name (sent to source via TCP protocol)

        self._source_client: Optional[IntercomTcpClient] = None
        self._dest_client: Optional[IntercomTcpClient] = None
        self._active = False

        # Audio queues for each direction (prevents task-per-packet storm)
        self._q_source_to_dest: asyncio.Queue = asyncio.Queue(maxsize=AUDIO_QUEUE_SIZE)
        self._q_dest_to_source: asyncio.Queue = asyncio.Queue(maxsize=AUDIO_QUEUE_SIZE)

        # Sender tasks
        self._sender_s2d: Optional[asyncio.Task] = None
        self._sender_d2s: Optional[asyncio.Task] = None

        # Stop lock to prevent race conditions
        self._stop_lock = asyncio.Lock()

    def _push_audio(self, queue: asyncio.Queue, data: bytes) -> None:
        """Push audio to queue, dropping if full (non-blocking)."""
        try:
            queue.put_nowait(data)
        except asyncio.QueueFull:
            pass  # Drop silently - low latency > perfect audio

    async def _sender_loop(
        self,
        queue: asyncio.Queue,
        client: IntercomTcpClient,
        direction: str
    ) -> None:
        """Dedicated sender task - pulls from queue and sends to client."""
        _LOGGER.debug("Bridge sender %s started", direction)
        try:
            while self._active:
                try:
                    # Wait for audio with timeout to allow checking _active
                    data = await asyncio.wait_for(queue.get(), timeout=1.0)
                    if self._active and client:
                        await client.send_audio(data)
                except asyncio.TimeoutError:
                    continue  # Check _active and loop
        except asyncio.CancelledError:
            pass
        except Exception as e:
            _LOGGER.error("Bridge sender %s fatal error: %s - stopping bridge", direction, e)
            # Trigger bridge stop on sender failure
            asyncio.create_task(self.stop())
        finally:
            _LOGGER.debug("Bridge sender %s stopped", direction)

    async def start(self) -> str:
        """Start the bridge session.

        Returns:
            "connected" - Both ESPs connected, bridge active
            "ringing" - Dest is ringing, waiting for answer
            "error" - Connection failed
        """
        if self._active:
            return "connected"

        bridge = self

        # Audio callbacks now push to queue instead of creating tasks
        def on_source_audio(data: bytes) -> None:
            if bridge._active:
                bridge._push_audio(bridge._q_source_to_dest, data)

        def on_dest_audio(data: bytes) -> None:
            if bridge._active:
                bridge._push_audio(bridge._q_dest_to_source, data)

        def on_source_disconnected() -> None:
            _LOGGER.debug("Bridge source disconnected: %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge._fire_state_event("disconnected")

        def on_dest_disconnected() -> None:
            _LOGGER.debug("Bridge dest disconnected: %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge._fire_state_event("disconnected")

        def on_source_answered() -> None:
            _LOGGER.debug("Bridge source answered: %s", bridge.bridge_id)

        def on_dest_answered() -> None:
            _LOGGER.debug("Bridge dest answered: %s", bridge.bridge_id)
            # When dest answers, start the sender tasks
            if bridge._active and not bridge._sender_s2d:
                bridge._start_sender_tasks()

        def on_source_stop() -> None:
            _LOGGER.info("Bridge source sent STOP (hangup): %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge._fire_state_event("disconnected")

        def on_dest_stop() -> None:
            _LOGGER.info("Bridge dest sent STOP (hangup): %s", bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge._fire_state_event("disconnected")

        def on_dest_ringing() -> None:
            _LOGGER.info("Bridge dest ringing: %s", bridge.bridge_id)
            bridge._fire_state_event("ringing")

        def on_source_error(code: int) -> None:
            _LOGGER.info("Bridge source sent ERROR (code=%d): %s", code, bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge._fire_state_event("disconnected")

        def on_dest_error(code: int) -> None:
            _LOGGER.info("Bridge dest sent ERROR/decline (code=%d): %s", code, bridge.bridge_id)
            asyncio.create_task(bridge.stop())
            bridge._fire_state_event("disconnected")

        # Create TCP clients for both ESPs
        self._source_client = IntercomTcpClient(
            host=self.source_host,
            port=INTERCOM_PORT,
            on_audio=on_source_audio,
            on_disconnected=on_source_disconnected,
            on_ringing=lambda: None,  # Source doesn't ring
            on_answered=on_source_answered,
            on_stop_received=on_source_stop,
            on_error_received=on_source_error,
        )

        self._dest_client = IntercomTcpClient(
            host=self.dest_host,
            port=INTERCOM_PORT,
            on_audio=on_dest_audio,
            on_disconnected=on_dest_disconnected,
            on_ringing=on_dest_ringing,
            on_answered=on_dest_answered,
            on_stop_received=on_dest_stop,
            on_error_received=on_dest_error,
        )

        # Fire "calling" state - bridge is being set up
        self._fire_state_event("calling")

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
        # Source (caller) uses NO_RING flag - should never ring, always start streaming
        # Both receive the other's name so they know who they're talking to
        source_result = await self._source_client.start_stream(flags=FLAG_NO_RING, caller_name=self.dest_name)
        dest_result = await self._dest_client.start_stream(caller_name=self.source_name)

        if source_result == "error" or dest_result == "error":
            _LOGGER.error("Bridge: failed to start stream")
            await self._source_client.disconnect()
            await self._dest_client.disconnect()
            return "error"

        self._active = True

        # If dest is ringing, don't start senders yet - wait for answer
        if dest_result == "ringing":
            _LOGGER.info("Bridge waiting for dest to answer: %s <-> %s",
                        self.source_host, self.dest_host)
            return "ringing"

        # Both streaming - start sender tasks
        self._start_sender_tasks()

        _LOGGER.info("Bridge started: %s <-> %s", self.source_host, self.dest_host)
        self._fire_state_event("connected")

        return "connected"

    def _start_sender_tasks(self) -> None:
        """Start the audio sender tasks."""
        if self._sender_s2d is None and self._dest_client:
            self._sender_s2d = asyncio.create_task(
                self._sender_loop(self._q_source_to_dest, self._dest_client, "s2d")
            )
        if self._sender_d2s is None and self._source_client:
            self._sender_d2s = asyncio.create_task(
                self._sender_loop(self._q_dest_to_source, self._source_client, "d2s")
            )
        self._fire_state_event("connected")

    def _fire_state_event(self, state: str) -> None:
        """Fire bridge state event."""
        self.hass.bus.async_fire(
            "intercom_bridge_state",
            {
                "bridge_id": self.bridge_id,
                "source_device_id": self.source_device_id,
                "dest_device_id": self.dest_device_id,
                "state": state,
            }
        )

    async def stop(self) -> None:
        """Stop the bridge session."""
        async with self._stop_lock:
            if not self._active:
                return  # Already stopped

            self._active = False

            # Cancel sender tasks first
            for task in [self._sender_s2d, self._sender_d2s]:
                if task:
                    task.cancel()
                    try:
                        await asyncio.wait_for(task, timeout=1.0)
                    except (asyncio.CancelledError, asyncio.TimeoutError):
                        pass
            self._sender_s2d = None
            self._sender_d2s = None

            # Clear queues
            while not self._q_source_to_dest.empty():
                try:
                    self._q_source_to_dest.get_nowait()
                except asyncio.QueueEmpty:
                    break
            while not self._q_dest_to_source.empty():
                try:
                    self._q_dest_to_source.get_nowait()
                except asyncio.QueueEmpty:
                    break

            # Stop TCP clients
            if self._source_client:
                await self._source_client.stop_stream()
                await self._source_client.disconnect()
                self._source_client = None

            if self._dest_client:
                await self._dest_client.stop_stream()
                await self._dest_client.disconnect()
                self._dest_client = None

            # Remove from global _bridges dict to allow new bridges
            _bridges.pop(self.bridge_id, None)

            _LOGGER.info("Bridge stopped and removed: %s", self.bridge_id)
            self._fire_state_event("idle")


def async_register_websocket_api(hass: HomeAssistant) -> None:
    """Register WebSocket API commands."""
    websocket_api.async_register_command(hass, websocket_start)
    websocket_api.async_register_command(hass, websocket_stop)
    websocket_api.async_register_command(hass, websocket_answer)
    websocket_api.async_register_command(hass, websocket_answer_esp_call)
    websocket_api.async_register_command(hass, websocket_audio)
    websocket_api.async_register_command(hass, websocket_list_devices)
    websocket_api.async_register_command(hass, websocket_bridge)
    websocket_api.async_register_command(hass, websocket_bridge_stop)
    websocket_api.async_register_command(hass, websocket_decline)


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
    """Stop intercom session or bridge involving this device."""
    device_id = msg["device_id"]
    msg_id = msg["id"]
    stopped = False

    _LOGGER.debug("websocket_stop called: device_id=%s", device_id)
    _LOGGER.debug("  Active sessions: %s", list(_sessions.keys()))
    _LOGGER.debug("  Active bridges: %s", list(_bridges.keys()))

    # Stop P2P session if exists
    session = _sessions.pop(device_id, None)
    if session:
        await session.stop()
        _LOGGER.debug("Session stopped: %s", device_id)
        stopped = True

    # Also stop any bridges involving this device (for callee hangup)
    bridges_to_stop = []
    for bridge_id, bridge in list(_bridges.items()):
        if bridge.source_device_id == device_id or bridge.dest_device_id == device_id:
            bridges_to_stop.append(bridge_id)

    for bridge_id in bridges_to_stop:
        bridge = _bridges.pop(bridge_id, None)
        if bridge:
            await bridge.stop()
            _LOGGER.debug("Bridge stopped via stop command: %s (device: %s)", bridge_id, device_id)
            stopped = True

    connection.send_result(msg_id, {"success": True, "stopped": stopped})


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_ANSWER,
        vol.Required("device_id"): str,
    }
)
@websocket_api.async_response
async def websocket_answer(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Answer a ringing call (send ANSWER to ESP)."""
    device_id = msg["device_id"]
    msg_id = msg["id"]

    # First check P2P sessions
    session = _sessions.get(device_id)
    if session:
        result = await session.answer()
        if result:
            connection.send_result(msg_id, {"success": True})
        else:
            connection.send_error(msg_id, "error", "Failed to send answer")
        return

    # Check bridges - device_id might be the dest (callee) of a bridge
    for bridge in _bridges.values():
        if bridge.dest_device_id == device_id and bridge._dest_client:
            result = await bridge._dest_client.send_answer()
            if result:
                connection.send_result(msg_id, {"success": True})
            else:
                connection.send_error(msg_id, "error", "Failed to send answer to bridge dest")
            return

    connection.send_error(msg_id, "not_found", f"No session or bridge for {device_id}")


WS_TYPE_ANSWER_ESP_CALL = f"{DOMAIN}/answer_esp_call"


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_ANSWER_ESP_CALL,
        vol.Required("device_id"): str,
        vol.Required("host"): str,
    }
)
@websocket_api.async_response
async def websocket_answer_esp_call(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Answer an ESP-initiated call to Home Assistant.

    Used when ESP called us (ESP in OUTGOING state) and we want to answer.
    Creates session, connects, sends ANSWER (not START).
    """
    device_id = msg["device_id"]
    host = msg["host"]
    msg_id = msg["id"]

    _LOGGER.debug("Answer ESP call: device=%s host=%s", device_id, host)

    try:
        # Stop existing session if any
        if device_id in _sessions:
            old_session = _sessions.pop(device_id)
            await old_session.stop()

        session = IntercomSession(hass=hass, device_id=device_id, host=host)
        result = await session.answer_esp_call()

        if result == "streaming":
            _sessions[device_id] = session
            _LOGGER.info("Answered ESP call (streaming): %s", device_id)
            connection.send_result(msg_id, {"success": True, "state": "streaming"})
        else:
            _LOGGER.error("Failed to answer ESP call: %s", device_id)
            connection.send_error(msg_id, "connection_failed", f"Failed to connect to {host}")
    except Exception as err:
        _LOGGER.exception("Answer ESP call exception: %s", err)
        connection.send_error(msg_id, "exception", str(err))


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

    # Note: Contacts are now managed via sensor.intercom_active_devices
    # ESPs subscribe to this sensor and update their contacts automatically

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
            # Collect entity IDs for this device (for card to use without admin perms)
            entities = {}
            for entity in entity_registry.entities.values():
                if entity.device_id != device_id:
                    continue
                eid = entity.entity_id
                if "intercom_state" in eid and "intercom_state" not in entities:
                    entities["intercom_state"] = eid
                elif ("incoming_caller" in eid or eid.endswith("_caller")) and "incoming_caller" not in entities:
                    entities["incoming_caller"] = eid
                elif "destination" in eid and "destination" not in entities:
                    entities["destination"] = eid
                elif eid.startswith("button.") and "previous" in eid and "previous" not in entities:
                    entities["previous"] = eid
                elif eid.startswith("button.") and "next" in eid and "next" not in entities:
                    entities["next"] = eid
                elif eid.startswith("button.") and "decline" in eid and "decline" not in entities:
                    entities["decline"] = eid
                elif eid.startswith("button.") and "call" in eid and "decline" not in eid and "call" not in entities:
                    entities["call"] = eid

            devices.append({
                "device_id": device_id,
                "name": device.name or esphome_id or device_id,
                "host": ip_address,
                "esphome_id": esphome_id,
                "entities": entities,
            })

    return devices


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
    """Start bridge between two ESP devices (full mode)."""
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

        # Note: incoming_caller is now sent via TCP protocol (START message payload)
        # No need to set it via ESPHome API anymore

        bridge = BridgeSession(
            hass=hass,
            bridge_id=bridge_id,
            source_device_id=source_device_id,
            source_host=source_host,
            source_name=source_name,
            dest_device_id=dest_device_id,
            dest_host=dest_host,
            dest_name=dest_name,
        )

        # Add to _bridges BEFORE start() to prevent auto-bridge race condition
        # Auto-bridge checks _bridges and will skip if already exists
        _bridges[bridge_id] = bridge

        result = await bridge.start()

        # Both "connected" and "ringing" are valid states - keep the bridge
        if result in ("connected", "ringing"):
            connection.send_result(msg_id, {
                "success": True,
                "bridge_id": bridge_id,
                "state": result  # Let card know if ringing
            })
        else:
            # Remove from _bridges if start failed
            _bridges.pop(bridge_id, None)
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

    _LOGGER.debug("bridge_stop called: bridge_id=%s", bridge_id)

    bridge = _bridges.pop(bridge_id, None)
    if bridge:
        await bridge.stop()
        _LOGGER.debug("Bridge stopped via bridge_stop: %s", bridge_id)
    else:
        _LOGGER.debug("bridge_stop: bridge not found: %s", bridge_id)

    connection.send_result(msg_id, {"success": True})


WS_TYPE_DECLINE = f"{DOMAIN}/decline"


@websocket_api.websocket_command(
    {
        vol.Required("type"): WS_TYPE_DECLINE,
        vol.Required("device_id"): str,
    }
)
@websocket_api.async_response
async def websocket_decline(
    hass: HomeAssistant,
    connection: websocket_api.ActiveConnection,
    msg: Dict[str, Any],
) -> None:
    """Decline an incoming call for a device.

    Finds any bridge or session where this device is involved and stops it.
    Works regardless of whether the caller has the bridge_id.
    """
    device_id = msg["device_id"]
    msg_id = msg["id"]
    stopped = False

    _LOGGER.debug("Decline request for device: %s", device_id)

    # Check P2P sessions first
    session = _sessions.pop(device_id, None)
    if session:
        await session.stop()
        _LOGGER.debug("Declined P2P session for device: %s", device_id)
        stopped = True

    # Check bridges - find any where this device is source or dest
    bridges_to_stop = []
    for bridge_id, bridge in list(_bridges.items()):
        if bridge.source_device_id == device_id or bridge.dest_device_id == device_id:
            bridges_to_stop.append(bridge_id)

    for bridge_id in bridges_to_stop:
        bridge = _bridges.pop(bridge_id, None)
        if bridge:
            await bridge.stop()
            _LOGGER.debug("Declined bridge %s for device: %s", bridge_id, device_id)
            stopped = True

    connection.send_result(msg_id, {"success": True, "stopped": stopped})


async def start_auto_bridge(hass: HomeAssistant, intercom_state_entity_id: str) -> None:
    """Auto-start bridge when ESP goes to 'calling' state.

    Called from __init__.py when an ESP's intercom_state changes to 'calling'.
    This enables ESP button-initiated calls without needing the card.
    """
    from homeassistant.helpers import entity_registry as er
    from homeassistant.helpers import device_registry as dr

    _LOGGER.info("Auto-bridge: processing call from %s", intercom_state_entity_id)

    entity_registry = er.async_get(hass)
    device_registry = dr.async_get(hass)

    # Find the device that owns this intercom_state sensor
    source_entity = entity_registry.async_get(intercom_state_entity_id)
    if not source_entity or not source_entity.device_id:
        _LOGGER.warning("Auto-bridge: could not find device for %s", intercom_state_entity_id)
        return

    source_device_id = source_entity.device_id
    source_device = device_registry.async_get(source_device_id)
    if not source_device:
        _LOGGER.warning("Auto-bridge: device not found: %s", source_device_id)
        return

    # Get source device name and IP
    source_name = source_device.name or "Unknown"
    source_host = None
    for conn_type, conn_value in source_device.connections:
        if 'ip' in conn_type.lower() or conn_type == 'network_ip':
            source_host = conn_value
            break
    if not source_host:
        for entry_id in source_device.config_entries:
            entry = hass.config_entries.async_get_entry(entry_id)
            if entry and entry.domain == "esphome":
                source_host = entry.data.get("host")
                break

    if not source_host:
        _LOGGER.warning("Auto-bridge: no IP found for source device %s", source_name)
        return

    # Find destination sensor for this device (sensor.XXX_dest or sensor.XXX_destination)
    dest_entity_id = None
    destination_name = None
    for entity in entity_registry.entities.values():
        if entity.device_id == source_device_id:
            if "destination" in entity.entity_id or "_dest" in entity.entity_id:
                dest_entity_id = entity.entity_id
                break

    if dest_entity_id:
        dest_state = hass.states.get(dest_entity_id)
        if dest_state:
            destination_name = dest_state.state
            _LOGGER.info("Auto-bridge: source=%s destination=%s", source_name, destination_name)

    if not destination_name or destination_name.lower() in ("unknown", "", "none"):
        _LOGGER.warning("Auto-bridge: no valid destination for %s", source_name)
        return

    # Special case: "Home Assistant" destination means browser call, not ESP bridge
    if destination_name.lower() == "home assistant":
        _LOGGER.info("Auto-bridge: destination is 'Home Assistant' - this requires card/browser, skipping auto-bridge")
        return

    # Find destination device by name
    dest_device = None
    dest_device_id = None
    dest_host = None
    dest_name = destination_name

    # Search through all devices for one matching the destination name
    for device in device_registry.devices.values():
        if device.name and device.name.lower() == destination_name.lower():
            dest_device = device
            dest_device_id = device.id
            break

    if not dest_device:
        _LOGGER.warning("Auto-bridge: destination device '%s' not found", destination_name)
        return

    # Get destination IP
    for conn_type, conn_value in dest_device.connections:
        if 'ip' in conn_type.lower() or conn_type == 'network_ip':
            dest_host = conn_value
            break
    if not dest_host:
        for entry_id in dest_device.config_entries:
            entry = hass.config_entries.async_get_entry(entry_id)
            if entry and entry.domain == "esphome":
                dest_host = entry.data.get("host")
                break

    if not dest_host:
        _LOGGER.warning("Auto-bridge: no IP found for destination device %s", dest_name)
        return

    # Check if bridge already exists (being set up or active)
    # Don't create duplicate - websocket_bridge adds to _bridges BEFORE start()
    bridge_id = f"{source_device_id}_{dest_device_id}"
    if bridge_id in _bridges:
        _LOGGER.debug("Auto-bridge: bridge already exists, skipping: %s", bridge_id)
        return

    _LOGGER.info(
        "Auto-bridge: starting bridge %s (%s) <-> %s (%s)",
        source_name, source_host, dest_name, dest_host
    )

    # Stop any existing sessions for these devices
    for device_id in [source_device_id, dest_device_id]:
        if device_id in _sessions:
            old_session = _sessions.pop(device_id)
            await old_session.stop()

    # Stop any existing bridges involving these devices
    bridges_to_stop = []
    for bid, bridge in list(_bridges.items()):
        if bridge.source_device_id in [source_device_id, dest_device_id] or \
           bridge.dest_device_id in [source_device_id, dest_device_id]:
            bridges_to_stop.append(bid)

    for bid in bridges_to_stop:
        old_bridge = _bridges.pop(bid, None)
        if old_bridge:
            _LOGGER.debug("Auto-bridge: stopping old bridge: %s", bid)
            await old_bridge.stop()

    # Create and start the bridge
    bridge = BridgeSession(
        hass=hass,
        bridge_id=bridge_id,
        source_device_id=source_device_id,
        source_host=source_host,
        source_name=source_name,
        dest_device_id=dest_device_id,
        dest_host=dest_host,
        dest_name=dest_name,
    )

    # Add to _bridges BEFORE start() to prevent race conditions
    _bridges[bridge_id] = bridge

    result = await bridge.start()

    if result in ("connected", "ringing"):
        _LOGGER.info("Auto-bridge: started successfully (state=%s)", result)
    else:
        # Remove from _bridges if start failed
        _bridges.pop(bridge_id, None)
        _LOGGER.error("Auto-bridge: failed to start bridge")
