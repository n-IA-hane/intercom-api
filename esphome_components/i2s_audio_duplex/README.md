# I2S Audio Duplex - Full-Duplex I2S for ESPHome

True simultaneous microphone and speaker operation on a single I2S bus for audio codecs.

## Why This Component?

Standard ESPHome `i2s_audio` creates separate I2S instances for microphone and speaker, which works for setups with separate I2S buses. However, audio codecs like **ES8311, ES8388, WM8960** use a single I2S bus for both input and output simultaneously.

```
Without i2s_audio_duplex:
  Mic and Speaker fight for I2S bus → Audio glitches, half-duplex only

With i2s_audio_duplex:
  Single I2S controller handles both directions → True full-duplex
```

## Key Feature: Standard Platform Classes

This component exposes **standard ESPHome `microphone` and `speaker` platform classes**:

```yaml
microphone:
  - platform: i2s_audio_duplex
    id: mic_component
    i2s_audio_duplex_id: i2s_duplex

speaker:
  - platform: i2s_audio_duplex
    id: spk_component
    i2s_audio_duplex_id: i2s_duplex
```

This means:
- **Compatible with intercom_api** - Works seamlessly with the intercom system
- **Standard interfaces** - Exposes the same classes used by Voice Assistant and other ESPHome audio components
- **Simultaneous operation** - Both platforms share the same I2S bus without conflicts

### Future: Voice Assistant Coexistence

Since both platforms follow ESPHome standards, this opens the door to future testing of running Voice Assistant and Intercom on the same device:

```yaml
# Theoretical - untested, may require state management
voice_assistant:
  microphone: mic_component
  speaker: spk_component

intercom_api:
  microphone: mic_component
  speaker: spk_component
```

> **Note**: This is a future development goal, not a currently tested feature. Simultaneous operation would require proper state management between components.

## Features

- **True Full-Duplex**: Simultaneous mic input and speaker output
- **Standard Platforms**: Exposes `microphone` and `speaker` platform classes
- **Single I2S Bus**: Efficient use of hardware resources
- **Volume Control**: Software gain for speaker output
- **Hardware Optimized**: Uses ESP-IDF native I2S drivers
- **Intercom Compatible**: Works seamlessly with `intercom_api` component

## Use Cases

- **Audio Codec Devices**: ES8311, ES8388, WM8960, MAX98090, etc.
- **Intercom Systems**: Full-duplex conversation
- **Voice Assistants**: Listen while providing audio feedback
- **Video Conferencing**: Simultaneous talk and listen

## Requirements

- **ESP32** or **ESP32-S3** (tested on S3)
- Audio codec with shared I2S bus (ES8311 recommended)
- ESP-IDF framework

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/intercom-api
      ref: main
    components: [i2s_audio_duplex]
    path: esphome_components
```

## Configuration

### Basic Setup

```yaml
i2s_audio_duplex:
  id: i2s_duplex
  i2s_lrclk_pin: GPIO45      # Word Select (WS/LRCLK)
  i2s_bclk_pin: GPIO9        # Bit Clock (BCK/BCLK)
  i2s_mclk_pin: GPIO16       # Master Clock (optional, some codecs need it)
  i2s_din_pin: GPIO10        # Data In (from codec ADC → ESP mic)
  i2s_dout_pin: GPIO8        # Data Out (from ESP → codec DAC speaker)
  sample_rate: 16000
  # NOTE: AEC should be configured in intercom_api, NOT here

# Standard microphone platform
microphone:
  - platform: i2s_audio_duplex
    id: mic_component
    i2s_audio_duplex_id: i2s_duplex

# Standard speaker platform
speaker:
  - platform: i2s_audio_duplex
    id: spk_component
    i2s_audio_duplex_id: i2s_duplex
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `id` | ID | Required | Component ID |
| `i2s_lrclk_pin` | pin | Required | Word Select / LR Clock pin |
| `i2s_bclk_pin` | pin | Required | Bit Clock pin |
| `i2s_mclk_pin` | pin | -1 | Master Clock pin (if codec requires) |
| `i2s_din_pin` | pin | -1 | Data input from codec (microphone) |
| `i2s_dout_pin` | pin | -1 | Data output to codec (speaker) |
| `sample_rate` | int | 16000 | Audio sample rate (8000-48000) |
| `aec_id` | ID | - | Reference to esp_aec component for echo cancellation |
| `aec_reference_delay_ms` | int | 80 | AEC reference delay (10-200ms) |
| `mic_attenuation` | float | 1.0 | Pre-AEC mic attenuation (0.01-1.0, for hot mics) |
| `use_stereo_aec_reference` | bool | false | ES8311 digital feedback mode (see below) |

### ES8311 Digital Feedback AEC (Recommended)

For **ES8311 codec**, enable `use_stereo_aec_reference` for **perfect echo cancellation**:

```yaml
i2s_audio_duplex:
  id: i2s_duplex
  # ... pins ...
  aec_id: aec_component
  use_stereo_aec_reference: true  # ES8311 digital feedback
  aec_reference_delay_ms: 10      # Minimal delay (sample-aligned)
```

