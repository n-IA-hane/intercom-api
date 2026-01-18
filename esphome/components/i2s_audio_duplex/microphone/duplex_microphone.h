#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/microphone/microphone.h"
#include "../i2s_audio_duplex.h"

namespace esphome {
namespace i2s_audio_duplex {

class I2SAudioDuplexMicrophone : public microphone::Microphone,
                                  public Component,
                                  public Parented<I2SAudioDuplex> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // microphone::Microphone interface
  void start() override;
  void stop() override;

 protected:
  void on_audio_data_(const uint8_t *data, size_t len);
};

}  // namespace i2s_audio_duplex
}  // namespace esphome

#endif  // USE_ESP32
