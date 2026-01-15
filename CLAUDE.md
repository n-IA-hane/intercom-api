# Claude Code Project Context - Intercom API

## CRITICAL SETUP INFO - DO NOT FORGET

- **HA External URL**: `https://514d1f563e4e1ad4.sn.mynetname.net`
- **HA Token**: `eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJlMTc5YzQ2ZmVkOGM0ZjU1OTQyOWRkNDg1OTI4ZDk2MiIsImlhdCI6MTc2ODQ5MTE3NywiZXhwIjoyMDgzODUxMTc3fQ.W6iHGkX1rLKNkmVjZgeTukWRUuBSqIupU2L1VYEOgWY`
- **Test Card**: `/lovelace/test`
- **Home Assistant**: `root@192.168.1.10` (LXC container, HA installed via pip)
- **ESPHome**: venv in `/home/daniele/cc/claude/intercom-api/` on THIS PC
- **ESP32 IP**: 192.168.1.18
- **HA Config Path**: `/home/homeassistant/.homeassistant/`
- **Deploy HA files**: `scp -r homeassistant/custom_components/intercom_native root@192.168.1.10:/home/homeassistant/.homeassistant/custom_components/`
- **Deploy frontend**: `scp frontend/www/*.js root@192.168.1.10:/home/homeassistant/.homeassistant/www/`
- **Restart HA**: `ssh root@192.168.1.10 'systemctl restart homeassistant'`
- **HA Logs**: `ssh root@192.168.1.10 'journalctl -u homeassistant -f'`
- **Compile & Upload ESP**: `source venv/bin/activate && esphome compile intercom-mini.yaml && esphome upload intercom-mini.yaml --device 192.168.1.18`

## Overview

Sistema intercom bidirezionale full-duplex che usa TCP invece di UDP/WebRTC.
Sostituisce `esphome-intercom` (legacy UDP) con un approccio più robusto.

## Repository

- **Questo repo**: `/home/daniele/cc/claude/intercom-api/`
- **Legacy (non sviluppare)**: `/home/daniele/cc/claude/esphome-intercom/`

---

## STATO ATTUALE (2026-01-15 sera)

### PROBLEMA URGENTE
**Upload OTA interrotto al 65%** - L'ESP potrebbe essere in safe mode o con firmware corrotto.
- Potrebbe servire **reset fisico** (stacca/attacca alimentazione)
- Se non risponde, potrebbe servire flash via USB

### Cosa funziona (testato prima del problema)
- **LATENZA RISOLTA!** - Era causata da `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))` che bloccava 100ms per ogni iterazione del server_task durante lo streaming
- Full duplex Browser ↔ HA ↔ ESP istantaneo in entrambe le direzioni
- Audio pulito senza glitch

### Modifiche pronte da deployare

**ESP (già compilato, da uploadare):**
1. Fix latenza: non-blocking poll durante streaming
2. Helper `set_active_()` e `set_streaming_()` - codice pulito
3. `client_.socket` e `client_.streaming` atomic - thread safety
4. Buffer overflow check in handle_message_(AUDIO)
5. FIONREAD monitoring per TCP backlog
6. Task priorities: server=7, TX=6, speaker=4
7. Speaker su Core 0 (era Core 1)
8. buffer_duration 100ms (era 500ms)

**HA (da deployare con scp):**
- websocket_api.py v4.1.0 - Queue-based audio (8 slot) invece di task per frame
- tcp_client.py v4.3.0 - No ping durante streaming

### Commit locali (non pushati)
```
6c6c8c9 Fix critical latency: Non-blocking poll during streaming
e797526 Refactor: Separate FreeRTOS tasks for TX/RX audio paths
```

---

## Architettura

```
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   Browser   │◄──WS───►│     HA      │◄──TCP──►│    ESP32    │
│             │         │             │  6054   │             │
└─────────────┘         └─────────────┘         └─────────────┘
     │                        │                       │
  AudioWorklet            tcp_client.py          FreeRTOS Tasks
  16kHz mono              async TCP              server_task (Core 1, prio 7)
  2048B chunks            queue-based            tx_task (Core 0, prio 6)
                                                 speaker_task (Core 0, prio 4)
```

## Componenti

### 1. ESPHome: `intercom_api`
- **Porta**: TCP 6054 (audio streaming)
- **Controllo**: via ESPHome API normale (6053) - switch, number entities
- **Modalità**: Server (attende connessioni) + Client (per ESP→ESP)
- **Task RTOS**:
  - `server_task_` (Core 1, priority 7) - TCP accept, receive, RX handling
  - `tx_task_` (Core 0, priority 6) - mic→network TX
  - `speaker_task_` (Core 0, priority 4) - speaker buffer→I2S playback

