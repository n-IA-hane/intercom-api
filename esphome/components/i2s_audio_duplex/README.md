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
- **AEC Integration**: Built-in echo cancellation via `esp_aec` component, three reference modes:
  - **Ring buffer** (default): Works with any codec. Speaker audio is copied to a delay buffer as reference. Configure `aec_reference_delay_ms` to match your acoustic path (typically 60-100ms). `aec_reference_volume` scales the reference to match hardware DAC volume.
  - **ES8311 Digital Feedback** (recommended for ES8311): Stereo I2S with L=DAC ref, R=ADC mic. Sample-accurate reference, no delay tuning needed. Enable with `use_stereo_aec_reference: true`. The digital loopback is post-DSP-volume — no `aec_reference_volume` scaling needed.
  - **TDM Hardware Reference** (for ES7210 + ES8311): ES7210 in TDM mode captures DAC analog output on a dedicated ADC channel (e.g. MIC3). Sample-aligned with mic data, no ring buffer delay. Enable with `use_tdm_reference: true`. The analog reference already reflects hardware volume — no scaling needed.
- **Dual Mic Path**: `pre_aec` option for raw mic (MWW) alongside AEC-processed mic (VA/STT)
- **Volume Controls**: Mic gain, mic attenuation (pre-AEC), speaker volume, AEC reference volume
- **AEC Gating**: Auto-disables AEC when speaker is silent (prevents filter drift)
- **Reference Counting**: Multiple mic consumers share the I2S bus safely (MWW + VA + intercom)
- **CPU-Aware Scheduling**: `taskYIELD()` between frames for MWW inference headroom during AEC
- **Multi-Rate Support**: Run I2S bus at 48kHz for high-quality DAC output while mic/AEC/VA operate at 16kHz via internal FIR decimation
- **Configurable Reference Channel**: Choose left or right stereo channel as AEC reference (supports ES8311, ES8388, and other codecs)

## Architecture

```
                      ┌─ Stereo mode (ES8311): split L=ref, R=mic ─────────┐
                      │                                                     │
I2S RX ──────────────┤─ TDM mode (ES7210): deinterleave mic_slot, ref_slot ┤
                      │                                                     │
                      └─ Mono mode: mic only, ref from ring buffer          │
                                               │                           │
                             mic_attenuation (optional)                     │
                                               │                      spk_ref_buffer
                        ┌──────────────────────┤                           │
                        ▼                      ▼                           ▼
              raw_mic_callbacks          AEC process(mic, ref) ←──── reference
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
       │                    └──→ ref ring buffer (mono mode only)
  mixer (VA TTS + Intercom RX)
```

### Task Layout

| Task | Core | Priority | Role |
|------|------|----------|------|
| `i2s_duplex` (audio_task) | **Core 0** | **19** | I2S read/write + FIR decimation + AEC |
| `intercom_tx` | Core 0 | 5 | Mic→network + AEC during intercom calls |
| `intercom_spk` | Core 0 | 4 | Network→speaker, AEC reference feed |
| `intercom_srv` | Core 1 | 5 | TCP RX, call FSM (stays Core 1 for LVGL callback safety) |
| `mixer` (ESPHome) | Any | 10 | Mix VA + intercom audio to speaker |
| `MWW inference` (ESPHome) | Unpinned | 3 | Wake word TFLite micro inference |
| ESPHome main loop / LVGL | Core 1 | 1 | Switches, sensors, display, etc. |
| WiFi driver (ESP-IDF) | Core 0 | 23 | System — can briefly preempt audio_task |

**Core allocation rationale:**
- **Core 0**: All real-time audio (I2S + AEC + intercom processing). WiFi (prio 23) can briefly preempt `i2s_duplex` (prio 19) for sub-millisecond bursts — acceptable for 16ms frames with DMA buffering.
- **Core 1**: LVGL display rendering + ESPHome main loop + intercom TCP. MWW (unpinned, prio 3) naturally schedules here since Core 0 is occupied by audio tasks, giving wake word inference a dedicated core free from AEC interference.

