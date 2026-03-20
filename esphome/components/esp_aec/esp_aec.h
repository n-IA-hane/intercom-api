#pragma once

#include "aec_processor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#ifdef USE_ESP32

#include <esp_aec.h>

namespace esphome {
namespace esp_aec {

class EspAec : public Component, public AecProcessor {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  // Configuration setters (called from Python)
  void set_sample_rate(int sample_rate) { this->sample_rate_ = sample_rate; }
  void set_filter_length(int filter_length) { this->filter_length_ = filter_length; }
  void set_mode(int mode) { this->mode_ = static_cast<aec_mode_t>(mode); }

  // AecProcessor interface
  bool is_initialized() const override { return this->handle_ != nullptr; }
  int get_frame_size() const override;
  void process(const int16_t *mic_in, const int16_t *ref_in, int16_t *out, int frame_size) override;

  /// Destroy and recreate AEC with a new mode. Returns true on success.
  /// Caller must stop audio processing before calling this (frame size may change).
  bool reinit(aec_mode_t new_mode);
  /// Reinit by mode name string (e.g. "sr_low_cost", "voip_high_perf").
  /// Returns true on success. Unknown names are ignored (returns false).
  bool reinit_by_name(const std::string &name);
  aec_mode_t get_mode() const { return this->mode_; }

  ~EspAec() override;

 protected:
  aec_handle_t *handle_{nullptr};
  int sample_rate_{16000};
  int filter_length_{4};
  int cached_frame_size_{512};  // Overwritten by aec_get_chunksize() in setup()
  aec_mode_t mode_{AEC_MODE_SR_LOW_COST};
};

// Action: esp_aec.set_mode
template<typename... Ts>
class SetModeAction : public Action<Ts...>, public Parented<EspAec> {
 public:
  TEMPLATABLE_VALUE(std::string, mode)
  void play(const Ts &...x) override {
    this->parent_->reinit_by_name(this->mode_.value(x...));
  }
};

}  // namespace esp_aec
}  // namespace esphome

#endif  // USE_ESP32
