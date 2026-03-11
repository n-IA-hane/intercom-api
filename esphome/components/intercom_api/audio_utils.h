#pragma once

#include <cstdint>

namespace esphome {

#ifndef ESPHOME_SCALE_SAMPLE_DEFINED
#define ESPHOME_SCALE_SAMPLE_DEFINED
// Scale a 16-bit PCM sample by a float gain with saturation clamping.
// Supports gains > 1.0 (amplification) unlike ESPHome's Q15 scale_audio_samples().
// On ESP32-S3 (hardware FPU) this compiles to a single MADD.S instruction.
static inline int16_t scale_sample(int16_t sample, float gain) {
  int32_t s = static_cast<int32_t>(sample * gain);
  if (s > 32767) return 32767;
  if (s < -32768) return -32768;
  return static_cast<int16_t>(s);
}
#endif

}  // namespace esphome
