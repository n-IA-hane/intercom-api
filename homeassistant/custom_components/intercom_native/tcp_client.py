"""Async TCP client for Intercom Native protocol."""

MAX_PAYLOAD_SIZE = 2048
DRAIN_INTERVAL = 10  # Drain every N packets instead of every packet

import asyncio
import logging
import struct
from typing import Callable, Optional

from .const import (
    INTERCOM_PORT,
    HEADER_SIZE,
    MSG_AUDIO,
    MSG_START,
    MSG_STOP,
    MSG_PING,
    MSG_PONG,
    MSG_ERROR,
    MSG_RING,
    MSG_ANSWER,
    FLAG_NONE,
    CONNECT_TIMEOUT,
    PING_INTERVAL,
)

_LOGGER = logging.getLogger(__name__)


class IntercomTcpClient:
    """Async TCP client for ESP intercom communication."""

    _instance_counter = 0

    def __init__(
        self,
        host: str,
        port: int = INTERCOM_PORT,
        on_audio: Optional[Callable[[bytes], None]] = None,
        on_connected: Optional[Callable[[], None]] = None,
        on_disconnected: Optional[Callable[[], None]] = None,
        on_ringing: Optional[Callable[[], None]] = None,
        on_answered: Optional[Callable[[], None]] = None,
        on_stop_received: Optional[Callable[[], None]] = None,
    ):
        IntercomTcpClient._instance_counter += 1
        self._instance_id = IntercomTcpClient._instance_counter

        self.host = host
        self.port = port
        self._on_audio = on_audio
        self._on_connected = on_connected
        self._on_disconnected = on_disconnected
        self._on_ringing = on_ringing
        self._on_answered = on_answered
        self._on_stop_received = on_stop_received

        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._connected = False
        self._streaming = False
        self._ringing = False  # ESP has auto_answer OFF, waiting for local answer
        self._receive_task: Optional[asyncio.Task] = None
        self._ping_task: Optional[asyncio.Task] = None

        self._audio_sent = 0
        self._audio_recv = 0
        self._disconnect_notified = False

        _LOGGER.debug("[TCP#%d] Created for %s:%d", self._instance_id, host, port)

    async def connect(self) -> bool:
        if self._connected:
            return True

        try:
            _LOGGER.debug("[TCP#%d] Connecting to %s:%d...", self._instance_id, self.host, self.port)
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=CONNECT_TIMEOUT,
            )
            self._connected = True
            self._disconnect_notified = False
            _LOGGER.debug("[TCP#%d] Connected", self._instance_id)

            self._receive_task = asyncio.create_task(self._receive_loop())
            self._ping_task = asyncio.create_task(self._ping_loop())

            if self._on_connected:
                self._on_connected()

            return True

        except asyncio.TimeoutError:
            _LOGGER.error("[TCP#%d] Connection timeout", self._instance_id)
            return False
        except OSError as err:
            _LOGGER.error("[TCP#%d] Connection error: %s", self._instance_id, err)
            return False

    async def disconnect(self) -> None:
        _LOGGER.debug("[TCP#%d] Disconnecting", self._instance_id)

        self._connected = False
        self._streaming = False
        self._ringing = False

        if self._receive_task:
            self._receive_task.cancel()
            try:
                await asyncio.wait_for(self._receive_task, timeout=1.0)
            except (asyncio.CancelledError, asyncio.TimeoutError):
                pass
            self._receive_task = None

        if self._ping_task:
            self._ping_task.cancel()
            try:
                await asyncio.wait_for(self._ping_task, timeout=1.0)
            except (asyncio.CancelledError, asyncio.TimeoutError):
                pass
            self._ping_task = None

        if self._writer:
            try:
                self._writer.close()
                await asyncio.wait_for(self._writer.wait_closed(), timeout=1.0)
            except Exception:
                pass
            self._writer = None
            self._reader = None

        if not self._disconnect_notified and self._on_disconnected:
            self._disconnect_notified = True
            self._on_disconnected()

        _LOGGER.debug("[TCP#%d] Disconnected (sent=%d recv=%d)",
                      self._instance_id, self._audio_sent, self._audio_recv)

    async def start_stream(self) -> str:
        """Start streaming.

        Returns:
            "streaming" - ESP accepted, streaming started
            "ringing" - ESP has auto_answer OFF, waiting for local answer
            "error" - Connection or send failed
        """
        _LOGGER.debug("[TCP#%d] start_stream()", self._instance_id)

        if not self._connected:
            if not await self.connect():
                return "error"

        if not await self._send_message(MSG_START):
            return "error"

        # Wait briefly for ESP response (PONG=accept, RING=waiting)
        # The actual state is set in _handle_message
        for _ in range(50):  # 500ms max wait
            await asyncio.sleep(0.01)
            if self._streaming:
                _LOGGER.debug("[TCP#%d] Stream started", self._instance_id)
                return "streaming"
            if self._ringing:
                _LOGGER.debug("[TCP#%d] ESP ringing", self._instance_id)
                return "ringing"

        # Timeout - assume old ESP that doesn't send response, treat as streaming
        self._streaming = True
        _LOGGER.warning("[TCP#%d] No response, assuming stream started", self._instance_id)
        return "streaming"

    @property
    def is_ringing(self) -> bool:
        """Return True if ESP is ringing (auto_answer OFF)."""
        return self._ringing

    @property
    def is_streaming(self) -> bool:
        """Return True if actively streaming."""
        return self._streaming

    async def stop_stream(self) -> None:
        _LOGGER.debug("[TCP#%d] stop_stream()", self._instance_id)

        # First stop accepting new audio
        self._streaming = False

        # Try to send STOP but don't block forever
        if self._connected and self._writer:
            try:
                await asyncio.wait_for(self._send_message(MSG_STOP), timeout=1.0)
                _LOGGER.debug("[TCP#%d] STOP sent", self._instance_id)
            except asyncio.TimeoutError:
                _LOGGER.warning("[TCP#%d] STOP timeout", self._instance_id)
            except Exception as err:
                _LOGGER.debug("[TCP#%d] STOP error: %s", self._instance_id, err)

    async def send_audio(self, data: bytes) -> bool:
        """Send audio data - drain periodically to avoid blocking."""
        if not self._connected or not self._streaming or not self._writer:
            return False

        self._audio_sent += 1

        try:
            header = struct.pack("<BBH", MSG_AUDIO, FLAG_NONE, len(data))
            self._writer.write(header + data)

            # Drain periodically to avoid blocking on every packet
            if self._audio_sent % DRAIN_INTERVAL == 0:
                try:
                    await asyncio.wait_for(self._writer.drain(), timeout=0.1)
                except asyncio.TimeoutError:
                    pass  # TCP congestion - continue anyway
            return True
        except Exception as err:
            _LOGGER.error("[TCP#%d] Audio send error: %s", self._instance_id, err)
            return False

    async def _send_message(self, msg_type: int, data: bytes = b"", flags: int = FLAG_NONE) -> bool:
        """Send control message with drain (blocking)."""
        if not self._writer:
            return False

        try:
            header = struct.pack("<BBH", msg_type, flags, len(data))
            self._writer.write(header + data)
            await self._writer.drain()
            return True
        except Exception as err:
            _LOGGER.error("[TCP#%d] Send error: %s", self._instance_id, err)
            return False

    async def _receive_loop(self) -> None:
        _LOGGER.debug("[TCP#%d] Receive loop started", self._instance_id)
        try:
            while self._connected and self._reader:
                header_data = await self._reader.readexactly(HEADER_SIZE)
                msg_type, flags, length = struct.unpack("<BBH", header_data)

                payload = b""
                if length > 0:
                    if length > MAX_PAYLOAD_SIZE:
                        # Protocol desync - cannot recover, must disconnect
                        _LOGGER.error("[TCP#%d] Protocol desync: bad length %d (max %d), closing",
                                     self._instance_id, length, MAX_PAYLOAD_SIZE)
                        raise ConnectionError("protocol desync")
                    payload = await self._reader.readexactly(length)

                await self._handle_message(msg_type, flags, payload)

        except asyncio.IncompleteReadError:
            _LOGGER.info("[TCP#%d] Connection closed by peer", self._instance_id)
        except asyncio.CancelledError:
            _LOGGER.debug("[TCP#%d] Receive loop cancelled", self._instance_id)
        except ConnectionError as err:
            _LOGGER.error("[TCP#%d] Connection error: %s", self._instance_id, err)
        except Exception as err:
            _LOGGER.error("[TCP#%d] Receive error: %s", self._instance_id, err)
        finally:
            # Mark as disconnected so other parts know the connection is dead
            self._connected = False
            self._streaming = False
            if not self._disconnect_notified and self._on_disconnected:
                self._disconnect_notified = True
                self._on_disconnected()

    async def _handle_message(self, msg_type: int, flags: int, payload: bytes) -> None:
        if msg_type == MSG_AUDIO:
            self._audio_recv += 1
            if self._on_audio:
                self._on_audio(payload)

        elif msg_type == MSG_PONG:
            _LOGGER.debug("[TCP#%d] PONG - stream accepted", self._instance_id)
            # PONG after START means ESP accepted (auto_answer ON)
            if not self._streaming and not self._ringing:
                self._streaming = True

        elif msg_type == MSG_RING:
            _LOGGER.debug("[TCP#%d] RING received", self._instance_id)
            self._ringing = True
            if self._on_ringing:
                self._on_ringing()

        elif msg_type == MSG_ANSWER:
            _LOGGER.debug("[TCP#%d] ANSWER received", self._instance_id)
            self._ringing = False
            self._streaming = True
            if self._on_answered:
                self._on_answered()

        elif msg_type == MSG_STOP:
            _LOGGER.debug("[TCP#%d] STOP received from ESP", self._instance_id)
            self._streaming = False
            self._ringing = False
            if self._on_stop_received:
                self._on_stop_received()

        elif msg_type == MSG_PING:
            _LOGGER.debug("[TCP#%d] PING -> PONG", self._instance_id)
            await self._send_message(MSG_PONG)

        elif msg_type == MSG_ERROR:
            _LOGGER.error("[TCP#%d] ERROR from ESP", self._instance_id)

    async def _ping_loop(self) -> None:
        try:
            while self._connected:
                await asyncio.sleep(PING_INTERVAL)
                # Don't ping during streaming - TCP already detects dead connections
                # and ping could interfere with audio flow
                if self._connected and not self._streaming:
                    await self._send_message(MSG_PING)
        except asyncio.CancelledError:
            pass
