# Intercom API Component

ESPHome component for bidirectional full-duplex audio streaming with Home Assistant.

## Overview

The `intercom_api` component creates a TCP server on port 6054 that handles audio streaming with Home Assistant. It integrates with ESPHome's standard `microphone` and `speaker` platforms and provides a complete state machine for call management.

## Features

- **TCP Server** on port 6054 for audio streaming
- **FreeRTOS Tasks** for non-blocking audio processing
- **Finite State Machine** for call states (Idle → Ringing → Streaming)
- **AEC Integration** via `esp_aec` component
- **Contact Management** for ESP↔ESP calls (Full mode)
- **Persistent Settings** saved to flash
- **ESPHome Native Platforms** for switches, numbers, sensors

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      intercom_api Component                      │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐ │
│  │   server_task   │  │    tx_task      │  │  speaker_task   │ │
│  │   (Core 1, p7)  │  │   (Core 0, p6)  │  │   (Core 0, p4)  │ │
│  │                 │  │                 │  │                 │ │
│  │ • TCP accept    │  │ • mic_buffer_   │  │ • speaker_buf   │ │
│  │ • RX handling   │  │ • AEC process   │  │ • I2S write     │ │
│  │ • Protocol FSM  │  │ • TCP send      │  │ • Scheduled     │ │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘ │
│           │                    │                    │          │
│           ▼                    ▼                    ▼          │
│  ┌─────────────────────────────────────────────────────────────┐│
│  │                    Ring Buffers                             ││
│  │  mic_buffer_ (8KB)  │  spk_ref_buffer_ (8KB)  │  speaker_  ││
│  │  mic→network        │  speaker ref for AEC    │  net→spk   ││
│  └─────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼ TCP :6054
                    ┌─────────────────┐
                    │  Home Assistant │
                    │  (TCP Client)   │
                    └─────────────────┘
```

## Configuration

### Basic Configuration

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/intercom-api
      ref: main
    components: [intercom_api, esp_aec]

intercom_api:
  id: intercom
  microphone: mic_component
  speaker: spk_component
```

### Full Configuration

```yaml
intercom_api:
  id: intercom
  mode: full                  # simple or full
  microphone: mic_component
  speaker: spk_component
  aec_id: aec_processor       # Optional: echo cancellation
  dc_offset_removal: true     # For mics with DC bias
  ringing_timeout: 30s        # Auto-decline timeout

  # Event callbacks
  on_outgoing_call:
    - logger.log: "Outgoing call"
  on_ringing:
    - logger.log: "Ringing"
  on_answered:
    - logger.log: "Answered"
  on_streaming:
    - logger.log: "Streaming started"
  on_idle:
    - logger.log: "Idle"
  on_hangup:
    - logger.log:
        format: "Hangup: %s"
        args: ['reason.c_str()']
  on_call_failed:
    - logger.log:
        format: "Failed: %s"
        args: ['reason.c_str()']
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `id` | ID | Required | Component ID for referencing |
| `mode` | string | `simple` | Operating mode: `simple` or `full` |
| `microphone` | ID | Required | Reference to microphone component |
| `speaker` | ID | Required | Reference to speaker component |
| `aec_id` | ID | - | Reference to esp_aec component |

| `dc_offset_removal` | bool | false | Remove DC offset from mic signal |
| `ringing_timeout` | time | 0s | Auto-decline after timeout (0 = disabled) |

## Operating Modes

### Simple Mode (`mode: simple`)

- Browser ↔ HA ↔ ESP communication only
- No contact management
- Creates only `intercom_state` sensor
- Minimal resource usage

### Full Mode (`mode: full`)

- ESP ↔ HA ↔ ESP communication
- Contact list with navigation
- Creates additional sensors: `destination`, `caller`, `contacts`
- Auto-bridge detection by HA

## State Machine

```
                    ┌──────────────────────────┐
                    │                          │
                    ▼                          │
              ┌──────────┐                     │
              │   IDLE   │◄────────────────────┤
              └────┬─────┘                     │
                   │                           │
        ┌──────────┼──────────┐                │
        │          │          │                │
   START (ring)  START     start()             │
        │       (no_ring)     │                │
        ▼          │          ▼                │
   ┌─────────┐     │    ┌──────────┐           │
   │ RINGING │     │    │ OUTGOING │           │
   └────┬────┘     │    └────┬─────┘           │
        │          │         │                 │
  answer_call()    │    PONG (answered)        │
        │          │         │                 │
        ▼          ▼         ▼                 │
      ┌────────────────────────┐               │
      │       STREAMING        │───stop()──────┘
      │  (bidirectional audio) │
      └────────────────────────┘
