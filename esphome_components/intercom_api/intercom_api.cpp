#include "intercom_api.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>

namespace esphome {
namespace intercom_api {

static const char *TAG = "intercom_api";

void IntercomApi::setup() {
  ESP_LOGI(TAG, "Setting up Intercom API...");

  // Create mutexes
  this->client_mutex_ = xSemaphoreCreateMutex();
  this->mic_mutex_ = xSemaphoreCreateMutex();
  this->speaker_mutex_ = xSemaphoreCreateMutex();
  this->send_mutex_ = xSemaphoreCreateMutex();

  if (!this->client_mutex_ || !this->mic_mutex_ || !this->speaker_mutex_ || !this->send_mutex_) {
    ESP_LOGE(TAG, "Failed to create mutexes");
    this->mark_failed();
    return;
  }

  // Create speaker stop semaphore (for single-owner speaker model)
  this->speaker_stopped_sem_ = xSemaphoreCreateBinary();
  if (!this->speaker_stopped_sem_) {
    ESP_LOGE(TAG, "Failed to create speaker semaphore");
    this->mark_failed();
    return;
  }

  // Allocate ring buffers
  this->mic_buffer_ = RingBuffer::create(TX_BUFFER_SIZE);
  this->speaker_buffer_ = RingBuffer::create(RX_BUFFER_SIZE);

  if (!this->mic_buffer_ || !this->speaker_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate ring buffers");
    this->mark_failed();
    return;
  }

  // Allocate frame buffers
  this->tx_buffer_ = (uint8_t *)heap_caps_malloc(MAX_MESSAGE_SIZE, MALLOC_CAP_INTERNAL);
  this->rx_buffer_ = (uint8_t *)heap_caps_malloc(MAX_MESSAGE_SIZE, MALLOC_CAP_INTERNAL);
  this->audio_tx_buffer_ = (uint8_t *)heap_caps_malloc(MAX_MESSAGE_SIZE, MALLOC_CAP_INTERNAL);

  if (!this->tx_buffer_ || !this->rx_buffer_ || !this->audio_tx_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate frame buffers");
    this->mark_failed();
    return;
  }

  // Setup microphone callback
#ifdef USE_MICROPHONE
  if (this->microphone_ != nullptr) {
    this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) {
      this->on_microphone_data_(data.data(), data.size());
    });
  }
#endif

#ifdef USE_ESP_AEC
  // Initialize AEC if configured
  if (this->aec_ != nullptr && this->aec_->is_initialized()) {
    this->aec_frame_samples_ = this->aec_->get_frame_size();
    if (this->aec_frame_samples_ <= 0 || this->aec_frame_samples_ > 1024) {
      ESP_LOGW(TAG, "AEC frame_size invalid (%d) -> disabled", this->aec_frame_samples_);
      this->aec_enabled_ = false;
    } else {
      // Create speaker reference buffer and mutex
      // Buffer needs to hold: delay samples + working frames
      this->spk_ref_mutex_ = xSemaphoreCreateMutex();
      this->spk_ref_buffer_ = RingBuffer::create(AEC_REF_DELAY_BYTES + RX_BUFFER_SIZE);

      // Allocate AEC frame buffers
      const size_t frame_bytes = (size_t)this->aec_frame_samples_ * sizeof(int16_t);
      this->aec_mic_ = (int16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      this->aec_ref_ = (int16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      this->aec_out_ = (int16_t *)heap_caps_malloc(frame_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

      if (!this->spk_ref_mutex_ || !this->spk_ref_buffer_ ||
          !this->aec_mic_ || !this->aec_ref_ || !this->aec_out_) {
        ESP_LOGE(TAG, "AEC buffer allocation failed -> disabled");
        this->aec_enabled_ = false;
        // Cleanup partial allocs
        if (this->aec_mic_) { heap_caps_free(this->aec_mic_); this->aec_mic_ = nullptr; }
        if (this->aec_ref_) { heap_caps_free(this->aec_ref_); this->aec_ref_ = nullptr; }
        if (this->aec_out_) { heap_caps_free(this->aec_out_); this->aec_out_ = nullptr; }
      } else {
        ESP_LOGI(TAG, "AEC ready: frame_size=%d samples (%dms)",
                 this->aec_frame_samples_,
                 this->aec_frame_samples_ * 1000 / SAMPLE_RATE);
        // AEC starts disabled, user enables via switch
        this->aec_enabled_ = false;
      }
    }
  }
#endif

  // Create server task (Core 1) - handles TCP connections and receiving
  // Highest priority (7) - RX must never starve, data must flow immediately
  BaseType_t ok = xTaskCreatePinnedToCore(
      IntercomApi::server_task,
      "intercom_srv",
      4096,
      this,
      7,  // Highest priority - RX must win always
      &this->server_task_handle_,
      1  // Core 1
  );

  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create server task");
    this->mark_failed();
    return;
  }

  // Create TX task (Core 0) - handles mic capture, AEC processing, and sending
  // High priority (6) for low latency mic→network
  // Stack increased to 12KB for AEC processing (uses FFT internally)
  ok = xTaskCreatePinnedToCore(
      IntercomApi::tx_task,
      "intercom_tx",
      12288,  // 12KB stack for AEC processing
      this,
      6,  // High priority for low latency
      &this->tx_task_handle_,
      0  // Core 0
  );

  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create TX task");
    this->mark_failed();
    return;
  }

  // Create speaker task (Core 0) - handles playback
  // Lower priority (4) - if speaker blocks, it shouldn't starve TX
  ok = xTaskCreatePinnedToCore(
      IntercomApi::speaker_task,
      "intercom_spk",
      8192,  // Larger stack for audio buffer
      this,
      4,  // Lower priority than TX
      &this->speaker_task_handle_,
      0  // Core 0 - same as TX, keeps Core 1 free for RX
  );

  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create speaker task");
    this->mark_failed();
    return;
  }

  // Load persisted settings from flash (volume, mic gain, auto-answer, AEC)
  this->load_settings_();

  // Deferred publish of initial sensor values (wait for sensors to be fully ready)
  this->set_timeout(250, [this]() {
    this->publish_state_();
    this->publish_destination_();
  });

  ESP_LOGI(TAG, "Intercom API ready on port %d", INTERCOM_PORT);
}

void IntercomApi::loop() {
  // Main loop - mostly handled by FreeRTOS tasks

  // Check call timeout (if configured and FSM in RINGING or OUTGOING state)
  // Use FSM state to handle case where TCP connection closed but call_state_ is stuck
  // Both timeouts send STOP to the other side to keep both ESPs in sync
  if (this->ringing_timeout_ms_ > 0) {
    uint32_t now = millis();

    // Timeout for RINGING state (incoming call not answered)
    if (this->call_state_ == CallState::RINGING) {
      if (now - this->ringing_start_time_ >= this->ringing_timeout_ms_) {
        ESP_LOGI(TAG, "Ringing timeout after %u ms - sending STOP to caller", this->ringing_timeout_ms_);
        // close_client_socket_() sends STOP before closing
        this->close_client_socket_();
        this->state_ = ConnectionState::DISCONNECTED;
        if (this->full_mode_) {
          this->publish_caller_("");
        }
        this->end_call_(CallEndReason::TIMEOUT);
      }
    }

    // Timeout for OUTGOING state (call not connected/answered)
    // Uses same timeout value as ringing
    if (this->call_state_ == CallState::OUTGOING) {
      if (now - this->outgoing_start_time_ >= this->ringing_timeout_ms_) {
        ESP_LOGI(TAG, "Outgoing call timeout after %u ms - sending STOP", this->ringing_timeout_ms_);
        // close_client_socket_() sends STOP before closing
        this->close_client_socket_();
        this->state_ = ConnectionState::DISCONNECTED;
        this->end_call_(CallEndReason::TIMEOUT);
      }
    }
  }
}