**CPU budget** (256 samples @ 16kHz = 16ms per frame):
- Without AEC: ~300µs processing (< 2% of a core)
- With AEC active: ~7ms per frame (~42% of Core 0)
- `taskYIELD()` after each frame yields Core 0 to `intercom_tx` and `intercom_spk` (I2S DMA provides natural blocking)

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
| `sample_rate` | int | 16000 | I2S bus sample rate (8000-48000) |
| `output_sample_rate` | int | - | Mic/AEC output rate. If set, enables FIR decimation (must divide `sample_rate` evenly, max ratio 6) |
| `aec_id` | ID | - | Reference to `esp_aec` component for echo cancellation |
| `aec_reference_delay_ms` | int | 80 | AEC reference delay in ms for ring buffer mode (typically 60-100ms). Ignored when `use_stereo_aec_reference` is enabled. |
| `mic_attenuation` | float | 1.0 | Pre-AEC mic attenuation (0.01-1.0, for hot mics like ES8311) |
| `use_stereo_aec_reference` | bool | false | ES8311 digital feedback mode (see below) |
| `reference_channel` | string | left | Which stereo channel carries AEC reference: `left` or `right` |
| `use_tdm_reference` | bool | false | TDM hardware reference mode (ES7210, see below) |
| `tdm_total_slots` | int | 4 | Number of TDM slots (2-8) |
| `tdm_mic_slot` | int | 0 | TDM slot index for voice microphone |
| `tdm_ref_slot` | int | 1 | TDM slot index for AEC reference (e.g. MIC3 capturing DAC output) |

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
  use_stereo_aec_reference: true   # ES8311 only — reference is sample-aligned, no delay needed

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

The ESP-SR AEC (closed-source Espressif library) has a **fixed CPU cost per frame** regardless of `filter_length`. Costs vary significantly by mode (official ESP-SR benchmark on ESP32-S3):

| Mode | CPU Feed task | CPU Fetch task | Notes |
|------|--------------|----------------|-------|
| `sr_low_cost` | 8.4% | 15.0% | For speech recognition only (no speaker playing) |
| `sr_high_perf` | 9.4% | 14.9% | **AVOID on ESP32-S3**: exhausts DMA memory → SPI err 101 |
| `voip_low_cost` | ~60% | 8.2% | ✅ Recommended — full VoIP echo cancellation, least memory |
| `voip_high_perf` | ~64% | 8.2% | Full VoIP, slightly heavier — no benefit over low_cost on S3 |

**Use `voip_low_cost`** for any setup where the speaker plays while the mic is active (VA, intercom, radio). The SR modes are designed for noise suppression only (no active speaker) and have better CPU numbers but inadequate echo cancellation for full-duplex use.

| Metric | Value |
|--------|-------|
| Frame size | 256 samples (16ms at 16kHz) |
| AEC processing time | ~7ms avg, ~10ms peak |
| CPU per core | ~42% of Core 0 |
| `filter_length` impact on CPU | None (tested: 4 vs 8 = identical) |

**MWW + AEC coexistence**: With `i2s_duplex` on Core 0 (prio 19), MWW (unpinned, prio 3) naturally schedules to Core 1 where it has no competition from AEC. This achieves 10/10 wake word detection even during active TTS. The `taskYIELD()` in `audio_task` yields Core 0 to intercom tasks between frames.

### ES8311 Digital Feedback AEC (Recommended)

For **ES8311 codec**, enable `use_stereo_aec_reference` for **perfect echo cancellation**:

```yaml
i2s_audio_duplex:
  id: i2s_duplex
  # ... pins ...
  aec_id: aec_component
  use_stereo_aec_reference: true  # ES8311 digital feedback
```

**How it works:**
- ES8311 register 0x44 is configured to output DAC+ADC on ASDOUT as stereo
- L channel = DAC loopback (reference signal), R channel = ADC (microphone) — configurable via `reference_channel`
- Reference is **sample-accurate** (same I2S frame as mic) → best possible AEC
- `aec_reference_delay_ms` is **ignored** in this mode — the reference comes directly from the I2S RX deinterleave, not from the ring buffer