```

### States

| State | Description | Triggers |
|-------|-------------|----------|
| `Idle` | No active call | Initial, after hangup |
| `Ringing` | Incoming call waiting | START message with ring flag |
| `Outgoing` | Outgoing call waiting | `start()` called |
| `Streaming` | Active audio call | Answer or auto-answer |

## Platform Entities

### Switch Platform

```yaml
switch:
  - platform: intercom_api
    intercom_api_id: intercom
    auto_answer:
      id: auto_answer_switch
      name: "Auto Answer"
      restore_mode: RESTORE_DEFAULT_OFF
    aec:
      id: aec_switch
      name: "Echo Cancellation"
      restore_mode: RESTORE_DEFAULT_ON
```

### Number Platform

```yaml
number:
  - platform: intercom_api
    intercom_api_id: intercom
    speaker_volume:
      id: speaker_volume
      name: "Speaker Volume"
      # Range: 0-100%
    mic_gain:
      id: mic_gain
      name: "Mic Gain"
      # Range: -20 to +30 dB
```

> **Note**: When `i2s_audio_duplex` is also present, `i2s_audio_duplex` owns the `mic_gain` and `speaker_volume` number entities with full dB-scale control and persistence. In that case, `intercom_api`'s number entities serve as **fallback only** for non-duplex setups (e.g., ESP32-S3 Mini with separate I2S buses). A `FINAL_VALIDATE_SCHEMA` at compile time prevents conflicts by detecting when both components try to own the same functionality (dual AEC, dual DC offset).

## Actions

Use these actions in automations or lambdas:

### intercom_api.start

Start an outgoing call to the currently selected contact.

```yaml
button:
  - platform: template
    name: "Call"
    on_press:
      - intercom_api.start:
          id: intercom
```

### intercom_api.stop

Hangup the current call.

```yaml
- intercom_api.stop:
    id: intercom
```

### intercom_api.answer_call

Answer an incoming call (when auto_answer is OFF).

```yaml
- intercom_api.answer_call:
    id: intercom
```

### intercom_api.decline_call

Decline an incoming call.

```yaml
- intercom_api.decline_call:
    id: intercom
```

### intercom_api.call_toggle

Smart action: idle→call, ringing→answer, streaming→hangup.

```yaml
- intercom_api.call_toggle:
    id: intercom
```

### intercom_api.next_contact / prev_contact

Navigate contact list (Full mode only).

```yaml
- intercom_api.next_contact:
    id: intercom

- intercom_api.prev_contact:
    id: intercom
```

### intercom_api.set_contact

Select a specific contact by name and optionally start a call. This is the key building block for **GPIO-triggered calls** — each button can call a different device.

```yaml
- intercom_api.set_contact:
    id: intercom
    contact: "Waveshare S3 Audio"