void IntercomApi::dump_config() {
  ESP_LOGCONFIG(TAG, "Intercom API:");
  ESP_LOGCONFIG(TAG, "  Port: %d", INTERCOM_PORT);
#ifdef USE_MICROPHONE
  ESP_LOGCONFIG(TAG, "  Microphone: %s", this->microphone_ ? "configured" : "none");
#endif
#ifdef USE_SPEAKER
  ESP_LOGCONFIG(TAG, "  Speaker: %s", this->speaker_ ? "configured" : "none");
#endif
#ifdef USE_ESP_AEC
  if (this->aec_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  AEC: configured (frame_size=%d samples)", this->aec_frame_samples_);
  } else {
    ESP_LOGCONFIG(TAG, "  AEC: none");
  }
#endif
}

void IntercomApi::publish_entity_states() {
  // Restore switch states using ESPHome's restore_mode mechanism
  // Call this from YAML: api: on_client_connected: - lambda: 'id(intercom).publish_entity_states();'

  // Auto-answer switch: restore state and apply to internal flag
  if (this->auto_answer_switch_ != nullptr) {
    auto initial = this->auto_answer_switch_->get_initial_state_with_restore_mode();
    if (initial.has_value()) {
      this->auto_answer_ = *initial;
      this->auto_answer_switch_->publish_state(*initial);
    }
  }

#ifdef USE_ESP_AEC
  // AEC switch: restore state and enable AEC if needed
  if (this->aec_switch_ != nullptr) {
    auto initial = this->aec_switch_->get_initial_state_with_restore_mode();
    if (initial.has_value()) {
      if (*initial) {
        this->set_aec_enabled(true);
      }
      this->aec_switch_->publish_state(this->aec_enabled_);
    }
  }
#endif

  ESP_LOGI(TAG, "Entity states synced (vol=%.0f%%, mic=%.1fdB, auto=%s, aec=%s)",
           this->volume_ * 100.0f, this->mic_gain_db_,
           this->auto_answer_ ? "ON" : "OFF", this->aec_enabled_ ? "ON" : "OFF");

  // For numbers: publish our internal values (loaded from flash)
  if (this->volume_number_ != nullptr) {
    this->volume_number_->publish_state(this->volume_ * 100.0f);
  }
  if (this->mic_gain_number_ != nullptr) {
    this->mic_gain_number_->publish_state(this->mic_gain_db_);
  }
}

// === Settings persistence ===

void IntercomApi::load_settings_() {
  // Use a fixed hash for the preference key (component doesn't have get_object_id_hash)
  this->settings_pref_ = global_preferences->make_preference<StoredSettings>(fnv1_hash("intercom_api_settings"));

  StoredSettings stored;
  if (this->settings_pref_.load(&stored) && stored.version == SETTINGS_VERSION) {
    this->suppress_save_ = true;  // Don't save while loading

    // Apply volume - must also call speaker_->set_volume() to actually apply it!
    this->volume_ = stored.volume_pct / 100.0f;
#ifdef USE_SPEAKER
    if (this->speaker_ != nullptr) {
      this->speaker_->set_volume(this->volume_);
    }
#endif
    ESP_LOGI(TAG, "Loaded volume: %d%%", stored.volume_pct);

    // Apply mic gain
    this->mic_gain_db_ = stored.mic_gain_db;
    this->mic_gain_ = std::pow(10.0f, this->mic_gain_db_ / 20.0f);
    ESP_LOGI(TAG, "Loaded mic_gain: %.1fdB", this->mic_gain_db_);

    // NOTE: auto_answer and AEC are handled by switch restore_mode, not here
    // This avoids conflicts between two persistence mechanisms

    this->suppress_save_ = false;
  } else {
    ESP_LOGI(TAG, "No saved settings, using defaults (vol=100%%, mic=0dB)");
  }
}

void IntercomApi::schedule_save_settings_() {
  if (this->suppress_save_ || this->save_scheduled_) {
    return;
  }
  this->save_scheduled_ = true;
  // Debounce: save after 250ms to avoid rapid writes when slider moves
  this->set_timeout(250, [this]() {
    this->save_scheduled_ = false;
    this->save_settings_();
  });
}

void IntercomApi::save_settings_() {
  StoredSettings stored;
  stored.version = SETTINGS_VERSION;
  stored.volume_pct = static_cast<uint8_t>(std::lround(this->volume_ * 100.0f));
  stored.mic_gain_db = static_cast<int8_t>(std::lround(this->mic_gain_db_));

  this->settings_pref_.save(&stored);
  ESP_LOGD(TAG, "Saved settings: vol=%d%%, mic=%ddB",
           stored.volume_pct, stored.mic_gain_db);
}

void IntercomApi::start() {
  // Use FSM state instead of atomic flag - FSM is the source of truth
  if (this->call_state_ != CallState::IDLE) {
    ESP_LOGW(TAG, "Already in call (state=%s)", call_state_to_str(this->call_state_));
    return;
  }

  ESP_LOGI(TAG, "Calling %s...", this->get_current_destination().c_str());
  this->set_active_(true);

  // Set FSM to OUTGOING - this triggers on_outgoing_call callback
  this->set_call_state_(CallState::OUTGOING);
  this->outgoing_start_time_ = millis();  // Start timeout counter

  // Notify tasks to wake up
  if (this->server_task_handle_) xTaskNotifyGive(this->server_task_handle_);
  if (this->tx_task_handle_) xTaskNotifyGive(this->tx_task_handle_);
  if (this->speaker_task_handle_) xTaskNotifyGive(this->speaker_task_handle_);
}

void IntercomApi::stop() {
  if (!this->active_.load(std::memory_order_acquire) && this->call_state_ == CallState::IDLE) {
    return;
  }

  ESP_LOGI(TAG, "Hanging up");

  // Send STOP message to client (HA) before closing
  if (this->client_.socket.load() >= 0) {
    this->send_message_(this->client_.socket.load(), MessageType::STOP);
    ESP_LOGD(TAG, "Sent STOP to client");
  }

  // set_active_(false) handles synchronization: waits for tasks, then stops hardware
  this->set_active_(false);

  // Close client connection and reset buffers
  this->close_client_socket_();
  if (this->mic_buffer_) this->mic_buffer_->reset();
  if (this->speaker_buffer_) this->speaker_buffer_->reset();

  this->state_ = ConnectionState::DISCONNECTED;
  this->end_call_(CallEndReason::LOCAL_HANGUP);  // FSM with reason
}

void IntercomApi::answer_call() {
  // Answer incoming call when auto_answer is OFF
  if (!this->is_ringing()) {
    ESP_LOGW(TAG, "answer_call() called but not ringing");
    return;
  }

  int sock = this->client_.socket.load();
  if (sock < 0) {
    ESP_LOGW(TAG, "answer_call() but no client connected");
    return;
  }

  ESP_LOGI(TAG, "Answering call");
  this->send_message_(sock, MessageType::ANSWER);
  this->set_call_state_(CallState::ANSWERING);  // FSM
  this->set_active_(true);
  this->set_streaming_(true);  // This will set CallState::STREAMING
}

