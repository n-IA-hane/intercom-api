#include "i2s_audio_duplex.h"

#ifdef USE_ESP32

#include <esp_timer.h>

#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_ESP_AEC
#include "../esp_aec/aec_processor.h"
#endif
#include "audio_utils.h"

namespace esphome {
namespace i2s_audio_duplex {

static constexpr size_t BYTES_PER_SAMPLE = 2;

static const char *const TAG = "i2s_duplex";

// Helper: get MCLK multiple enum from integer value
static i2s_mclk_multiple_t get_mclk_multiple(uint32_t mult) {
  switch (mult) {
    case 128: return I2S_MCLK_MULTIPLE_128;
    case 384: return I2S_MCLK_MULTIPLE_384;
    case 512: return I2S_MCLK_MULTIPLE_512;
    default: return I2S_MCLK_MULTIPLE_256;
  }
}

// Helper: get STD slot config for the configured comm format (0=philips, 1=msb, 2=pcm_short, 3=pcm_long)
// Note: PCM short/long are TDM-only in ESP-IDF — fall back to Philips in STD mode
static i2s_std_slot_config_t get_std_slot_config(uint8_t fmt, i2s_data_bit_width_t bw, i2s_slot_mode_t mode) {
  switch (fmt) {
    case 1: return I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bw, mode);
    default: return I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bw, mode);
  }
}

#if SOC_I2S_SUPPORTS_TDM
// Helper: get TDM slot config for the configured comm format
static i2s_tdm_slot_config_t get_tdm_slot_config(uint8_t fmt, i2s_data_bit_width_t bw,
                                                   i2s_slot_mode_t mode, i2s_tdm_slot_mask_t mask) {
  switch (fmt) {
    case 1: return I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(bw, mode, mask);
    case 2: return I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(bw, mode, mask);
    case 3: return I2S_TDM_PCM_LONG_SLOT_DEFAULT_CONFIG(bw, mode, mask);
    default: return I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bw, mode, mask);
  }
}
#endif  // SOC_I2S_SUPPORTS_TDM

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

  // AEC reference (mono mode only — stereo/TDM get ref from I2S RX).
  if (this->aec_ != nullptr && !this->use_stereo_aec_ref_ && !this->use_tdm_ref_) {
    if (this->aec_ref_delay_ms_ > 0) {
      // Ring buffer mode: delayed reference for acoustic path compensation
      size_t delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;
      size_t ref_buffer_size = delay_bytes + this->speaker_buffer_size_;
      this->speaker_ref_buffer_ = RingBuffer::create(ref_buffer_size);
      if (this->speaker_ref_buffer_) {
        ESP_LOGD(TAG, "AEC reference: ring buffer %u bytes (delay=%ums)",
                 (unsigned)ref_buffer_size, (unsigned)this->aec_ref_delay_ms_);
      } else {
        ESP_LOGE(TAG, "Failed to create AEC speaker reference buffer");
      }
    } else {
      // Direct mode: reference from previous TX frame (no ring buffer, ~1 chunk latency)
      size_t bus_frame_size = this->sample_rate_ / 1000 * 32;  // ~32ms at bus rate
      this->direct_aec_ref_ = static_cast<int16_t *>(
          heap_caps_calloc(bus_frame_size, sizeof(int16_t), MALLOC_CAP_INTERNAL));
      if (this->direct_aec_ref_) {
        ESP_LOGD(TAG, "AEC reference: direct from TX (no ring buffer, %u samples)", (unsigned)bus_frame_size);
      } else {
        ESP_LOGE(TAG, "Failed to allocate direct AEC reference buffer");
      }
    }
  }

  ESP_LOGI(TAG, "I2S Audio Duplex ready (speaker_buf=%u bytes)", (unsigned)this->speaker_buffer_size_);
}

void I2SAudioDuplex::set_aec(AecProcessor *aec) {
  this->aec_ = aec;
  this->aec_enabled_.store(aec != nullptr, std::memory_order_relaxed);
  // Note: speaker_ref_buffer_ is created in setup() after decimation_ratio_ is computed
}