### TDM Hardware Reference (ES7210 + ES8311)

For boards with **ES7210** (multi-channel ADC) + **ES8311** (DAC), such as the Waveshare ESP32-S3-AUDIO-Board, the ES7210 can capture the ES8311 DAC analog output on a dedicated ADC channel. This provides a sample-aligned AEC reference without needing the ES8311 digital feedback mode.

```yaml
i2s_audio_duplex:
  id: i2s_duplex
  # ... pins ...
  aec_id: aec_component
  use_tdm_reference: true
  tdm_total_slots: 4       # ES7210 4-slot TDM
  tdm_mic_slot: 0           # Slot 0 = MIC1 (voice)
  tdm_ref_slot: 1           # Slot 1 = MIC3 (DAC feedback via analog loopback)
```

**How it works:**
- ES7210 operates in TDM mode with 4 interleaved slots per I2S frame
- MIC1 (slot 0) captures voice, MIC3 (slot 1) captures the ES8311 analog output
- I2S is configured as `I2S_SLOT_MODE_STEREO` with TDM slot mask so all slots appear in DMA
- The audio task deinterleaves mic and ref from the TDM frame — they are inherently sample-aligned
- No ring buffer delay, no speaker gating — the hardware ref is naturally silent when speaker is silent
- The analog reference already reflects DAC hardware volume — no `aec_reference_volume` scaling needed

**ES7210 TDM register configuration** (required in `on_boot` lambda):
```yaml
esphome:
  on_boot:
    priority: 200
    then:
      - lambda: |-
          // Enable ES7210 TDM mode + MIC3 for AEC reference
          uint8_t data[2];
          // Clock enable: clear bits [5:0] to enable all clocks
          data[0] = 0x01; data[1] = 0x00;
          id(i2c_bus).write(0x40, data, 2);
          // TDM mode enable
          data[0] = 0x12; data[1] = 0x02;
          id(i2c_bus).write(0x40, data, 2);
          // MIC3 gain 0dB (for clean reference)
          data[0] = 0x45; data[1] = 0x10;
          id(i2c_bus).write(0x40, data, 2);
```

> **Note**: `use_tdm_reference` and `use_stereo_aec_reference` are mutually exclusive. TDM mode uses `I2S_SLOT_MODE_STEREO` for the I2S channel (required to get all TDM slots in DMA).

### Multi-Rate: 48kHz I2S Bus with FIR Decimation

Many audio codecs (ES8311, ES7210, WM8960) operate **natively at 48kHz**. Running the I2S bus at 16kHz forces the codec's internal PLL to generate a non-standard clock, which often results in audible artifacts, worse SNR, and suboptimal DAC/ADC performance. At 48kHz the codec produces noticeably cleaner audio — lower noise floor, better high-frequency response for TTS and media playback.

The challenge: AEC (ESP-SR), Micro Wake Word (TFLite Micro), Voice Assistant STT, and intercom all require **16kHz** input. The solution is to run the I2S bus at 48kHz and internally decimate the mic path to 16kHz using a FIR anti-alias filter.

#### Signal Flow

```
                    ┌─── Speaker path ──────────────────────────────→ I2S TX (48kHz)
                    │    (native rate, no resampling)
I2S bus: 48kHz ─────┤
                    │    ┌─ FIR decimate ×3 ─┐
                    └─── Mic path (48kHz) ───┘──→ 16kHz ──→ AEC / MWW / VA / intercom
```

The FIR decimator uses a **31-tap lowpass filter** (Kaiser window β=8.0, cutoff 7.5kHz, ~35dB stopband attenuation (adequate for speech)) implemented in **float32** on the ESP32-S3 hardware FPU. It is applied separately to the mic channel and the AEC reference channel. CPU overhead at ratio=3 is approximately **0.5% of Core 0** per frame — negligible.

If `output_sample_rate` is omitted the decimation ratio is 1 and the FIR code is **completely bypassed** — zero overhead, fully backward compatible.

