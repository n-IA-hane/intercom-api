#include "duplex_speaker.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio_duplex {

static const UBaseType_t MAX_LISTENERS = 16;
static const char *const TAG = "i2s_duplex.spk";

void I2SAudioDuplexSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Duplex Speaker...");

  this->active_listeners_semaphore_ = xSemaphoreCreateCounting(MAX_LISTENERS, MAX_LISTENERS);
  if (this->active_listeners_semaphore_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create semaphore");
    this->mark_failed();
    return;
  }

  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, this->parent_->get_sample_rate());

  // Forward frame-played notifications from I2S audio task to mixer callbacks.
  // Without this, mixer source speakers can't track pending_playback_frames.
  this->parent_->add_speaker_output_callback([this](uint32_t frames, int64_t timestamp) {
    this->audio_output_callback_.call(frames, timestamp);
  });
}

void I2SAudioDuplexSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "I2S Audio Duplex Speaker:");
  ESP_LOGCONFIG(TAG, "  Sample Rate: %u Hz", this->parent_->get_sample_rate());
  ESP_LOGCONFIG(TAG, "  Bits Per Sample: 16");
  ESP_LOGCONFIG(TAG, "  Channels: 1 (mono)");
}

void I2SAudioDuplexSpeaker::start() {
  if (this->is_failed())
    return;

  // Idempotent: register listener only once per stream session.
  bool expected = false;
  if (!this->listener_registered_.compare_exchange_strong(expected, true))
    return;

  if (xSemaphoreTake(this->active_listeners_semaphore_, 0) != pdTRUE) {
    this->listener_registered_.store(false);
    ESP_LOGW(TAG, "No free semaphore slots");
    return;
  }
}

void I2SAudioDuplexSpeaker::stop() {
  if (this->is_failed())
    return;

  if (!this->listener_registered_.exchange(false))
    return;

  xSemaphoreGive(this->active_listeners_semaphore_);
}

void I2SAudioDuplexSpeaker::finish() {
  int wait_count = 0;
  while (this->has_buffered_data() && wait_count < 100) {
    vTaskDelay(pdMS_TO_TICKS(10));
    wait_count++;
  }

  this->stop();
}

size_t I2SAudioDuplexSpeaker::play(const uint8_t *data, size_t length) {
  return this->play(data, length, 0);
}

size_t I2SAudioDuplexSpeaker::play(const uint8_t *data, size_t length,
                                    TickType_t ticks_to_wait) {
  if (this->state_ != speaker::STATE_RUNNING) {
    this->start();
  }

  return this->parent_->play(data, length, ticks_to_wait);
}

bool I2SAudioDuplexSpeaker::has_buffered_data() const {
  return this->parent_->get_speaker_buffer_available() > 0;
}

void I2SAudioDuplexSpeaker::set_volume(float volume) {
  speaker::Speaker::set_volume(volume);

  if (!this->mute_state_) {
    this->parent_->set_speaker_volume(volume);
  }
}

void I2SAudioDuplexSpeaker::set_mute_state(bool mute_state) {
  speaker::Speaker::set_mute_state(mute_state);

  if (mute_state) {
    this->parent_->set_speaker_volume(0.0f);
  } else {
    this->parent_->set_speaker_volume(this->volume_);
  }
}

void I2SAudioDuplexSpeaker::loop() {
  UBaseType_t count = uxSemaphoreGetCount(this->active_listeners_semaphore_);

  if ((count < MAX_LISTENERS) && (this->state_ == speaker::STATE_STOPPED)) {
    this->state_ = speaker::STATE_STARTING;
  }

  if ((count == MAX_LISTENERS) && (this->state_ == speaker::STATE_RUNNING)) {
    this->state_ = speaker::STATE_STOPPING;
  }

  switch (this->state_) {
    case speaker::STATE_STARTING:
      if (this->status_has_error()) {
        break;
      }
      this->parent_->start_speaker();
      this->state_ = speaker::STATE_RUNNING;
      break;

    case speaker::STATE_RUNNING:
      break;

    case speaker::STATE_STOPPING:
      this->parent_->stop_speaker();
      this->state_ = speaker::STATE_STOPPED;
      break;

    case speaker::STATE_STOPPED:
      break;
  }
}

}  // namespace i2s_audio_duplex
}  // namespace esphome

#endif  // USE_ESP32