void I2SAudioDuplex::dump_config() {
  ESP_LOGCONFIG(TAG, "I2S Audio Duplex:");
  ESP_LOGCONFIG(TAG, "  LRCLK Pin: %d", this->lrclk_pin_);
  ESP_LOGCONFIG(TAG, "  BCLK Pin: %d", this->bclk_pin_);
  ESP_LOGCONFIG(TAG, "  MCLK Pin: %d", this->mclk_pin_);
  ESP_LOGCONFIG(TAG, "  DIN Pin: %d", this->din_pin_);
  ESP_LOGCONFIG(TAG, "  DOUT Pin: %d", this->dout_pin_);
  ESP_LOGCONFIG(TAG, "  I2S Port: %u", this->i2s_num_);
  ESP_LOGCONFIG(TAG, "  I2S Role: %s", this->i2s_mode_secondary_ ? "secondary (slave)" : "primary (master)");
  ESP_LOGCONFIG(TAG, "  I2S Bus Rate: %u Hz", (unsigned)this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  I2S Bits Per Sample: %u", this->bits_per_sample_);
  if (this->slot_bit_width_ > 0) {
    ESP_LOGCONFIG(TAG, "  Slot Bit Width: %u", this->slot_bit_width_);
  }
  ESP_LOGCONFIG(TAG, "  TX Channels: %u (%s)", this->num_channels_,
                this->num_channels_ == 2 ? "stereo" : "mono");
  ESP_LOGCONFIG(TAG, "  RX Mic Channel: %s", this->mic_channel_right_ ? "RIGHT" : "LEFT");
  static const char *const fmt_names[] = {"Philips", "MSB", "PCM Short", "PCM Long"};
  ESP_LOGCONFIG(TAG, "  Comm Format: %s", fmt_names[this->i2s_comm_fmt_ & 3]);
  ESP_LOGCONFIG(TAG, "  MCLK Multiple: %u", (unsigned)this->mclk_multiple_);
  if (this->use_apll_) {
    ESP_LOGCONFIG(TAG, "  APLL: enabled");
  }
  if (this->correct_dc_offset_) {
    ESP_LOGCONFIG(TAG, "  DC Offset Correction: enabled");
  }
  if (this->decimation_ratio_ > 1) {
    ESP_LOGCONFIG(TAG, "  Output Rate: %u Hz (decimation x%u)",
                  (unsigned)this->get_output_sample_rate(), (unsigned)this->decimation_ratio_);
  }
  ESP_LOGCONFIG(TAG, "  Speaker Buffer: %u bytes", (unsigned)this->speaker_buffer_size_);
  if (this->use_stereo_aec_ref_) {
    ESP_LOGCONFIG(TAG, "  Stereo AEC Reference: %s channel", this->ref_channel_right_ ? "RIGHT" : "LEFT");
  }
  if (this->use_tdm_ref_) {
    ESP_LOGCONFIG(TAG, "  TDM Reference: %u slots, mic_slot=%u, ref_slot=%u",
                  this->tdm_total_slots_, this->tdm_mic_slot_, this->tdm_ref_slot_);
  }
  ESP_LOGCONFIG(TAG, "  AEC: %s", this->aec_ != nullptr ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Task: priority=%u, core=%d, stack=%u",
                this->task_priority_, this->task_core_, (unsigned)this->task_stack_size_);
}

bool I2SAudioDuplex::init_i2s_duplex_() {
  ESP_LOGCONFIG(TAG, "Initializing I2S in DUPLEX mode...");

  // Map configured bit depth to I2S enum
  // Note: 24-bit data is stored in 32-bit DMA containers (MSB-aligned)
  i2s_data_bit_width_t bit_width;
  switch (this->bits_per_sample_) {
    case 32: bit_width = I2S_DATA_BIT_WIDTH_32BIT; break;
    case 24: bit_width = I2S_DATA_BIT_WIDTH_24BIT; break;
    default: bit_width = I2S_DATA_BIT_WIDTH_16BIT; break;
  }

  // Slot bit width: auto = match data bit width, or explicit override
  i2s_slot_bit_width_t slot_bw = I2S_SLOT_BIT_WIDTH_AUTO;
  if (this->slot_bit_width_ > 0) {
    switch (this->slot_bit_width_) {
      case 32: slot_bw = I2S_SLOT_BIT_WIDTH_32BIT; break;
      case 24: slot_bw = I2S_SLOT_BIT_WIDTH_24BIT; break;
      case 16: slot_bw = I2S_SLOT_BIT_WIDTH_16BIT; break;
      default: slot_bw = I2S_SLOT_BIT_WIDTH_AUTO; break;
    }
  }

  bool need_tx = (this->dout_pin_ >= 0);
  bool need_rx = (this->din_pin_ >= 0);

  if (!need_tx && !need_rx) {
    ESP_LOGE(TAG, "At least one of din_pin or dout_pin must be configured");
    return false;
  }

  // Channel configuration
  // Clock source: APLL for accurate clocking (ESP32 original only)
  i2s_clock_src_t clk_src = I2S_CLK_SRC_DEFAULT;
#ifdef I2S_CLK_SRC_APLL
  if (this->use_apll_) clk_src = I2S_CLK_SRC_APLL;
#endif
  i2s_mclk_multiple_t mclk_mult = get_mclk_multiple(this->mclk_multiple_);

  // DMA descriptor limit is 4092 bytes. Compute max bytes per frame across
  // TX and RX configs, then clamp dma_frame_num to stay within the limit.
  // RX can be wider than TX (e.g., mono TX but stereo RX for AEC feedback).
  uint32_t bytes_per_sample = (this->bits_per_sample_ > 16) ? 4 : 2;  // 24/32-bit → 4-byte DMA container
  uint32_t tx_bytes_per_frame = this->num_channels_ * bytes_per_sample;
  uint32_t rx_bytes_per_frame = tx_bytes_per_frame;
  if (this->use_stereo_aec_ref_) {
    rx_bytes_per_frame = 2 * bytes_per_sample;  // stereo RX forced
  }
#if SOC_I2S_SUPPORTS_TDM
  if (this->use_tdm_ref_) {
    uint32_t tdm_frame = this->tdm_total_slots_ * bytes_per_sample;
    rx_bytes_per_frame = tdm_frame;
    tx_bytes_per_frame = tdm_frame;
  }
#endif
  uint32_t max_bytes_per_frame = std::max(tx_bytes_per_frame, rx_bytes_per_frame);
  uint32_t dma_frame_num = DMA_BUFFER_SIZE;  // 512 default
  if (max_bytes_per_frame > 0) {
    uint32_t max_frames = 4092 / max_bytes_per_frame;
    if (dma_frame_num > max_frames) {
      dma_frame_num = max_frames;
    }
  }
  i2s_chan_config_t chan_cfg = {
      .id = static_cast<i2s_port_t>(this->i2s_num_),
      .role = this->i2s_mode_secondary_ ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER,
      .dma_desc_num = DMA_BUFFER_COUNT,
      .dma_frame_num = dma_frame_num,
      .auto_clear_after_cb = true,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
      .auto_clear_before_cb = false,
      .intr_priority = 0,
#endif
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

#if SOC_I2S_SUPPORTS_TDM
  if (this->use_tdm_ref_) {
    // ── TDM MODE: ES7210 multi-slot RX + ES8311 slot-0 TX ──
    // STEREO with 4 slots: DMA contains all 4 interleaved slots, BCLK/FS = 64.
    // ESP-IDF MONO only puts slot 0 in DMA; STEREO gives all active slots.
    // total_slot is derived from slot_mask (not slot_mode), so BCLK doesn't change.
    // ES8311 reads/writes slot 0 as standard I2S (first 16 bits after LRCLK edge).
    // DMA frame = tdm_total_slots × 2 bytes. At 4 slots, 256 frames = 2048 bytes/desc (< 4092 limit).
    i2s_tdm_slot_mask_t tdm_mask = I2S_TDM_SLOT0;
    for (int i = 1; i < this->tdm_total_slots_; i++)
      tdm_mask = static_cast<i2s_tdm_slot_mask_t>(tdm_mask | (I2S_TDM_SLOT0 << i));

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = this->sample_rate_,
            .clk_src = clk_src,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = mclk_mult,
        },
        .slot_cfg = get_tdm_slot_config(this->i2s_comm_fmt_, bit_width, I2S_SLOT_MODE_STEREO, tdm_mask),
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

    // Apply slot_bit_width override BEFORE init
    if (slot_bw != I2S_SLOT_BIT_WIDTH_AUTO) {
      tdm_cfg.slot_cfg.slot_bit_width = slot_bw;
    }

    if (this->tx_handle_) {
      err = i2s_channel_init_tdm_mode(this->tx_handle_, &tdm_cfg);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TDM TX: %s", esp_err_to_name(err));
        this->deinit_i2s_();
        return false;
      }
    }
    if (this->rx_handle_) {
      err = i2s_channel_init_tdm_mode(this->rx_handle_, &tdm_cfg);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TDM RX: %s", esp_err_to_name(err));
        this->deinit_i2s_();
        return false;
      }
    }

    ESP_LOGD(TAG, "TDM mode: %d slots, mic_slot=%d, ref_slot=%d, mask=0x%x",
             this->tdm_total_slots_, this->tdm_mic_slot_, this->tdm_ref_slot_, (unsigned)tdm_mask);
  } else
