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
**Versione: 1.1.0** - AEC (Echo Cancellation) funzionante.

## Repository

- **Questo repo**: `/home/daniele/cc/claude/intercom-api/`
- **Legacy (non sviluppare)**: `/home/daniele/cc/claude/esphome-intercom/`

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
- **Queue-based**: 8 slot audio queue, drop if full (low latency > perfect audio)

### 3. Frontend: `intercom-card.js` (v1.0.0)
- **Lovelace card** custom
- **getUserMedia** + AudioWorklet (16kHz)
- **WebSocket** JSON + base64 audio verso HA
- **Scheduled playback** per bassa latenza

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

**Nota**: Il contatore pacchetti mostra ~4x più "received" che "sent" perché ESP invia chunk da 16ms mentre il browser invia chunk da 64ms. È normale.

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
│           └── number.py             # Volume entity (ENTITY_CATEGORY_CONFIG)
│
├── homeassistant/
│   └── custom_components/
│       └── intercom_native/
│           ├── __init__.py           # Integration setup
│           ├── manifest.json         # HA manifest (v1.0.0)
│           ├── config_flow.py        # Config UI
│           ├── websocket_api.py      # WS commands + session manager
│           ├── tcp_client.py         # Async TCP client
│           └── const.py              # Constants
│
├── frontend/
│   └── www/
│       ├── intercom-card.js          # Lovelace card (v1.0.0)
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

# Deploy HA integration + frontend
scp -r homeassistant/custom_components/intercom_native root@192.168.1.10:/home/homeassistant/.homeassistant/custom_components/
scp frontend/www/*.js root@192.168.1.10:/home/homeassistant/.homeassistant/www/
ssh root@192.168.1.10 'systemctl restart homeassistant'

# Monitor logs
ssh root@192.168.1.10 'journalctl -u homeassistant -f'
```

---

## Fix principali applicati (v1.0.1)

### ROOT CAUSE della latenza HA→ESP
In `server_task_()` c'era:
```cpp
ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
```
Questo bloccava **100ms per ogni iterazione** del loop, anche durante streaming attivo!

**Fix:**
```cpp
if (this->client_.streaming.load()) {
  ulTaskNotifyTake(pdTRUE, 0);  // Non-blocking durante streaming
} else {
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // Idle: risparmia CPU
}
```

### Ottimizzazioni applicate
1. `client_.socket` e `client_.streaming` sono `std::atomic` - thread safety senza mutex
2. Task priorities ottimizzate: server=7, TX=6, speaker=4
3. Speaker task su Core 0 (separa RX da audio output)
4. Speaker buffer 100ms (era 500ms)
5. HA: Queue-based audio (8 slot) invece di task per frame
6. HA: No ping durante streaming
7. Volume unico (template con restore_value) invece di due
8. **v1.0.1**: Mic gain control (0%-200%) - utile per effetto doppler

---

## TODO - Prossime Feature

Vedi `PLAN-BROKER.md` per il piano completo del broker.

- [x] **Fase 1**: Echo Cancellation (esp_aec) ✅ v1.1.0
- [x] **Fase 2A**: HA Broker Server ✅ v1.2.0-wip
- [x] **Fase 2B**: ESP Broker Client ✅ v1.2.0-wip
- [ ] **Fase 3**: Card refactor (device picker, dropdown contatti)

## Fase 2: HA Broker (IMPLEMENTATO)

**Architettura:** ESP si connette a HA (outbound), HA fa da relay audio.
- Funziona attraverso NAT/firewall (ESP fa solo connessioni uscenti)
- Broker opzionale: senza config broker = sistema attuale (1 ESP)
- Con broker: supporto multi-ESP, ESP remoti, chiamate ESP↔ESP

**Protocollo Broker (porta 6060):**
```
Header: 12 bytes (type, flags, length, call_id, seq)
Messaggi: REGISTER, INVITE, RING, ANSWER, DECLINE, HANGUP, BYE, AUDIO, CONTACTS, ERROR
```

**File implementati:**
- `homeassistant/custom_components/intercom_native/broker.py` - HA Broker Server
- `homeassistant/custom_components/intercom_native/const.py` - Broker constants
- `esphome/components/intercom_api/intercom_protocol.h` - Broker protocol
- `esphome/components/intercom_api/intercom_api.cpp` - Broker client task
- `esphome/components/intercom_api/__init__.py` - Broker YAML config

**Config YAML:**
```yaml
intercom_api:
  id: intercom
  microphone: mic_component
  speaker: spk_component
  broker:              # Opzionale - senza questa sezione funziona come prima
    host: "192.168.1.10"
    port: 6060
    device_name: "Intercom Cucina"  # Opzionale, usa nome ESPHome se omesso
```

**API ESP (broker mode):**
```cpp
// Chiamare un altro ESP
id(intercom).broker_call("intercom-mini-2");

// Rispondere a chiamata in arrivo
id(intercom).broker_answer();

// Rifiutare chiamata
id(intercom).broker_decline();

// Terminare chiamata
id(intercom).broker_hangup();

// Stato chiamata
id(intercom).get_call_state_str();  // "Idle", "Calling", "Ringing", "In Call"

// Lista contatti
size_t n = id(intercom).get_contact_count();
const char* name = id(intercom).get_contact_name(0);
bool busy = id(intercom).is_contact_busy(0);
```

**Drop policy:** Queue 10 frame (~160ms), drop oldest se piena.

## TODO - Priorità Bassa

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

### Latenza

- ESP→Browser chunk: 16ms
- Browser→ESP chunk: 64ms
- Round-trip totale: **< 500ms**