```

> **Important: Name matching is exact (case-sensitive).** The `contact` value must match the device name exactly as it appears in the contacts list. The contacts list is populated from the `name:` substitution in each device's YAML. For example, if the target device has `substitutions: name: waveshare-s3-audio`, then the contact name in Home Assistant will be `Waveshare S3 Audio` (HA converts hyphens to spaces and capitalizes words). Always verify the exact name in the `sensor.{name}_destination` entity.

#### Example: Multi-Button Intercom (Apartment Doorbell)

Each GPIO button calls a different room — like a condominium intercom panel:

```yaml
binary_sensor:
  # Button 1: Call Kitchen
  - platform: gpio
    pin:
      number: GPIO4
      mode: INPUT_PULLUP
      inverted: true
    on_press:
      - intercom_api.set_contact:
          id: intercom
          contact: "Kitchen Intercom"
      - intercom_api.start:
          id: intercom

  # Button 2: Call Living Room
  - platform: gpio
    pin:
      number: GPIO5
      mode: INPUT_PULLUP
      inverted: true
    on_press:
      - intercom_api.set_contact:
          id: intercom
          contact: "Living Room Intercom"
      - intercom_api.start:
          id: intercom

  # Button 3: Call Bedroom
  - platform: gpio
    pin:
      number: GPIO6
      mode: INPUT_PULLUP
      inverted: true
    on_press:
      - intercom_api.set_contact:
          id: intercom
          contact: "Bedroom Intercom"
      - intercom_api.start:
          id: intercom
```

Each device name must match the target device's YAML `substitutions.name` exactly as displayed in HA. For example:
- YAML: `name: kitchen-intercom` → HA device name: `Kitchen Intercom` → contact: `"Kitchen Intercom"`
- YAML: `name: waveshare-s3-audio` → HA device name: `Waveshare S3 Audio` → contact: `"Waveshare S3 Audio"`

If `set_contact` fails to find the name, it fires `on_call_failed` with `"Contact not found: <name>"`.

### intercom_api.set_contacts

Update contact list from CSV string. Normally handled automatically by the `sensor.intercom_active_devices` sensor.

```yaml
text_sensor:
  - platform: homeassistant
    entity_id: sensor.intercom_active_devices
    on_value:
      - intercom_api.set_contacts:
          id: intercom
          contacts_csv: !lambda 'return x;'
```

## Conditions

Use in `if:` blocks:

```yaml
- if:
    condition:
      intercom_api.is_idle:
        id: intercom
    then:
      - logger.log: "Intercom is idle"
```

| Condition | True when |
|-----------|-----------|
| `is_idle` | State is Idle |
| `is_ringing` | State is Ringing |
| `is_calling` | State is Outgoing |
| `is_in_call` | State is Streaming |
| `is_incoming` | Has pending incoming call |

## Lambda API

Access component methods from lambdas:

```cpp
// Get current state
const char* state = id(intercom).get_state_str();
// Returns: "Idle", "Ringing", "Outgoing", "Streaming", etc.

// Get current destination (Full mode)
std::string dest = id(intercom).get_current_destination();

// Get caller name (during incoming call)
std::string caller = id(intercom).get_caller();

// Get contacts as CSV
std::string contacts = id(intercom).get_contacts_csv();