#endif  // SOC_I2S_SUPPORTS_TDM
  {
    // ── STANDARD MODE ──
    i2s_slot_mode_t tx_slot_mode = (this->num_channels_ == 2)
        ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    i2s_std_config_t tx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = this->sample_rate_,
            .clk_src = clk_src,
            .mclk_multiple = mclk_mult,
        },
        .slot_cfg = get_std_slot_config(this->i2s_comm_fmt_, bit_width, tx_slot_mode),
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
    tx_cfg.slot_cfg.slot_mask = (this->num_channels_ == 2)
        ? I2S_STD_SLOT_BOTH : I2S_STD_SLOT_LEFT;
    // Apply slot_bit_width override
    if (slot_bw != I2S_SLOT_BIT_WIDTH_AUTO) {
      tx_cfg.slot_cfg.slot_bit_width = slot_bw;
    }

    // RX configuration - always independent of TX num_channels
    i2s_std_config_t rx_cfg = tx_cfg;
    if (this->use_stereo_aec_ref_) {
      rx_cfg.slot_cfg = get_std_slot_config(this->i2s_comm_fmt_, bit_width, I2S_SLOT_MODE_STEREO);
      rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
      ESP_LOGD(TAG, "RX configured as STEREO for ES8311 digital feedback AEC");
    } else {
      rx_cfg.slot_cfg = get_std_slot_config(this->i2s_comm_fmt_, bit_width, I2S_SLOT_MODE_MONO);
      rx_cfg.slot_cfg.slot_mask = this->mic_channel_right_ ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    }
    // Apply slot_bit_width override to RX
    if (slot_bw != I2S_SLOT_BIT_WIDTH_AUTO) {
      rx_cfg.slot_cfg.slot_bit_width = slot_bw;
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

  ESP_LOGI(TAG, "I2S DUPLEX initialized (%s)", this->use_tdm_ref_ ? "TDM" : "standard");
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
      this->aec_ref_delay_ms_ > 0 && !this->use_stereo_aec_ref_ && !this->use_tdm_ref_) {
    this->speaker_ref_buffer_->reset();
    size_t delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;
    static constexpr uint8_t silence[64] = {};
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
  if (this->duplex_running_.load(std::memory_order_relaxed)) {
    ESP_LOGW(TAG, "Already running");
    return;
  }

  ESP_LOGI(TAG, "Starting duplex audio...");

  if (!this->init_i2s_duplex_()) {
    ESP_LOGE(TAG, "Failed to initialize I2S");
    return;
  }

  this->duplex_running_.store(true, std::memory_order_relaxed);
  this->task_exited_.store(false, std::memory_order_relaxed);
  this->has_i2s_error_.store(false, std::memory_order_relaxed);
  this->speaker_running_.store(this->tx_handle_ != nullptr, std::memory_order_relaxed);

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
  if (this->use_tdm_ref_) {
    ESP_LOGD(TAG, "TDM hardware reference - slot %u is echo ref", this->tdm_ref_slot_);
  }
#endif

  BaseType_t task_created = xTaskCreatePinnedToCore(
      audio_task,
      "i2s_duplex",
      this->task_stack_size_,
      this,
      this->task_priority_,
      &this->audio_task_handle_,
      this->task_core_ >= 0 ? this->task_core_ : tskNO_AFFINITY
  );

  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio task");
    this->duplex_running_.store(false, std::memory_order_relaxed);
    this->speaker_running_.store(false, std::memory_order_relaxed);
    this->deinit_i2s_();
    return;
  }

  ESP_LOGI(TAG, "Duplex audio started");
}

