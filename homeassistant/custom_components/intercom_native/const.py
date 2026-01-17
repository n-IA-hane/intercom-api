"""Constants for Intercom Native integration."""

DOMAIN = "intercom_native"

# TCP Protocol
INTERCOM_PORT = 6054
PROTOCOL_VERSION = 1

# Message types
MSG_AUDIO = 0x01
MSG_START = 0x02
MSG_STOP = 0x03
MSG_PING = 0x04
MSG_PONG = 0x05
MSG_ERROR = 0x06
MSG_RING = 0x07      # ESP→HA: auto_answer OFF, waiting for local answer
MSG_ANSWER = 0x08    # ESP→HA: call answered locally, start stream

# Message flags
FLAG_NONE = 0x00
FLAG_END = 0x01

# Error codes
ERR_OK = 0x00
ERR_BUSY = 0x01
ERR_INVALID = 0x02
ERR_NOT_READY = 0x03
ERR_INTERNAL = 0xFF

# Audio format
SAMPLE_RATE = 16000
BITS_PER_SAMPLE = 16
CHANNELS = 1
AUDIO_CHUNK_SIZE = 512  # bytes
SAMPLES_PER_CHUNK = 256
CHUNK_DURATION_MS = 16

# Header size
HEADER_SIZE = 4

# Timeouts
CONNECT_TIMEOUT = 5.0
PING_INTERVAL = 5.0
PING_TIMEOUT = 10.0

# WebSocket commands
WS_TYPE_START = f"{DOMAIN}/start"
WS_TYPE_STOP = f"{DOMAIN}/stop"
WS_TYPE_LIST = f"{DOMAIN}/list_devices"

# Events
EVENT_AUDIO_RECEIVED = f"{DOMAIN}_audio_received"
EVENT_CONNECTED = f"{DOMAIN}_connected"
EVENT_DISCONNECTED = f"{DOMAIN}_disconnected"

# ============================================================================
# Broker Protocol (ESP↔ESP via HA relay) - Port 6060
# ============================================================================

BROKER_PORT = 6060
BROKER_HEADER_SIZE = 12  # type(1) + flags(1) + length(2) + call_id(4) + seq(4)

# Broker message types (0x10-0x1F range to avoid collision with existing protocol)
BROKER_MSG_REGISTER = 0x10   # ESP→HA: device registration
BROKER_MSG_INVITE = 0x11     # ESP→HA: initiate call to target
BROKER_MSG_RING = 0x12       # HA→ESP: incoming call notification
BROKER_MSG_ANSWER = 0x13     # ESP→HA: accept incoming call
BROKER_MSG_DECLINE = 0x14    # ESP→HA: reject incoming call
BROKER_MSG_HANGUP = 0x15     # Both: end call
BROKER_MSG_BYE = 0x16        # HA→ESP: call ended by peer
BROKER_MSG_AUDIO = 0x17      # Both: audio data during call
BROKER_MSG_CONTACTS = 0x18   # HA→ESP: list of available devices
BROKER_MSG_PING = 0x19       # Both: keepalive
BROKER_MSG_PONG = 0x1A       # Both: keepalive response
BROKER_MSG_ERROR = 0x1B      # HA→ESP: error notification

# Broker error codes
BROKER_ERR_NOT_FOUND = 0x01     # Target device not connected
BROKER_ERR_BUSY = 0x02          # Target device already in call
BROKER_ERR_TIMEOUT = 0x03       # Call timeout (no answer)
BROKER_ERR_PROTOCOL = 0x04      # Protocol error

# Decline reasons
DECLINE_BUSY = 0x00
DECLINE_REJECT = 0x01

# Call states
CALL_STATE_RINGING = "RINGING"
CALL_STATE_IN_CALL = "IN_CALL"

# Broker timeouts
BROKER_CALL_TIMEOUT = 30.0       # Seconds to wait for answer
BROKER_PING_INTERVAL = 10.0      # Seconds between pings
BROKER_PING_TIMEOUT = 30.0       # Seconds before considering device dead

# Audio queue
BROKER_AUDIO_QUEUE_SIZE = 10     # Max frames in queue (~160ms at 16ms/frame)
BROKER_DRAIN_INTERVAL = 10       # Drain writer every N packets
