# AEC Implementation Plan - Intercom API

## Research Summary

### ESP-SR AEC API (esp_aec.h)

**API Functions:**
```c
// Create AEC instance
aec_handle_t *aec_create(int sample_rate, int filter_length, int channel_num, aec_mode_t mode);

// Process one frame (512 samples = 32ms at 16kHz)
void aec_process(const aec_handle_t *handle, int16_t *indata, int16_t *refdata, int16_t *outdata);

// Get frame size in samples
int aec_get_chunksize(const aec_handle_t *handle);

// Destroy instance
void aec_destroy(aec_handle_t *handle);
```

**Parameters:**
| Parameter | Value | Notes |
|-----------|-------|-------|
| Sample Rate | 16000 Hz | Only supported rate |
| Frame Length | 32ms | AEC_FRAME_LENGTH_MS constant |
| Frame Size | 512 samples | 32ms × 16kHz |
| Filter Length | 4 | Recommended for ESP32-S3 |
| Mode | AEC_MODE_VOIP_LOW_COST | For voice communication |

**Memory Requirements:**
- Buffers MUST be 16-byte aligned: `heap_caps_aligned_alloc(16, n, size, caps)`
- Full AFE: ~22% CPU, 48KB SRAM, 1.1MB PSRAM
- AEC alone: significantly less (estimated ~10% CPU, ~20KB)