void I2SAudioDuplex::stop() {
  if (!this->duplex_running_.load(std::memory_order_relaxed)) {
    return;
  }

  ESP_LOGI(TAG, "Stopping duplex audio...");

  this->mic_ref_count_.store(0, std::memory_order_relaxed);
  this->speaker_running_.store(false, std::memory_order_relaxed);
  this->duplex_running_.store(false, std::memory_order_relaxed);

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
    while (!this->task_exited_.load(std::memory_order_relaxed) && wait_count < 50) {
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
  if (!this->duplex_running_.load(std::memory_order_relaxed)) {
    this->start();
  }
  this->mic_ref_count_.fetch_add(1, std::memory_order_relaxed);
}

void I2SAudioDuplex::stop_mic() {
  int prev = this->mic_ref_count_.fetch_sub(1, std::memory_order_relaxed);
  if (prev <= 1) {
    this->mic_ref_count_.store(0, std::memory_order_relaxed);
  }
}

void I2SAudioDuplex::start_speaker() {
  if (!this->duplex_running_.load(std::memory_order_relaxed)) {
    this->start();
  }
  this->speaker_running_.store(true, std::memory_order_relaxed);

  this->play_ref_decimator_.reset();

  // Request audio task to reset speaker buffer and prefill AEC reference.
  // Avoids concurrent ring buffer access (main thread vs audio task).
  this->request_ref_prefill_.store(true, std::memory_order_relaxed);
}

void I2SAudioDuplex::stop_speaker() {
  this->speaker_running_.store(false, std::memory_order_relaxed);
  // Request audio task to reset ring buffers (avoids concurrent access).
  this->request_speaker_reset_.store(true, std::memory_order_relaxed);
}

size_t I2SAudioDuplex::play(const uint8_t *data, size_t len, TickType_t ticks_to_wait) {
  if (!this->speaker_buffer_) {
    return 0;
  }

  // Data arrives at bus rate (e.g. 48kHz from mixer/resampler). Write directly.
  size_t written = this->speaker_buffer_->write_without_replacement((void *) data, len, ticks_to_wait, true);

  if (written > 0) {
    this->last_speaker_audio_ms_.store(millis(), std::memory_order_relaxed);
  }

#ifdef USE_ESP_AEC
  // Write bus-rate reference for AEC (mono mode only — stereo/TDM get ref from I2S RX).
  // Reference is decimated to output rate in audio_task before feeding to AEC.
  if (this->speaker_ref_buffer_ != nullptr && written > 0 && this->speaker_running_.load(std::memory_order_relaxed) &&
      !this->use_stereo_aec_ref_ && !this->use_tdm_ref_) {
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
  AudioTaskCtx ctx{};

  // ── Populate invariants ──
  ctx.ratio = this->decimation_ratio_;
  ctx.i2s_bps = (this->bits_per_sample_ > 16) ? 4 : 2;
  ctx.num_ch = this->num_channels_;
  ctx.use_stereo_aec_ref = this->use_stereo_aec_ref_;
  ctx.use_tdm_ref = this->use_tdm_ref_;
  ctx.ref_channel_right = this->ref_channel_right_;
  ctx.correct_dc_offset = this->correct_dc_offset_;
  ctx.tdm_total_slots = this->tdm_total_slots_;
  ctx.tdm_mic_slot = this->tdm_mic_slot_;
  ctx.tdm_ref_slot = this->tdm_ref_slot_;

  ESP_LOGD(TAG, "Audio task started (stereo=%s, tdm=%s, decimation=%ux)",
           ctx.use_stereo_aec_ref ? "YES" : "no",
           ctx.use_tdm_ref ? "YES" : "no", (unsigned)ctx.ratio);

  // Determine output frame size: use AEC's required chunk size if available, otherwise default.
  ctx.out_frame_size = DEFAULT_FRAME_SIZE;
#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr && this->aec_->is_initialized()) {
    ctx.out_frame_size = this->aec_->get_frame_size();
    uint32_t out_rate = this->get_output_sample_rate();
    ESP_LOGD(TAG, "AEC frame size: %u samples (%ums @ %uHz)",
             (unsigned)ctx.out_frame_size, (unsigned)(ctx.out_frame_size * 1000 / out_rate), (unsigned)out_rate);
  }
#endif

  // ── Frame sizing ──
  ctx.bus_frame_size = ctx.out_frame_size * ctx.ratio;
  ctx.out_frame_bytes = ctx.out_frame_size * sizeof(int16_t);
  ctx.bus_frame_bytes = ctx.bus_frame_size * sizeof(int16_t);
  ctx.aec_delay_bytes = (this->sample_rate_ * this->aec_ref_delay_ms_ / 1000) * BYTES_PER_SAMPLE;

  if (ctx.use_tdm_ref) {
    ctx.rx_frame_bytes = ctx.bus_frame_size * ctx.tdm_total_slots * ctx.i2s_bps;
  } else if (ctx.use_stereo_aec_ref) {
    ctx.rx_frame_bytes = ctx.bus_frame_size * 2 * ctx.i2s_bps;
  } else {
    ctx.rx_frame_bytes = ctx.bus_frame_size * ctx.i2s_bps;
  }

  // ── Buffer allocations ──
  // AEC buffers use 16-byte alignment (ESP-SR may use SIMD internally)
  static constexpr size_t AEC_ALIGN = 16;

  // ESP-IDF new I2S driver manages its own internal DMA ring buffers.
  // i2s_channel_read/write use memcpy to/from user buffers (verified in i2s_common.c:1337,1387).
  // User buffers do NOT need MALLOC_CAP_DMA — they can safely be in PSRAM.
  // With buffers_in_psram=true, all buffers go to PSRAM (~28KB internal heap saved).
  // Required for sr_low_cost AEC mode (512-sample frames = larger buffers).
  const uint32_t buf_caps = this->buffers_in_psram_ ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
  ctx.rx_buffer = static_cast<int16_t *>(
      heap_caps_malloc(ctx.rx_frame_bytes, buf_caps));

  ctx.mic_separate = (ctx.ratio > 1) || ctx.use_stereo_aec_ref || ctx.use_tdm_ref;
  ctx.mic_buffer = ctx.mic_separate
      ? static_cast<int16_t *>(heap_caps_aligned_alloc(AEC_ALIGN, ctx.out_frame_bytes, buf_caps))
      : ctx.rx_buffer;

  ctx.spk_buffer = static_cast<int16_t *>(
      heap_caps_malloc(ctx.bus_frame_size * ctx.num_ch * ctx.i2s_bps, buf_caps));

  if (ctx.use_stereo_aec_ref || ctx.use_tdm_ref) {
    ctx.spk_ref_buffer = static_cast<int16_t *>(
        heap_caps_aligned_alloc(AEC_ALIGN, ctx.out_frame_bytes, buf_caps));
  }

  if (ctx.use_stereo_aec_ref && ctx.ratio > 1) {
    ctx.deint_ref = static_cast<int16_t *>(heap_caps_malloc(ctx.bus_frame_bytes, buf_caps));
    ctx.deint_mic = static_cast<int16_t *>(heap_caps_malloc(ctx.bus_frame_bytes, buf_caps));
  }

  if (ctx.use_tdm_ref && ctx.ratio > 1) {
    ctx.tdm_deint_mic = static_cast<int16_t *>(heap_caps_malloc(ctx.bus_frame_bytes, buf_caps));
    ctx.tdm_deint_ref = static_cast<int16_t *>(heap_caps_malloc(ctx.bus_frame_bytes, buf_caps));
  }

  if (ctx.use_tdm_ref) {
    ctx.tdm_tx_frame_bytes = ctx.bus_frame_size * ctx.tdm_total_slots * ctx.i2s_bps;
    ctx.tdm_tx_buffer = static_cast<int16_t *>(
        heap_caps_malloc(ctx.tdm_tx_frame_bytes, buf_caps));
  }

#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr) {
    if (!ctx.spk_ref_buffer && !ctx.use_tdm_ref)
      ctx.spk_ref_buffer = static_cast<int16_t *>(
          heap_caps_aligned_alloc(AEC_ALIGN, ctx.out_frame_bytes, buf_caps));
    ctx.aec_output = static_cast<int16_t *>(
        heap_caps_aligned_alloc(AEC_ALIGN, ctx.out_frame_bytes, buf_caps));
    if (!ctx.use_stereo_aec_ref && !ctx.use_tdm_ref) {
      ctx.ref_bus_buffer = static_cast<int16_t *>(heap_caps_malloc(ctx.bus_frame_bytes, buf_caps));
    }
  }
#endif

  // ── Verify critical allocations ──
  auto alloc_fail = [this](const char *what) {
    ESP_LOGE(TAG, "Failed to allocate %s", what);
    this->has_i2s_error_.store(true, std::memory_order_relaxed);
    this->task_exited_.store(true, std::memory_order_relaxed);
  };
  if (!ctx.rx_buffer || !ctx.spk_buffer || (ctx.mic_separate && !ctx.mic_buffer)) {
    alloc_fail("audio buffers"); goto cleanup;
  }
  if (ctx.use_stereo_aec_ref && ctx.ratio > 1 && (!ctx.deint_ref || !ctx.deint_mic)) {
    alloc_fail("stereo decimation buffers"); goto cleanup;
  }
  if (ctx.use_tdm_ref && !ctx.tdm_tx_buffer) {
    alloc_fail("TDM TX buffer"); goto cleanup;
  }
  if (ctx.use_tdm_ref && ctx.ratio > 1 && (!ctx.tdm_deint_mic || !ctx.tdm_deint_ref)) {
    alloc_fail("TDM decimation buffers"); goto cleanup;
  }
#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr) {
    if (!ctx.aec_output) { alloc_fail("AEC output buffer"); goto cleanup; }
    if ((ctx.use_stereo_aec_ref || ctx.use_tdm_ref) && !ctx.spk_ref_buffer) {
      alloc_fail("AEC reference buffer"); goto cleanup;
    }
    if (!ctx.use_stereo_aec_ref && !ctx.use_tdm_ref && !ctx.ref_bus_buffer) {
      alloc_fail("AEC mono reference buffer"); goto cleanup;
    }
  }