| Parameter | Value |
|-----------|-------|
| FIR taps | 31 |
| Window | Kaiser β=8.0 |
| Cutoff | 7.5kHz (below Nyquist @ 16kHz) |
| Stopband attenuation | ~60dB |
| Arithmetic | float32 (hardware FPU) |
| Supported ratios | 2, 3, 4, 5, 6 |
| CPU overhead (ratio=3) | ~0.5% of Core 0 per frame |
| Memory per decimator | 31 × 4 = 124 bytes delay line |

#### i2s_audio_duplex Config

```yaml
i2s_audio_duplex:
  id: i2s_duplex
  # ... pins ...
  sample_rate: 48000           # I2S bus rate — ES8311/ES7210 native, best DAC quality
  output_sample_rate: 16000    # Mic/AEC/MWW/VA decimated to 16kHz via FIR filter
  aec_id: aec_component
  use_stereo_aec_reference: true    # Reference from I2S RX stereo deinterleave (no delay needed)

esp_aec:
  id: aec_component
  sample_rate: 16000           # AEC always operates on 16kHz audio
  filter_length: 4
  mode: voip_low_cost
```

#### Speaker Path: ResamplerSpeaker + Mixer

Since the I2S bus runs at 48kHz the speaker must also receive 48kHz audio. ESPHome's `resampler` speaker platform transparently converts any input rate to the target rate:

```yaml
speaker:
  # Hardware output — writes 48kHz PCM to the I2S bus
  - platform: i2s_audio_duplex
    id: hw_speaker
    i2s_audio_duplex_id: i2s_duplex

  # Mixer combines VA TTS and intercom at 48kHz
  - platform: mixer
    id: audio_mixer
    output_speaker: hw_speaker
    num_channels: 1
    source_speakers:
      - id: va_speaker_mix
        timeout: 10s
      - id: intercom_speaker_mix
        timeout: 10s

  # ResamplerSpeakers: convert any input rate → 48kHz before the mixer
  - platform: resampler
    id: va_speaker               # VA TTS and media player output here
    output_speaker: va_speaker_mix

  - platform: resampler
    id: intercom_speaker         # 16kHz intercom RX upsampled → 48kHz
    output_speaker: intercom_speaker_mix
```

The `resampler` platform uses polyphase interpolation. For 16kHz→48kHz with default settings (`filters: 16, taps: 16`), CPU overhead on ESP32-S3 is approximately 2% of Core 1 during playback. If you see `[W] component took a long time` warnings for `resampler.speaker` you can try `filters: 8, taps: 8` to reduce CPU at a minimal quality cost, or `filters: 4, taps: 4` for minimal CPU.

#### How Home Assistant Knows to Send 48kHz

HA reads the `sample_rate` from the `announcement_pipeline` in the `media_player` config and transcodes audio accordingly via `ffmpeg_proxy`:

```yaml
media_player:
  - platform: speaker
    announcement_pipeline:
      speaker: va_speaker        # Points to the resampler speaker
      format: FLAC
      sample_rate: 48000         # HA will transcode TTS and media to FLAC 48kHz
      num_channels: 1
```

For TTS, HA requests the TTS engine at 48kHz directly. For radio/media streams, `ffmpeg_proxy` transcodes the source to FLAC 48kHz before sending it to the device. In both cases audio arrives at the ESP at 48kHz and goes to the speaker without any intermediate downsampling.

**Configure ES8311 register in on_boot:**
```yaml
esphome:
  on_boot:
    - lambda: |-
        uint8_t data[2] = {0x44, 0x48};  // ADCDAT_SEL = DACL+ADC (stereo AEC ref)
        id(i2c_bus).write(0x18, data, 2);
```

> **Note**: Without `use_stereo_aec_reference`, the component uses a ring buffer with configurable delay (`aec_reference_delay_ms`, default 80ms) for the AEC reference signal. The stereo mode eliminates timing issues and is strongly recommended for ES8311.

## Pin Mapping by Codec

