#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

#include <driver/i2s_std.h>
#include <driver/i2s_tdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

// Forward declare AEC
namespace esphome {
namespace esp_aec {
class EspAec;
}  // namespace esp_aec
}  // namespace esphome

namespace esphome {
namespace i2s_audio_duplex {

// Maximum listener count for microphone/speaker reference counting
static constexpr UBaseType_t MAX_LISTENERS = 16;

// Callback type for mic data: receives raw PCM samples (pointer + length, zero-copy)
using MicDataCallback = std::function<void(const uint8_t *data, size_t len)>;
// Callback type for speaker output: reports frames played and timestamp (for mixer pending_playback tracking)
using SpeakerOutputCallback = std::function<void(uint32_t frames, int64_t timestamp)>;

// FIR coefficients: 32-tap (31 original + 1 zero pad), cutoff=7500Hz, fs=48kHz, Kaiser beta=8.0
// Unity DC gain, ~60dB stopband attenuation, symmetric (linear phase)
// Padded to power-of-2 so modulo can use bitmask (& 0x1F) instead of division
static constexpr size_t FIR_NUM_TAPS = 32;
static constexpr float FIR_COEFFS[FIR_NUM_TAPS] = {
    4.1270231666e-05f, 2.1633893589e-04f, 1.2531119530e-04f, -9.9999988238e-04f,
    -2.6821920740e-03f, -1.8518117881e-03f, 4.4563387256e-03f, 1.2653483833e-02f,
    1.0683467077e-02f, -1.0893520506e-02f, -4.0743026823e-02f, -4.2934182572e-02f,
    1.7799016112e-02f, 1.3755146771e-01f, 2.6031620059e-01f, 3.1252367847e-01f,
    2.6031620059e-01f, 1.3755146771e-01f, 1.7799016112e-02f, -4.2934182572e-02f,
    -4.0743026823e-02f, -1.0893520506e-02f, 1.0683467077e-02f, 1.2653483833e-02f,
    4.4563387256e-03f, -1.8518117881e-03f, -2.6821920740e-03f, -9.9999988238e-04f,
    1.2531119530e-04f, 2.1633893589e-04f, 4.1270231666e-05f, 0.0f,
};

// Lightweight FIR decimator: consumes (in_count) samples at high rate,
// produces (in_count / ratio) samples at low rate.
// Uses float accumulation for robustness (ESP32-S3 has hardware FPU).
// When ratio == 1, process() is a simple memcpy (zero overhead for legacy configs).
class FirDecimator {
 public:
  void init(uint32_t ratio) {
    this->ratio_ = ratio;
    this->reset();
  }

  void reset() {
    memset(this->delay_line_, 0, sizeof(this->delay_line_));
    this->delay_pos_ = 0;
  }

  // Decimate in_count input samples to (in_count / ratio) output samples.
  // in_count MUST be a multiple of ratio.
  void process(const int16_t *in, int16_t *out, size_t in_count) {
    if (this->ratio_ <= 1) {
      memcpy(out, in, in_count * sizeof(int16_t));
      return;
    }

    size_t out_count = in_count / this->ratio_;
    for (size_t o = 0; o < out_count; o++) {
      // Push ratio_ new samples into the delay line
      for (uint32_t r = 0; r < this->ratio_; r++) {
        this->delay_line_[this->delay_pos_] = static_cast<float>(*in++);
        this->delay_pos_ = (this->delay_pos_ + 1) & (FIR_NUM_TAPS - 1);
      }

      // Convolve: FIR filter output
      float acc = 0.0f;
      uint32_t idx = this->delay_pos_;
      for (size_t t = 0; t < FIR_NUM_TAPS; t++) {
        acc += this->delay_line_[idx] * FIR_COEFFS[t];
        idx = (idx + 1) & (FIR_NUM_TAPS - 1);
      }

      // Clamp to int16 range
      if (acc > 32767.0f) acc = 32767.0f;
      if (acc < -32768.0f) acc = -32768.0f;
      out[o] = static_cast<int16_t>(acc);
    }
  }