#endif

  // ── Main loop ──
  while (this->duplex_running_.load(std::memory_order_relaxed)) {

    // Service ring buffer operations requested by main thread
    if (this->request_speaker_reset_.exchange(false, std::memory_order_relaxed)) {
      this->speaker_buffer_->reset();
      if (this->speaker_ref_buffer_)
        this->speaker_ref_buffer_->reset();
    }
    if (this->request_ref_prefill_.exchange(false, std::memory_order_relaxed)) {
      this->speaker_buffer_->reset();
      this->prefill_aec_ref_buffer_();
    }

    // Reset per-frame state
    ctx.output_buffer = nullptr;

    // Snapshot atomic state for this frame (avoids repeated .load() in sample loops)
    ctx.mic_attenuation = this->mic_attenuation_.load(std::memory_order_relaxed);
    ctx.mic_gain = this->mic_gain_.load(std::memory_order_relaxed);
    ctx.speaker_volume = this->speaker_volume_.load(std::memory_order_relaxed);
    ctx.speaker_running = this->speaker_running_.load(std::memory_order_relaxed);
    ctx.speaker_paused = this->speaker_paused_.load(std::memory_order_relaxed);
    ctx.mic_running = this->mic_ref_count_.load(std::memory_order_relaxed) > 0;

    this->process_rx_path_(ctx);

    // Snapshot AEC gate state right before AEC decision for timing precision
    ctx.aec_enabled = this->aec_enabled_.load(std::memory_order_relaxed);
    // aec_ref_volume only used in mono software-ref mode (not TDM, not stereo)
    if (!ctx.use_stereo_aec_ref && !ctx.use_tdm_ref) {
      ctx.aec_ref_volume = this->aec_ref_volume_.load(std::memory_order_relaxed);
    }
    ctx.now_ms = millis();

    this->process_aec_and_callbacks_(ctx);
    this->process_tx_path_(ctx);

    // Yield: I2S read/write already block on DMA, so taskYIELD suffices.
    if (ctx.consecutive_i2s_errors > 0) {
      delay(1);
    } else {
      taskYIELD();
    }
  }

  this->task_exited_.store(true, std::memory_order_relaxed);