### ES8311 (Xiaozhi Ball V3, AI Voice Kits)
```yaml
i2s_audio_duplex:
  i2s_lrclk_pin: GPIO45   # LRCK
  i2s_bclk_pin: GPIO9     # SCLK
  i2s_mclk_pin: GPIO16    # MCLK (required)
  i2s_din_pin: GPIO10     # SDOUT (codec → ESP)
  i2s_dout_pin: GPIO8     # SDIN (ESP → codec)
  sample_rate: 48000             # ES8311 native rate (better DAC quality)
  output_sample_rate: 16000      # Mic/AEC/MWW/VA at 16kHz (FIR decimation ×3)
  use_stereo_aec_reference: true # Digital feedback (recommended)
```

### ES8311 + ES7210 TDM (Waveshare ESP32-S3-AUDIO-Board)
```yaml
i2s_audio_duplex:
  i2s_lrclk_pin: GPIO14   # LRCK (shared bus)
  i2s_bclk_pin: GPIO13    # SCLK
  i2s_mclk_pin: GPIO12    # MCLK (required)
  i2s_din_pin: GPIO15     # ES7210 SDOUT (codec → ESP)
  i2s_dout_pin: GPIO16    # ES8311 SDIN (ESP → codec)
  sample_rate: 16000
  use_tdm_reference: true
  tdm_total_slots: 4
  tdm_mic_slot: 0          # MIC1 = voice
  tdm_ref_slot: 1          # MIC3 = DAC analog feedback
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
| VA + MWW + Intercom on same device | Yes (single bus) | Yes (dual bus with mixer speaker) |

## GPIO Mode Switching (VA + Intercom)

When combining Voice Assistant and Intercom on the same device, a single GPIO button can handle all actions using page-aware logic:

```yaml
binary_sensor:
  - platform: gpio
    pin:
      number: GPIO0
      inverted: true
    id: main_button
    on_click:
      - min_length: 0ms
        max_length: 500ms
        then:
          # Single click: action depends on current page
          - if:
              condition:
                # Timer ringing → stop timer (highest priority, any page)
                lambda: 'return id(voice_assist_phase) == 20;'
              then:
                - voice_assistant.stop:
          - if:
              condition:
                lvgl.page.is_showing: ic_idle_page
              then:
                - intercom_api.call_toggle:
                    id: intercom
          - if:
              condition:
                lvgl.page.is_showing: ic_ringing_in_page
              then:
                - intercom_api.answer_call:
                    id: intercom
          - if:
              condition:
                or:
                  - lvgl.page.is_showing: ic_ringing_out_page
                  - lvgl.page.is_showing: ic_in_call_page
              then:
                - intercom_api.call_toggle:
                    id: intercom
          # VA pages: start/stop voice assistant (default)
      - min_length: 500ms
        max_length: 1000ms
        then:
          # Double click: next contact (IC idle) or decline (IC ringing)
          - if:
              condition:
                lvgl.page.is_showing: ic_idle_page
              then:
                - intercom_api.next_contact:
                    id: intercom
          - if:
              condition:
                lvgl.page.is_showing: ic_ringing_in_page
              then:
                - intercom_api.decline_call:
                    id: intercom
    on_multi_click:
      - timing:
          - ON for at least 1s
        then:
          # Long press: switch between VA and Intercom modes