void IntercomApi::decline_call() {
  // Decline incoming call when auto_answer is OFF
  if (!this->is_ringing()) {
    ESP_LOGW(TAG, "decline_call() called but not ringing");
    return;
  }

  int sock = this->client_.socket.load();
  if (sock < 0) {
    return;
  }

  ESP_LOGI(TAG, "Declining call");
  uint8_t reason = static_cast<uint8_t>(ErrorCode::BUSY);
  this->send_message_(sock, MessageType::ERROR, MessageFlags::NONE, &reason, 1);
  this->close_client_socket_();
  this->state_ = ConnectionState::DISCONNECTED;
  this->end_call_(CallEndReason::DECLINED);  // FSM with reason
}

void IntercomApi::call_toggle() {
  // Smart call toggle: ringing → answer, active → hangup, idle → start
  if (this->is_ringing()) {
    ESP_LOGI(TAG, "call_toggle: answering ringing call");
    this->answer_call();
  } else if (this->is_active()) {
    ESP_LOGI(TAG, "call_toggle: hanging up active call");
    this->stop();
  } else {
    ESP_LOGI(TAG, "call_toggle: starting new call");
    this->start();
  }
}

void IntercomApi::set_volume(float volume) {
  this->volume_ = std::max(0.0f, std::min(1.0f, volume));
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->speaker_->set_volume(this->volume_);
  }
#endif
  this->schedule_save_settings_();
}

void IntercomApi::set_auto_answer(bool enabled) {
  this->auto_answer_ = enabled;
  ESP_LOGI(TAG, "Auto-answer set to %s", enabled ? "ON" : "OFF");
  // NOTE: persistence handled by switch restore_mode, not save_settings_()
}

void IntercomApi::set_mic_gain_db(float db) {
  // Convert dB to linear gain: gain = 10^(dB/20)
  // Range: -20dB (0.1x) to +20dB (10x)
  db = std::max(-20.0f, std::min(20.0f, db));
  this->mic_gain_db_ = db;
  this->mic_gain_ = std::pow(10.0f, db / 20.0f);
  ESP_LOGD(TAG, "Mic gain set to %.1f dB (%.2fx)", db, this->mic_gain_);
  this->schedule_save_settings_();
}