cleanup:
  heap_caps_free(ctx.rx_buffer);
  if (ctx.mic_separate && ctx.mic_buffer) heap_caps_free(ctx.mic_buffer);
  heap_caps_free(ctx.spk_buffer);
  if (ctx.spk_ref_buffer) heap_caps_free(ctx.spk_ref_buffer);
  if (ctx.deint_ref) heap_caps_free(ctx.deint_ref);
  if (ctx.deint_mic) heap_caps_free(ctx.deint_mic);
  if (ctx.tdm_deint_mic) heap_caps_free(ctx.tdm_deint_mic);
  if (ctx.tdm_deint_ref) heap_caps_free(ctx.tdm_deint_ref);
  if (ctx.tdm_tx_buffer) heap_caps_free(ctx.tdm_tx_buffer);
  if (ctx.ref_bus_buffer) heap_caps_free(ctx.ref_bus_buffer);
  if (ctx.aec_output) heap_caps_free(ctx.aec_output);
  ESP_LOGI(TAG, "Audio task stopped");
}

// ════════════════════════════════════════════════════════════════════════════
// RX PATH: I2S read → deinterleave/decimate → mic_buffer + spk_ref_buffer
// ════════════════════════════════════════════════════════════════════════════
void I2SAudioDuplex::process_rx_path_(AudioTaskCtx &ctx) {
  if (!this->rx_handle_)
    return;

  size_t bytes_read;
  esp_err_t err = i2s_channel_read(this->rx_handle_, ctx.rx_buffer, ctx.rx_frame_bytes,
                                    &bytes_read, I2S_IO_TIMEOUT_MS);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
    if (++ctx.consecutive_i2s_errors > 100) {
      ESP_LOGE(TAG, "Persistent I2S read errors (%d)", ctx.consecutive_i2s_errors);
      this->has_i2s_error_.store(true, std::memory_order_relaxed);
      this->duplex_running_.store(false, std::memory_order_relaxed);
    }
    return;
  }
  if (err != ESP_OK || bytes_read != ctx.rx_frame_bytes)
    return;

  ctx.consecutive_i2s_errors = 0;

  // Convert 32-bit I2S samples to 16-bit for internal processing
  if (ctx.i2s_bps == 4) {
    auto *src32 = reinterpret_cast<int32_t *>(ctx.rx_buffer);
    size_t total_i2s_samples = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < total_i2s_samples; i++) {
      ctx.rx_buffer[i] = static_cast<int16_t>(src32[i] >> 16);
    }
  }

  ctx.output_buffer = ctx.mic_buffer;  // Default: no AEC processing

#if SOC_I2S_SUPPORTS_TDM
  if (ctx.use_tdm_ref) {
    const uint8_t ts = ctx.tdm_total_slots;
    const uint8_t ms = ctx.tdm_mic_slot;
    const uint8_t rs = ctx.tdm_ref_slot;
    if (ctx.ratio > 1) {
      for (size_t i = 0; i < ctx.bus_frame_size; i++) {
        ctx.tdm_deint_mic[i] = ctx.rx_buffer[i * ts + ms];
        ctx.tdm_deint_ref[i] = ctx.rx_buffer[i * ts + rs];
      }
      this->mic_decimator_.process(ctx.tdm_deint_mic, ctx.mic_buffer, ctx.bus_frame_size);
      this->ref_decimator_.process(ctx.tdm_deint_ref, ctx.spk_ref_buffer, ctx.bus_frame_size);
    } else {
      for (size_t i = 0; i < ctx.out_frame_size; i++) {
        ctx.mic_buffer[i] = ctx.rx_buffer[i * ts + ms];
        ctx.spk_ref_buffer[i] = ctx.rx_buffer[i * ts + rs];
      }
    }
  } else