**How it works:**
- ES8311 register 0x44 is configured to output DAC+ADC on ASDOUT as stereo
- L channel = DAC loopback (reference signal)
- R channel = ADC (microphone)
- Reference is **sample-accurate** (same I2S frame as mic) → perfect AEC

**Configure ES8311 register in on_boot:**
```yaml
esphome:
  on_boot:
    - lambda: |-
        uint8_t data[2] = {0x44, 0x48};  // ADCDAT_SEL = DACL+ADC
        id(i2c_bus).write(0x18, data, 2);
```

> **Note**: Without `use_stereo_aec_reference`, the component uses a ring buffer for reference which has timing issues. The stereo mode eliminates these problems entirely.

## Pin Mapping by Codec

### ES8311 (Xiaozhi Ball, AI Voice Kits)
```yaml
i2s_audio_duplex:
  i2s_lrclk_pin: GPIO45   # LRCK
  i2s_bclk_pin: GPIO9     # SCLK
  i2s_mclk_pin: GPIO16    # MCLK (required)
  i2s_din_pin: GPIO10     # SDOUT (codec → ESP)
  i2s_dout_pin: GPIO8     # SDIN (ESP → codec)
  sample_rate: 16000
```

### ES8388 (LyraT, Audio Dev Boards)
```yaml
i2s_audio_duplex:
  i2s_lrclk_pin: GPIO25   # LRCK
  i2s_bclk_pin: GPIO5     # SCLK
  i2s_mclk_pin: GPIO0     # MCLK (required)
  i2s_din_pin: GPIO35     # DOUT
  i2s_dout_pin: GPIO26    # DIN
  sample_rate: 16000
```

### WM8960 (Various Dev Boards)
```yaml
i2s_audio_duplex:
  i2s_lrclk_pin: GPIO4    # LRCLK
  i2s_bclk_pin: GPIO5     # BCLK
  i2s_mclk_pin: GPIO0     # MCLK (required)
  i2s_din_pin: GPIO35     # ADCDAT
  i2s_dout_pin: GPIO25    # DACDAT
  sample_rate: 16000
```

## Complete Example with Intercom

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/intercom-api
      ref: main
    components: [i2s_audio_duplex, intercom_api, esp_aec]
    path: esphome_components

i2c:
  sda: GPIO15
  scl: GPIO14

audio_dac:
  - platform: es8311
    id: codec_dac
    bits_per_sample: 16bit
    sample_rate: 16000

esp_aec:
  id: aec
  sample_rate: 16000
  filter_length: 4

i2s_audio_duplex:
  id: i2s_duplex
  i2s_lrclk_pin: GPIO45
  i2s_bclk_pin: GPIO9
  i2s_mclk_pin: GPIO16
  i2s_din_pin: GPIO10
  i2s_dout_pin: GPIO8
  sample_rate: 16000
  # NOTE: AEC is handled by intercom_api, NOT here

microphone:
  - platform: i2s_audio_duplex
    id: mic_component
    i2s_audio_duplex_id: i2s_duplex

speaker:
  - platform: i2s_audio_duplex
    id: spk_component
    i2s_audio_duplex_id: i2s_duplex

intercom_api:
  id: intercom
  microphone: mic_component
  speaker: spk_component
  aec_id: aec  # AEC configured HERE, not in i2s_audio_duplex
```

## When to Use This vs Standard i2s_audio

| Scenario | Use This Component | Use Standard i2s_audio |
|----------|-------------------|----------------------|
| ES8311/ES8388/WM8960 codec | Yes | No (won't work properly) |
| Separate INMP441 + MAX98357A | No | Yes (two I2S buses) |
| PDM microphone + I2S speaker | No | Yes (different protocols) |
| Need true full-duplex on single bus | Yes | Limited |

## Technical Notes

- **Sample Format**: 16-bit stereo (32 bits per frame)
- **DMA Buffers**: 8 buffers x 1024 bytes for smooth streaming
- **Task Priority**: 9 (below WiFi/BLE at 18)
- **Core Affinity**: Pinned to Core 1 to avoid WiFi interference

## Troubleshooting

### No Audio Output
1. Check MCLK connection (many codecs require it)
2. Verify codec I2C initialization (check logs)
3. Ensure speaker amp is enabled (GPIO control if applicable)

### Audio Crackling
1. Reduce sample_rate (try 8000 or 16000)
2. Check for WiFi interference (pin to Core 1)
3. Verify PSRAM is working if using AEC

### Echo Issues
1. Configure AEC in `intercom_api` component via `aec_id`
2. Increase AEC filter_length (4-6 recommended)
3. Ensure `esp_aec` component is properly set up

### Mic Not Working
1. Check din_pin wiring
2. Verify codec ADC is configured via I2C
3. Try increasing mic_gain

## Comparison with Standard ESPHome

| Feature | i2s_audio_duplex | Standard i2s_audio |
|---------|-----------------|-------------------|
| Single-bus codecs | Full support | Half-duplex only |
| Standard mic/speaker class | Yes | Yes |
| Simultaneous I/O | Native | Shared bus conflicts |
| Intercom API compatible | Yes | Yes (separate buses only) |
| Voice Assistant compatible | Untested | Yes |

## License

MIT License
