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

## Features

- **True Full-Duplex**: Simultaneous mic input and speaker output on one I2S bus
- **Standard Platforms**: Exposes `microphone` and `speaker` platform classes (compatible with Voice Assistant, MWW, intercom_api)
- **AEC Integration**: Built-in echo cancellation via `esp_aec` component, two reference modes:
  - **Ring buffer** (default): Works with any codec. Speaker audio is copied to a delay buffer as reference. Configure `aec_reference_delay_ms` to match your acoustic path (typically 60-100ms).
  - **ES8311 Digital Feedback** (recommended for ES8311): Stereo I2S with L=DAC ref, R=ADC mic. Sample-accurate reference, no delay tuning needed. Enable with `use_stereo_aec_reference: true`.
- **Dual Mic Path**: `pre_aec` option for raw mic (MWW) alongside AEC-processed mic (VA/STT)
- **Volume Controls**: Mic gain, mic attenuation (pre-AEC), speaker volume, AEC reference volume
- **AEC Gating**: Auto-disables AEC when speaker is silent (prevents filter drift)
- **Reference Counting**: Multiple mic consumers share the I2S bus safely (MWW + VA + intercom)
- **CPU-Aware Scheduling**: `vTaskDelay` yield for MWW inference headroom during AEC

## Architecture

```
                      ┌─ Stereo mode (ES8311): split L=ref, R=mic
I2S RX (mic) ────────┤
                      └─ Mono mode: mic only, ref from ring buffer
                                               │
                             mic_attenuation (optional)
                                               │
                        ┌──────────────────────┤
                        ▼                      ▼
              raw_mic_callbacks          AEC process(mic, ref)
              (mic_raw, pre-AEC)              │
                   │                          ▼
                   ▼                    mic_callbacks
                  MWW                   (mic_aec, post-AEC)
                                             │
                                    ┌────────┴────────┐
                                    ▼                 ▼
                              Voice Assistant    Intercom TX

Speaker buffer ──→ volume scaling ──→ I2S TX (speaker)
       ▲                    │
       │                    └──→ ref ring buffer (mono mode AEC reference)
  mixer (VA TTS + Intercom RX)
```

### Task Layout

| Task | Core | Priority | Role |
|------|------|----------|------|
| `audio_task` | Core 1 | 9 | I2S read/write + AEC processing |
| `mixer` (ESPHome) | Any | 10 | Mix VA + intercom audio to speaker |
| `MWW inference` (ESPHome) | Any | 3 | Wake word TFLite micro inference |
| ESPHome main loop | Core 0 | 1 | Switches, sensors, display, etc. |

**CPU budget** (256 samples @ 16kHz = 16ms per frame):
- Without AEC: ~300µs processing (< 2% CPU)
- With AEC active: ~7ms processing (~42% of Core 1)
- `vTaskDelay(3)` after each frame yields 3ms to MWW and main loop

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
    components: [i2s_audio_duplex, esp_aec]
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

microphone:
  - platform: i2s_audio_duplex
    id: mic_component
    i2s_audio_duplex_id: i2s_duplex

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
| `aec_id` | ID | - | Reference to `esp_aec` component for echo cancellation |
| `aec_reference_delay_ms` | int | 80 | AEC reference delay in ms (10 for stereo feedback, 80 for ring buffer) |
| `mic_attenuation` | float | 1.0 | Pre-AEC mic attenuation (0.01-1.0, for hot mics like ES8311) |
| `use_stereo_aec_reference` | bool | false | ES8311 digital feedback mode (see below) |

### Microphone Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `pre_aec` | bool | false | If true, receives raw mic audio (before AEC). Use for MWW. |

### AEC with Voice Assistant + MWW

When using AEC with both Voice Assistant and Micro Wake Word, create two microphone instances:

```yaml
esp_aec:
  id: aec_component
  sample_rate: 16000
  filter_length: 4      # 64ms tail (4 for integrated codec, 8 for separate mic+speaker)
  mode: voip_low_cost    # Recommended for VA+MWW (same quality, lighter memory)
  # AVOID sr_high_perf: exhausts DMA memory, causes SPI errors on ESP32-S3

i2s_audio_duplex:
  id: i2s_duplex
  # ... pins ...
  aec_id: aec_component
  use_stereo_aec_reference: true   # ES8311 only
  aec_reference_delay_ms: 10       # Stereo feedback = sample-aligned

microphone:
  # Post-AEC: echo-cancelled audio for VA STT and intercom
  - platform: i2s_audio_duplex
    id: mic_aec
    i2s_audio_duplex_id: i2s_duplex

  # Pre-AEC: raw mic for wake word detection (hears voice through TTS echo)
  - platform: i2s_audio_duplex
    id: mic_raw
    i2s_audio_duplex_id: i2s_duplex
    pre_aec: true

micro_wake_word:
  microphone: mic_raw     # Raw mic: detects wake word even during TTS

voice_assistant:
  microphone: mic_aec     # AEC mic: clean STT without speaker echo
```

