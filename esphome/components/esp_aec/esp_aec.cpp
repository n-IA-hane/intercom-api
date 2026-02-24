#include "esp_aec.h"

#ifdef USE_ESP32

#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace esp_aec {

static const char *const TAG = "esp_aec";

void EspAec::setup() {
  ESP_LOGI(TAG, "Initializing AEC...");

  // Create AEC instance
  // aec_create(sample_rate, filter_length, channel_num, mode)
  this->handle_ = aec_create(this->sample_rate_, this->filter_length_, 1, this->mode_);

  if (this->handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create AEC instance");
    this->mark_failed();
    return;
  }

  this->cached_frame_size_ = aec_get_chunksize(this->handle_);
  ESP_LOGI(TAG, "AEC initialized: sample_rate=%d, filter_length=%d, frame_size=%d samples (%dms)",
           this->sample_rate_, this->filter_length_, this->cached_frame_size_,
           this->cached_frame_size_ * 1000 / this->sample_rate_);
}

EspAec::~EspAec() {
  if (this->handle_ != nullptr) {
    aec_destroy(this->handle_);
    this->handle_ = nullptr;
  }
}

void EspAec::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP AEC (ESP-SR):");
  ESP_LOGCONFIG(TAG, "  Sample Rate: %d Hz", this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  Filter Length: %d", this->filter_length_);
  ESP_LOGCONFIG(TAG, "  Frame Size: %d samples", this->get_frame_size());
  ESP_LOGCONFIG(TAG, "  Initialized: %s", this->is_initialized() ? "YES" : "NO");
}

int EspAec::get_frame_size() const {
  return this->cached_frame_size_;
}

void EspAec::process(const int16_t *mic_in, const int16_t *ref_in, int16_t *out, int frame_size) {
  if (this->handle_ == nullptr) {
    // Passthrough if not initialized
    memcpy(out, mic_in, frame_size * sizeof(int16_t));
    return;
  }

  // Note: aec_process expects non-const pointers but doesn't modify input
  // Cast away const for API compatibility
  aec_process(this->handle_,
              const_cast<int16_t *>(mic_in),
              const_cast<int16_t *>(ref_in),
              out);
}

}  // namespace esp_aec
}  // namespace esphome

#endif  // USE_ESP32
