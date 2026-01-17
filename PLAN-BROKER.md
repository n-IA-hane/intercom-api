# Piano: HA Broker per ESP↔ESP Communication

## Architettura

**Principio chiave**: ESP si connette A HA (outbound), NON HA si connette a ESP.
Questo permette di funzionare attraverso NAT/firewall.

```
┌─────────────┐                    ┌─────────────┐
│   ESP-A     │───────────────────►│     HA      │◄───────────────────│   ESP-B     │
│  (porta 6060 outbound)           │   BROKER    │  (porta 6060 outbound)
│             │◄───────────────────│  (porta 6060)│───────────────────►│             │
│             │  audio relay       │             │  audio relay        │             │
└─────────────┘                    └─────────────┘                    └─────────────┘
       │                                  │                                  │
       │ REGISTER on boot                 │ maintains device registry        │
       │ INVITE to call                   │ routes calls between ESPs        │
       │ AUDIO during call                │ relays audio bidirectionally     │
       └──────────────────────────────────┴──────────────────────────────────┘
```

### Vantaggi
- **NAT traversal**: ESP fa solo connessioni uscenti, funziona dietro router
- **Centralizzato**: HA ha visibilità completa di tutti i dispositivi
- **Opzionale**: Senza config broker, ESP funziona come prima (1 ESP ↔ browser)
- **Scalabile**: Aggiungere nuovi ESP = registrazione automatica

---

## Protocollo Broker (Porta 6060)

### Header (12 bytes)

```
┌────────────┬────────────┬────────────────────┬────────────────────┬────────────────────┐
│ Type (1B)  │ Flags (1B) │ Length (2B LE)     │ Call ID (4B LE)    │ Sequence (4B LE)   │
└────────────┴────────────┴────────────────────┴────────────────────┴────────────────────┘
```

### Message Types

| Type | Name      | Direction         | Payload |
|------|-----------|-------------------|---------|
| 0x10 | REGISTER  | ESP→HA            | device_id (string, null-terminated) |
| 0x11 | INVITE    | ESP→HA            | target_device_id (string) |
| 0x12 | RING      | HA→ESP            | caller_device_id (string) |
| 0x13 | ANSWER    | ESP→HA            | - |
| 0x14 | DECLINE   | ESP→HA            | reason (1 byte): 0=busy, 1=reject |
| 0x15 | HANGUP    | Both              | - |
| 0x16 | BYE       | HA→ESP            | - (call terminated by peer) |
| 0x17 | AUDIO     | Both              | PCM 16-bit mono 16kHz |
| 0x18 | CONTACTS  | HA→ESP            | JSON array of available devices |
| 0x19 | PING      | Both              | - |
| 0x1A | PONG      | Both              | - |
| 0x1B | ERROR     | HA→ESP            | error_code (1 byte) |

### Error Codes

| Code | Meaning |
|------|---------|
| 0x01 | Device not found |
| 0x02 | Device busy |
| 0x03 | Call timeout |
| 0x04 | Protocol error |

### Call ID
- Generato da HA quando riceve INVITE
- Usato per identificare la chiamata in corso
- 0 = nessuna chiamata attiva

### Sequence Number
- Incrementato per ogni pacchetto AUDIO
- Usato per rilevare packet loss (non retransmit, solo logging)
- 0 per messaggi di controllo

---

## State Machine ESP

```
                         ┌─────────────────────────────────────┐
                         │                                     │
                         ▼                                     │
                   ┌───────────┐                               │
       ┌──────────►│   IDLE    │◄──────────────────────────────┤
       │           └─────┬─────┘                               │
       │                 │                                     │
       │    ┌────────────┼────────────┐                       │
       │    │ INVITE     │            │ RING (incoming)       │
       │    ▼            │            ▼                       │
  ┌─────────────┐        │      ┌─────────────┐               │
  │  CALLING    │        │      │  RINGING    │               │
  │ (wait answer)        │      │ (wait user) │               │
  └──────┬──────┘        │      └──────┬──────┘               │
         │ ANSWER        │             │ user answers         │
         │               │             │ (send ANSWER)        │
         ▼               │             ▼                       │
  ┌──────────────────────┴──────────────────────┐              │
  │                  IN_CALL                    │──────────────┘
  │         (bidirectional audio)              │  HANGUP/BYE
  └─────────────────────────────────────────────┘
```