**Why two mics?** AEC suppresses all audio that correlates with the speaker reference — including your voice when you speak over TTS. MWW on post-AEC audio detects wake words only ~10% of the time during TTS. On raw mic, the neural model handles speaker echo much better than AEC-suppressed audio.

### AEC CPU Impact

The ESP-SR AEC (closed-source Espressif library) has a **fixed CPU cost per frame** regardless of `filter_length`:

| Metric | Value |
|--------|-------|
| Frame size | 256 samples (16ms at 16kHz) |
| AEC processing time | ~7ms avg, ~10ms peak |
| CPU per core | ~42% of Core 1 |
| `filter_length` impact on CPU | None (tested: 4 vs 8 = identical) |

The `audio_task` uses `vTaskDelay(3)` after each frame to yield 3ms of CPU to lower-priority tasks (MWW inference at priority 3, ESPHome main loop at priority 1). Without this yield, MWW cannot detect wake words during TTS and switches/display become unresponsive.

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
- Reference is **sample-accurate** (same I2S frame as mic) → best possible AEC

**Configure ES8311 register in on_boot:**
```yaml
esphome:
  on_boot:
    - lambda: |-
        uint8_t data[2] = {0x44, 0x48};  // ADCDAT_SEL = DACL+ADC
        id(i2c_bus).write(0x18, data, 2);
```

> **Note**: Without `use_stereo_aec_reference`, the component uses a ring buffer with configurable delay (`aec_reference_delay_ms`, default 80ms) for the reference signal. The stereo mode eliminates timing issues entirely.

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

## When to Use This vs Standard i2s_audio

| Scenario | Use This Component | Use Standard i2s_audio |
|----------|-------------------|----------------------|
| ES8311/ES8388/WM8960 codec | Yes | No (won't work properly) |
| Separate INMP441 + MAX98357A | No | Yes (two I2S buses) |
| PDM microphone + I2S speaker | No | Yes (different protocols) |
| Need true full-duplex on single bus | Yes | Limited |
| VA + MWW + Intercom on same device | Yes | Not possible |

## Technical Notes

- **Sample Format**: 16-bit signed PCM, mono TX / stereo RX (ES8311 feedback mode)
- **DMA Buffers**: 8 buffers x 512 frames for smooth streaming (~256ms total)
- **Speaker Buffer**: 8192 bytes ring buffer (~256ms at 16kHz mono)
- **Task Priority**: 9 (below WiFi/BLE at ~18, below mixer at 10)
- **Core Affinity**: Pinned to Core 1 to avoid WiFi interference on Core 0
- **AEC Gating**: Processes AEC only when speaker had real audio within last 250ms
- **Thread Safety**: `speaker_running_` and `aec_enabled_` are `std::atomic<bool>`, `mic_ref_count_` is `std::atomic<int>`

## Troubleshooting

### No Audio Output
1. Check MCLK connection (many codecs require it)
2. Verify codec I2C initialization (check logs)
3. Ensure speaker amp is enabled (GPIO control if applicable)

### Audio Crackling
1. Reduce sample_rate (try 8000 or 16000)
2. Check for WiFi interference (pin to Core 1)
3. Verify PSRAM is available if using AEC

### Echo During Calls
1. Enable AEC: set `aec_id` on `i2s_audio_duplex`
2. For ES8311: enable `use_stereo_aec_reference: true` + configure register 0x44
3. Adjust `filter_length` (4 for integrated codec, 8 for separate speaker)

### MWW Not Detecting During TTS
1. Use `pre_aec: true` microphone for MWW (raw mic, not AEC-processed)
2. AEC suppresses voice during TTS — MWW needs raw audio
3. Check `vTaskDelay` in audio_task (3ms minimum for MWW inference headroom)

### Switches/Display Slow With AEC On
1. AEC uses ~42% of Core 1 CPU. The `vTaskDelay(3)` in audio_task yields CPU to the main loop.
2. ESPHome mixer runs at priority 10 (unpinned) and can starve lower-priority tasks.
3. This is expected during heavy TTS playback with AEC active.

### SPI Errors (err 101) With AEC
1. Use `mode: voip_low_cost` — `sr_high_perf` exhausts DMA memory on ESP32-S3
2. Reduce display update interval (500ms+) to avoid SPI bus contention
3. Check free heap in logs after boot

## Known Limitations

- **Media files must match sample rate**: Files played through the ESPHome announcement pipeline (timer sounds, notifications via `media_player.speaker.play_on_device_media_file`) must be at the configured `sample_rate` (typically 16kHz). The mixer rejects incompatible streams with "Incompatible audio streams" error. Convert with: `ffmpeg -y -i input.flac -ar 16000 -ac 1 output.flac`
- **No runtime sample rate conversion**: A future improvement could add resampling in the mixer or speaker layer so media files at any sample rate are accepted without manual conversion.
- **AEC is ESP-SR closed-source**: Cannot reset the adaptive filter without recreating the handle. Gating (timeout-based bypass when speaker is silent) is the workaround.

## License

MIT License