#ifdef USE_ESP_AEC
void IntercomApi::reset_aec_buffers_() {
  if (!this->aec_enabled_ || this->spk_ref_buffer_ == nullptr) return;

  this->aec_mic_fill_ = 0;
  if (xSemaphoreTake(this->spk_ref_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
    this->spk_ref_buffer_->reset();
    // Pre-fill reference buffer with silence to create delay
    // This compensates for I2S DMA latency + acoustic delay
    // The mic captures echo from audio played ~80ms ago, so we delay the reference
    uint8_t *silence = (uint8_t *) heap_caps_calloc(1, AEC_REF_DELAY_BYTES, MALLOC_CAP_INTERNAL);
    if (silence) {
      this->spk_ref_buffer_->write(silence, AEC_REF_DELAY_BYTES);
      heap_caps_free(silence);
      ESP_LOGD(TAG, "AEC buffers reset, pre-filled %ums silence", (unsigned)AEC_REF_DELAY_MS);
    }
    xSemaphoreGive(this->spk_ref_mutex_);
  }
}

void IntercomApi::set_aec_enabled(bool enabled) {
  if (enabled) {
    // Only allow enabling if AEC is properly initialized
    if (this->aec_ == nullptr || !this->aec_->is_initialized()) {
      ESP_LOGW(TAG, "Cannot enable AEC: not initialized");
      this->aec_enabled_ = false;
      return;
    }
    if (this->aec_mic_ == nullptr) {
      ESP_LOGW(TAG, "Cannot enable AEC: buffers not allocated");
      this->aec_enabled_ = false;
      return;
    }
  }
  this->aec_enabled_ = enabled;
  if (enabled) {
    this->reset_aec_buffers_();
  } else {
    this->aec_mic_fill_ = 0;
  }
  ESP_LOGI(TAG, "AEC %s", enabled ? "enabled" : "disabled");
  // NOTE: persistence handled by switch restore_mode, not save_settings_()
}
#endif

void IntercomApi::connect_to(const std::string &host, uint16_t port) {
  this->client_mode_ = true;
  this->remote_host_ = host;
  this->remote_port_ = port;
  this->start();
}

void IntercomApi::disconnect() {
  this->stop();
  this->client_mode_ = false;
}

const char *IntercomApi::get_state_str() const {
  // Use CallState for human-readable status (capitalizes first letter)
  switch (this->call_state_) {
    case CallState::IDLE: return "Idle";
    case CallState::OUTGOING: return "Outgoing";
    case CallState::INCOMING: return "Incoming";
    case CallState::RINGING: return "Ringing";
    case CallState::ANSWERING: return "Answering";
    case CallState::STREAMING: return "Streaming";
    default: return "Unknown";
  }
}

void IntercomApi::publish_state_() {
  if (this->state_sensor_ != nullptr) {
    this->state_sensor_->publish_state(this->get_state_str());
  }
}

// === Contacts Management ===

void IntercomApi::set_contacts(const std::string &contacts_csv) {
  // Full mode only - in simple mode, contacts are not used
  if (!this->full_mode_) return;

  // Save current selection to preserve it if possible
  const std::string previous = this->get_current_destination();

  // Parse CSV: "Home Assistant,Intercom Mini,Intercom Xiaozhi"
  // Exclude this device's own name from contacts
  this->contacts_.clear();
  this->contacts_.reserve(16);

  if (contacts_csv.empty()) {
    this->contacts_.push_back("Home Assistant");
  } else {
    size_t start = 0;
    while (start <= contacts_csv.size()) {
      size_t pos = contacts_csv.find(',', start);
      size_t end = (pos == std::string::npos) ? contacts_csv.size() : pos;

      std::string name = contacts_csv.substr(start, end - start);

      // Trim whitespace
      while (!name.empty() && name.front() == ' ') name.erase(0, 1);
      while (!name.empty() && name.back() == ' ') name.pop_back();

      // Add if not empty and not this device
      if (!name.empty() && name != this->device_name_) {
        this->contacts_.push_back(name);
      }

      if (pos == std::string::npos) break;
      start = pos + 1;
    }
  }

  // Ensure at least "Home Assistant" is available
  if (this->contacts_.empty()) {
    this->contacts_.push_back("Home Assistant");
  }

  // Preserve selection if contact still exists, otherwise reset to 0
  auto it = std::find(this->contacts_.begin(), this->contacts_.end(), previous);
  this->contact_index_ = (it != this->contacts_.end())
      ? static_cast<size_t>(std::distance(this->contacts_.begin(), it))
      : 0;

  this->publish_destination_();
  this->publish_contacts_();  // Publish updated contacts list

  ESP_LOGI(TAG, "Contacts updated: %d devices", this->contacts_.size());
}

void IntercomApi::next_contact() {
  if (!this->full_mode_) return;  // Full mode only
  if (this->contacts_.empty()) return;
  this->contact_index_ = (this->contact_index_ + 1) % this->contacts_.size();
  this->publish_destination_();
  ESP_LOGI(TAG, "Selected contact: %s", this->get_current_destination().c_str());
}

void IntercomApi::prev_contact() {
  if (!this->full_mode_) return;  // Full mode only
  if (this->contacts_.empty()) return;
  this->contact_index_ = (this->contact_index_ + this->contacts_.size() - 1) % this->contacts_.size();
  this->publish_destination_();
  ESP_LOGI(TAG, "Selected contact: %s", this->get_current_destination().c_str());
}

const std::string &IntercomApi::get_current_destination() const {
  static const std::string default_dest = "Home Assistant";
  if (this->contacts_.empty()) return default_dest;
  return this->contacts_[this->contact_index_ % this->contacts_.size()];
}

void IntercomApi::publish_destination_() {
  if (this->destination_sensor_ != nullptr) {
    this->destination_sensor_->publish_state(this->get_current_destination());
  }
}

void IntercomApi::publish_caller_(const std::string &caller_name) {
  if (this->caller_sensor_ != nullptr) {
    this->caller_sensor_->publish_state(caller_name);
  }
}

void IntercomApi::publish_contacts_() {
  if (this->contacts_sensor_ != nullptr) {
    // Publish count only (e.g. "3 contacts"), not the full CSV
    // Full list available via get_contacts_csv() if needed
    char buf[32];
    snprintf(buf, sizeof(buf), "%d contact%s",
             (int)this->contacts_.size(),
             this->contacts_.size() == 1 ? "" : "s");
    this->contacts_sensor_->publish_state(buf);
  }
}

std::string IntercomApi::get_contacts_csv() const {
  // Full CSV available for lambdas/debugging, not published to sensor
  std::string result;
  for (size_t i = 0; i < this->contacts_.size(); i++) {
    if (i > 0) result += ",";
    result += this->contacts_[i];
  }
  return result;
}

// === State Helpers ===

void IntercomApi::set_active_(bool on) {
  bool was = this->active_.exchange(on, std::memory_order_acq_rel);
  if (was == on) return;  // No change

  if (on) {
    // Starting - clear any pending stop request and start hardware
    this->speaker_stop_requested_.store(false, std::memory_order_release);

#ifdef USE_MICROPHONE
    if (this->microphone_) {
      this->microphone_->start();
    }
#endif
#ifdef USE_SPEAKER
    if (this->speaker_) {
      this->speaker_->start();
    }
#endif
    this->start_trigger_.trigger();
  } else {
    // Stopping - use single-owner model for speaker to avoid race conditions
    // 1. Request speaker_task to stop the speaker
    // 2. Wait for acknowledgment (with timeout)
    // 3. Speaker task will call speaker_->stop() safely

#ifdef USE_SPEAKER
    if (this->speaker_ && this->speaker_stopped_sem_) {
      // Request speaker task to stop
      this->speaker_stop_requested_.store(true, std::memory_order_release);

      // Wait for speaker task to acknowledge (max 200ms)
      if (xSemaphoreTake(this->speaker_stopped_sem_, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "Speaker stop timeout - forcing stop");
        // Fallback: stop directly if task didn't respond
        this->speaker_->stop();
      }
      this->speaker_stop_requested_.store(false, std::memory_order_release);
    }
#endif

#ifdef USE_MICROPHONE
    if (this->microphone_) {
      this->microphone_->stop();
    }
#endif

    this->stop_trigger_.trigger();
  }
}

void IntercomApi::set_streaming_(bool on) {
  this->client_.streaming.store(on, std::memory_order_release);
  this->state_ = on ? ConnectionState::STREAMING : ConnectionState::CONNECTED;
  if (on) {
    // Reset audio buffers for new call - prevents stale data on quick reconnect
    if (this->mic_buffer_) {
      if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        this->mic_buffer_->reset();
        xSemaphoreGive(this->mic_mutex_);
      }
    }
    if (this->speaker_buffer_) {
      if (xSemaphoreTake(this->speaker_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        this->speaker_buffer_->reset();
        xSemaphoreGive(this->speaker_mutex_);
      }
    }

#ifdef USE_ESP_AEC
    // Reset AEC state for new call - critical for proper echo cancellation
    this->reset_aec_buffers_();
#endif

    this->set_call_state_(CallState::STREAMING);  // FSM - trigger fired there
  }
  this->publish_state_();
}

void IntercomApi::set_call_state_(CallState new_state) {
  if (this->call_state_ == new_state) return;

  CallState old_state = this->call_state_;
  this->call_state_ = new_state;

  ESP_LOGI(TAG, "Call state: %s -> %s", call_state_to_str(old_state), call_state_to_str(new_state));

  // Fire appropriate trigger
  switch (new_state) {
    case CallState::IDLE:
      this->idle_trigger_.trigger();  // legacy
      break;
    case CallState::OUTGOING:
      this->outgoing_call_trigger_.trigger();
      break;
    case CallState::INCOMING:
      this->incoming_call_trigger_.trigger();
      break;
    case CallState::RINGING:
      this->ringing_trigger_.trigger();  // legacy
      break;
    case CallState::ANSWERING:
      this->answered_trigger_.trigger();
      break;
    case CallState::STREAMING:
      this->streaming_trigger_.trigger();
      break;
  }

  this->publish_state_();
}

void IntercomApi::end_call_(CallEndReason reason) {
  if (this->call_state_ == CallState::IDLE) return;

  std::string reason_str = call_end_reason_to_str(reason);
  ESP_LOGI(TAG, "Call ended: %s", reason_str.c_str());

  // Fire appropriate trigger based on reason type
  if (reason == CallEndReason::UNREACHABLE ||
      reason == CallEndReason::BUSY ||
      reason == CallEndReason::PROTOCOL_ERROR ||
      reason == CallEndReason::BRIDGE_ERROR) {
    this->call_failed_trigger_.trigger(reason_str);
  } else {
    this->hangup_trigger_.trigger(reason_str);
  }

  // Also fire legacy triggers
  this->call_end_trigger_.trigger();
  this->stop_trigger_.trigger();

  this->set_call_state_(CallState::IDLE);
}

// === Server Task ===

void IntercomApi::server_task(void *param) {
  static_cast<IntercomApi *>(param)->server_task_();
}

void IntercomApi::server_task_() {
  ESP_LOGI(TAG, "Server task started");

  // In server mode, always set up the listening socket immediately
  if (!this->client_mode_) {
    if (!this->setup_server_socket_()) {
      ESP_LOGE(TAG, "Failed to setup server socket on startup");
    }
  }

  while (true) {
    // When streaming, don't wait - poll as fast as possible
    // When idle, wait up to 100ms to save CPU
    if (this->client_.streaming.load()) {
      // During streaming: just check notification without blocking
      ulTaskNotifyTake(pdTRUE, 0);  // Non-blocking
    } else {
      // When idle: wait for activation signal
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }

    // Client mode - only connect when active
    if (this->client_mode_) {
      if (!this->active_.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      if (this->client_.socket.load() < 0) {
        this->state_ = ConnectionState::CONNECTING;

        // Create socket
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
          ESP_LOGE(TAG, "Failed to create client socket: %d", errno);
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }

        // Set non-blocking
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        // Connect
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(this->remote_port_);
        inet_pton(AF_INET, this->remote_host_.c_str(), &addr.sin_addr);

        int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
          ESP_LOGE(TAG, "Connect failed: %d", errno);
          close(sock);
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }

        // Wait for connection
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);
        struct timeval tv = {.tv_sec = 5, .tv_usec = 0};

        ret = ::select(sock + 1, nullptr, &write_fds, nullptr, &tv);
        if (ret <= 0) {
          ESP_LOGE(TAG, "Connect timeout");
          close(sock);
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }

        // Check connection result
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error != 0) {
          ESP_LOGE(TAG, "Connect error: %d", error);
          close(sock);
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }

        ESP_LOGI(TAG, "Connected to %s:%d", this->remote_host_.c_str(), this->remote_port_);

        xSemaphoreTake(this->client_mutex_, portMAX_DELAY);
        this->client_.socket.store(sock);
        this->client_.streaming.store(false);
        this->client_.last_ping = millis();
        xSemaphoreGive(this->client_mutex_);

        this->state_ = ConnectionState::CONNECTED;
        this->connect_trigger_.trigger();

        // Send START
        this->send_message_(sock, MessageType::START);
      }
    } else {
      // Server mode - listen for connections
      if (this->server_socket_ < 0) {
        if (!this->setup_server_socket_()) {
          vTaskDelay(pdMS_TO_TICKS(1000));
          continue;
        }
      }

      // Accept new connection if none
      if (this->client_.socket.load() < 0) {
        this->accept_client_();
      }
    }

    // Handle existing client
    if (this->client_.socket.load() >= 0) {
      // Check for incoming data
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(this->client_.socket.load(), &read_fds);
      struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};  // 10ms

      int ret = ::select(this->client_.socket.load() + 1, &read_fds, nullptr, nullptr, &tv);
      if (ret > 0 && FD_ISSET(this->client_.socket.load(), &read_fds)) {
        MessageHeader header;
        if (this->receive_message_(this->client_.socket.load(), header, this->rx_buffer_, MAX_MESSAGE_SIZE)) {
          this->handle_message_(header, this->rx_buffer_ + HEADER_SIZE);
        } else {
          // Connection closed or error
          ESP_LOGI(TAG, "Client disconnected");
          // IMPORTANT: Order matters to avoid race conditions
          // 1. Stop streaming flag first
          this->client_.streaming.store(false);
          // 2. Close socket immediately
          this->close_client_socket_();
          // 3. Now stop audio hardware
          this->set_active_(false);
          this->state_ = ConnectionState::DISCONNECTED;

          // Clear caller sensor in full mode
          if (this->full_mode_) {
            this->publish_caller_("");
          }

          // If call was in progress, end it properly to reset FSM
          if (this->call_state_ != CallState::IDLE) {
            this->end_call_(CallEndReason::REMOTE_HANGUP);
          } else {
            this->publish_state_();
          }
          this->disconnect_trigger_.trigger();
        }
      }

      // Send ping if needed - but NOT during streaming to avoid interference with audio
      if (this->state_ != ConnectionState::STREAMING &&
          millis() - this->client_.last_ping > PING_INTERVAL_MS) {
        this->send_message_(this->client_.socket.load(), MessageType::PING);
        this->client_.last_ping = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1));  // Yield
  }
}

