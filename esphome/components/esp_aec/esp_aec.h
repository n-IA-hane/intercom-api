#pragma once

#include "aec_processor.h"
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

  ~EspAec() override;

 protected:
  aec_handle_t *handle_{nullptr};
  int sample_rate_{16000};
  int filter_length_{4};
  int cached_frame_size_{512};
  aec_mode_t mode_{AEC_MODE_VOIP_LOW_COST};
};

}  // namespace esp_aec
}  // namespace esphome

#endif  // USE_ESP32
