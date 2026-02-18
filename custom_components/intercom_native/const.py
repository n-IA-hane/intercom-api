"""Constants for Intercom Native integration."""

import json
from pathlib import Path

DOMAIN = "intercom_native"

# Version from manifest.json
_MANIFEST = Path(__file__).parent / "manifest.json"
with open(_MANIFEST, encoding="utf-8") as _f:
    INTEGRATION_VERSION = json.load(_f).get("version", "0.0.0")

# Frontend URL base for serving the Lovelace card
URL_BASE = "/intercom-native"

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
FLAG_NO_RING = 0x02  # START flag: skip ringing, start streaming directly (for caller in bridge)

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