#endif
  if (ctx.ratio > 1) {
    if (ctx.use_stereo_aec_ref) {
      const int ri = ctx.ref_channel_right ? 1 : 0;
      const int mi = ctx.ref_channel_right ? 0 : 1;
      for (size_t i = 0; i < ctx.bus_frame_size; i++) {
        ctx.deint_ref[i] = ctx.rx_buffer[i * 2 + ri];
        ctx.deint_mic[i] = ctx.rx_buffer[i * 2 + mi];
      }
      this->ref_decimator_.process(ctx.deint_ref, ctx.spk_ref_buffer, ctx.bus_frame_size);
      this->mic_decimator_.process(ctx.deint_mic, ctx.mic_buffer, ctx.bus_frame_size);
    } else {
      this->mic_decimator_.process(ctx.rx_buffer, ctx.mic_buffer, ctx.bus_frame_size);
    }
  } else {
    if (ctx.use_stereo_aec_ref) {
      const int ri = ctx.ref_channel_right ? 1 : 0;
      const int mi = ctx.ref_channel_right ? 0 : 1;
      for (size_t i = 0; i < ctx.out_frame_size; i++) {
        ctx.spk_ref_buffer[i] = ctx.rx_buffer[i * 2 + ri];
        ctx.mic_buffer[i] = ctx.rx_buffer[i * 2 + mi];
      }
    }
    // Mono without decimation: mic_buffer == rx_buffer (aliased), nothing to do
  }

  // DC offset correction (musicdsp.org DC-block in Q31, matches upstream)
  if (ctx.correct_dc_offset) {
    for (size_t i = 0; i < ctx.out_frame_size; i++) {
      int32_t input = (int32_t) ctx.mic_buffer[i] << 16;
      int32_t output = input - ctx.dc_prev_input + ctx.dc_prev_output - (ctx.dc_prev_output >> 10);
      ctx.dc_prev_input = input;
      ctx.dc_prev_output = output;
      ctx.mic_buffer[i] = static_cast<int16_t>(output >> 16);
    }
  }

  // Pre-AEC mic attenuation (snapshot value, no atomic load in loop)
  if (ctx.mic_attenuation != 1.0f) {
    for (size_t i = 0; i < ctx.out_frame_size; i++) {
      ctx.mic_buffer[i] = scale_sample(ctx.mic_buffer[i], ctx.mic_attenuation);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// AEC + CALLBACKS: raw callbacks → AEC processing → gain → post callbacks
// ════════════════════════════════════════════════════════════════════════════
void I2SAudioDuplex::process_aec_and_callbacks_(AudioTaskCtx &ctx) {
  if (!this->rx_handle_ || ctx.output_buffer == nullptr)
    return;

  // Raw mic callbacks: pre-AEC audio for MWW
  if (ctx.mic_running && !this->raw_mic_callbacks_.empty()) {
    for (auto &callback : this->raw_mic_callbacks_) {
      callback((const uint8_t *) ctx.mic_buffer, ctx.out_frame_bytes);
    }
  }

#ifdef USE_ESP_AEC
#if SOC_I2S_SUPPORTS_TDM
  if (ctx.use_tdm_ref && this->aec_ != nullptr && ctx.aec_enabled &&
      this->aec_->is_initialized() && ctx.spk_ref_buffer != nullptr && ctx.aec_output != nullptr) {
    // TDM: hardware-synced reference, no speaker gating needed.
    // TDM analog ref already reflects DAC volume — only match mic_attenuation.
    if (ctx.mic_attenuation != 1.0f) {
      for (size_t i = 0; i < ctx.out_frame_size; i++) {
        ctx.spk_ref_buffer[i] = scale_sample(ctx.spk_ref_buffer[i], ctx.mic_attenuation);
      }
    }
    this->aec_->process(ctx.mic_buffer, ctx.spk_ref_buffer, ctx.aec_output, ctx.out_frame_size);
    ctx.output_buffer = ctx.aec_output;
  } else
#endif
  if (!ctx.use_tdm_ref && this->aec_ != nullptr && ctx.aec_enabled && this->aec_->is_initialized() &&
      ctx.spk_ref_buffer != nullptr && ctx.aec_output != nullptr && ctx.speaker_running &&
      (ctx.now_ms - this->last_speaker_audio_ms_.load(std::memory_order_relaxed) <= AEC_ACTIVE_TIMEOUT_MS)) {

    // Mono mode: get AEC reference (direct from TX or ring buffer)
    if (!ctx.use_stereo_aec_ref) {
      if (this->direct_aec_ref_ != nullptr) {
        // Direct mode: use previous TX frame as reference (no ring buffer)
        if (this->direct_aec_ref_valid_) {
          this->play_ref_decimator_.process(this->direct_aec_ref_, ctx.spk_ref_buffer, ctx.bus_frame_size);
        } else {
          memset(ctx.spk_ref_buffer, 0, ctx.out_frame_bytes);
        }
      } else if (this->speaker_ref_buffer_ != nullptr) {
        // Ring buffer mode: read delayed reference
        size_t min_ref_bytes = ctx.aec_delay_bytes + ctx.bus_frame_bytes;
        size_t ref_available = this->speaker_ref_buffer_->available();
        if (ref_available >= min_ref_bytes && ctx.ref_bus_buffer != nullptr) {
          this->speaker_ref_buffer_->read((void *) ctx.ref_bus_buffer, ctx.bus_frame_bytes, 0);
          this->play_ref_decimator_.process(ctx.ref_bus_buffer, ctx.spk_ref_buffer, ctx.bus_frame_size);
        } else {
          memset(ctx.spk_ref_buffer, 0, ctx.out_frame_bytes);
        }
      } else {
        memset(ctx.spk_ref_buffer, 0, ctx.out_frame_bytes);
      }

      float ref_scale = ctx.aec_ref_volume * ctx.mic_attenuation;
      if (ref_scale != 1.0f) {
        for (size_t i = 0; i < ctx.out_frame_size; i++) {
          ctx.spk_ref_buffer[i] = scale_sample(ctx.spk_ref_buffer[i], ref_scale);
        }
      }
    }
    // Stereo mode: spk_ref_buffer already filled from deinterleave. Match mic_attenuation only.
    if (ctx.use_stereo_aec_ref && ctx.mic_attenuation != 1.0f) {
      for (size_t i = 0; i < ctx.out_frame_size; i++) {
        ctx.spk_ref_buffer[i] = scale_sample(ctx.spk_ref_buffer[i], ctx.mic_attenuation);
      }
    }

    this->aec_->process(ctx.mic_buffer, ctx.spk_ref_buffer, ctx.aec_output, ctx.out_frame_size);
    ctx.output_buffer = ctx.aec_output;
  }
#endif

  // Apply mic gain (snapshot value)
  if (ctx.mic_gain != 1.0f) {
    for (size_t i = 0; i < ctx.out_frame_size; i++) {
      ctx.output_buffer[i] = scale_sample(ctx.output_buffer[i], ctx.mic_gain);
    }
  }

  // Post-AEC callbacks (VA/STT)
  if (ctx.mic_running) {
    for (auto &callback : this->mic_callbacks_) {
      callback((const uint8_t *) ctx.output_buffer, ctx.out_frame_bytes);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// TX PATH: ring buffer read → volume → format expand → I2S write
// ════════════════════════════════════════════════════════════════════════════
void I2SAudioDuplex::process_tx_path_(AudioTaskCtx &ctx) {
  if (!this->tx_handle_)
    return;

  if (ctx.speaker_running) {
    size_t got = this->speaker_buffer_->read((void *) ctx.spk_buffer, ctx.bus_frame_bytes, 0);

    if (ctx.speaker_paused) {
      memset(ctx.spk_buffer, 0, ctx.bus_frame_bytes);
    } else if (got > 0) {
      if (ctx.speaker_volume != 1.0f) {
        size_t got_samples = got / sizeof(int16_t);
        for (size_t i = 0; i < got_samples; i++) {
          ctx.spk_buffer[i] = scale_sample(ctx.spk_buffer[i], ctx.speaker_volume);
        }
      }
      if (got < ctx.bus_frame_bytes) {
        memset(((uint8_t *) ctx.spk_buffer) + got, 0, ctx.bus_frame_bytes - got);
      }
    } else {
      memset(ctx.spk_buffer, 0, ctx.bus_frame_bytes);
    }
  } else {
    memset(ctx.spk_buffer, 0, ctx.bus_frame_bytes);
  }

  // Save post-volume TX data as direct AEC reference (used in next iteration)
#ifdef USE_ESP_AEC
  if (this->direct_aec_ref_ != nullptr) {
    memcpy(this->direct_aec_ref_, ctx.spk_buffer, ctx.bus_frame_size * sizeof(int16_t));
    this->direct_aec_ref_valid_ = ctx.speaker_running && !ctx.speaker_paused;
  }
#endif

  // Prepare TX: format expansion + TDM interleave
  const void *tx_data;
  size_t tx_bytes;
#if SOC_I2S_SUPPORTS_TDM
  if (ctx.use_tdm_ref && ctx.tdm_tx_buffer != nullptr) {
    if (ctx.i2s_bps == 4) {
      auto *tdm32 = reinterpret_cast<int32_t *>(ctx.tdm_tx_buffer);
      memset(tdm32, 0, ctx.tdm_tx_frame_bytes);
      for (size_t i = 0; i < ctx.bus_frame_size; i++) {
        tdm32[i * ctx.tdm_total_slots] = static_cast<int32_t>(ctx.spk_buffer[i]) << 16;
      }
    } else {
      memset(ctx.tdm_tx_buffer, 0, ctx.tdm_tx_frame_bytes);
      for (size_t i = 0; i < ctx.bus_frame_size; i++) {
        ctx.tdm_tx_buffer[i * ctx.tdm_total_slots] = ctx.spk_buffer[i];
      }
    }
    tx_data = ctx.tdm_tx_buffer;
    tx_bytes = ctx.tdm_tx_frame_bytes;
  } else
#endif
  {
    if (ctx.num_ch == 2) {
      for (int i = static_cast<int>(ctx.bus_frame_size) - 1; i >= 0; i--) {
        ctx.spk_buffer[i * 2 + 1] = ctx.spk_buffer[i];
        ctx.spk_buffer[i * 2] = ctx.spk_buffer[i];
      }
    }
    size_t total_tx_samples = ctx.bus_frame_size * ctx.num_ch;
    if (ctx.i2s_bps == 4) {
      auto *dst32 = reinterpret_cast<int32_t *>(ctx.spk_buffer);
      for (int i = static_cast<int>(total_tx_samples) - 1; i >= 0; i--) {
        dst32[i] = static_cast<int32_t>(ctx.spk_buffer[i]) << 16;
      }
    }
    tx_data = ctx.spk_buffer;
    tx_bytes = total_tx_samples * ctx.i2s_bps;
  }

  size_t bytes_written;
  esp_err_t err = i2s_channel_write(this->tx_handle_, tx_data, tx_bytes, &bytes_written, I2S_IO_TIMEOUT_MS);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
    if (++ctx.consecutive_i2s_errors > 100) {
      ESP_LOGE(TAG, "Persistent I2S write errors (%d)", ctx.consecutive_i2s_errors);
      this->has_i2s_error_.store(true, std::memory_order_relaxed);
      this->duplex_running_.store(false, std::memory_order_relaxed);
    }
  } else if (err == ESP_OK) {
    ctx.consecutive_i2s_errors = 0;
  }

  // Report frames played (for mixer pending_playback tracking)
  if (err == ESP_OK && bytes_written > 0 && !this->speaker_output_callbacks_.empty()) {
    uint32_t frames_played = ctx.use_tdm_ref
        ? bytes_written / (ctx.tdm_total_slots * ctx.i2s_bps)
        : bytes_written / (ctx.num_ch * ctx.i2s_bps);
    int64_t timestamp = esp_timer_get_time();
    for (auto &cb : this->speaker_output_callbacks_) {
      cb(frames_played, timestamp);
    }
  }
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
