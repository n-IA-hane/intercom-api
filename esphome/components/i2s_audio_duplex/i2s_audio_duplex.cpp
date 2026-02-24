#include "i2s_audio_duplex.h"

#ifdef USE_ESP32

#include <cmath>
#include <esp_timer.h>

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
static const size_t DEFAULT_FRAME_SIZE = 256;  // samples per frame at output rate (used when no AEC)
static const size_t SPEAKER_BUFFER_BASE = 8192; // base speaker buffer, scaled by decimation_ratio_

// I2S new driver uses milliseconds directly, NOT FreeRTOS ticks
static const uint32_t I2S_IO_TIMEOUT_MS = 50;

void I2SAudioDuplex::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Duplex...");

  // Compute decimation ratio: only active when output_sample_rate is explicitly set
  // and differs from sample_rate. If not set, ratio stays 1 (no decimation, zero overhead).
  if (this->output_sample_rate_ > 0 && this->output_sample_rate_ != this->sample_rate_) {
    this->decimation_ratio_ = this->sample_rate_ / this->output_sample_rate_;
    if (this->decimation_ratio_ * this->output_sample_rate_ != this->sample_rate_) {
      ESP_LOGE(TAG, "sample_rate (%u) must be an exact multiple of output_sample_rate (%u)",
               (unsigned)this->sample_rate_, (unsigned)this->output_sample_rate_);
      this->mark_failed();
      return;
    }
    if (this->decimation_ratio_ > 6) {
      ESP_LOGE(TAG, "Decimation ratio %u exceeds maximum of 6", (unsigned)this->decimation_ratio_);
      this->mark_failed();
      return;
    }
    this->mic_decimator_.init(this->decimation_ratio_);
    this->ref_decimator_.init(this->decimation_ratio_);
    this->play_ref_decimator_.init(this->decimation_ratio_);
    ESP_LOGI(TAG, "Multi-rate: bus=%uHz, output=%uHz, ratio=%u",
             (unsigned)this->sample_rate_, (unsigned)this->output_sample_rate_,
             (unsigned)this->decimation_ratio_);
  }

  // Speaker ring buffer: stores data at bus rate (e.g. 48kHz).
  // Scale buffer size with decimation ratio to accommodate higher data rate.
  this->speaker_buffer_size_ = SPEAKER_BUFFER_BASE * this->decimation_ratio_;
  this->speaker_buffer_ = RingBuffer::create(this->speaker_buffer_size_);
  if (!this->speaker_buffer_) {
    ESP_LOGE(TAG, "Failed to create speaker ring buffer (%u bytes)", (unsigned)this->speaker_buffer_size_);
    this->mark_failed();
    return;
  }

  // AEC reference buffer (mono mode only — stereo mode gets ref from I2S RX).
  // Stores data at bus rate; decimated to output rate in audio_task before AEC.
  if (this->aec_ != nullptr && !this->speaker_ref_buffer_) {
    size_t delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;
    size_t ref_buffer_size = delay_bytes + this->speaker_buffer_size_;
    this->speaker_ref_buffer_ = RingBuffer::create(ref_buffer_size);
    if (this->speaker_ref_buffer_) {
      ESP_LOGD(TAG, "AEC reference buffer: %u bytes (delay=%ums)", (unsigned)ref_buffer_size,
               (unsigned)this->aec_ref_delay_ms_);
    } else {
      ESP_LOGE(TAG, "Failed to create AEC speaker reference buffer");
    }
  }

  ESP_LOGI(TAG, "I2S Audio Duplex ready (speaker_buf=%u bytes)", (unsigned)this->speaker_buffer_size_);
}

void I2SAudioDuplex::set_aec(esp_aec::EspAec *aec) {
  this->aec_ = aec;
  this->aec_enabled_ = (aec != nullptr);
  // Note: speaker_ref_buffer_ is created in setup() after decimation_ratio_ is computed
}