### Transizioni
- `IDLE` → `CALLING`: utente preme Call (send INVITE)
- `IDLE` → `RINGING`: riceve RING
- `CALLING` → `IN_CALL`: riceve ANSWER
- `CALLING` → `IDLE`: timeout (30s) o riceve DECLINE
- `RINGING` → `IN_CALL`: utente preme Answer (send ANSWER)
- `RINGING` → `IDLE`: timeout (30s) o utente preme Decline
- `IN_CALL` → `IDLE`: utente preme Hangup o riceve BYE

---

## Implementazione

### Fase 2A: HA Broker Server

File: `homeassistant/custom_components/intercom_native/broker.py`

```python
class IntercomBroker:
    """TCP broker for ESP↔ESP communication."""

    def __init__(self, hass, port=6060):
        self.hass = hass
        self.port = port
        self.server = None
        self.devices: Dict[str, DeviceConnection] = {}  # device_id -> connection
        self.calls: Dict[int, Call] = {}  # call_id -> Call
        self.next_call_id = 1

    async def start(self):
        """Start TCP server on port 6060."""
        self.server = await asyncio.start_server(
            self._handle_connection,
            '0.0.0.0', self.port
        )

    async def _handle_connection(self, reader, writer):
        """Handle new ESP connection."""
        device = DeviceConnection(reader, writer)
        try:
            while True:
                header = await reader.readexactly(12)
                msg_type, flags, length, call_id, seq = struct.unpack('<BBHII', header)
                payload = await reader.readexactly(length) if length > 0 else b''
                await self._handle_message(device, msg_type, flags, call_id, seq, payload)
        except asyncio.IncompleteReadError:
            await self._handle_disconnect(device)

    async def _handle_message(self, device, msg_type, flags, call_id, seq, payload):
        if msg_type == MSG_REGISTER:
            device.device_id = payload.decode('utf-8').rstrip('\x00')
            self.devices[device.device_id] = device
            await self._send_contacts(device)

        elif msg_type == MSG_INVITE:
            target_id = payload.decode('utf-8').rstrip('\x00')
            await self._initiate_call(device, target_id)

        elif msg_type == MSG_ANSWER:
            await self._handle_answer(device, call_id)

        elif msg_type == MSG_AUDIO:
            await self._relay_audio(device, call_id, payload)

        # ... etc

    async def _relay_audio(self, device, call_id, audio_data):
        """Relay audio to the other party."""
        call = self.calls.get(call_id)
        if not call:
            return

        # Find peer
        peer = call.callee if device == call.caller else call.caller

        # Enqueue audio (drop oldest if full)
        if len(peer.audio_queue) >= 10:
            peer.audio_queue.popleft()  # Drop oldest
        peer.audio_queue.append(audio_data)

        # Notify TX task
        peer.audio_event.set()

@dataclass
class DeviceConnection:
    reader: asyncio.StreamReader
    writer: asyncio.StreamWriter
    device_id: str = ""
    audio_queue: deque = field(default_factory=lambda: deque(maxlen=10))
    audio_event: asyncio.Event = field(default_factory=asyncio.Event)
    current_call_id: int = 0

@dataclass
class Call:
    call_id: int
    caller: DeviceConnection
    callee: DeviceConnection
    state: str = "RINGING"  # RINGING, IN_CALL
```

### Fase 2B: ESP Broker Client

File: `esphome/components/intercom_api/intercom_api.cpp`

Aggiungere modalità broker client:

