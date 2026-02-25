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
FLAG_NO_RING = 0x02  # START flag: skip ringing, start streaming directly (for caller in bridge)

# Header size
HEADER_SIZE = 4

# Timeouts
CONNECT_TIMEOUT = 5.0
PING_INTERVAL = 5.0