### Sources:
- [ESP-SR Audio Front-End Documentation](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html)
- [ESP-SR GitHub](https://github.com/espressif/esp-sr)
- [ESP AFE Solutions](https://www.espressif.com/en/solutions/audio-solutions/esp-afe)
- [ESP32 FreeRTOS Task Management](https://controllerstech.com/esp32-freertos-task-priority-stack-management/)

---

## Current Architecture Analysis

### Existing Tasks in intercom_api:
| Task | Core | Stack | Priority | Function |
|------|------|-------|----------|----------|
| server_task | 1 | 4KB | 7 | TCP RX, highest priority |
| tx_task | 0 | 4KB | 6 | Mic→Network |
| speaker_task | 0 | 8KB | 4 | Network→Speaker |

### Existing Buffers:
| Buffer | Size | Purpose |
|--------|------|---------|
| mic_buffer_ | 2KB (TX_BUFFER_SIZE) | ~64ms mic data for TX |
| speaker_buffer_ | 8KB (RX_BUFFER_SIZE) | ~256ms speaker data |

### ESPHome RingBuffer Pattern:
```cpp
// Creation
std::unique_ptr<RingBuffer> buf = RingBuffer::create(size_bytes);

// Non-blocking read
size_t read = buf->read(data, len, 0);  // 0 = don't wait

// Write (overwrites if full)
size_t written = buf->write(data, len);

// Thread-safe via FreeRTOS xRingbuffer internally
```

---

## Problem Analysis: Why Previous Implementation Failed

### Root Cause: Stack Overflow
The mic callback runs in ESPHome's `mic_task` which has only **4KB stack**.
Calling `aec_process()` directly from the callback caused stack overflow because:
1. `aec_process()` uses FFT internally (requires stack for temporary arrays)
2. Mic callback already uses stack for sample conversion
3. Combined usage exceeded 4KB

### Solution: Dedicated AEC Task
Process AEC in a separate FreeRTOS task with sufficient stack (8KB+).

---

## Proposed Architecture

### Data Flow Diagram:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              MIC PATH                                    │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   ┌───────────────┐     ┌───────────────┐     ┌───────────────┐         │
│   │  mic_callback │────►│ mic_raw_buf   │────►│   aec_task    │         │
│   │  (mic_task)   │     │ (4KB ~128ms)  │     │ (Core 1, 8KB) │         │
│   │   4KB stack   │     │   NEW         │     │  Priority 5   │         │
│   └───────────────┘     └───────────────┘     └───────┬───────┘         │
│         │                                             │                  │
│         │ (32→16bit, DC removal, gain)                │ aec_process()    │
│         │                                             │                  │
│         │                                             ▼                  │
│         │              ┌───────────────┐     ┌───────────────┐          │
│         │              │  mic_buffer   │◄────│  AEC output   │          │
│         │              │ (2KB, existing)│     │  (or bypass)  │          │
│         │              └───────┬───────┘     └───────────────┘          │
│         │                      │                                         │
│         │                      ▼                                         │
│         │              ┌───────────────┐                                 │
│         │              │   tx_task     │────► TCP to HA                  │
│         │              │ (Core 0, 4KB) │                                 │
│         │              │  Priority 6   │                                 │
│         │              └───────────────┘                                 │
│         │                                                                │
└─────────┼────────────────────────────────────────────────────────────────┘
          │
          │ If AEC disabled: mic_callback writes directly to mic_buffer_
          │
┌─────────┼────────────────────────────────────────────────────────────────┐
│         │                    SPEAKER PATH                                │
├─────────┼────────────────────────────────────────────────────────────────┤
│         │                                                                │
│   ┌─────▼─────────┐                                                      │
│   │ server_task   │ (TCP RX)                                             │
│   │ (Core 1, 4KB) │                                                      │
│   │  Priority 7   │                                                      │
│   └───────┬───────┘                                                      │
│           │                                                              │
│           ├─────────────────────┐                                        │
│           │                     │                                        │
│           ▼                     ▼                                        │
│   ┌───────────────┐     ┌───────────────┐                               │
│   │speaker_buffer │     │speaker_ref_buf│ (NEW - AEC reference)          │
│   │ (8KB existing)│     │ (4KB ~128ms)  │                               │
│   └───────┬───────┘     └───────┬───────┘                               │
│           │                     │                                        │
│           ▼                     │                                        │
│   ┌───────────────┐             │                                        │
│   │ speaker_task  │             │                                        │
│   │ (Core 0, 8KB) │             │                                        │
│   │  Priority 4   │             │                                        │
│   └───────────────┘             │                                        │
│                                 │                                        │
│                                 ▼                                        │
│                         ┌───────────────┐                               │
│                         │   aec_task    │ (reads speaker reference)      │
│                         └───────────────┘                               │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

### Task Configuration:

| Task | Core | Stack | Priority | Notes |
|------|------|-------|----------|-------|
| server_task | 1 | 4KB | 7 | TCP RX - highest |
| tx_task | 0 | 4KB | 6 | Mic TX - high |
| **aec_task** | **1** | **8KB** | **5** | **NEW - AEC processing** |
| speaker_task | 0 | 8KB | 4 | Speaker output - lower |

**Priority Rationale:**
- server_task (7): Must receive TCP data immediately
- tx_task (6): Low-latency mic transmission
- aec_task (5): Process AEC frames as they arrive
- speaker_task (4): Can tolerate slight delays, has buffer

**Core Assignment:**
- Core 0: tx_task + speaker_task (audio I/O)
- Core 1: server_task + aec_task (network + processing)

### Buffer Configuration:

| Buffer | Size | Duration | Purpose |
|--------|------|----------|---------|
| mic_raw_buffer_ | 4KB | ~128ms | NEW - Mic callback output |
| mic_buffer_ | 2KB | ~64ms | AEC output → TX |
| speaker_buffer_ | 8KB | ~256ms | Existing - playback |
| speaker_ref_buffer_ | 4KB | ~128ms | NEW - AEC reference |
| aec_mic_buffer_ | 1KB | 32ms | Frame accumulator (aligned) |
| aec_ref_buffer_ | 1KB | 32ms | Reference frame (aligned) |
| aec_out_buffer_ | 1KB | 32ms | Output frame (aligned) |

---

## Implementation Details

### 1. ESP_AEC Component (esphome/components/esp_aec/)

Simple wrapper around ESP-SR AEC API:

```cpp
// esp_aec.h
class EspAec : public Component {
 public:
  void setup() override;
  void dump_config() override;

  // Configuration
  void set_filter_length(int length) { filter_length_ = length; }
  void set_mode(aec_mode_t mode) { mode_ = mode; }

  // API
  bool is_initialized() const { return handle_ != nullptr; }
  int get_frame_size() const;  // Returns 512 for 32ms @ 16kHz
  void process(const int16_t *mic, const int16_t *ref, int16_t *out);

 protected:
  aec_handle_t *handle_{nullptr};
  int filter_length_{4};
  aec_mode_t mode_{AEC_MODE_VOIP_LOW_COST};
};
```

### 2. AEC Task Implementation (in intercom_api.cpp)

```cpp
void IntercomApi::aec_task_(void *arg) {
  auto *this_ = static_cast<IntercomApi *>(arg);
  const int frame_size = this_->aec_->get_frame_size();  // 512 samples
  const size_t frame_bytes = frame_size * sizeof(int16_t);  // 1024 bytes

  // Aligned buffers for AEC
  int16_t *mic_frame = (int16_t *)heap_caps_aligned_alloc(16, frame_bytes,
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t *ref_frame = (int16_t *)heap_caps_aligned_alloc(16, frame_bytes,
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int16_t *out_frame = (int16_t *)heap_caps_aligned_alloc(16, frame_bytes,
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  while (true) {
    // Wait for data or check periodically
    if (!this_->active_.load() || !this_->aec_enabled_.load()) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // Check if we have a full frame in mic_raw_buffer_
    if (this_->mic_raw_buffer_->available() < frame_bytes) {
      vTaskDelay(pdMS_TO_TICKS(5));  // ~1/6 of a frame
      continue;
    }

    // Read mic frame
    this_->mic_raw_buffer_->read(mic_frame, frame_bytes, 0);

    // Read reference frame (may be zeros if no speaker data)
    size_t ref_avail = this_->speaker_ref_buffer_->available();
    if (ref_avail >= frame_bytes) {
      this_->speaker_ref_buffer_->read(ref_frame, frame_bytes, 0);
    } else {
      memset(ref_frame, 0, frame_bytes);  // Silence as reference
    }

    // Process AEC
    this_->aec_->process(mic_frame, ref_frame, out_frame);

    // Write to mic_buffer_ for TX task
    xSemaphoreTake(this_->mic_mutex_, portMAX_DELAY);
    this_->mic_buffer_->write(out_frame, frame_bytes);
    xSemaphoreGive(this_->mic_mutex_);

    taskYIELD();
  }

  // Cleanup (never reached in normal operation)
  heap_caps_free(mic_frame);
  heap_caps_free(ref_frame);
  heap_caps_free(out_frame);
  vTaskDelete(nullptr);
}
```

### 3. Modified Mic Callback

```cpp
void IntercomApi::on_microphone_data_(const uint8_t *data, size_t len) {
  // ... existing conversion code (32→16 bit, DC removal, gain) ...

  if (this->aec_enabled_.load() && this->aec_ != nullptr) {
    // Write to raw buffer for AEC task
    this->mic_raw_buffer_->write(converted, num_samples * sizeof(int16_t));
  } else {
    // Bypass AEC - write directly to mic_buffer_
    xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(10));
    this->mic_buffer_->write(converted, num_samples * sizeof(int16_t));
    xSemaphoreGive(this->mic_mutex_);
  }
}
```

### 4. Speaker Reference Capture

In `handle_message_()` when receiving AUDIO:
```cpp
case MessageType::AUDIO:
  // Write to speaker buffer (existing)
  xSemaphoreTake(this->speaker_mutex_, pdMS_TO_TICKS(10));
  this->speaker_buffer_->write(payload, header.length);
  xSemaphoreGive(this->speaker_mutex_);

  // Also write to AEC reference buffer (NEW)
  if (this->aec_enabled_.load() && this->speaker_ref_buffer_) {
    this->speaker_ref_buffer_->write(payload, header.length);
  }
  break;
```

---

## YAML Configuration

```yaml
# intercom-mini.yaml
esp32:
  framework:
    type: esp-idf
    components:
      - espressif/esp-sr^2.3.0  # Correct version format

external_components:
  - source:
      type: local
      path: esphome/components
    components: [intercom_api, esp_aec]

esp_aec:
  id: aec_component
  filter_length: 4         # Recommended for ESP32-S3
  mode: voip_low_cost      # For voice communication

intercom_api:
  id: intercom
  microphone: mic_component
  speaker: spk_component
  aec: aec_component       # Optional - if not set, AEC disabled

switch:
  - platform: template
    name: "Echo Cancellation"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_OFF
    turn_on_action:
      - lambda: 'id(intercom).set_aec_enabled(true);'
    turn_off_action:
      - lambda: 'id(intercom).set_aec_enabled(false);'
```

---

## Testing Plan

1. **Compile without AEC** - Verify system works as before
2. **Add esp_aec component** - Verify it compiles and initializes
3. **Add AEC task** - Monitor stack usage with `uxTaskGetStackHighWaterMark()`
4. **Test AEC off** - Verify audio still works (bypass path)
5. **Test AEC on** - Verify echo cancellation works
6. **Stress test** - Long calls, high volume, etc.

---

## ESPHome Best Practices Followed

1. **Component structure**: Separate `esp_aec` component for reusability
2. **Optional features**: AEC is fully optional via yaml config
3. **Thread safety**: Using `std::atomic` for state, mutexes for buffers
4. **Memory allocation**: Using ESPHome's RingBuffer, aligned alloc for AEC
5. **Task priorities**: Following ESPHome patterns (higher = more critical)
6. **Core pinning**: Separating I/O from processing
7. **Logging**: DEBUG level logging for troubleshooting
8. **Configuration**: Clean YAML schema with validation

---

## File Changes Summary

### New Files:
- `esphome/components/esp_aec/__init__.py` - Component registration
- `esphome/components/esp_aec/esp_aec.h` - Header
- `esphome/components/esp_aec/esp_aec.cpp` - Implementation

### Modified Files:
- `esphome/components/intercom_api/__init__.py` - Add aec_id config
- `esphome/components/intercom_api/intercom_api.h` - Add AEC members
- `esphome/components/intercom_api/intercom_api.cpp` - Add AEC task, modify callbacks
- `intercom-mini.yaml` - Add esp_aec config and switch

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Stack overflow | 8KB stack for AEC task, verified with watermark |
| Memory exhaustion | Use PSRAM for large buffers, internal for AEC frames |
| Latency increase | AEC adds ~32ms, acceptable for intercom |
| Reference sync | Buffer speaker audio before it reaches mic |
| CPU overload | AEC_MODE_VOIP_LOW_COST uses less CPU |

---

## Approval Checklist

- [ ] Architecture reviewed
- [ ] Buffer sizes appropriate
- [ ] Task priorities correct
- [ ] Thread safety verified
- [ ] ESPHome patterns followed
- [ ] Testing plan complete