```

### Button Behavior Summary

| Current Page | Single Click | Double Click | Long Press |
|---|---|---|---|
| VA idle | Start voice assistant | — | Switch to intercom |
| VA active | Stop voice assistant | — | — |
| IC idle | Call selected contact | Next contact | Switch to VA |
| IC ringing in | Answer call | Decline call | Switch to VA |
| IC ringing out | Hangup | — | Switch to VA |
| IC in call | Hangup | — | Switch to VA |
| Timer ringing | Stop timer (any page) | — | — |

> **Note**: Wake word detection is ALWAYS active regardless of the current mode. Mode switching only affects the display and button behavior.

## Technical Notes

- **Sample Format**: 16-bit signed PCM, mono TX / stereo RX (ES8311 feedback mode)
- **DMA Buffers**: 8 buffers x 512 frames for smooth streaming (~256ms total)
- **Speaker Buffer**: 8192 bytes ring buffer (~256ms at 16kHz mono), scales with decimation ratio (24576 bytes at 48kHz)
- **Task Priority**: 19 (matches ESPHome stock i2s_audio speaker; above lwIP at 18, below WiFi at 23)
- **Core Affinity**: Pinned to Core 0 (canonical Espressif AEC pattern; frees Core 1 for MWW inference and LVGL)
- **AEC Gating**: Processes AEC only when speaker had real audio within last 250ms
- **Thread Safety**: All cross-thread flags use `std::atomic` with `memory_order_relaxed` (safe on ESP32-S3 cache-coherent architecture, avoids unnecessary MEMW fence instructions). Ring buffer resets use atomic request flags (`request_speaker_reset_`, `request_ref_prefill_`) to avoid concurrent access between main thread and audio task.

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

### TDM Mode: Audio Corruption (Whistle/Machine Gun Noise)
1. Ensure I2S uses `I2S_SLOT_MODE_STEREO` (not MONO) for TDM — MONO only puts slot 0 in DMA
2. Check ES7210 TDM register initialization (reg 0x12 = 0x02 for TDM mode)
3. Verify `tdm_total_slots` matches the actual ES7210 slot count
4. Check DMA buffer size — at 4 slots, `dma_frame_num` should be 256 (2048 bytes/descriptor, under 4092 limit)

### MWW Not Detecting During TTS
1. Use `pre_aec: true` microphone for MWW (raw mic, not AEC-processed) — AEC suppresses voice during TTS
2. Verify `i2s_duplex` task is on **Core 0** (prio 19). MWW (unpinned, prio 3) will then schedule to Core 1, away from AEC. With the wrong core assignment, MWW gets starved for 7ms out of every 16ms frame → ~1/10 detection rate.
3. Do NOT use `sr_high_perf` AEC mode — it causes SPI errors and the lighter CPU cost is not worth the loss of echo cancellation during TTS.

### Switches/Display Slow With AEC On
With `i2s_duplex` on **Core 0**, AEC no longer competes with LVGL/display on Core 1. This issue is resolved by correct core assignment. If you still see display slowness, check that no other high-priority task is pinned to Core 1.

### SPI Errors (err 101) With AEC
1. Use `mode: voip_low_cost` — `sr_high_perf` exhausts DMA memory on ESP32-S3
2. Reduce display update interval (500ms+) to avoid SPI bus contention
3. Check free heap in logs after boot

## Known Limitations

- **Media files should match bus sample rate**: For best quality, use media files at the bus `sample_rate` (e.g. 48kHz). The `resampler` speaker handles conversion from any rate, but native rate avoids resampling artifacts.
- **loopTask long-operation warnings during 48kHz streaming**: ESPHome reports `[W] mixer.speaker took a long time (110ms)` and similar warnings for `resampler.speaker`, `api`, `wifi` during audio playback. This is **expected and harmless** — these components run in loopTask (Core 1, prio 1) and process audio/network chunks that take >30ms. All real-time audio runs in dedicated tasks on Core 0 and is unaffected.
- **AEC is ESP-SR closed-source**: Cannot reset the adaptive filter without recreating the handle. Gating (timeout-based bypass when speaker has been silent for >250ms) is the workaround.
- **TDM analog reference vs ES8311 digital feedback**: The digital feedback path (ES8311 stereo loopback) provides a cleaner reference signal for AEC than the TDM analog path (ES7210 MIC3 capturing speaker output). Analog loopback introduces non-linear distortion from the DAC/amplifier chain that the AEC linear adaptive filter cannot fully model. Expect ~95-98% echo cancellation with analog reference vs ~99% with digital feedback. Both are adequate for voice assistant and intercom use.
- **AEC reference volume**: `aec_reference_volume` should ONLY be used with ring buffer mode (mono, no stereo/TDM). Both ES8311 digital feedback and TDM analog reference already include the hardware volume in the reference signal — applying additional scaling would double-attenuate and degrade AEC performance.

## License

MIT License
