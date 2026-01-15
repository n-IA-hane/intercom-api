# Claude Code Project Context - Intercom API

## Overview

Sistema intercom bidirezionale full-duplex che usa TCP invece di UDP/WebRTC.
Sostituisce `esphome-intercom` (legacy UDP) con un approccio più robusto.

## Repository

- **Questo repo**: `/home/daniele/cc/claude/intercom-api/`
- **Legacy (non sviluppare)**: `/home/daniele/cc/claude/esphome-intercom/`

## Architettura

```
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   Browser   │◄──WS───►│     HA      │◄──TCP──►│    ESP32    │
│             │         │             │  6054   │             │
└─────────────┘         └─────────────┘         └─────────────┘
                                                      │
                                                 TCP 6054
                                                      │
                                                      ▼
                                                ┌─────────────┐
                                                │  ESP32 #2   │
                                                └─────────────┘
```

## Componenti

### 1. ESPHome: `intercom_api`
- **Porta**: TCP 6054 (audio streaming)
- **Controllo**: via ESPHome API normale (6053) - switch, number entities
- **Modalità**: Server (attende connessioni) + Client (per ESP→ESP)
- **Coesiste con**: voice_assistant, intercom_audio legacy

### 2. HA Integration: `intercom_native`
- **Discovery**: trova device con entity `switch.*_intercom_api`
- **WebSocket API**: `intercom_native/start`, `intercom_native/stop`
- **Binary handler**: per audio browser ↔ HA
- **TCP client**: verso ESP porta 6054

### 3. Frontend: `intercom-card.js`
- **Lovelace card** custom
- **getUserMedia** + AudioWorklet
- **WebSocket** binary streaming verso HA

---

## Protocollo TCP (Porta 6054)

### Header (4 bytes)

```
┌──────────────┬──────────────┬──────────────────────┐
│ Type (1 byte)│ Flags (1 byte)│ Length (2 bytes LE) │
└──────────────┴──────────────┴──────────────────────┘
```

### Message Types

| Type | Name | Direction | Payload |
|------|------|-----------|---------|
| 0x01 | AUDIO | Both | PCM 16-bit mono 16kHz |
| 0x02 | START | Client→Server | - |
| 0x03 | STOP | Both | - |
| 0x04 | PING | Both | - |
| 0x05 | PONG | Both | - |
| 0x06 | ERROR | Server→Client | Error code (1 byte) |

### Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | END | Last packet of stream |
| 1-7 | Reserved | - |

### Audio Format

| Parameter | Value |
|-----------|-------|
| Sample Rate | 16000 Hz |
| Bit Depth | 16-bit signed PCM |
| Channels | Mono |
| Chunk Size | 512 bytes (256 samples = 16ms) |

### Handshake

```
Client                          Server
   │                               │
   │──── TCP Connect ────────────►│
   │                               │
   │──── START ──────────────────►│
   │                               │
   │◄─── PONG (ack) ──────────────│
   │                               │
   │◄─── AUDIO ───────────────────│
   │──── AUDIO ──────────────────►│
   │         (full duplex)         │
   │                               │
   │──── STOP ───────────────────►│
   │◄─── STOP ────────────────────│
   │                               │
   │──── TCP Close ──────────────►│
```

---

## Struttura File

```
intercom-api/
├── esphome/
│   └── components/
│       └── intercom_api/
│           ├── __init__.py           # ESPHome component config
│           ├── intercom_api.h        # Header
│           ├── intercom_api.cpp      # TCP server + audio handling
│           ├── intercom_protocol.h   # Protocol constants
│           ├── switch.py             # Switch entity
│           └── number.py             # Volume entity
│
├── homeassistant/
│   └── custom_components/
│       └── intercom_native/
│           ├── __init__.py           # Integration setup
│           ├── manifest.json         # HA manifest
│           ├── config_flow.py        # Config UI
│           ├── websocket_api.py      # WS commands + binary handler
│           ├── tcp_client.py         # Async TCP client to ESP
│           └── const.py              # Constants
│
├── frontend/
│   └── www/
│       ├── intercom-card.js          # Lovelace card
│       └── intercom-processor.js     # AudioWorklet
│
├── CLAUDE.md                         # This file
└── README.md                         # User documentation
```

---

## Development

```bash
# No venv needed for this repo (pure components)

# Test ESPHome component: copy to esphome-intercom and compile
cp -r esphome/components/intercom_api /home/daniele/cc/claude/esphome-intercom/components/

# Test HA integration: symlink to HA config
ln -s /home/daniele/cc/claude/intercom-api/homeassistant/custom_components/intercom_native \
      ~/.homeassistant/custom_components/intercom_native
```

---

## Dipendenze da esphome-intercom

Questi componenti del repo legacy possono essere riutilizzati:

| Component | Uso |
|-----------|-----|
| `i2s_audio_duplex` | Full-duplex su singolo bus I2S (ES8311) |
| `esp_aec` | Echo cancellation |
| `mdns_discovery` | Discovery peers (opzionale per ESP→ESP) |

---

## Roadmap

### Fase 1: Protocollo ✅
- [x] Definito protocollo TCP binario

### Fase 2: ESPHome Component
- [ ] `intercom_protocol.h` - costanti protocollo
- [ ] `intercom_api.h/cpp` - TCP server, audio handling
- [ ] `__init__.py` - config validation
- [ ] `switch.py` - on/off streaming
- [ ] `number.py` - volume control
- [ ] Test compilazione
- [ ] Test connessione TCP

### Fase 3: HA Integration
- [ ] `manifest.json`
- [ ] `__init__.py` - setup, discovery
- [ ] `tcp_client.py` - async TCP verso ESP
- [ ] `websocket_api.py` - binary handler browser
- [ ] Test browser → HA → ESP

### Fase 4: Lovelace Card
- [ ] `intercom-card.js` - UI
- [ ] `intercom-processor.js` - AudioWorklet
- [ ] Test Chrome, Safari, Firefox

### Fase 5: ESP → ESP
- [ ] Client mode in intercom_api
- [ ] Test chiamata diretta tra due ESP

---

## Note Tecniche

### Perché TCP invece di UDP?

| UDP (legacy) | TCP (nuovo) |
|--------------|-------------|
| Problemi NAT/firewall | Passa attraverso HA |
| Packet loss | Reliable delivery |
| Richiede port forwarding | Nessuna config rete |
| go2rtc/WebRTC complesso | Protocollo semplice |

### Coesistenza con voice_assistant

- `intercom_api` usa porta 6054
- `voice_assistant` usa porta 6053 (API nativa)
- Zero conflitti, possono coesistere

### Latenza Target

- Chunk size: 512 bytes = 16ms di audio
- Round-trip target: < 200ms
- Buffer playback: 2-3 chunks = 32-48ms