// === TX Task (Core 0) - Mic to Network ===

void IntercomApi::tx_task(void *param) {
  static_cast<IntercomApi *>(param)->tx_task_();
}

void IntercomApi::tx_task_() {
  ESP_LOGD(TAG, "TX task started");

  uint8_t audio_chunk[AUDIO_CHUNK_SIZE];

  while (true) {
    // Wait until active and connected
    if (!this->active_.load(std::memory_order_acquire) ||
        this->client_.socket.load() < 0 ||
        !this->client_.streaming.load()) {
#ifdef USE_ESP_AEC
      // Reset AEC accumulator when paused
      this->aec_mic_fill_ = 0;
#endif
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Read from mic buffer
    if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    size_t avail = this->mic_buffer_->available();
    if (avail < AUDIO_CHUNK_SIZE) {
      xSemaphoreGive(this->mic_mutex_);
      // No data, short sleep
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    size_t read = this->mic_buffer_->read(audio_chunk, AUDIO_CHUNK_SIZE, 0);
    xSemaphoreGive(this->mic_mutex_);

    if (read != AUDIO_CHUNK_SIZE) {
      continue;
    }

#ifdef USE_ESP_AEC
    // AEC Processing: accumulate samples, process when full frame ready
    if (this->aec_enabled_ && this->aec_ != nullptr && this->aec_mic_ != nullptr) {
      const int16_t *mic_samples = reinterpret_cast<const int16_t *>(audio_chunk);
      size_t num_samples = AUDIO_CHUNK_SIZE / sizeof(int16_t);  // 256 samples per chunk

      // Copy mic samples to accumulator
      size_t samples_to_copy = std::min(num_samples,
                                        (size_t)this->aec_frame_samples_ - this->aec_mic_fill_);
      memcpy(this->aec_mic_ + this->aec_mic_fill_, mic_samples, samples_to_copy * sizeof(int16_t));
      this->aec_mic_fill_ += samples_to_copy;

      // If we have a full AEC frame, process it
      if (this->aec_mic_fill_ >= (size_t)this->aec_frame_samples_) {
        // Read speaker reference from buffer (same frame size)
        size_t ref_bytes_needed = this->aec_frame_samples_ * sizeof(int16_t);

        if (xSemaphoreTake(this->spk_ref_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
          size_t ref_avail = this->spk_ref_buffer_->available();
          if (ref_avail >= ref_bytes_needed) {
            this->spk_ref_buffer_->read(this->aec_ref_, ref_bytes_needed, 0);
          } else {
            // Not enough reference - use silence (still process to reduce latency)
            memset(this->aec_ref_, 0, ref_bytes_needed);
            static uint32_t last_warn = 0;
            if (millis() - last_warn > 5000) {
              ESP_LOGW(TAG, "AEC: ref buffer low (%zu/%zu bytes)", ref_avail, ref_bytes_needed);
              last_warn = millis();
            }
          }
          xSemaphoreGive(this->spk_ref_mutex_);
        } else {
          memset(this->aec_ref_, 0, ref_bytes_needed);
          ESP_LOGW(TAG, "AEC: mutex timeout");
        }

        // Always process AEC - no skip threshold to avoid audio discontinuities
        this->aec_->process(this->aec_mic_, this->aec_ref_, this->aec_out_, this->aec_frame_samples_);

        // Debug: log AEC stats periodically
        static uint32_t aec_frame_count = 0;
        if (++aec_frame_count % 100 == 0) {  // Every ~3 seconds at 32ms/frame
          int64_t mic_sum = 0, ref_sum = 0, out_sum = 0;
          for (int i = 0; i < this->aec_frame_samples_; i++) {
            mic_sum += (int64_t)this->aec_mic_[i] * this->aec_mic_[i];
            ref_sum += (int64_t)this->aec_ref_[i] * this->aec_ref_[i];
            out_sum += (int64_t)this->aec_out_[i] * this->aec_out_[i];
          }
          int mic_rms = (int)sqrt((double)mic_sum / this->aec_frame_samples_);
          int ref_rms = (int)sqrt((double)ref_sum / this->aec_frame_samples_);
          int out_rms = (int)sqrt((double)out_sum / this->aec_frame_samples_);
          int reduction = (mic_rms > 0) ? (100 - (out_rms * 100 / mic_rms)) : 0;
          ESP_LOGI(TAG, "AEC #%lu: mic=%d ref=%d out=%d (%d%% reduction)",
                   (unsigned long)aec_frame_count, mic_rms, ref_rms, out_rms, reduction);
        }

        // Send processed audio (may be larger than AUDIO_CHUNK_SIZE)
        size_t out_bytes = this->aec_frame_samples_ * sizeof(int16_t);

        // Check still active before sending
        if (this->active_.load(std::memory_order_acquire) && this->client_.socket.load() >= 0) {
          int socket = this->client_.socket.load();
          MessageHeader header;
          header.type = static_cast<uint8_t>(MessageType::AUDIO);
          header.flags = static_cast<uint8_t>(MessageFlags::NONE);
          header.length = out_bytes;

          memcpy(this->audio_tx_buffer_, &header, HEADER_SIZE);
          memcpy(this->audio_tx_buffer_ + HEADER_SIZE, this->aec_out_, out_bytes);

          size_t total = HEADER_SIZE + out_bytes;
          ssize_t sent = send(socket, this->audio_tx_buffer_, total, MSG_DONTWAIT);

          if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            // Only log if still streaming - avoid noise during shutdown
            if (this->client_.streaming.load(std::memory_order_acquire)) {
              ESP_LOGW(TAG, "TX send error: %d", errno);
            }
          }
        }

        // Reset accumulators
        this->aec_mic_fill_ = 0;

        // Handle overflow: if we had more samples than frame_size, carry over
        if (samples_to_copy < num_samples) {
          size_t remaining = num_samples - samples_to_copy;
          memcpy(this->aec_mic_, mic_samples + samples_to_copy, remaining * sizeof(int16_t));
          this->aec_mic_fill_ = remaining;
        }
      }

      // Minimal delay
      taskYIELD();
      continue;  // Skip non-AEC path
    }
#endif

    // Non-AEC path: send directly
    // Check still active before sending
    if (!this->active_.load(std::memory_order_acquire) || this->client_.socket.load() < 0) {
      continue;
    }

    // Send directly using dedicated audio_tx_buffer_ (no mutex needed)
    int socket = this->client_.socket.load();
    if (socket >= 0) {
      MessageHeader header;
      header.type = static_cast<uint8_t>(MessageType::AUDIO);
      header.flags = static_cast<uint8_t>(MessageFlags::NONE);
      header.length = AUDIO_CHUNK_SIZE;

      memcpy(this->audio_tx_buffer_, &header, HEADER_SIZE);
      memcpy(this->audio_tx_buffer_ + HEADER_SIZE, audio_chunk, AUDIO_CHUNK_SIZE);

      size_t total = HEADER_SIZE + AUDIO_CHUNK_SIZE;
      ssize_t sent = send(socket, this->audio_tx_buffer_, total, MSG_DONTWAIT);

      if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Only log if still streaming - avoid noise during shutdown
        if (this->client_.streaming.load(std::memory_order_acquire)) {
          ESP_LOGW(TAG, "TX send error: %d", errno);
        }
      }
    }

    // Minimal delay - let FreeRTOS scheduler handle timing
    taskYIELD();
  }
}

// === Speaker Task (Core 0) - Network to Speaker ===

void IntercomApi::speaker_task(void *param) {
  static_cast<IntercomApi *>(param)->speaker_task_();
}

void IntercomApi::speaker_task_() {
  ESP_LOGD(TAG, "Speaker task started");

#ifdef USE_SPEAKER
  uint8_t audio_chunk[AUDIO_CHUNK_SIZE * 4];

  while (true) {
    // Check for stop request - single-owner model: only this task stops speaker
    if (this->speaker_stop_requested_.load(std::memory_order_acquire)) {
      if (this->speaker_ != nullptr) {
        ESP_LOGD(TAG, "Speaker task: stopping speaker");
        this->speaker_->stop();
      }
      // Signal that speaker has stopped
      if (this->speaker_stopped_sem_ != nullptr) {
        xSemaphoreGive(this->speaker_stopped_sem_);
      }
      // Wait for next activation
      while (this->speaker_stop_requested_.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      continue;
    }

    // Wait until active
    if (!this->active_.load(std::memory_order_acquire) || this->speaker_ == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    // Read from speaker buffer - grab as much as available up to 8 chunks
    if (xSemaphoreTake(this->speaker_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
      taskYIELD();
      continue;
    }

    size_t avail = this->speaker_buffer_->available();
    if (avail < AUDIO_CHUNK_SIZE) {
      xSemaphoreGive(this->speaker_mutex_);
      // Very short delay when buffer is empty
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // Read up to 4 chunks at once to reduce overhead
    size_t to_read = avail;
    if (to_read > AUDIO_CHUNK_SIZE * 4) to_read = AUDIO_CHUNK_SIZE * 4;
    // Align to chunk size
    to_read = (to_read / AUDIO_CHUNK_SIZE) * AUDIO_CHUNK_SIZE;

    size_t read = this->speaker_buffer_->read(audio_chunk, to_read, 0);
    xSemaphoreGive(this->speaker_mutex_);

    if (read > 0 && this->volume_ > 0.001f) {
      this->speaker_->play(audio_chunk, read, 0);

#ifdef USE_ESP_AEC
      // Feed speaker reference buffer for AEC
      // IMPORTANT: Apply same volume scaling as speaker output so reference matches actual echo
      if (this->aec_enabled_ && this->spk_ref_buffer_ != nullptr) {
        if (xSemaphoreTake(this->spk_ref_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
          // Use separate buffer for scaled reference (don't modify audio_chunk!)
          if (this->volume_ != 1.0f) {
            static int16_t ref_scaled[AUDIO_CHUNK_SIZE * 4 / sizeof(int16_t)];
            const int16_t *src = reinterpret_cast<const int16_t *>(audio_chunk);
            size_t num_samples = read / sizeof(int16_t);
            for (size_t i = 0; i < num_samples; i++) {
              int32_t scaled = static_cast<int32_t>(src[i] * this->volume_);
              if (scaled > 32767) scaled = 32767;
              if (scaled < -32768) scaled = -32768;
              ref_scaled[i] = static_cast<int16_t>(scaled);
            }
            this->spk_ref_buffer_->write(ref_scaled, read);
          } else {
            this->spk_ref_buffer_->write(audio_chunk, read);
          }
          xSemaphoreGive(this->spk_ref_mutex_);
        }
      }
#endif
    }

    // Minimal delay
    taskYIELD();
  }
#else
  // No speaker, just idle
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
#endif
}

// === Protocol ===

bool IntercomApi::send_message_(int socket, MessageType type, MessageFlags flags,
                                 const uint8_t *data, size_t len) {
  if (socket < 0) return false;

  // Take mutex to protect tx_buffer_ from concurrent access
  if (xSemaphoreTake(this->send_mutex_, pdMS_TO_TICKS(10)) != pdTRUE) {
    // Could not get mutex - another task is sending
    return false;
  }

  MessageHeader header;
  header.type = static_cast<uint8_t>(type);
  header.flags = static_cast<uint8_t>(flags);
  header.length = static_cast<uint16_t>(len);

  // Build message in tx_buffer
  memcpy(this->tx_buffer_, &header, HEADER_SIZE);
  if (data && len > 0) {
    memcpy(this->tx_buffer_ + HEADER_SIZE, data, len);
  }

  size_t total = HEADER_SIZE + len;
  size_t offset = 0;
  uint32_t start_ms = millis();

  // Handle partial sends with retry
  while (offset < total) {
    ssize_t sent = send(socket, this->tx_buffer_ + offset, total - offset, MSG_DONTWAIT);

    if (sent > 0) {
      offset += (size_t)sent;
      continue;
    }

    if (sent == 0) {
      // Connection closed
      xSemaphoreGive(this->send_mutex_);
      return false;
    }

    // sent < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Buffer full - wait briefly and retry
      if (millis() - start_ms > 20) {
        xSemaphoreGive(this->send_mutex_);
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // Real error - only log if we expect the connection to be valid
    if (this->client_.streaming.load(std::memory_order_relaxed)) {
      ESP_LOGW(TAG, "Send failed: errno=%d sent=%zd offset=%zu total=%zu", errno, sent, offset, total);
    }
    xSemaphoreGive(this->send_mutex_);
    return false;
  }

  xSemaphoreGive(this->send_mutex_);
  return true;
}

bool IntercomApi::receive_message_(int socket, MessageHeader &header, uint8_t *buffer, size_t buffer_size) {
  // Read header - handle partial reads (non-blocking socket)
  size_t header_read = 0;
  int retry = 0;
  const int MAX_RETRY = 50;  // 50ms max wait for complete message

  while (header_read < HEADER_SIZE && retry < MAX_RETRY) {
    ssize_t received = recv(socket, buffer + header_read, HEADER_SIZE - header_read, 0);
    if (received > 0) {
      header_read += received;
      retry = 0;  // Reset on progress
      continue;
    }
    if (received == 0) {
      return false;  // Connection closed
    }
    // received < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      retry++;
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    return false;  // Real error
  }

  if (header_read != HEADER_SIZE) {
    if (header_read > 0) {
      ESP_LOGW(TAG, "Header incomplete: %zu/%d", header_read, HEADER_SIZE);
    }
    return false;
  }

  memcpy(&header, buffer, HEADER_SIZE);

  if (header.length > buffer_size - HEADER_SIZE) {
    ESP_LOGW(TAG, "Message too large: %d", header.length);
    return false;
  }

  // Read payload
  if (header.length > 0) {
    size_t payload_read = 0;
    retry = 0;
    while (payload_read < header.length && retry < MAX_RETRY) {
      ssize_t received = recv(socket, buffer + HEADER_SIZE + payload_read,
                              header.length - payload_read, 0);
      if (received > 0) {
        payload_read += received;
        retry = 0;
        continue;
      }
      if (received == 0) {
        return false;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        retry++;
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      return false;
    }

    if (payload_read != header.length) {
      ESP_LOGW(TAG, "Payload incomplete: %zu/%d", payload_read, header.length);
      return false;
    }
  }

  return true;
}

void IntercomApi::handle_message_(const MessageHeader &header, const uint8_t *data) {
  MessageType type = static_cast<MessageType>(header.type);

  switch (type) {
    case MessageType::AUDIO:
      // Write to speaker buffer with overflow tracking
      if (xSemaphoreTake(this->speaker_mutex_, pdMS_TO_TICKS(1)) == pdTRUE) {
        size_t written = this->speaker_buffer_->write((void *)data, header.length);
        xSemaphoreGive(this->speaker_mutex_);
        if (written != header.length) {
          static uint32_t spk_drop = 0;
          spk_drop++;
          if (spk_drop <= 5 || spk_drop % 100 == 0) {
            ESP_LOGW(TAG, "SPK buffer overflow: %zu/%d (drops=%lu)",
                     written, header.length, (unsigned long)spk_drop);
          }
        }
      }
      if (this->state_ != ConnectionState::STREAMING) {
        this->state_ = ConnectionState::STREAMING;
      }
      // If we're in OUTGOING state (caller waiting for dest to answer),
      // receiving audio means dest answered - transition to STREAMING
      if (this->call_state_ == CallState::OUTGOING) {
        ESP_LOGI(TAG, "Dest answered - received audio, transitioning to STREAMING");
        this->set_call_state_(CallState::STREAMING);  // trigger fired in set_call_state_
      }
      break;

    case MessageType::START: {
      // Check for NO_RING flag (used for caller in bridge mode - skip ringing)
      const bool no_ring = (header.flags & static_cast<uint8_t>(MessageFlags::NO_RING)) != 0;

      // Extract caller name from payload (if present)
      std::string caller_name;
      if (header.length > 0 && data != nullptr) {
        // Payload is the caller name (null-terminated or up to length)
        size_t name_len = strnlen(reinterpret_cast<const char *>(data), header.length);
        caller_name.assign(reinterpret_cast<const char *>(data), name_len);
      }

      // Log user-friendly message
      if (no_ring) {
        // We are the caller in a bridge, destination is the caller_name
        ESP_LOGI(TAG, "Calling %s...", caller_name.empty() ? "unknown" : caller_name.c_str());
      } else {
        // We are being called
        ESP_LOGI(TAG, "Incoming call from %s", caller_name.empty() ? "Home Assistant" : caller_name.c_str());
      }

      // Publish caller name (even if empty - clears previous)
      if (this->full_mode_) {
        this->publish_caller_(caller_name);
      }

      if (no_ring) {
        // NO_RING flag: we are the CALLER in a bridge, not the callee
        // Go to OUTGOING state and wait for audio (dest to answer)
        this->outgoing_start_time_ = millis();  // Start timeout counter BEFORE state change
        this->set_call_state_(CallState::OUTGOING);  // FSM: outgoing call
        this->set_active_(true);
        // Enable audio flow, but don't set STREAMING yet - wait for first audio
        this->client_.streaming.store(true, std::memory_order_release);
        this->state_ = ConnectionState::STREAMING;
        this->send_message_(this->client_.socket.load(), MessageType::PONG);
      } else if (this->auto_answer_) {
        // Auto-answer ON: start streaming immediately, skip INCOMING/RINGING states
        // This prevents on_incoming_call trigger from firing for auto-answered calls
        this->set_call_state_(CallState::ANSWERING);  // FSM: go directly to answering
        this->set_active_(true);
        this->set_streaming_(true);  // This will set CallState::STREAMING
        this->send_message_(this->client_.socket.load(), MessageType::PONG);
      } else {
        // Auto-answer OFF: go to ringing state, wait for local answer
        this->set_call_state_(CallState::INCOMING);  // FSM: incoming call first
        this->state_ = ConnectionState::CONNECTED;  // Stay connected but not streaming
        this->send_message_(this->client_.socket.load(), MessageType::RING);
        ESP_LOGI(TAG, "Auto-answer OFF - sending RING, waiting for local answer");
        this->ringing_start_time_ = millis();  // Start ringing timeout timer
        this->set_call_state_(CallState::RINGING);  // FSM: then ringing (triggers on_ringing)
      }
      break;
    }

    case MessageType::STOP:
      ESP_LOGI(TAG, "Received STOP from client");
      // Clear caller name in full mode
      if (this->full_mode_) {
        this->publish_caller_("");
      }
      // IMPORTANT: Order matters to avoid race conditions
      // 1. Stop streaming flag first (TX task checks this)
      this->set_streaming_(false);
      // 2. Close socket immediately (before set_active_ which takes time)
      this->close_client_socket_();
      // 3. Now stop audio hardware (waits for tasks)
      this->set_active_(false);
      this->state_ = ConnectionState::DISCONNECTED;
      this->end_call_(CallEndReason::REMOTE_HANGUP);  // FSM with reason
      break;

    case MessageType::PING:
      this->send_message_(this->client_.socket.load(), MessageType::PONG);
      break;

    case MessageType::PONG:
      this->client_.last_ping = millis();
      if (this->client_mode_ && this->state_ == ConnectionState::CONNECTED) {
        // ACK for START - begin streaming
        this->client_.streaming.store(true);
        this->state_ = ConnectionState::STREAMING;
      }
      break;

    case MessageType::ANSWER:
      // ANSWER: call was answered (either our outgoing call or remote answer)
      if (this->call_state_ == CallState::OUTGOING) {
        // We called them, they answered - start streaming
        ESP_LOGI(TAG, "Call answered");
        this->set_streaming_(true);  // This will set CallState::STREAMING
        this->send_message_(this->client_.socket.load(), MessageType::PONG);
      } else if (this->call_state_ == CallState::RINGING) {
        // We were ringing, HA answered for us remotely
        ESP_LOGI(TAG, "Call answered (remote)");
        this->set_call_state_(CallState::ANSWERING);  // FSM
        this->set_active_(true);
        this->set_streaming_(true);  // This will set CallState::STREAMING
        this->send_message_(this->client_.socket.load(), MessageType::PONG);
      } else {
        ESP_LOGW(TAG, "ANSWER received in unexpected state");
      }
      break;

    case MessageType::ERROR:
      if (header.length > 0) {
        ESP_LOGE(TAG, "Received ERROR: %d", data[0]);
      }
      break;

    default:
      ESP_LOGW(TAG, "Unknown message type: 0x%02X", header.type);
      break;
  }
}

// === Socket Helpers ===

bool IntercomApi::setup_server_socket_() {
  this->server_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (this->server_socket_ < 0) {
    ESP_LOGE(TAG, "Failed to create server socket: %d", errno);
    return false;
  }

  int opt = 1;
  setsockopt(this->server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // Set non-blocking
  int flags = fcntl(this->server_socket_, F_GETFL, 0);
  fcntl(this->server_socket_, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(INTERCOM_PORT);

  if (bind(this->server_socket_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "Bind failed: %d", errno);
    close(this->server_socket_);
    this->server_socket_ = -1;
    return false;
  }

  if (listen(this->server_socket_, 1) < 0) {
    ESP_LOGE(TAG, "Listen failed: %d", errno);
    close(this->server_socket_);
    this->server_socket_ = -1;
    return false;
  }

  ESP_LOGI(TAG, "Server listening on port %d", INTERCOM_PORT);
  return true;
}

void IntercomApi::close_server_socket_() {
  if (this->server_socket_ >= 0) {
    close(this->server_socket_);
    this->server_socket_ = -1;
  }
}

void IntercomApi::close_client_socket_() {
  // Lock-free socket close: atomically get and invalidate socket
  // This prevents race conditions without needing mutex timeout hacks
  this->client_.streaming.store(false);

  int sock = this->client_.socket.exchange(-1);
  if (sock >= 0) {
    // Try to send STOP before closing (best effort)
    this->send_message_(sock, MessageType::STOP);
    // Graceful shutdown then close
    shutdown(sock, SHUT_RDWR);
    close(sock);
  }
}

void IntercomApi::accept_client_() {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  int client_sock = accept(this->server_socket_, (struct sockaddr *)&client_addr, &client_len);
  if (client_sock < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "Accept error: %d", errno);
    }
    return;
  }

  // Helper to reject with BUSY error
  auto reject_busy = [&](const char *reason) {
    ESP_LOGW(TAG, "Rejecting connection - %s", reason);
    MessageHeader header;
    header.type = static_cast<uint8_t>(MessageType::ERROR);
    header.flags = 0;
    header.length = 1;
    uint8_t msg[HEADER_SIZE + 1];
    memcpy(msg, &header, HEADER_SIZE);
    msg[HEADER_SIZE] = static_cast<uint8_t>(ErrorCode::BUSY);
    send(client_sock, msg, sizeof(msg), 0);
    close(client_sock);
  };

  // Check if already have a client
  if (this->client_.socket.load() >= 0) {
    reject_busy("already have client");
    return;
  }

  // Check if we're in a state that shouldn't accept new connections
  // Allow IDLE (normal) and OUTGOING (ESP called someone, waiting for answer)
  if (this->call_state_ != CallState::IDLE && this->call_state_ != CallState::OUTGOING) {
    reject_busy(call_state_to_str(this->call_state_));
    return;
  }

  // Set socket options
  int opt = 1;
  setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

  // Increase send/receive buffer sizes for better throughput
  int buf_size = 32768;  // 32KB buffer
  setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
  setsockopt(client_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

  // Set non-blocking for async operation
  int flags = fcntl(client_sock, F_GETFL, 0);
  fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
  ESP_LOGI(TAG, "Client connected from %s", ip_str);

  // Use mutex for non-atomic addr field
  xSemaphoreTake(this->client_mutex_, portMAX_DELAY);
  this->client_.socket.store(client_sock);
  this->client_.addr = client_addr;
  this->client_.last_ping = millis();
  this->client_.streaming.store(false);
  xSemaphoreGive(this->client_mutex_);

  this->state_ = ConnectionState::CONNECTED;
  this->connect_trigger_.trigger();
}

// === Microphone Callback ===

void IntercomApi::on_microphone_data_(const uint8_t *data, size_t len) {
  if (!this->active_.load(std::memory_order_acquire) ||
      this->client_.socket.load() < 0 ||
      !this->client_.streaming.load()) {
    return;
  }

  static constexpr size_t MAX_SAMPLES = 512;
  int16_t converted[MAX_SAMPLES];
  size_t num_samples;
  bool needs_processing = this->mic_gain_ != 1.0f || this->dc_offset_removal_;

  if (this->mic_bits_ == 32) {
    // 32-bit mic (e.g., SPH0645) - always needs conversion to 16-bit
    const int32_t *src = reinterpret_cast<const int32_t *>(data);
    num_samples = std::min(len / sizeof(int32_t), MAX_SAMPLES);

    for (size_t i = 0; i < num_samples; i++) {
      int32_t sample = src[i] >> 16;  // Extract upper 16 bits
      if (this->dc_offset_removal_) {
        this->dc_offset_ = ((this->dc_offset_ * 255) >> 8) + sample;
        sample -= (this->dc_offset_ >> 8);
      }
      sample = static_cast<int32_t>(sample * this->mic_gain_);
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      converted[i] = static_cast<int16_t>(sample);
    }
  } else if (needs_processing) {
    // 16-bit mic with gain or DC offset
    const int16_t *src = reinterpret_cast<const int16_t *>(data);
    num_samples = std::min(len / sizeof(int16_t), MAX_SAMPLES);

    for (size_t i = 0; i < num_samples; i++) {
      int32_t sample = src[i];
      if (this->dc_offset_removal_) {
        this->dc_offset_ = ((this->dc_offset_ * 255) >> 8) + sample;
        sample -= (this->dc_offset_ >> 8);
      }
      sample = static_cast<int32_t>(sample * this->mic_gain_);
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;
      converted[i] = static_cast<int16_t>(sample);
    }
  } else {
    // 16-bit mic, direct passthrough (gain=1.0, no DC offset)
    if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
      this->mic_buffer_->write((void *)data, len);
      xSemaphoreGive(this->mic_mutex_);
    } else {
      static uint32_t mic_drops = 0;
      if (++mic_drops <= 5 || mic_drops % 100 == 0) {
        ESP_LOGW(TAG, "Mic data dropped: %lu total", (unsigned long)mic_drops);
      }
    }
    return;
  }

  // Write processed samples
  if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
    this->mic_buffer_->write(converted, num_samples * sizeof(int16_t));
    xSemaphoreGive(this->mic_mutex_);
  } else {
    static uint32_t mic_drops = 0;
    if (++mic_drops <= 5 || mic_drops % 100 == 0) {
      ESP_LOGW(TAG, "Mic data dropped: %lu total", (unsigned long)mic_drops);
    }
  }
}

}  // namespace intercom_api
}  // namespace esphome

#endif  // USE_ESP32
