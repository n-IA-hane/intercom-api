#include "i2s_audio_duplex.h"

#ifdef USE_ESP32

#include <cmath>

#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_AEC
#include "../esp_aec/esp_aec.h"
#endif

namespace esphome {
namespace i2s_audio_duplex {

static constexpr size_t BYTES_PER_SAMPLE = 2;

static const char *const TAG = "i2s_duplex";

// Audio parameters
static const size_t DMA_BUFFER_COUNT = 8;
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t DEFAULT_FRAME_SIZE = 256;  // samples per frame (used when no AEC)
static const size_t SPEAKER_BUFFER_SIZE = 8192;

// I2S new driver uses milliseconds directly, NOT FreeRTOS ticks
static const uint32_t I2S_IO_TIMEOUT_MS = 50;

void I2SAudioDuplex::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Duplex...");

  // Create speaker ring buffer
  this->speaker_buffer_ = RingBuffer::create(SPEAKER_BUFFER_SIZE);
  if (!this->speaker_buffer_) {
    ESP_LOGE(TAG, "Failed to create speaker ring buffer");
    this->mark_failed();
    return;
  }

  // Note: speaker_ref_buffer_ for AEC is created in set_aec() which is called after setup()

  ESP_LOGI(TAG, "I2S Audio Duplex ready");
}

void I2SAudioDuplex::set_aec(esp_aec::EspAec *aec) {
  this->aec_ = aec;
  // Enable AEC runtime flag only if AEC is actually configured
  this->aec_enabled_ = (aec != nullptr);
  // Create speaker reference buffer for AEC now (since set_aec is called after setup)
  if (aec != nullptr && !this->speaker_ref_buffer_) {
    // Buffer needs to hold: delay samples + working frames
    // Use configurable delay (default 80ms, can be set lower for integrated codecs)
    size_t delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;
    size_t ref_buffer_size = delay_bytes + SPEAKER_BUFFER_SIZE;
    this->speaker_ref_buffer_ = RingBuffer::create(ref_buffer_size);
    if (this->speaker_ref_buffer_) {
      ESP_LOGI(TAG, "AEC speaker reference buffer created (size=%u, delay=%ums)",
               (unsigned)ref_buffer_size, (unsigned)this->aec_ref_delay_ms_);
    } else {
      ESP_LOGE(TAG, "Failed to create AEC speaker reference buffer");
    }
  }
}

void I2SAudioDuplex::dump_config() {
  ESP_LOGCONFIG(TAG, "I2S Audio Duplex:");
  ESP_LOGCONFIG(TAG, "  LRCLK Pin: %d", this->lrclk_pin_);
  ESP_LOGCONFIG(TAG, "  BCLK Pin: %d", this->bclk_pin_);
  ESP_LOGCONFIG(TAG, "  MCLK Pin: %d", this->mclk_pin_);
  ESP_LOGCONFIG(TAG, "  DIN Pin: %d", this->din_pin_);
  ESP_LOGCONFIG(TAG, "  DOUT Pin: %d", this->dout_pin_);
  ESP_LOGCONFIG(TAG, "  Sample Rate: %d Hz", this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  AEC: %s", this->aec_ != nullptr ? "enabled" : "disabled");
}

bool I2SAudioDuplex::init_i2s_duplex_() {
  ESP_LOGD(TAG, "Initializing I2S in DUPLEX mode...");

  bool need_tx = (this->dout_pin_ >= 0);
  bool need_rx = (this->din_pin_ >= 0);

  if (!need_tx && !need_rx) {
    ESP_LOGE(TAG, "At least one of din_pin or dout_pin must be configured");
    return false;
  }

  // Channel configuration
  i2s_chan_config_t chan_cfg = {
      .id = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = DMA_BUFFER_COUNT,
      .dma_frame_num = DMA_BUFFER_SIZE,
      .auto_clear_after_cb = true,
      .auto_clear_before_cb = false,
      .intr_priority = 0,
  };

  // KEY FOR DUPLEX: Pass BOTH tx and rx pointers to create both channels at once
  i2s_chan_handle_t *tx_ptr = need_tx ? &this->tx_handle_ : nullptr;
  i2s_chan_handle_t *rx_ptr = need_rx ? &this->rx_handle_ : nullptr;

  esp_err_t err = i2s_new_channel(&chan_cfg, tx_ptr, rx_ptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGD(TAG, "I2S channel created: TX=%s RX=%s",
           this->tx_handle_ ? "yes" : "no",
           this->rx_handle_ ? "yes" : "no");

  // Helper to convert pin number to gpio_num_t, using GPIO_NUM_NC for unused pins
  auto pin_or_nc = [](int pin) -> gpio_num_t {
    return pin >= 0 ? static_cast<gpio_num_t>(pin) : GPIO_NUM_NC;
  };

  // Standard mode configuration for TX (always mono)
  i2s_std_config_t tx_cfg = {
      .clk_cfg = {
          .sample_rate_hz = this->sample_rate_,
          .clk_src = I2S_CLK_SRC_DEFAULT,
          .mclk_multiple = I2S_MCLK_MULTIPLE_256,
      },
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = pin_or_nc(this->mclk_pin_),
          .bclk = pin_or_nc(this->bclk_pin_),
          .ws = pin_or_nc(this->lrclk_pin_),
          .dout = pin_or_nc(this->dout_pin_),
          .din = pin_or_nc(this->din_pin_),
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };
  tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  // RX configuration - stereo if using ES8311 digital feedback, mono otherwise
  i2s_std_config_t rx_cfg = tx_cfg;
  if (this->use_stereo_aec_ref_) {
    // ES8311 digital feedback: L=DAC(ref), R=ADC(mic)
    rx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_LOGI(TAG, "RX configured as STEREO for ES8311 digital feedback AEC");
  } else {
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  }

  // Initialize TX channel if available
  if (this->tx_handle_) {
    err = i2s_channel_init_std_mode(this->tx_handle_, &tx_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to init TX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
    ESP_LOGD(TAG, "TX channel initialized");
  }

  // Initialize RX channel if available (may be stereo for ES8311 digital feedback)
  if (this->rx_handle_) {
    err = i2s_channel_init_std_mode(this->rx_handle_, &rx_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to init RX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
    ESP_LOGD(TAG, "RX channel initialized (%s)", this->use_stereo_aec_ref_ ? "stereo" : "mono");
  }

  // Enable channels with error checking
  if (this->tx_handle_) {
    err = i2s_channel_enable(this->tx_handle_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
  }
  if (this->rx_handle_) {
    err = i2s_channel_enable(this->rx_handle_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
  }

  ESP_LOGI(TAG, "I2S DUPLEX initialized successfully");
  return true;
}

void I2SAudioDuplex::deinit_i2s_() {
  // Used for cleanup during init errors; stop() handles normal shutdown
  if (this->tx_handle_) {
    i2s_channel_disable(this->tx_handle_);
    i2s_del_channel(this->tx_handle_);
    this->tx_handle_ = nullptr;
  }
  if (this->rx_handle_) {
    i2s_channel_disable(this->rx_handle_);
    i2s_del_channel(this->rx_handle_);
    this->rx_handle_ = nullptr;
  }
  ESP_LOGD(TAG, "I2S deinitialized");
}

void I2SAudioDuplex::start() {
  if (this->duplex_running_) {
    ESP_LOGW(TAG, "Already running");
    return;
  }

  // Small delay to ensure I2S is fully deinitialized from previous session
  delay(50);

  ESP_LOGI(TAG, "Starting duplex audio...");

  if (!this->init_i2s_duplex_()) {
    ESP_LOGE(TAG, "Failed to initialize I2S");
    return;
  }

  this->duplex_running_ = true;
  this->mic_running_ = false;
  this->speaker_running_ = false;

  // Reset debug counters
  this->aec_frame_count_ = 0;

  // Clear speaker buffer
  this->speaker_buffer_->reset();

#ifdef USE_ESP_AEC
  // Pre-fill reference buffer with silence to create delay (MONO mode only)
  // This compensates for I2S DMA latency + acoustic delay
  // The mic captures echo from audio played X ms ago, so we delay the reference
  // Delay is configurable: 80ms for separate I2S, 20-40ms for integrated codecs like ES8311
  // STEREO mode (ES8311 digital feedback) doesn't need this - reference is sample-aligned
  if (this->speaker_ref_buffer_ != nullptr && this->aec_ != nullptr && !this->use_stereo_aec_ref_) {
    this->speaker_ref_buffer_->reset();
    size_t delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;
    // Allocate temp buffer for silence
    auto *silence = static_cast<uint8_t *>(heap_caps_calloc(1, delay_bytes, MALLOC_CAP_INTERNAL));
    if (silence) {
      this->speaker_ref_buffer_->write_without_replacement(silence, delay_bytes, 0, true);
      heap_caps_free(silence);
      ESP_LOGI(TAG, "AEC reference buffer pre-filled with %ums of silence for delay compensation",
               (unsigned)this->aec_ref_delay_ms_);
    }
  } else if (this->use_stereo_aec_ref_) {
    ESP_LOGI(TAG, "Using ES8311 digital feedback - reference is sample-aligned (no delay needed)");
  }
#endif

  // Create audio task on core 1
  xTaskCreatePinnedToCore(
      audio_task,
      "i2s_duplex",
      8192,
      this,
      9,  // Priority below WiFi/BLE (typically 18), above normal tasks
      &this->audio_task_handle_,
      1    // Core 1
  );

  ESP_LOGI(TAG, "Duplex audio started");
}

void I2SAudioDuplex::stop() {
  if (!this->duplex_running_) {
    return;
  }

  ESP_LOGI(TAG, "Stopping duplex audio...");

  // Step 1: Set all flags to false FIRST so audio task stops doing I/O
  this->mic_running_ = false;
  this->speaker_running_ = false;
  this->duplex_running_ = false;

  // Step 2: Wait for audio task to complete current I/O operation
  // I2S operations have 50ms timeout, so wait a bit longer
  delay(60);

  // Step 3: Now safe to disable channels (task should be idle or exited)
  esp_err_t err;
  if (this->tx_handle_) {
    err = i2s_channel_disable(this->tx_handle_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "TX channel disable failed: %s", esp_err_to_name(err));
    }
  }
  if (this->rx_handle_) {
    err = i2s_channel_disable(this->rx_handle_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "RX channel disable failed: %s", esp_err_to_name(err));
    }
  }

  // Step 4: Wait for task to fully exit
  if (this->audio_task_handle_) {
    int wait_count = 0;
    while (eTaskGetState(this->audio_task_handle_) != eDeleted && wait_count < 50) {
      delay(10);
      wait_count++;
    }
    this->audio_task_handle_ = nullptr;
  }

  // Step 5: Delete channels
  if (this->tx_handle_) {
    i2s_del_channel(this->tx_handle_);
    this->tx_handle_ = nullptr;
  }
  if (this->rx_handle_) {
    i2s_del_channel(this->rx_handle_);
    this->rx_handle_ = nullptr;
  }

  ESP_LOGI(TAG, "Duplex audio stopped");
}

void I2SAudioDuplex::start_mic() {
  if (!this->duplex_running_) {
    this->start();
  }
  this->mic_running_ = true;
}

void I2SAudioDuplex::stop_mic() {
  this->mic_running_ = false;
  if (!this->speaker_running_) {
    this->stop();
  }
}

void I2SAudioDuplex::start_speaker() {
  if (!this->duplex_running_) {
    this->start();
  }
  this->speaker_running_ = true;
}

void I2SAudioDuplex::stop_speaker() {
  this->speaker_running_ = false;
  if (!this->mic_running_) {
    this->stop();
  }
}

size_t I2SAudioDuplex::play(const uint8_t *data, size_t len, TickType_t ticks_to_wait) {
  if (!this->speaker_buffer_) {
    return 0;
  }

  // Write to speaker buffer for playback
  size_t written = this->speaker_buffer_->write_without_replacement((void *) data, len, ticks_to_wait, true);

#ifdef USE_ESP_AEC
  // CRITICAL FIX: Capture reference HERE when data enters the pipeline
  // This matches Mini's timing where reference is captured when speaker->play() is called
  // NOT in audio_task where speaker_buffer latency (50-100ms) would desync the reference
  if (this->speaker_ref_buffer_ != nullptr && written > 0) {
    // Scale reference to match what the attenuated mic will "hear"
    float ref_scale = this->aec_ref_volume_ * this->mic_attenuation_;
    if (ref_scale != 1.0f) {
      // Need temp buffer for scaling
      size_t num_samples = written / sizeof(int16_t);
      const int16_t *src = reinterpret_cast<const int16_t *>(data);
      // Use stack buffer for small writes, heap for large
      if (num_samples <= 1024) {
        int16_t scaled[1024];
        for (size_t i = 0; i < num_samples; i++) {
          int32_t sample = static_cast<int32_t>(src[i] * ref_scale);
          if (sample > 32767) sample = 32767;
          if (sample < -32768) sample = -32768;
          scaled[i] = static_cast<int16_t>(sample);
        }
        this->speaker_ref_buffer_->write_without_replacement((void *) scaled, written, 0, true);
      } else {
        // Fallback: write unscaled for very large buffers
        this->speaker_ref_buffer_->write_without_replacement((void *) data, written, 0, true);
      }
    } else {
      this->speaker_ref_buffer_->write_without_replacement((void *) data, written, 0, true);
    }
  }
#endif

  return written;
}

void I2SAudioDuplex::audio_task(void *param) {
  I2SAudioDuplex *self = static_cast<I2SAudioDuplex *>(param);
  self->audio_task_();
  vTaskDelete(nullptr);
}

void I2SAudioDuplex::audio_task_() {
  ESP_LOGI(TAG, "Audio task started (stereo_aec_ref=%s)", this->use_stereo_aec_ref_ ? "YES" : "no");

  // Determine frame size: use AEC's required chunk size if available, otherwise default
  size_t frame_size = DEFAULT_FRAME_SIZE;
#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr && this->aec_->is_initialized()) {
    frame_size = this->aec_->get_frame_size();
    ESP_LOGI(TAG, "Using AEC frame size: %u samples (%ums at 16kHz)",
             (unsigned)frame_size, (unsigned)(frame_size * 1000 / 16000));
  }
#endif
  size_t frame_bytes = frame_size * sizeof(int16_t);

  // For stereo mode (ES8311 digital feedback), we read 2x data (L=ref, R=mic)
  size_t rx_frame_bytes = this->use_stereo_aec_ref_ ? (frame_bytes * 2) : frame_bytes;

  // Allocate DMA-capable buffers for I2S operations
  auto *rx_buffer = static_cast<int16_t *>(heap_caps_malloc(rx_frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  int16_t *mic_buffer = this->use_stereo_aec_ref_ ?
      static_cast<int16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL)) : rx_buffer;
  auto *spk_buffer = static_cast<int16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  int16_t *spk_ref_buffer = nullptr;
  int16_t *aec_output = nullptr;

  // Stereo split needs spk_ref_buffer even without AEC
  if (this->use_stereo_aec_ref_) {
    spk_ref_buffer = static_cast<int16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL));
  }

#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr) {
    if (!spk_ref_buffer)
      spk_ref_buffer = static_cast<int16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL));
    aec_output = static_cast<int16_t *>(heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL));
  }
