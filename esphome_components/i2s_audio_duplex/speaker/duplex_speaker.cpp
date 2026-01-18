#include "duplex_speaker.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio_duplex {

static const char *const TAG = "i2s_duplex.spk";

void I2SAudioDuplexSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Duplex Speaker...");

  // Configure audio stream info for 16-bit mono PCM
  // AudioStreamInfo constructor: (bits_per_sample, channels, sample_rate)
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, this->parent_->get_sample_rate());
}

void I2SAudioDuplexSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "I2S Audio Duplex Speaker:");
  ESP_LOGCONFIG(TAG, "  Sample Rate: %u Hz", this->parent_->get_sample_rate());
  ESP_LOGCONFIG(TAG, "  Bits Per Sample: 16");
  ESP_LOGCONFIG(TAG, "  Channels: 1 (mono)");
}

void I2SAudioDuplexSpeaker::start() {
  if (this->state_ != speaker::STATE_STOPPED) {
    ESP_LOGW(TAG, "Speaker already running or starting");
    return;
  }

  ESP_LOGI(TAG, "Starting speaker...");
  this->state_ = speaker::STATE_STARTING;

  // Start the parent duplex component (handles both mic and speaker)
  this->parent_->start();

  this->state_ = speaker::STATE_RUNNING;
  ESP_LOGI(TAG, "Speaker started");
}

void I2SAudioDuplexSpeaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED ||
      this->state_ == speaker::STATE_STOPPING) {
    return;
  }

  ESP_LOGI(TAG, "Stopping speaker...");
  this->state_ = speaker::STATE_STOPPING;

  // Stop the parent duplex component
  this->parent_->stop();

  this->state_ = speaker::STATE_STOPPED;
  ESP_LOGI(TAG, "Speaker stopped");
}

void I2SAudioDuplexSpeaker::finish() {
  // Wait for buffer to drain, then stop
  ESP_LOGI(TAG, "Finishing speaker (waiting for buffer to drain)...");

  // Wait up to 1 second for buffer to drain
  int wait_count = 0;
  while (this->has_buffered_data() && wait_count < 100) {
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_count++;
  }

  this->stop();
}

size_t I2SAudioDuplexSpeaker::play(const uint8_t *data, size_t length) {
  return this->play(data, length, 0);  // Non-blocking by default
}

size_t I2SAudioDuplexSpeaker::play(const uint8_t *data, size_t length,
                                    TickType_t ticks_to_wait) {
  if (this->state_ != speaker::STATE_RUNNING) {
    this->start();
  }

  // Delegate to parent's play method
  return this->parent_->play(data, length, ticks_to_wait);
}

bool I2SAudioDuplexSpeaker::has_buffered_data() const {
  return this->parent_->get_speaker_buffer_available() > 0;
}

void I2SAudioDuplexSpeaker::set_volume(float volume) {
  // Call base class implementation
  speaker::Speaker::set_volume(volume);

  // Set volume on parent duplex component
  if (!this->mute_state_) {
    this->parent_->set_speaker_volume(volume);
  }
}

void I2SAudioDuplexSpeaker::set_mute_state(bool mute_state) {
  // Call base class implementation
  speaker::Speaker::set_mute_state(mute_state);

  // When muted, set volume to 0; when unmuted, restore volume
  if (mute_state) {
    this->parent_->set_speaker_volume(0.0f);
  } else {
    this->parent_->set_speaker_volume(this->volume_);
  }
}

}  // namespace i2s_audio_duplex
}  // namespace esphome

#endif  // USE_ESP32
