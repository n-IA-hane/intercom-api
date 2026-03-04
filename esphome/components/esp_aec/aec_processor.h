#pragma once

#include <cstdint>

namespace esphome {

/// Abstract interface for acoustic echo cancellation processors.
/// Implemented by esp_aec::EspAec (single-mic AEC) and esp_afe::EspAfe (full AFE pipeline).
/// i2s_audio_duplex uses this interface so it works with any AEC backend.
class AecProcessor {
 public:
  virtual ~AecProcessor() = default;

  virtual bool is_initialized() const = 0;

  /// Frame size in samples (not bytes). Typically 512 samples = 32ms at 16kHz.
  virtual int get_frame_size() const = 0;

  /// Number of microphone channels this processor expects. Default: 1 (single-mic AEC).
  virtual int get_mic_num() const { return 1; }

  /// Process one AEC frame (single-mic).
  virtual void process(const int16_t *mic_in, const int16_t *ref_in, int16_t *out, int frame_size) = 0;

  /// Process one AEC frame (multi-mic). Default: uses first mic channel.
  virtual void process_multi(const int16_t *const *mic_channels, int num_mics,
                             const int16_t *ref_in, int16_t *out, int frame_size) {
    if (num_mics > 0)
      process(mic_channels[0], ref_in, out, frame_size);
  }
};

}  // namespace esphome