// Control methods
id(intercom).start();
id(intercom).stop();
id(intercom).answer_call();
id(intercom).set_volume(0.8f);        // 0.0 - 1.0
id(intercom).set_mic_gain_db(6.0f);   // -20 to +30
id(intercom).set_aec_enabled(true);
id(intercom).set_auto_answer(false);
```

## TCP Protocol

### Message Format

```
┌──────────────┬──────────────┬──────────────────────┬─────────────┐
│ Type (1 byte)│ Flags (1 byte)│ Length (2 bytes LE) │ Payload     │
└──────────────┴──────────────┴──────────────────────┴─────────────┘
```

### Message Types

| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| AUDIO | 0x01 | Both | PCM audio data |
| START | 0x02 | Client→Server | Start call (payload: caller_name) |
| STOP | 0x03 | Both | End call |
| PING | 0x04 | Both | Keep-alive |
| PONG | 0x05 | Both | Keep-alive response / Answer |
| ERROR | 0x06 | Server→Client | Error notification |

### START Message Flags

| Flag | Value | Description |
|------|-------|-------------|
| NO_RING | 0x01 | Don't ring, auto-answer immediately |

### Audio Format

- Sample rate: 16000 Hz
- Bit depth: 16-bit signed PCM
- Channels: Mono
- Chunk size: 512 bytes (256 samples = 16ms)

## Auto-created Sensors

The component automatically creates these text sensors:

### Always created

| Sensor | Entity ID | Values |
|--------|-----------|--------|
| Intercom State | `sensor.{name}_intercom_state` | Idle, Ringing, Outgoing, Streaming |

### Full mode only

| Sensor | Entity ID | Description |
|--------|-----------|-------------|
| Destination | `sensor.{name}_destination` | Selected contact |
| Caller | `sensor.{name}_caller` | Incoming caller name |
| Contacts | `sensor.{name}_contacts` | Contact count |

## Entity State Publishing

Entities publish their state when the ESPHome API connects. Use `on_client_connected` to ensure values are visible in Home Assistant:

```yaml
api:
  on_client_connected:
    - lambda: 'id(intercom).publish_entity_states();'
```

## Hardware Requirements

- **ESP32-S3** or **ESP32-P4** with PSRAM (required for AEC)
- I2S microphone component
- I2S speaker component
- ESP-IDF framework

## Memory Usage

| Component | Approximate RAM |
|-----------|-----------------|
| mic_buffer_ | 8 KB |
| spk_ref_buffer_ | 8 KB |
| speaker_buffer_ | 3.2 KB |
| FreeRTOS tasks | ~12 KB |
| AEC (if enabled) | ~80 KB |

## FreeRTOS Task Configuration

| Task | Core | Priority | Stack | Notes |
|------|------|----------|-------|-------|
| server_task | 1 | 5 | 8192 | Always created. Handles TCP RX, call FSM, YAML callbacks. |
| tx_task | 0 | 5 | 12288 | **Only created when `aec_id` is set on `intercom_api`**. Mic→network + AEC. |
| speaker_task | 0 | 4 | 8192 | **Only created when `aec_id` is set on `intercom_api`**. Network→speaker, AEC ref. |

> **Task elimination**: When `intercom_api` does NOT have its own `aec_id` (the standard case — AEC is handled by `i2s_audio_duplex`), `tx_task` and `speaker_task` are NOT created. The server_task handles TX inline and plays audio directly via `speaker_->play()`. This saves ~32KB of internal RAM (12KB tx_task stack + 8KB speaker_task stack + 8KB speaker_buffer + 2KB audio_tx_buffer + 2KB spk_ref_scaled + semaphore). The `largest_free_block` jumps from ~12.8KB to ~25KB.

## Troubleshooting

### "Connection refused" on port 6054

- Check `CONFIG_LWIP_MAX_SOCKETS` is increased (default 10 → 16)
- Verify no other service uses port 6054

### Audio glitches

- Ensure PSRAM is enabled for AEC
- Check WiFi signal strength
- Reduce log level to WARN

### AEC not working

- Verify `aec_id` is linked
- Check `esp_aec` component is configured
- Ensure AEC switch is ON

### State stuck in "Ringing"

- Check `ringing_timeout` is set
- Verify `auto_answer` setting
- Look for connection errors in logs

## Example: Button Control

```yaml
binary_sensor:
  - platform: gpio
    pin:
      number: GPIO0
      mode: INPUT_PULLUP
      inverted: true
    on_multi_click:
      # Single click: call/answer/hangup
      - timing:
          - ON for 50ms to 500ms
          - OFF for at least 400ms
        then:
          - intercom_api.call_toggle:
              id: intercom
      # Double click: next contact
      - timing:
          - ON for 50ms to 500ms
          - OFF for 50ms to 400ms
          - ON for 50ms to 500ms
        then:
          - intercom_api.next_contact:
              id: intercom
```

## License

MIT License
