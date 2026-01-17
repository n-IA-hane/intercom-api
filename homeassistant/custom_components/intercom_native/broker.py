"""TCP Broker for ESP↔ESP intercom communication.

This broker allows ESP devices to communicate with each other through Home Assistant.
ESPs connect TO the broker (outbound connections), allowing traversal of NAT/firewalls.

Protocol:
- Port 6060 (configurable)
- 12-byte header: type(1) + flags(1) + length(2) + call_id(4) + seq(4)
- Devices register with REGISTER message containing device_id
- Calls initiated with INVITE, answered with ANSWER, ended with HANGUP
- Audio relayed between call participants with AUDIO messages
"""

import asyncio
import json
import logging
import struct
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Dict, Optional

from homeassistant.core import HomeAssistant

from .const import (
    BROKER_PORT,
    BROKER_HEADER_SIZE,
    BROKER_MSG_REGISTER,
    BROKER_MSG_INVITE,
    BROKER_MSG_RING,
    BROKER_MSG_ANSWER,
    BROKER_MSG_DECLINE,
    BROKER_MSG_HANGUP,
    BROKER_MSG_BYE,
    BROKER_MSG_AUDIO,
    BROKER_MSG_CONTACTS,
    BROKER_MSG_PING,
    BROKER_MSG_PONG,
    BROKER_MSG_ERROR,
    BROKER_ERR_NOT_FOUND,
    BROKER_ERR_BUSY,
    BROKER_ERR_TIMEOUT,
    BROKER_ERR_PROTOCOL,
    DECLINE_BUSY,
    CALL_STATE_RINGING,
    CALL_STATE_IN_CALL,
    BROKER_CALL_TIMEOUT,
    BROKER_PING_INTERVAL,
    BROKER_AUDIO_QUEUE_SIZE,
    BROKER_DRAIN_INTERVAL,
)

_LOGGER = logging.getLogger(__name__)


@dataclass
class DeviceConnection:
    """Represents a connected ESP device."""

    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    device_id: str = ""
    current_call_id: int = 0
    audio_queue: deque = field(default_factory=lambda: deque(maxlen=BROKER_AUDIO_QUEUE_SIZE))
    audio_event: asyncio.Event = field(default_factory=asyncio.Event)
    audio_seq: int = 0
    packets_sent: int = 0
    last_ping: float = 0.0

    def __hash__(self):
        """Hash by device_id for dict usage."""
        return hash(self.device_id) if self.device_id else id(self)


@dataclass
class Call:
    """Represents an active or pending call."""

    call_id: int
    caller: DeviceConnection
    callee: DeviceConnection
    state: str = CALL_STATE_RINGING
    timeout_task: Optional[asyncio.Task] = None