```cpp
// In setup() - se broker configurato
if (!this->broker_host_.empty()) {
    // Crea task per connessione outbound a HA
    xTaskCreatePinnedToCore(
        IntercomApi::broker_task, "broker",
        8192, this, 5, &this->broker_task_handle_, 0
    );
}

void IntercomApi::broker_task_() {
    while (true) {
        // Connetti a broker
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(this->broker_port_);
        inet_pton(AF_INET, this->broker_host_.c_str(), &addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            this->broker_socket_ = sock;

            // Invia REGISTER
            this->send_broker_message_(MSG_REGISTER, 0, 0,
                (uint8_t*)this->device_id_.c_str(), this->device_id_.length() + 1);

            // Loop ricezione messaggi
            while (this->broker_connected_) {
                // ... handle messages
            }
        }

        // Riconnetti dopo 5 secondi
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

### Fase 2C: Config YAML

```yaml
intercom_api:
  id: intercom
  microphone: mic_component
  speaker: spk_component

  # Broker opzionale - senza questa sezione funziona come prima
  broker:
    host: "192.168.1.10"  # IP di Home Assistant
    port: 6060
    device_name: "Intercom Cucina"  # Nome mostrato agli altri ESP
```

---

## Audio Flow (durante chiamata)

```
ESP-A (mic)                    HA Broker                    ESP-B (speaker)
    │                              │                              │
    │  AUDIO (call_id, seq, pcm)   │                              │
    │─────────────────────────────►│                              │
    │                              │  AUDIO (call_id, seq, pcm)   │
    │                              │─────────────────────────────►│
    │                              │                              │
    │                              │  AUDIO (call_id, seq, pcm)   │
    │◄─────────────────────────────│                              │
    │  AUDIO (call_id, seq, pcm)   │                              │
    │◄─────────────────────────────│◄─────────────────────────────│
    │                              │                              │
```

### Drop Policy
- Queue: 10 frame (~160ms a 16ms/frame)
- Se queue piena: drop oldest (tail drop è peggio per latenza)
- No retransmit: real-time audio, meglio dropout che delay

### Performance
- `writer.write()` senza await per ogni frame
- `await writer.drain()` ogni 10 frame (batch)
- Questo evita context switch per ogni pacchetto audio

---

## Deliverables

### Fase 2A: HA Broker Server
- [ ] `broker.py` con TCP server
- [ ] Device registry (REGISTER/disconnect)
- [ ] Call manager (INVITE/RING/ANSWER/HANGUP)
- [ ] Audio relay con drop policy
- [ ] CONTACTS broadcast quando device si connette/disconnette
- [ ] Integration con `__init__.py` per startup automatico

### Fase 2B: ESP Broker Client
- [ ] `broker_task_()` per connessione outbound
- [ ] State machine per chiamate
- [ ] REGISTER automatico al boot
- [ ] Gestione RING/BYE/CONTACTS
- [ ] Riconnessione automatica se broker disconnette

### Fase 2C: Config e Testing
- [ ] Schema YAML per broker config
- [ ] Test: ESP-A chiama ESP-B via broker
- [ ] Test: Hangup da entrambi i lati
- [ ] Test: Chiamata mentre già in chiamata (busy)

---

## Note

### Backwards Compatibility
Il broker è **completamente opzionale**:
- Senza `broker:` in YAML → funziona come prima (1 ESP ↔ browser via TCP 6054)
- Con `broker:` → ESP si connette anche a HA:6060 per chiamate ESP↔ESP

### Browser Calls
Le chiamate browser ↔ ESP continuano a usare il sistema attuale (WebSocket via HA).
Il broker è solo per ESP↔ESP.

### Latenza Attesa
- ESP → HA: ~5-10ms (LAN)
- HA processing: ~1-2ms
- HA → ESP: ~5-10ms (LAN)
- **Totale broker overhead**: ~15-25ms

Comparato con direct ESP↔ESP (che non funzionerebbe attraverso NAT),
questo overhead è accettabile.