 private:
  uint32_t ratio_{1};
  float delay_line_[FIR_NUM_TAPS]{};
  uint32_t delay_pos_{0};
};

class I2SAudioDuplex : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Pin setters
  void set_lrclk_pin(int pin) { this->lrclk_pin_ = pin; }
  void set_bclk_pin(int pin) { this->bclk_pin_ = pin; }
  void set_mclk_pin(int pin) { this->mclk_pin_ = pin; }
  void set_din_pin(int pin) { this->din_pin_ = pin; }
  void set_dout_pin(int pin) { this->dout_pin_ = pin; }
  void set_sample_rate(uint32_t rate) { this->sample_rate_ = rate; }
  void set_output_sample_rate(uint32_t rate) { this->output_sample_rate_ = rate; }

  // AEC setter
  void set_aec(esp_aec::EspAec *aec);
  void set_aec_enabled(bool enabled) { this->aec_enabled_ = enabled; }
  bool is_aec_enabled() const { return this->aec_enabled_; }

  // Volume control (0.0 - 1.0)
  void set_mic_gain(float gain) { this->mic_gain_ = gain; }
  float get_mic_gain() const { return this->mic_gain_; }

  // Pre-AEC mic attenuation - for hot mics like ES8311 that overdrive
  void set_mic_attenuation(float atten) { this->mic_attenuation_ = atten; }
  float get_mic_attenuation() const { return this->mic_attenuation_; }
  void set_speaker_volume(float volume) { this->speaker_volume_ = volume; }
  float get_speaker_volume() const { return this->speaker_volume_; }

  // AEC reference volume - for codecs with hardware volume (ES8311)
  void set_aec_reference_volume(float volume) { this->aec_ref_volume_ = volume; }
  float get_aec_reference_volume() const { return this->aec_ref_volume_; }

  // AEC reference delay - acoustic path delay in milliseconds
  void set_aec_reference_delay_ms(uint32_t delay_ms) { this->aec_ref_delay_ms_ = delay_ms; }
  uint32_t get_aec_reference_delay_ms() const { return this->aec_ref_delay_ms_; }

  // ES8311 Digital Feedback mode: RX is stereo with L=DAC(ref), R=ADC(mic)
  void set_use_stereo_aec_reference(bool use) { this->use_stereo_aec_ref_ = use; }
  bool get_use_stereo_aec_reference() const { return this->use_stereo_aec_ref_; }

  // Reference channel selection: false=left (default), true=right
  void set_reference_channel_right(bool right) { this->ref_channel_right_ = right; }
  bool get_reference_channel_right() const { return this->ref_channel_right_; }

  // TDM hardware reference: ES7210 in TDM mode with one slot carrying DAC feedback
  void set_use_tdm_reference(bool use) { this->use_tdm_ref_ = use; }
  void set_tdm_total_slots(uint8_t n) { this->tdm_total_slots_ = n; }
  void set_tdm_mic_slot(uint8_t slot) { this->tdm_mic_slot_ = slot; }
  void set_tdm_ref_slot(uint8_t slot) { this->tdm_ref_slot_ = slot; }

  // Microphone interface
  void add_mic_data_callback(MicDataCallback callback) { this->mic_callbacks_.push_back(callback); }
  void add_raw_mic_data_callback(MicDataCallback callback) { this->raw_mic_callbacks_.push_back(callback); }
  void start_mic();
  void stop_mic();
  bool is_mic_running() const { return this->mic_ref_count_.load() > 0; }

  // Speaker interface — data arrives at bus rate (from mixer/resampler)
  size_t play(const uint8_t *data, size_t len, TickType_t ticks_to_wait = portMAX_DELAY);
  void start_speaker();
  void stop_speaker();
  bool is_speaker_running() const { return this->speaker_running_; }
  void set_speaker_paused(bool paused) { this->speaker_paused_ = paused; }
  bool is_speaker_paused() const { return this->speaker_paused_; }

  // Full duplex control
  void start();  // Start both mic and speaker
  void stop();   // Stop both

  bool is_running() const { return this->duplex_running_; }
  bool has_i2s_error() const { return this->has_i2s_error_; }

  // Speaker output callback registration (for mixer pending_playback_frames tracking)
  void add_speaker_output_callback(SpeakerOutputCallback callback) {
    this->speaker_output_callbacks_.push_back(std::move(callback));
  }