class IntercomBroker:
    """TCP broker for ESP↔ESP audio communication."""

    def __init__(self, hass: HomeAssistant, port: int = BROKER_PORT):
        """Initialize the broker."""
        self.hass = hass
        self.port = port
        self.server: Optional[asyncio.Server] = None

        # Device registry: device_id -> DeviceConnection
        self.devices: Dict[str, DeviceConnection] = {}

        # Active calls: call_id -> Call
        self.calls: Dict[int, Call] = {}
        self._next_call_id = 1

        # Callbacks for external notification
        self._on_device_connected: Optional[Callable[[str], None]] = None
        self._on_device_disconnected: Optional[Callable[[str], None]] = None
        self._on_call_started: Optional[Callable[[int, str, str], None]] = None
        self._on_call_ended: Optional[Callable[[int], None]] = None

    async def start(self) -> bool:
        """Start the TCP broker server."""
        try:
            self.server = await asyncio.start_server(
                self._handle_connection,
                "0.0.0.0",
                self.port,
            )
            _LOGGER.info("Intercom broker started on port %d", self.port)
            return True
        except OSError as err:
            _LOGGER.error("Failed to start broker: %s", err)
            return False

    async def stop(self) -> None:
        """Stop the broker and disconnect all devices."""
        _LOGGER.info("Stopping intercom broker...")

        # End all active calls
        for call_id in list(self.calls.keys()):
            await self._end_call(call_id)

        # Close all device connections
        for device in list(self.devices.values()):
            await self._disconnect_device(device)

        # Stop server
        if self.server:
            self.server.close()
            await self.server.wait_closed()
            self.server = None

        _LOGGER.info("Intercom broker stopped")

    def get_connected_devices(self) -> list[str]:
        """Return list of connected device IDs."""
        return list(self.devices.keys())

    def is_device_in_call(self, device_id: str) -> bool:
        """Check if device is currently in a call."""
        device = self.devices.get(device_id)
        return device is not None and device.current_call_id != 0

    # =========================================================================
    # Connection handling
    # =========================================================================

    async def _handle_connection(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        """Handle a new incoming ESP connection."""
        addr = writer.get_extra_info("peername")
        _LOGGER.info("New connection from %s", addr)

        device = DeviceConnection(reader=reader, writer=writer)
        device.last_ping = asyncio.get_event_loop().time()

        try:
            # Start audio TX task for this device
            audio_task = asyncio.create_task(self._audio_tx_task(device))

            while True:
                # Read header
                header_data = await reader.readexactly(BROKER_HEADER_SIZE)
                msg_type, flags, length, call_id, seq = struct.unpack(
                    "<BBHII", header_data
                )

                # Read payload
                payload = b""
                if length > 0:
                    if length > 4096:  # Sanity check
                        _LOGGER.error("Invalid payload length %d from %s", length, device.device_id or addr)
                        break
                    payload = await reader.readexactly(length)

                # Handle message
                await self._handle_message(device, msg_type, flags, call_id, seq, payload)

        except asyncio.IncompleteReadError:
            _LOGGER.info("Device %s disconnected", device.device_id or addr)
        except asyncio.CancelledError:
            _LOGGER.debug("Connection handler cancelled for %s", device.device_id or addr)
        except Exception as err:
            _LOGGER.error("Error handling device %s: %s", device.device_id or addr, err)
        finally:
            audio_task.cancel()
            try:
                await audio_task
            except asyncio.CancelledError:
                pass
            await self._disconnect_device(device)

    async def _disconnect_device(self, device: DeviceConnection) -> None:
        """Clean up disconnected device."""
        device_id = device.device_id

        # End any active call
        if device.current_call_id != 0:
            await self._end_call(device.current_call_id, notify_peer=True)

        # Remove from registry
        if device_id and device_id in self.devices:
            del self.devices[device_id]
            _LOGGER.info("Device %s unregistered", device_id)

            # Notify other devices about updated contact list
            await self._broadcast_contacts()

            if self._on_device_disconnected:
                self._on_device_disconnected(device_id)

        # Close socket
        try:
            device.writer.close()
            await device.writer.wait_closed()
        except Exception:
            pass

    # =========================================================================
    # Message handling
    # =========================================================================

    async def _handle_message(
        self,
        device: DeviceConnection,
        msg_type: int,
        flags: int,
        call_id: int,
        seq: int,
        payload: bytes,
    ) -> None:
        """Handle incoming message from device."""
        if msg_type == BROKER_MSG_REGISTER:
            await self._handle_register(device, payload)

        elif msg_type == BROKER_MSG_INVITE:
            await self._handle_invite(device, payload)

        elif msg_type == BROKER_MSG_ANSWER:
            await self._handle_answer(device, call_id)

        elif msg_type == BROKER_MSG_DECLINE:
            reason = payload[0] if payload else DECLINE_BUSY
            await self._handle_decline(device, call_id, reason)

        elif msg_type == BROKER_MSG_HANGUP:
            await self._handle_hangup(device, call_id)

        elif msg_type == BROKER_MSG_AUDIO:
            await self._handle_audio(device, call_id, seq, payload)

        elif msg_type == BROKER_MSG_PING:
            device.last_ping = asyncio.get_event_loop().time()
            await self._send_message(device, BROKER_MSG_PONG, 0, 0, 0)

        elif msg_type == BROKER_MSG_PONG:
            device.last_ping = asyncio.get_event_loop().time()

        else:
            _LOGGER.warning("Unknown message type 0x%02x from %s", msg_type, device.device_id)

    async def _handle_register(self, device: DeviceConnection, payload: bytes) -> None:
        """Handle REGISTER message - device identification."""
        device_id = payload.decode("utf-8").rstrip("\x00")
        if not device_id:
            _LOGGER.warning("Empty device_id in REGISTER")
            return

        # Check for duplicate registration
        if device_id in self.devices:
            old_device = self.devices[device_id]
            _LOGGER.warning("Device %s re-registering, closing old connection", device_id)
            await self._disconnect_device(old_device)

        device.device_id = device_id
        self.devices[device_id] = device
        _LOGGER.info("Device registered: %s", device_id)

        # Send contact list to new device
        await self._send_contacts(device)

        # Notify other devices about updated contact list
        await self._broadcast_contacts()

        if self._on_device_connected:
            self._on_device_connected(device_id)

    async def _handle_invite(self, device: DeviceConnection, payload: bytes) -> None:
        """Handle INVITE message - initiate call to target device."""
        target_id = payload.decode("utf-8").rstrip("\x00")

        # Check caller is registered
        if not device.device_id:
            await self._send_error(device, 0, BROKER_ERR_PROTOCOL)
            return

        # Check caller not already in call
        if device.current_call_id != 0:
            await self._send_error(device, 0, BROKER_ERR_BUSY)
            return

        # Check target exists
        target = self.devices.get(target_id)
        if not target:
            _LOGGER.info("INVITE from %s to %s: target not found", device.device_id, target_id)
            await self._send_error(device, 0, BROKER_ERR_NOT_FOUND)
            return

        # Check target not in call
        if target.current_call_id != 0:
            _LOGGER.info("INVITE from %s to %s: target busy", device.device_id, target_id)
            await self._send_error(device, 0, BROKER_ERR_BUSY)
            return

        # Create call
        call_id = self._next_call_id
        self._next_call_id += 1

        call = Call(call_id=call_id, caller=device, callee=target)
        self.calls[call_id] = call

        # Mark both devices as in call (pending)
        device.current_call_id = call_id
        target.current_call_id = call_id

        _LOGGER.info("Call %d: %s -> %s (RINGING)", call_id, device.device_id, target_id)

        # Send RING to target
        await self._send_message(
            target,
            BROKER_MSG_RING,
            0,
            call_id,
            0,
            device.device_id.encode("utf-8") + b"\x00",
        )

        # Start timeout task
        call.timeout_task = asyncio.create_task(
            self._call_timeout(call_id, BROKER_CALL_TIMEOUT)
        )

        if self._on_call_started:
            self._on_call_started(call_id, device.device_id, target_id)

    async def _handle_answer(self, device: DeviceConnection, call_id: int) -> None:
        """Handle ANSWER message - accept incoming call."""
        call = self.calls.get(call_id)
        if not call:
            _LOGGER.warning("ANSWER for unknown call %d from %s", call_id, device.device_id)
            return

        # Verify device is callee
        if call.callee != device:
            _LOGGER.warning("ANSWER from non-callee %s for call %d", device.device_id, call_id)
            return

        # Cancel timeout
        if call.timeout_task:
            call.timeout_task.cancel()
            call.timeout_task = None

        # Update state
        call.state = CALL_STATE_IN_CALL
        _LOGGER.info("Call %d: ANSWERED (%s <-> %s)",
                     call_id, call.caller.device_id, call.callee.device_id)

        # Notify caller that call was answered (send ANSWER)
        await self._send_message(call.caller, BROKER_MSG_ANSWER, 0, call_id, 0)

    async def _handle_decline(
        self, device: DeviceConnection, call_id: int, reason: int
    ) -> None:
        """Handle DECLINE message - reject incoming call."""
        call = self.calls.get(call_id)
        if not call:
            return

        # Verify device is callee
        if call.callee != device:
            return

        reason_str = "busy" if reason == DECLINE_BUSY else "rejected"
        _LOGGER.info("Call %d: DECLINED by %s (%s)", call_id, device.device_id, reason_str)

        # Send DECLINE to caller
        await self._send_message(call.caller, BROKER_MSG_DECLINE, 0, call_id, 0, bytes([reason]))

        await self._end_call(call_id, notify_peer=False)

    async def _handle_hangup(self, device: DeviceConnection, call_id: int) -> None:
        """Handle HANGUP message - end active call."""
        call = self.calls.get(call_id)
        if not call:
            return

        _LOGGER.info("Call %d: HANGUP from %s", call_id, device.device_id)
        await self._end_call(call_id, notify_peer=True, hangup_by=device)

    async def _handle_audio(
        self, device: DeviceConnection, call_id: int, seq: int, payload: bytes
    ) -> None:
        """Handle AUDIO message - relay to peer."""
        call = self.calls.get(call_id)
        if not call or call.state != CALL_STATE_IN_CALL:
            return

        # Determine peer
        if device == call.caller:
            peer = call.callee
        elif device == call.callee:
            peer = call.caller
        else:
            return

        # Enqueue audio for peer (deque automatically drops oldest if full)
        peer.audio_queue.append((seq, payload))
        peer.audio_event.set()

    # =========================================================================
    # Audio TX task
    # =========================================================================

    async def _audio_tx_task(self, device: DeviceConnection) -> None:
        """Task to send queued audio to device."""
        try:
            while True:
                # Wait for audio to send
                await device.audio_event.wait()
                device.audio_event.clear()

                # Send all queued audio
                while device.audio_queue:
                    seq, audio_data = device.audio_queue.popleft()
                    await self._send_message(
                        device,
                        BROKER_MSG_AUDIO,
                        0,
                        device.current_call_id,
                        seq,
                        audio_data,
                        drain=False,
                    )
                    device.packets_sent += 1

                    # Periodic drain to avoid buffer buildup
                    if device.packets_sent % BROKER_DRAIN_INTERVAL == 0:
                        try:
                            await asyncio.wait_for(device.writer.drain(), timeout=0.05)
                        except asyncio.TimeoutError:
                            pass  # Continue anyway, TCP handles congestion

        except asyncio.CancelledError:
            pass

    # =========================================================================
    # Call management
    # =========================================================================

    async def _call_timeout(self, call_id: int, timeout: float) -> None:
        """Handle call timeout (no answer)."""
        await asyncio.sleep(timeout)

        call = self.calls.get(call_id)
        if call and call.state == CALL_STATE_RINGING:
            _LOGGER.info("Call %d: TIMEOUT (no answer)", call_id)
            # Send timeout error to caller
            await self._send_error(call.caller, call_id, BROKER_ERR_TIMEOUT)
            await self._end_call(call_id, notify_peer=True)

    async def _end_call(
        self,
        call_id: int,
        notify_peer: bool = False,
        hangup_by: Optional[DeviceConnection] = None,
    ) -> None:
        """End a call and clean up."""
        call = self.calls.pop(call_id, None)
        if not call:
            return

        # Cancel timeout task
        if call.timeout_task:
            call.timeout_task.cancel()
            try:
                await call.timeout_task
            except asyncio.CancelledError:
                pass

        # Clear call_id from devices
        if call.caller.current_call_id == call_id:
            call.caller.current_call_id = 0
        if call.callee.current_call_id == call_id:
            call.callee.current_call_id = 0

        # Notify peer if requested
        if notify_peer:
            peer = call.callee if hangup_by == call.caller else call.caller
            if hangup_by:
                await self._send_message(peer, BROKER_MSG_BYE, 0, call_id, 0)
            else:
                # Both get BYE (e.g., timeout)
                await self._send_message(call.caller, BROKER_MSG_BYE, 0, call_id, 0)
                await self._send_message(call.callee, BROKER_MSG_BYE, 0, call_id, 0)

        _LOGGER.info("Call %d ended", call_id)

        if self._on_call_ended:
            self._on_call_ended(call_id)

    # =========================================================================
    # Message sending
    # =========================================================================

    async def _send_message(
        self,
        device: DeviceConnection,
        msg_type: int,
        flags: int,
        call_id: int,
        seq: int,
        payload: bytes = b"",
        drain: bool = True,
    ) -> bool:
        """Send a message to a device."""
        try:
            header = struct.pack("<BBHII", msg_type, flags, len(payload), call_id, seq)
            device.writer.write(header + payload)
            if drain:
                await device.writer.drain()
            return True
        except Exception as err:
            _LOGGER.error("Failed to send to %s: %s", device.device_id, err)
            return False

    async def _send_error(
        self, device: DeviceConnection, call_id: int, error_code: int
    ) -> None:
        """Send error message to device."""
        await self._send_message(
            device, BROKER_MSG_ERROR, 0, call_id, 0, bytes([error_code])
        )

    async def _send_contacts(self, device: DeviceConnection) -> None:
        """Send contact list to a device."""
        # Build contact list (all devices except self)
        contacts = [
            {"id": dev_id, "name": dev_id, "busy": self.is_device_in_call(dev_id)}
            for dev_id in self.devices.keys()
            if dev_id != device.device_id
        ]

        payload = json.dumps(contacts).encode("utf-8")
        await self._send_message(device, BROKER_MSG_CONTACTS, 0, 0, 0, payload)

    async def _broadcast_contacts(self) -> None:
        """Broadcast updated contact list to all devices."""
        for device in self.devices.values():
            await self._send_contacts(device)


# Global broker instance (created by integration setup)
_broker: Optional[IntercomBroker] = None


def get_broker() -> Optional[IntercomBroker]:
    """Get the global broker instance."""
    return _broker


async def async_setup_broker(hass: HomeAssistant, port: int = BROKER_PORT) -> bool:
    """Set up the broker service."""
    global _broker

    if _broker is not None:
        _LOGGER.warning("Broker already running")
        return True

    _broker = IntercomBroker(hass, port)
    if not await _broker.start():
        _broker = None
        return False

    return True


async def async_stop_broker() -> None:
    """Stop the broker service."""
    global _broker

    if _broker:
        await _broker.stop()
        _broker = None