#endif

  if (!rx_buffer || !spk_buffer || (this->use_stereo_aec_ref_ && !mic_buffer)) {
    ESP_LOGE(TAG, "Failed to allocate audio buffers");
    if (rx_buffer) heap_caps_free(rx_buffer);
    if (this->use_stereo_aec_ref_ && mic_buffer) heap_caps_free(mic_buffer);
    if (spk_buffer) heap_caps_free(spk_buffer);
    if (spk_ref_buffer) heap_caps_free(spk_ref_buffer);
    if (aec_output) heap_caps_free(aec_output);
    return;
  }

  size_t bytes_read, bytes_written;

  while (this->duplex_running_) {
    bool did_work = false;  // Track if we did useful I/O this iteration

    // ══════════════════════════════════════════════════════════════════
    // MICROPHONE READ (RX)
    // Stereo mode (ES8311 digital feedback): L=reference(DAC), R=mic(ADC)
    // Mono mode: just mic data, reference from ring buffer
    // ══════════════════════════════════════════════════════════════════
    if (this->rx_handle_ && this->mic_running_) {
      // Read into rx_buffer (stereo or mono depending on mode)
      esp_err_t err = i2s_channel_read(this->rx_handle_, rx_buffer, rx_frame_bytes,
                                        &bytes_read, I2S_IO_TIMEOUT_MS);
      // Don't log INVALID_STATE - this is expected during shutdown (race condition)
      if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
      }
      if (err == ESP_OK && bytes_read == rx_frame_bytes) {
        did_work = true;
        int16_t *output_buffer = mic_buffer;  // Default: no AEC processing

        // ── STEREO MODE: Split interleaved L/R into ref and mic ──
        if (this->use_stereo_aec_ref_) {
          // ES8311 digital feedback: L=DAC(ref), R=ADC(mic)
          // Interleaved: [L0,R0,L1,R1,...] → split to [ref] and [mic]
          for (size_t i = 0; i < frame_size; i++) {
            spk_ref_buffer[i] = rx_buffer[i * 2];      // L channel = reference
            mic_buffer[i] = rx_buffer[i * 2 + 1];      // R channel = mic
          }
        }

        // Apply pre-AEC mic attenuation for hot mics (ES8311)
        // This prevents clipping/distortion BEFORE AEC processing
        if (this->mic_attenuation_ != 1.0f) {
          for (size_t i = 0; i < frame_size; i++) {
            mic_buffer[i] = static_cast<int16_t>(mic_buffer[i] * this->mic_attenuation_);
          }
        }

#ifdef USE_ESP_AEC
        if (this->aec_ != nullptr && this->aec_enabled_ && this->aec_->is_initialized() &&
            spk_ref_buffer != nullptr && aec_output != nullptr) {

          // ── MONO MODE: Get reference from ring buffer with delay ──
          if (!this->use_stereo_aec_ref_) {
            // Ring buffer approach: need to maintain delay
            size_t delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;
            size_t min_ref_bytes = delay_bytes + frame_bytes;
            size_t ref_available = this->speaker_ref_buffer_ ? this->speaker_ref_buffer_->available() : 0;

            if (this->speaker_ref_buffer_ != nullptr && ref_available >= min_ref_bytes) {
              this->speaker_ref_buffer_->read((void *) spk_ref_buffer, frame_bytes, 0);
            } else {
              memset(spk_ref_buffer, 0, frame_bytes);
            }
          }
          // STEREO MODE: spk_ref_buffer already filled from L channel split above

          // Process AEC: removes echo from mic_buffer using spk_ref_buffer
          this->aec_->process(mic_buffer, spk_ref_buffer, aec_output, frame_size);
          output_buffer = aec_output;

          ++this->aec_frame_count_;
        }
#endif

        // Apply mic gain
        if (this->mic_gain_ != 1.0f) {
          for (size_t i = 0; i < frame_size; i++) {
            int32_t sample = static_cast<int32_t>(output_buffer[i] * this->mic_gain_);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            output_buffer[i] = static_cast<int16_t>(sample);
          }
        }

        // Call callbacks with zero-copy pointer (no vector allocation per frame)
        for (auto &callback : this->mic_callbacks_) {
          callback((const uint8_t *) output_buffer, frame_bytes);
        }
      }
    }

    // ══════════════════════════════════════════════════════════════════
    // SPEAKER WRITE (TX)
    // Avoid available() which is not thread-safe; read directly and pad
    // ══════════════════════════════════════════════════════════════════
    if (this->tx_handle_ && this->speaker_running_) {
      // Read whatever is available (non-blocking), pad remainder with silence
      size_t got = this->speaker_buffer_->read((void *) spk_buffer, frame_bytes, 0);
      if (got > 0) {
        did_work = true;  // Had actual audio data to play
      }
      if (got < frame_bytes) {
        // Pad with silence to maintain frame alignment
        memset(((uint8_t *) spk_buffer) + got, 0, frame_bytes - got);
      }

      // Apply speaker volume with clamp
      if (this->speaker_volume_ != 1.0f) {
        for (size_t i = 0; i < frame_size; i++) {
          int32_t sample = static_cast<int32_t>(spk_buffer[i] * this->speaker_volume_);
          if (sample > 32767) sample = 32767;
          if (sample < -32768) sample = -32768;
          spk_buffer[i] = static_cast<int16_t>(sample);
        }
      }

      // NOTE: AEC reference is now captured in play() method
      // This fixes the timing - reference is captured when data enters the pipeline
      // NOT here where speaker_buffer latency would desync the reference

      // Note: i2s_channel_write timeout is in milliseconds (new driver), not ticks
      esp_err_t err = i2s_channel_write(this->tx_handle_, spk_buffer, frame_bytes, &bytes_written, I2S_IO_TIMEOUT_MS);
      // Don't log INVALID_STATE - this is expected during shutdown (race condition)
      if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
      }
    }

    // Smart yield: taskYIELD when working (minimal latency), delay when idle (save CPU)
    if (did_work) {
      taskYIELD();
    } else {
      delay(1);
    }
  }

  heap_caps_free(rx_buffer);
  if (this->use_stereo_aec_ref_ && mic_buffer) heap_caps_free(mic_buffer);
  heap_caps_free(spk_buffer);
  if (spk_ref_buffer) heap_caps_free(spk_ref_buffer);
  if (aec_output) heap_caps_free(aec_output);
  ESP_LOGI(TAG, "Audio task stopped");
}

size_t I2SAudioDuplex::get_speaker_buffer_available() const {
  if (!this->speaker_buffer_) return 0;
  return this->speaker_buffer_->available();
}

size_t I2SAudioDuplex::get_speaker_buffer_size() const {
  return SPEAKER_BUFFER_SIZE;
}

}  // namespace i2s_audio_duplex
}  // namespace esphome

#endif  // USE_ESP32