void I2SAudioDuplex::dump_config() {
  ESP_LOGCONFIG(TAG, "I2S Audio Duplex:");
  ESP_LOGCONFIG(TAG, "  LRCLK Pin: %d", this->lrclk_pin_);
  ESP_LOGCONFIG(TAG, "  BCLK Pin: %d", this->bclk_pin_);
  ESP_LOGCONFIG(TAG, "  MCLK Pin: %d", this->mclk_pin_);
  ESP_LOGCONFIG(TAG, "  DIN Pin: %d", this->din_pin_);
  ESP_LOGCONFIG(TAG, "  DOUT Pin: %d", this->dout_pin_);
  ESP_LOGCONFIG(TAG, "  I2S Bus Rate: %u Hz", (unsigned)this->sample_rate_);
  if (this->decimation_ratio_ > 1) {
    ESP_LOGCONFIG(TAG, "  Output Rate: %u Hz (decimation x%u)",
                  (unsigned)this->get_output_sample_rate(), (unsigned)this->decimation_ratio_);
  }
  ESP_LOGCONFIG(TAG, "  Speaker Buffer: %u bytes", (unsigned)this->speaker_buffer_size_);
  if (this->use_stereo_aec_ref_) {
    ESP_LOGCONFIG(TAG, "  Stereo AEC Reference: %s channel", this->ref_channel_right_ ? "RIGHT" : "LEFT");
  }
  ESP_LOGCONFIG(TAG, "  AEC: %s", this->aec_ != nullptr ? "enabled" : "disabled");
}