### 2. HA Integration: `intercom_native`
- **WebSocket API**: `intercom_native/start`, `intercom_native/stop`, `intercom_native/audio`
- **TCP client**: Async verso ESP porta 6054
- **Versione**: websocket_api.py v4.1.0, tcp_client.py v4.3.0

### 3. Frontend: `intercom-card.js`
- **Lovelace card** custom
- **getUserMedia** + AudioWorklet (16kHz)
- **WebSocket** JSON + base64 audio verso HA
- **Versione**: 4.3.0 (scheduled playback)

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

### Audio Format

| Parameter | Value |
|-----------|-------|
| Sample Rate | 16000 Hz |
| Bit Depth | 16-bit signed PCM |
| Channels | Mono |
| ESP Chunk Size | 512 bytes (256 samples = 16ms) |
| Browser Chunk Size | 2048 bytes (1024 samples = 64ms) |

---

## Struttura File

```
intercom-api/
├── esphome/
│   └── components/
│       └── intercom_api/
│           ├── __init__.py           # ESPHome component config
│           ├── intercom_api.h        # Header + class definition
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
│           ├── websocket_api.py      # WS commands + session manager (v4.1.0)
│           ├── tcp_client.py         # Async TCP client (v4.3.0)
│           └── const.py              # Constants
│
├── frontend/
│   └── www/
│       ├── intercom-card.js          # Lovelace card
│       └── intercom-processor.js     # AudioWorklet
│
├── intercom-mini.yaml                # ESP32-S3 config
├── CLAUDE.md                         # This file
└── README.md                         # User documentation
```

---

## Development

```bash
# Compile e upload ESP
source venv/bin/activate
esphome compile intercom-mini.yaml
esphome upload intercom-mini.yaml --device 192.168.1.18

# Deploy HA integration
scp -r homeassistant/custom_components/intercom_native root@192.168.1.10:/home/homeassistant/.homeassistant/custom_components/
ssh root@192.168.1.10 'systemctl restart homeassistant'

# Monitor logs
ssh root@192.168.1.10 'journalctl -u homeassistant -f'
```

---

## Fix principali applicati

### ROOT CAUSE della latenza HA→ESP
In `server_task_()` c'era:
```cpp
ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
```
Questo bloccava **100ms per ogni iterazione** del loop, anche durante streaming attivo!
I dati TCP si accumulavano nel buffer invece di essere letti subito.

**Fix:**
```cpp
if (this->client_.streaming.load()) {
  ulTaskNotifyTake(pdTRUE, 0);  // Non-blocking durante streaming
} else {
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // Idle: risparmia CPU
}
```

### Cleanup applicato
1. Helper `set_active_()` e `set_streaming_()` - elimina duplicazione codice
2. `client_.socket` e `client_.streaming` sono ora `std::atomic` - thread safety senza mutex
3. Buffer overflow tracking in handle_message_(AUDIO)
4. FIONREAD monitoring per vedere backlog TCP
5. Task priorities ottimizzate: server=7 (RX vince sempre), TX=6, speaker=4
6. Speaker task su Core 0 (separa RX da audio output)

---

## TODO - Prossimi step

### Immediato (quando torni a casa)
- [ ] Verificare stato ESP (potrebbe servire reset fisico o flash USB)
- [ ] Upload firmware ESP
- [ ] Deploy HA integration
- [ ] Test completo

### Priorità Bassa
- [ ] ESP→ESP direct mode
- [ ] Echo cancellation (AEC)
- [ ] Frontend: resampling 44.1kHz

---

## Note Tecniche

### Perché TCP invece di UDP?

| UDP (legacy) | TCP (nuovo) |
|--------------|-------------|
| Problemi NAT/firewall | Passa attraverso HA |
| Packet loss | Reliable delivery |
| Richiede port forwarding | Nessuna config rete |
| go2rtc/WebRTC complesso | Protocollo semplice |

### Voice Assistant Reference

ESPHome Voice Assistant usa architettura simile con:
- Ring buffers FreeRTOS per decoupling
- Task separati per mic e speaker
- Counting semaphores per reference counting
- Event groups per comunicazione task→main loop

### Latenza Target

- Chunk size: 512 bytes = 16ms di audio
- Round-trip: **< 500ms RAGGIUNTO!**
- Buffer playback: 2-3 chunks = 32-48ms
