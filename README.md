# ESPHome Intercom API

A bidirectional full-duplex audio intercom system for ESP32 devices integrated with Home Assistant.

![Dashboard Preview](readme-img/dashboard.png)

## Overview

**Intercom API** is an ESPHome component that enables real-time bidirectional audio streaming between ESP32 devices and Home Assistant. Unlike traditional approaches that require complex WebRTC/go2rtc setups, this component uses a simple TCP protocol that works seamlessly with Home Assistant's existing infrastructure.

### Why This Project?

This component was born from the interest shown by users of [esphome-intercom](https://github.com/n-IA-hane/esphome-intercom), which remains the best approach for direct ESP-to-ESP communication over the network. However, integrating that system with Home Assistant required go2rtc and WebRTC cards, which made setup and usage frustrating. Additionally, WebRTC often failed in remote access scenarios due to NAT traversal issues - a common problem when trying to use the intercom from outside your home network.

**Intercom API** takes a different approach:
- Uses ESPHome's native API for control (port 6053)
- Opens a dedicated TCP socket for audio streaming (port 6054)
- Similar architecture to how Voice Assistant works in ESPHome
- No WebRTC, no go2rtc, no port forwarding required
- **Works flawlessly remotely** - Since audio streams through Home Assistant's WebSocket API, remote access via browser or Companion App works out of the box (through Nabu Casa, reverse proxy, or any existing HA remote access setup)

### How It Works

```
┌─────────────┐         ┌─────────────┐         ┌─────────────┐
│   Browser   │◄──WS───►│ Home Assist │◄──TCP──►│    ESP32    │
│  (Card UI)  │         │   (Relay)   │  6054   │  (Intercom) │
└─────────────┘         └─────────────┘         └─────────────┘
```

**Audio Flow:**
1. **Browser → ESP**: Microphone audio captured via Web Audio API (16kHz mono) → WebSocket to HA → TCP to ESP → Speaker output
2. **ESP → Browser**: Microphone input → TCP to HA → WebSocket event → Scheduled playback in browser

Audio is processed in 16ms chunks on the ESP side and 64ms chunks on the browser side, achieving sub-500ms round-trip latency.

---

## Features

- **Full-duplex audio** - Talk and listen simultaneously
- **Echo Cancellation (AEC)** - Built-in acoustic echo cancellation using ESP-SR
- **Auto Answer** - Configurable automatic call acceptance
- **Volume Control** - Adjustable speaker volume and microphone gain
- **Status LED** - Visual feedback for call states (idle, ringing, streaming)
- **Home Assistant Integration** - Custom Lovelace card with device discovery
- **Multiple Hardware Support** - Works with various I2S microphones and speakers

---

## Installation

### 1. ESPHome Component

Add the external component to your ESPHome configuration:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/intercom-api
      ref: main
    components: [intercom_api, esp_aec]
    path: esphome_components
```

Or for local development:

```yaml
external_components:
  - source:
      type: local
      path: esphome_components
    components: [intercom_api, esp_aec]
```

### 2. Basic ESPHome Configuration

```yaml
# I2S Audio setup (example with separate mic/speaker buses)
i2s_audio:
  - id: i2s_mic_bus
    i2s_lrclk_pin: GPIO3
    i2s_bclk_pin: GPIO2
  - id: i2s_spk_bus
    i2s_lrclk_pin: GPIO6
    i2s_bclk_pin: GPIO7

microphone:
  - platform: i2s_audio
    id: mic_component
    i2s_audio_id: i2s_mic_bus
    i2s_din_pin: GPIO4
    adc_type: external
    pdm: false
    bits_per_sample: 32bit
    sample_rate: 16000
    channel: left

speaker:
  - platform: i2s_audio
    id: spk_component
    i2s_audio_id: i2s_spk_bus
    i2s_dout_pin: GPIO8
    dac_type: external
    sample_rate: 16000
    bits_per_sample: 16bit

# Echo Cancellation (optional but recommended)
esp_aec:
  id: aec_processor
  sample_rate: 16000
  filter_length: 4
  mode: voip_low_cost

# Intercom API component
intercom_api:
  id: intercom
  microphone: mic_component
  speaker: spk_component
  aec_id: aec_processor  # Optional: link to AEC component
```

### 3. Home Assistant Integration

Copy the custom component to your Home Assistant configuration:

```bash
# Copy integration
cp -r homeassistant/custom_components/intercom_native /config/custom_components/

# Copy card files
cp frontend/www/intercom-card.js /config/www/
cp frontend/www/intercom-processor.js /config/www/
```

Add the card resource to your Lovelace configuration:

```yaml
# In configuration.yaml or via UI
lovelace:
  resources:
    - url: /local/intercom-card.js
      type: module
```

Restart Home Assistant.

### 4. Add the Card

Add the Intercom Card to your dashboard:

![Card Configuration](readme-img/card-configuration.png)

The card automatically discovers ESPHome devices with the `intercom_api` component.

> **Important**: Devices must first be added to Home Assistant via the ESPHome integration (autodiscovery or manually) before they appear in the card's device list.

![ESPHome Add Device](readme-img/esphome-add-device.png)

---

## API Reference

### Component Configuration

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `id` | ID | Required | Component ID for referencing |
| `microphone` | ID | Required | Reference to microphone component |
| `speaker` | ID | Required | Reference to speaker component |
| `mic_bits` | int | 16 | Microphone bit depth (16 or 32) |
| `dc_offset_removal` | bool | false | Enable DC offset removal (for mics like SPH0645) |
| `aec_id` | ID | - | Optional: Reference to esp_aec component |

### Runtime Methods

Control the intercom from lambdas or automations:

```cpp
// Start a call (connect to Home Assistant)
id(intercom).start();

// Stop/hangup the current call
id(intercom).stop();

// Answer an incoming call (when auto_answer is OFF)
id(intercom).answer_call();

// Set speaker volume (0.0 to 1.0)
id(intercom).set_volume(0.8);

// Set microphone gain in dB (-20 to +20)
id(intercom).set_mic_gain_db(6.0);

// Enable/disable echo cancellation
id(intercom).set_aec_enabled(true);

// Enable/disable auto answer
// NOTE: Default is ON if you don't declare the switch
id(intercom).set_auto_answer(true);
```

### Exposed Entities

The component exposes these entities to Home Assistant:

| Entity | Type | Description |
|--------|------|-------------|
| `Intercom State` | Text Sensor | Current state: Idle, Ringing, Streaming |
| `Speaker Volume` | Number | Speaker volume (0-100%) |
| `Mic Gain` | Number | Microphone gain (-20 to +20 dB) |
| `Echo Cancellation` | Switch | Enable/disable AEC |
| `Auto Answer` | Switch | Auto-accept incoming calls |
| `Call` | Button | Start/answer/hangup calls |

### Auto Answer Behavior

By default, `auto_answer` is **enabled**. This means:
- When Home Assistant connects, the ESP immediately starts streaming
- No user interaction required on the ESP side

If you want the ESP to "ring" and wait for user confirmation:

```yaml
switch:
  - platform: template
    id: auto_answer_switch
    name: "Auto Answer"
    icon: "mdi:phone-incoming"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_OFF  # Start with auto_answer disabled
    turn_on_action:
      - lambda: 'id(intercom).set_auto_answer(true);'
    turn_off_action:
      - lambda: 'id(intercom).set_auto_answer(false);'
```

---

## Example Dashboard

Here's a complete dashboard configuration with two intercom devices:

```yaml
title: Intercom
views:
  - title: Intercom
    icon: mdi:phone-voip
    cards: []
    type: sections
    max_columns: 2
    sections:
      - type: grid
        cards:
          - type: custom:intercom-card
            entity_id: 862f3e10eb2d2d63160d393d23474fac
            name: Intercom Mini
          - type: entities
            entities:
              - entity: number.intercom_mini_speaker_volume
                name: Volume
              - entity: number.intercom_mini_mic_gain
                name: Mic gain
              - entity: switch.intercom_mini_echo_cancellation
              - entity: switch.intercom_mini_auto_answer
      - type: grid
        cards:
          - type: custom:intercom-card
            name: Intercom
            entity_id: df18a94e7c6ebcb84b183ac7c081805d
          - type: entities
            entities:
              - entity: number.intercom_xiaozhi_speaker_volume
                name: Volume
              - entity: number.intercom_xiaozhi_mic_gain
                name: Mic gain
              - entity: switch.intercom_xiaozhi_echo_cancellation
              - entity: switch.intercom_xiaozhi_auto_answer
```

---

## Supported Hardware

### Tested Configurations

| Device | Microphone | Speaker | I2S Mode |
|--------|------------|---------|----------|
| ESP32-S3 Mini | SPH0645 | MAX98357A | Dual bus (standard `i2s_audio`) |
| Xiaozhi Ball V3 | ES8311 | ES8311 | Single bus (`i2s_audio_duplex`) |

### Requirements

- ESP32-S3 with PSRAM (recommended for AEC)
- I2S microphone (INMP441, SPH0645, ES8311, etc.)
- I2S speaker amplifier (MAX98357A, ES8311, etc.)

---

## Single-Bus Audio Codecs (i2s_audio_duplex)

Many integrated audio codecs like **ES8311, ES8388, WM8960** share a single I2S bus for both microphone and speaker. The standard ESPHome `i2s_audio` component **cannot handle this** - it creates separate I2S instances that conflict with each other.

**The `i2s_audio_duplex` component solves this problem** by managing both directions on a single I2S controller.

### When You NEED i2s_audio_duplex

| Hardware Setup | Component to Use |
|----------------|------------------|
| Separate mic + speaker (INMP441 + MAX98357A) | Standard `i2s_audio` |
| ES8311 codec (Xiaozhi Ball, AI kits) | **`i2s_audio_duplex`** (required) |
| ES8388 codec (LyraT boards) | **`i2s_audio_duplex`** (required) |
| WM8960 codec | **`i2s_audio_duplex`** (required) |

### Example: Dual Bus (Standard i2s_audio)

For devices with separate I2S buses (like ESP32-S3 Mini with INMP441 + MAX98357A):

```yaml
# Two separate I2S buses - use standard ESPHome components
i2s_audio:
  - id: i2s_mic_bus
    i2s_lrclk_pin: GPIO3
    i2s_bclk_pin: GPIO2
  - id: i2s_spk_bus
    i2s_lrclk_pin: GPIO6
    i2s_bclk_pin: GPIO7

microphone:
  - platform: i2s_audio
    id: mic_component
    i2s_audio_id: i2s_mic_bus
    i2s_din_pin: GPIO4
    # ... other config

speaker:
  - platform: i2s_audio
    id: spk_component
    i2s_audio_id: i2s_spk_bus
    i2s_dout_pin: GPIO8
    # ... other config
```

### Example: Single Bus (i2s_audio_duplex)

For devices with integrated codecs (like Xiaozhi Ball V3 with ES8311):

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/intercom-api
      ref: main
    components: [intercom_api, i2s_audio_duplex, esp_aec]
    path: esphome_components

# Single I2S bus - MUST use i2s_audio_duplex
i2s_audio_duplex:
  id: i2s_duplex
  i2s_lrclk_pin: GPIO45
  i2s_bclk_pin: GPIO9
  i2s_mclk_pin: GPIO16    # Many codecs require MCLK!
  i2s_din_pin: GPIO10     # Mic data (codec → ESP)
  i2s_dout_pin: GPIO8     # Speaker data (ESP → codec)
  sample_rate: 16000

# Standard microphone platform (from i2s_audio_duplex)
microphone:
  - platform: i2s_audio_duplex
    id: mic_component
    i2s_audio_duplex_id: i2s_duplex

# Standard speaker platform (from i2s_audio_duplex)
speaker:
  - platform: i2s_audio_duplex
    id: spk_component
    i2s_audio_duplex_id: i2s_duplex

# Intercom uses standard mic/speaker interfaces
intercom_api:
  id: intercom
  microphone: mic_component
  speaker: spk_component
```

### Future: Voice Assistant Compatibility

The `i2s_audio_duplex` component exposes **standard ESPHome `microphone` and `speaker` platform classes**, the same interfaces used by Voice Assistant. This opens the door to future testing of coexistence between the two components on the same device.

```yaml
# Theoretical example - untested, may require state management
voice_assistant:
  microphone: mic_component
  speaker: spk_component

intercom_api:
  microphone: mic_component
  speaker: spk_component
```

> **Note**: This is a future development goal, not a currently tested feature.

See the [i2s_audio_duplex README](esphome_components/i2s_audio_duplex/README.md) for detailed configuration options.

---

## Roadmap

### Current Version (v1.0.0)
- P2P mode: Browser ↔ Home Assistant ↔ ESP

### Planned Features

**PTMP Mode (Point-to-MultiPoint)**
- ESP-to-ESP calls through Home Assistant relay
- PBX-like intercom functionality
- Contact lists and call routing

**Voice Assistant Integration**
- Voice commands: "Call kitchen", "Call Home Assistant"
- Integration with existing Voice Assistant pipelines

---

## Troubleshooting

### No audio from ESP
- Check speaker wiring and I2S pin configuration
- Verify `speaker_enable` GPIO if your amp has an enable pin
- Check volume level (default 80%)

### Echo or feedback
- Enable AEC: `id(intercom).set_aec_enabled(true);`
- Reduce speaker volume
- Increase physical distance between mic and speaker

### High latency
- Ensure good WiFi signal (check `WiFi Signal` sensor)
- Reduce buffer sizes if acceptable audio quality
- Check Home Assistant logs for queue overflows

### Card doesn't show devices
- Verify the ESP has `intercom_api` component with `text_sensor`
- Check that ESPHome integration is connected in HA
- Clear browser cache and reload

---

## License

MIT License - See [LICENSE](LICENSE) for details.

---

## Contributing

Contributions are welcome! Please open an issue or pull request on GitHub.

## Credits

Developed with the help of the ESPHome and Home Assistant communities, and [Claude Code](https://claude.ai/code) as AI pair programming assistant.