  // Getters for platform wrappers
  // get_sample_rate() returns the I2S bus rate (used by speaker for audio_stream_info)
  uint32_t get_sample_rate() const { return this->sample_rate_; }
  // get_output_sample_rate() returns the decimated rate for mic consumers (MWW/AEC/VA/intercom)
  uint32_t get_output_sample_rate() const {
    return this->output_sample_rate_ > 0 ? this->output_sample_rate_ : this->sample_rate_;
  }
  size_t get_speaker_buffer_available() const;
  size_t get_speaker_buffer_size() const;

 protected:
  bool init_i2s_duplex_();
  void deinit_i2s_();
  void prefill_aec_ref_buffer_();

  static void audio_task(void *param);
  void audio_task_();

  // Pin configuration
  int lrclk_pin_{-1};
  int bclk_pin_{-1};
  int mclk_pin_{-1};
  int din_pin_{-1};   // Mic data in
  int dout_pin_{-1};  // Speaker data out

  uint32_t sample_rate_{16000};
  uint32_t output_sample_rate_{0};     // 0 = use sample_rate_ (no decimation)
  uint32_t decimation_ratio_{1};       // sample_rate_ / output_sample_rate_ (computed in setup)

  // FIR decimators for mic path
  FirDecimator mic_decimator_;
  FirDecimator ref_decimator_;          // Stereo mode: RX L channel ref
  FirDecimator play_ref_decimator_;     // Mono mode: bus-rate ref from play() decimated in audio_task

  // I2S handles - BOTH created from single channel for duplex
  i2s_chan_handle_t tx_handle_{nullptr};
  i2s_chan_handle_t rx_handle_{nullptr};

  // State
  std::atomic<bool> duplex_running_{false};
  std::atomic<int> mic_ref_count_{0};  // Reference-counted mic (multiple microphone instances)
  std::atomic<bool> speaker_running_{false};
  std::atomic<bool> speaker_paused_{false};
  TaskHandle_t audio_task_handle_{nullptr};

  // Mic data callbacks
  std::vector<MicDataCallback> mic_callbacks_;       // Post-AEC (for VA/STT)
  std::vector<MicDataCallback> raw_mic_callbacks_;   // Pre-AEC (for MWW)

  // Speaker output callbacks (for mixer pending_playback_frames tracking)
  std::vector<SpeakerOutputCallback> speaker_output_callbacks_;

  // Speaker ring buffer — stores data at bus rate (sample_rate_)
  std::unique_ptr<RingBuffer> speaker_buffer_;
  size_t speaker_buffer_size_{0};  // Actual allocated size (scales with decimation_ratio_)

  // AEC support
  esp_aec::EspAec *aec_{nullptr};
  std::atomic<bool> aec_enabled_{false};  // Runtime toggle (only enabled when aec_ is set)
  std::unique_ptr<RingBuffer> speaker_ref_buffer_;  // Reference for AEC (bus rate in mono mode)

  // Volume control
  float mic_gain_{1.0f};         // 0.0 - 2.0 (1.0 = unity gain, applied AFTER AEC)
  float mic_attenuation_{1.0f};  // Pre-AEC attenuation for hot mics (0.1 = -20dB, applied BEFORE AEC)
  float speaker_volume_{1.0f};   // 0.0 - 1.0 (for digital volume, keep 1.0 if codec has hardware volume)
  float aec_ref_volume_{1.0f};   // AEC reference scaling (set to codec's output volume for proper echo matching)
  uint32_t aec_ref_delay_ms_{80}; // AEC reference delay in ms (80 for separate I2S, 20-40 for ES8311)
  bool use_stereo_aec_ref_{false}; // ES8311 digital feedback: RX stereo with L=ref, R=mic
  bool ref_channel_right_{false};  // Which channel is AEC reference: false=L, true=R

  // TDM hardware reference (ES7210 in TDM mode)
  bool use_tdm_ref_{false};
  uint8_t tdm_total_slots_{4};
  uint8_t tdm_mic_slot_{0};    // TDM slot index for voice mic
  uint8_t tdm_ref_slot_{1};    // TDM slot index for AEC reference

  // AEC gating: only run echo canceller while speaker has recent real audio.
  uint32_t last_speaker_audio_ms_{0};
  static constexpr uint32_t AEC_ACTIVE_TIMEOUT_MS{250};

  // Error propagation: set by audio_task_ on persistent I2S failures
  std::atomic<bool> has_i2s_error_{false};

};

}  // namespace i2s_audio_duplex
}  // namespace esphome

#endif  // USE_ESP32