bool I2SAudioDuplex::init_i2s_duplex_() {
  ESP_LOGCONFIG(TAG, "Initializing I2S in DUPLEX mode...");

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
    rx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_LOGD(TAG, "RX configured as STEREO for ES8311 digital feedback AEC");
  } else {
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  }

  if (this->tx_handle_) {
    err = i2s_channel_init_std_mode(this->tx_handle_, &tx_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to init TX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
    ESP_LOGD(TAG, "TX channel initialized");
  }

  if (this->rx_handle_) {
    err = i2s_channel_init_std_mode(this->rx_handle_, &rx_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to init RX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
    ESP_LOGD(TAG, "RX channel initialized (%s)", this->use_stereo_aec_ref_ ? "stereo" : "mono");
  }

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

void I2SAudioDuplex::prefill_aec_ref_buffer_() {
#ifdef USE_ESP_AEC
  if (this->speaker_ref_buffer_ != nullptr && this->aec_ != nullptr &&
      this->aec_ref_delay_ms_ > 0 && !this->use_stereo_aec_ref_) {
    this->speaker_ref_buffer_->reset();
    size_t delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;
    uint8_t silence[512] = {};
    size_t remaining = delay_bytes;
    while (remaining > 0) {
      size_t chunk = std::min(remaining, sizeof(silence));
      this->speaker_ref_buffer_->write_without_replacement(silence, chunk, 0, true);
      remaining -= chunk;
    }
    ESP_LOGD(TAG, "AEC reference buffer pre-filled with %ums of silence",
             (unsigned)this->aec_ref_delay_ms_);
  }
#endif
}

void I2SAudioDuplex::start() {
  if (this->duplex_running_) {
    ESP_LOGW(TAG, "Already running");
    return;
  }

  ESP_LOGI(TAG, "Starting duplex audio...");

  if (!this->init_i2s_duplex_()) {
    ESP_LOGE(TAG, "Failed to initialize I2S");
    return;
  }

  this->duplex_running_ = true;
  this->speaker_running_ = (this->tx_handle_ != nullptr);

  this->speaker_buffer_->reset();

  // Reset FIR decimators for clean state
  this->mic_decimator_.reset();
  this->ref_decimator_.reset();
  this->play_ref_decimator_.reset();

  this->prefill_aec_ref_buffer_();
#ifdef USE_ESP_AEC
  if (this->use_stereo_aec_ref_) {
    ESP_LOGD(TAG, "ES8311 digital feedback - reference is sample-aligned");
  }
#endif

  xTaskCreatePinnedToCore(
      audio_task,
      "i2s_duplex",
      8192,
      this,
      9,
      &this->audio_task_handle_,
      1
  );

  ESP_LOGI(TAG, "Duplex audio started");
}

void I2SAudioDuplex::stop() {
  if (!this->duplex_running_) {
    return;
  }

  ESP_LOGI(TAG, "Stopping duplex audio...");

  this->mic_ref_count_.store(0);
  this->speaker_running_ = false;
  this->duplex_running_ = false;

  delay(60);

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

  if (this->audio_task_handle_) {
    int wait_count = 0;
    while (eTaskGetState(this->audio_task_handle_) != eDeleted && wait_count < 50) {
      delay(10);
      wait_count++;
    }
    this->audio_task_handle_ = nullptr;
  }

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
  this->mic_ref_count_.fetch_add(1);
}

void I2SAudioDuplex::stop_mic() {
  int prev = this->mic_ref_count_.fetch_sub(1);
  if (prev <= 1) {
    this->mic_ref_count_.store(0);
  }
}

void I2SAudioDuplex::start_speaker() {
  if (!this->duplex_running_) {
    this->start();
  }
  this->speaker_running_ = true;

  this->play_ref_decimator_.reset();

  this->prefill_aec_ref_buffer_();
}

void I2SAudioDuplex::stop_speaker() {
  this->speaker_running_ = false;
  if (this->speaker_buffer_) {
    this->speaker_buffer_->reset();
  }
  if (this->speaker_ref_buffer_) {
    this->speaker_ref_buffer_->reset();
  }
}

size_t I2SAudioDuplex::play(const uint8_t *data, size_t len, TickType_t ticks_to_wait) {
  if (!this->speaker_buffer_) {
    return 0;
  }

  // Data arrives at bus rate (e.g. 48kHz from mixer/resampler). Write directly.
  size_t written = this->speaker_buffer_->write_without_replacement((void *) data, len, ticks_to_wait, true);

  if (written > 0) {
    this->last_speaker_audio_ms_ = millis();
  }

#ifdef USE_ESP_AEC
  // Write bus-rate reference for AEC (mono mode only — stereo mode gets ref from I2S RX).
  // Reference is decimated to output rate in audio_task before feeding to AEC.
  if (this->speaker_ref_buffer_ != nullptr && written > 0 && this->speaker_running_ &&
      !this->use_stereo_aec_ref_) {
    this->speaker_ref_buffer_->write_without_replacement((void *) data, written, 0, true);
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
  const uint32_t ratio = this->decimation_ratio_;
  ESP_LOGD(TAG, "Audio task started (stereo_aec_ref=%s, decimation=%ux)",
           this->use_stereo_aec_ref_ ? "YES" : "no", (unsigned)ratio);

  // Determine output frame size: use AEC's required chunk size if available, otherwise default.
  size_t out_frame_size = DEFAULT_FRAME_SIZE;
#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr && this->aec_->is_initialized()) {
    out_frame_size = this->aec_->get_frame_size();
    uint32_t out_rate = this->get_output_sample_rate();
    ESP_LOGD(TAG, "AEC frame size: %u samples (%ums @ %uHz)",
             (unsigned)out_frame_size, (unsigned)(out_frame_size * 1000 / out_rate), (unsigned)out_rate);
  }
#endif

  // Bus frame size: how many samples per I2S read/write at bus rate
  size_t bus_frame_size = out_frame_size * ratio;
  size_t out_frame_bytes = out_frame_size * sizeof(int16_t);
  size_t bus_frame_bytes = bus_frame_size * sizeof(int16_t);

  // RX read size: stereo doubles it (L+R interleaved)
  size_t rx_frame_bytes = this->use_stereo_aec_ref_ ? (bus_frame_bytes * 2) : bus_frame_bytes;

  // ── Buffer allocations ──
  // rx_buffer: DMA-capable, holds one I2S RX frame at bus rate
  auto *rx_buffer = static_cast<int16_t *>(
      heap_caps_malloc(rx_frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

  // mic_buffer: holds output-rate mic data (decimated or direct)
  bool mic_separate = (ratio > 1) || this->use_stereo_aec_ref_;
  int16_t *mic_buffer = mic_separate
      ? static_cast<int16_t *>(heap_caps_malloc(out_frame_bytes, MALLOC_CAP_INTERNAL))
      : rx_buffer;  // mono, no decimation: alias rx_buffer

  // spk_buffer: DMA-capable, TX output at bus rate (read from ring buffer, write to I2S)
  auto *spk_buffer = static_cast<int16_t *>(
      heap_caps_malloc(bus_frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));

  // spk_ref_buffer: output-rate AEC reference (from stereo split or mono ring buffer after decimation)
  int16_t *spk_ref_buffer = nullptr;
  if (this->use_stereo_aec_ref_) {
    spk_ref_buffer = static_cast<int16_t *>(heap_caps_malloc(out_frame_bytes, MALLOC_CAP_INTERNAL));
  }

  // Intermediate 48kHz buffers for stereo deinterleave before decimation
  int16_t *deint_ref = nullptr;
  int16_t *deint_mic = nullptr;
  if (this->use_stereo_aec_ref_ && ratio > 1) {
    deint_ref = static_cast<int16_t *>(heap_caps_malloc(bus_frame_bytes, MALLOC_CAP_INTERNAL));
    deint_mic = static_cast<int16_t *>(heap_caps_malloc(bus_frame_bytes, MALLOC_CAP_INTERNAL));
  }

  // Mono mode AEC ref: temp buffer for reading bus-rate ref from ring buffer before decimation
  int16_t *ref_bus_buffer = nullptr;
  int16_t *aec_output = nullptr;

#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr) {
    if (!spk_ref_buffer)
      spk_ref_buffer = static_cast<int16_t *>(heap_caps_malloc(out_frame_bytes, MALLOC_CAP_INTERNAL));
    aec_output = static_cast<int16_t *>(heap_caps_malloc(out_frame_bytes, MALLOC_CAP_INTERNAL));
    // For mono mode: need temp buffer for bus-rate ref read from ring buffer
    if (!this->use_stereo_aec_ref_) {
      ref_bus_buffer = static_cast<int16_t *>(heap_caps_malloc(bus_frame_bytes, MALLOC_CAP_INTERNAL));
    }
  }
#endif

  // Verify critical allocations
  if (!rx_buffer || !spk_buffer || (mic_separate && !mic_buffer)) {
    ESP_LOGE(TAG, "Failed to allocate audio buffers");
    goto cleanup;
  }
  if (this->use_stereo_aec_ref_ && ratio > 1 && (!deint_ref || !deint_mic)) {
    ESP_LOGE(TAG, "Failed to allocate stereo decimation buffers");
    goto cleanup;
  }

  {
    size_t bytes_read, bytes_written;
    // Pre-compute AEC delay bytes outside loop (L12: avoid per-frame recalculation)
    const size_t aec_delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;

    while (this->duplex_running_) {

      // ══════════════════════════════════════════════════════════════════
      // MICROPHONE READ (RX)
      // Bus rate: reads bus_frame_size samples (mono) or bus_frame_size*2 (stereo)
      // Output: out_frame_size samples at output rate after decimation
      // ══════════════════════════════════════════════════════════════════
      if (this->rx_handle_) {
        esp_err_t err = i2s_channel_read(this->rx_handle_, rx_buffer, rx_frame_bytes,
                                          &bytes_read, I2S_IO_TIMEOUT_MS);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
          ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
          this->has_i2s_error_ = true;
        }
        if (err == ESP_OK && bytes_read == rx_frame_bytes) {
          int16_t *output_buffer = mic_buffer;  // Default: no AEC processing

          if (ratio > 1) {
            // ── DECIMATION PATH: bus rate -> output rate ──
            if (this->use_stereo_aec_ref_) {
              // Stereo 48kHz: deinterleave L/R then decimate both channels
              const int ref_idx = this->ref_channel_right_ ? 1 : 0;
              const int mic_idx = this->ref_channel_right_ ? 0 : 1;
              for (size_t i = 0; i < bus_frame_size; i++) {
                deint_ref[i] = rx_buffer[i * 2 + ref_idx];
                deint_mic[i] = rx_buffer[i * 2 + mic_idx];
              }
              this->ref_decimator_.process(deint_ref, spk_ref_buffer, bus_frame_size);
              this->mic_decimator_.process(deint_mic, mic_buffer, bus_frame_size);
            } else {
              // Mono 48kHz: decimate mic directly from rx_buffer
              this->mic_decimator_.process(rx_buffer, mic_buffer, bus_frame_size);
            }
          } else {
            // ── LEGACY PATH (no decimation) ──
            if (this->use_stereo_aec_ref_) {
              const int ref_idx = this->ref_channel_right_ ? 1 : 0;
              const int mic_idx = this->ref_channel_right_ ? 0 : 1;
              for (size_t i = 0; i < out_frame_size; i++) {
                spk_ref_buffer[i] = rx_buffer[i * 2 + ref_idx];
                mic_buffer[i] = rx_buffer[i * 2 + mic_idx];
              }
            }
            // Mono: mic_buffer == rx_buffer (aliased), nothing to do
          }

          // ── From here, all processing uses output-rate data ──

          // Apply pre-AEC mic attenuation for hot mics (ES8311)
          if (this->mic_attenuation_ != 1.0f) {
            for (size_t i = 0; i < out_frame_size; i++) {
              int32_t s = static_cast<int32_t>(mic_buffer[i] * this->mic_attenuation_);
              if (s > 32767) s = 32767;
              if (s < -32768) s = -32768;
              mic_buffer[i] = static_cast<int16_t>(s);
            }
          }

          // Raw mic callbacks: pre-AEC audio for MWW
          if (this->is_mic_running() && !this->raw_mic_callbacks_.empty()) {
            for (auto &callback : this->raw_mic_callbacks_) {
              callback((const uint8_t *) mic_buffer, out_frame_bytes);
            }
          }

#ifdef USE_ESP_AEC
          if (this->aec_ != nullptr && this->aec_enabled_ && this->aec_->is_initialized() &&
              spk_ref_buffer != nullptr && aec_output != nullptr && this->speaker_running_ &&
              (millis() - this->last_speaker_audio_ms_ <= AEC_ACTIVE_TIMEOUT_MS)) {

            // ── MONO MODE: Get reference from ring buffer, decimate to output rate ──
            if (!this->use_stereo_aec_ref_) {
              size_t min_ref_bytes = aec_delay_bytes + bus_frame_bytes;
              size_t ref_available = this->speaker_ref_buffer_ ? this->speaker_ref_buffer_->available() : 0;

              if (this->speaker_ref_buffer_ != nullptr && ref_available >= min_ref_bytes && ref_bus_buffer != nullptr) {
                this->speaker_ref_buffer_->read((void *) ref_bus_buffer, bus_frame_bytes, 0);
                // Decimate bus-rate → output-rate for AEC
                this->play_ref_decimator_.process(ref_bus_buffer, spk_ref_buffer, bus_frame_size);
              } else {
                memset(spk_ref_buffer, 0, out_frame_bytes);
              }

              // Scale ref for AEC alignment (match mic attenuation level)
              float ref_scale = this->aec_ref_volume_ * this->mic_attenuation_;
              if (ref_scale != 1.0f) {
                for (size_t i = 0; i < out_frame_size; i++) {
                  int32_t s = static_cast<int32_t>(spk_ref_buffer[i] * ref_scale);
                  if (s > 32767) s = 32767;
                  if (s < -32768) s = -32768;
                  spk_ref_buffer[i] = static_cast<int16_t>(s);
                }
              }
            }
            // STEREO MODE: spk_ref_buffer already filled from deinterleave (+decimate) above

            this->aec_->process(mic_buffer, spk_ref_buffer, aec_output, out_frame_size);
            output_buffer = aec_output;
          }
#endif

          // Apply mic gain
          if (this->mic_gain_ != 1.0f) {
            for (size_t i = 0; i < out_frame_size; i++) {
              int32_t sample = static_cast<int32_t>(output_buffer[i] * this->mic_gain_);
              if (sample > 32767) sample = 32767;
              if (sample < -32768) sample = -32768;
              output_buffer[i] = static_cast<int16_t>(sample);
            }
          }

          // Call callbacks only when mic is active
          if (this->is_mic_running()) {
            for (auto &callback : this->mic_callbacks_) {
              callback((const uint8_t *) output_buffer, out_frame_bytes);
            }
          }
        }
      }

      // ══════════════════════════════════════════════════════════════════
      // SPEAKER WRITE (TX)
      // Read bus-rate data from ring buffer, apply volume, write to I2S
      // ══════════════════════════════════════════════════════════════════
      if (this->tx_handle_) {
        if (this->speaker_running_) {
          size_t got = this->speaker_buffer_->read((void *) spk_buffer, bus_frame_bytes, 0);

          if (this->speaker_paused_) {
            // Paused: drain buffer but write silence to I2S (keeps mixer flowing)
            memset(spk_buffer, 0, bus_frame_bytes);
          } else if (got > 0) {
            size_t got_samples = got / sizeof(int16_t);

            // Apply speaker volume
            if (this->speaker_volume_ != 1.0f) {
              for (size_t i = 0; i < got_samples; i++) {
                int32_t sample = static_cast<int32_t>(spk_buffer[i] * this->speaker_volume_);
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                spk_buffer[i] = static_cast<int16_t>(sample);
              }
            }

            // Pad remainder with silence if partial read
            if (got < bus_frame_bytes) {
              memset(((uint8_t *) spk_buffer) + got, 0, bus_frame_bytes - got);
            }
          } else {
            memset(spk_buffer, 0, bus_frame_bytes);
          }
        } else {
          memset(spk_buffer, 0, bus_frame_bytes);
        }

        esp_err_t err = i2s_channel_write(this->tx_handle_, spk_buffer, bus_frame_bytes, &bytes_written, I2S_IO_TIMEOUT_MS);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
          ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
          this->has_i2s_error_ = true;
        }

        // Report frames played at bus rate (mixer operates at bus rate)
        if (err == ESP_OK && bytes_written > 0 && !this->speaker_output_callbacks_.empty()) {
          uint32_t frames_played = bytes_written / sizeof(int16_t);
          int64_t timestamp = esp_timer_get_time();
          for (auto &cb : this->speaker_output_callbacks_) {
            cb(frames_played, timestamp);
          }
        }
      }

      // Yield to lower-priority tasks
      delay(1);
    }
  }

cleanup:
  heap_caps_free(rx_buffer);
  if (mic_separate && mic_buffer) heap_caps_free(mic_buffer);
  heap_caps_free(spk_buffer);
  if (spk_ref_buffer) heap_caps_free(spk_ref_buffer);
  if (deint_ref) heap_caps_free(deint_ref);
  if (deint_mic) heap_caps_free(deint_mic);
  if (ref_bus_buffer) heap_caps_free(ref_bus_buffer);
  if (aec_output) heap_caps_free(aec_output);
  ESP_LOGI(TAG, "Audio task stopped");
}

size_t I2SAudioDuplex::get_speaker_buffer_available() const {
  if (!this->speaker_buffer_) return 0;
  return this->speaker_buffer_->available();
}

size_t I2SAudioDuplex::get_speaker_buffer_size() const {
  return this->speaker_buffer_size_;
}

}  // namespace i2s_audio_duplex
}  // namespace esphome

#endif  // USE_ESP32
