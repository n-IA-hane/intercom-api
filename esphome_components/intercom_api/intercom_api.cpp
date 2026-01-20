#include "intercom_api.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <esp_heap_caps.h>
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
      this->spk_ref_mutex_ = xSemaphoreCreateMutex();
      this->spk_ref_buffer_ = RingBuffer::create(RX_BUFFER_SIZE);  // ~256ms of reference

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

  // Deferred publish of initial sensor values (wait for sensors to be fully ready)
  this->set_timeout(250, [this]() {
    this->publish_state_();
    this->publish_destination_();
  });

  // Sync state from registered entities AFTER ESPHome has restored them
  // This ensures the parent component has the correct state from boot
  // NOTE: Don't use has_state() here - it's false at boot even if state is valid from flash
  this->set_timeout(0, [this]() {
    // Sync auto_answer from switch (state is valid even if has_state() is false)
    if (this->auto_answer_switch_ != nullptr) {
      this->auto_answer_ = this->auto_answer_switch_->state;
      ESP_LOGD(TAG, "Synced auto_answer from switch: %s", this->auto_answer_ ? "ON" : "OFF");
    }

    // Sync volume from number
    if (this->volume_number_ != nullptr) {
      this->volume_ = this->volume_number_->state / 100.0f;
      ESP_LOGD(TAG, "Synced volume from number: %.0f%%", this->volume_number_->state);
    }

    // Sync mic gain from number
    if (this->mic_gain_number_ != nullptr) {
      float db = this->mic_gain_number_->state;
      this->mic_gain_ = powf(10.0f, db / 20.0f);
      ESP_LOGD(TAG, "Synced mic_gain from number: %.1fdB", db);
    }

#ifdef USE_ESP_AEC
    // Sync AEC from switch
    if (this->aec_switch_ != nullptr) {
      this->aec_enabled_ = this->aec_switch_->state;
      ESP_LOGD(TAG, "Synced AEC from switch: %s", this->aec_enabled_ ? "ON" : "OFF");
    }
#endif
  });

  ESP_LOGI(TAG, "Intercom API ready on port %d", INTERCOM_PORT);
}

void IntercomApi::loop() {
  // Main loop - mostly handled by FreeRTOS tasks

  // Check ringing timeout (if configured and currently ringing)
  if (this->ringing_timeout_ms_ > 0 && this->is_ringing()) {
    uint32_t now = millis();
    if (now - this->ringing_start_time_ >= this->ringing_timeout_ms_) {
      ESP_LOGI(TAG, "Ringing timeout after %u ms - auto-declining", this->ringing_timeout_ms_);
      this->decline_call();
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
  // Publish restored entity states to HA
  // Call this from YAML: api: on_client_connected: - lambda: 'id(intercom).publish_entity_states();'
  // This ensures HA sees values on boot, reconnect, and HA restart
  ESP_LOGI(TAG, "Publishing entity states to HA");

  if (this->volume_number_ != nullptr) {
    this->volume_number_->publish_state(this->volume_number_->state);
  }
  if (this->mic_gain_number_ != nullptr) {
    this->mic_gain_number_->publish_state(this->mic_gain_number_->state);
  }
  if (this->auto_answer_switch_ != nullptr) {
    this->auto_answer_switch_->publish_state(this->auto_answer_);
  }
#ifdef USE_ESP_AEC
  if (this->aec_switch_ != nullptr) {
    this->aec_switch_->publish_state(this->aec_enabled_);
  }
#endif
}

void IntercomApi::start() {
  if (this->active_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Already active");
    return;
  }

  ESP_LOGI(TAG, "Starting intercom");
  this->set_active_(true);

  // Notify tasks to wake up
  if (this->server_task_handle_) xTaskNotifyGive(this->server_task_handle_);
  if (this->tx_task_handle_) xTaskNotifyGive(this->tx_task_handle_);
  if (this->speaker_task_handle_) xTaskNotifyGive(this->speaker_task_handle_);
}

void IntercomApi::stop() {
  if (!this->active_.load(std::memory_order_acquire)) {
    return;
  }

  ESP_LOGI(TAG, "Stopping intercom");

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
  this->publish_state_();
  this->idle_trigger_.trigger();  // Fire on_idle automation
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

  ESP_LOGI(TAG, "Answering call manually");
  this->send_message_(sock, MessageType::ANSWER);
  this->set_active_(true);
  this->set_streaming_(true);
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
  this->publish_state_();
  this->idle_trigger_.trigger();  // Fire on_idle automation
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
}

void IntercomApi::set_auto_answer(bool enabled) {
  this->auto_answer_ = enabled;
  ESP_LOGI(TAG, "Auto-answer set to %s", enabled ? "ON" : "OFF");
}

void IntercomApi::set_mic_gain_db(float db) {
  // Convert dB to linear gain: gain = 10^(dB/20)
  // Range: -20dB (0.1x) to +20dB (10x)
  db = std::max(-20.0f, std::min(20.0f, db));
  this->mic_gain_ = powf(10.0f, db / 20.0f);
  ESP_LOGD(TAG, "Mic gain set to %.1f dB (%.2fx)", db, this->mic_gain_);
}

#ifdef USE_ESP_AEC
void IntercomApi::set_aec_enabled(bool enabled) {
  if (this->aec_ == nullptr || !this->aec_->is_initialized()) {
    ESP_LOGW(TAG, "Cannot enable AEC: not initialized");
    return;
  }
  if (this->aec_mic_ == nullptr) {
    ESP_LOGW(TAG, "Cannot enable AEC: buffers not allocated");
    return;
  }
  this->aec_enabled_ = enabled;
  // Reset fill levels when toggling
  this->aec_mic_fill_ = 0;
  this->aec_ref_fill_ = 0;
  if (this->spk_ref_buffer_) {
    this->spk_ref_buffer_->reset();
  }
  ESP_LOGI(TAG, "AEC %s", enabled ? "enabled" : "disabled");
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
  // Check for ringing state (connected but waiting for answer)
  if (this->is_ringing()) {
    return "Ringing";
  }
  switch (this->state_) {
    case ConnectionState::DISCONNECTED: return "Idle";
    case ConnectionState::CONNECTING: return "Connecting";
    case ConnectionState::CONNECTED: return "Connected";
    case ConnectionState::STREAMING: return "Streaming";
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
  // PTMP only - in P2P mode, contacts are not used
  if (!this->ptmp_mode_) return;

  // Parse CSV: "Home Assistant,Intercom Mini,Intercom Xiaozhi"
  // Exclude this device's own name from contacts
  this->contacts_.clear();

  if (contacts_csv.empty()) {
    this->contacts_.push_back("Home Assistant");
  } else {
    std::string data = contacts_csv;
    size_t pos;
    while (!data.empty()) {
      pos = data.find(',');
      std::string name = (pos != std::string::npos) ? data.substr(0, pos) : data;

      // Trim whitespace
      while (!name.empty() && name[0] == ' ') name.erase(0, 1);
      while (!name.empty() && name[name.size() - 1] == ' ') name.erase(name.size() - 1);

      // Add if not empty and not this device
      if (!name.empty() && name != this->device_name_) {
        this->contacts_.push_back(name);
      }

      if (pos == std::string::npos) break;
      data.erase(0, pos + 1);
    }
  }

  // Ensure at least "Home Assistant" is available
  if (this->contacts_.empty()) {
    this->contacts_.push_back("Home Assistant");
  }

  // Reset to first contact
  this->contact_index_ = 0;
  this->publish_destination_();
  this->publish_contacts_();  // Publish updated contacts list

  ESP_LOGI(TAG, "Contacts updated: %d devices", this->contacts_.size());
}

void IntercomApi::next_contact() {
  if (!this->ptmp_mode_) return;  // PTMP only
  if (this->contacts_.empty()) return;
  this->contact_index_ = (this->contact_index_ + 1) % this->contacts_.size();
  this->publish_destination_();
  ESP_LOGI(TAG, "Selected contact: %s", this->get_current_destination().c_str());
}

void IntercomApi::prev_contact() {
  if (!this->ptmp_mode_) return;  // PTMP only
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
    this->pending_incoming_call_ = false;  // Call answered, no longer pending
    this->streaming_trigger_.trigger();  // Fire on_streaming automation
  }
  this->publish_state_();
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
          this->close_client_socket_();
          this->set_active_(false);
          this->state_ = ConnectionState::DISCONNECTED;
          this->publish_state_();
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
      // Reset AEC accumulators when paused
      this->aec_mic_fill_ = 0;
      this->aec_ref_fill_ = 0;
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
          }
          xSemaphoreGive(this->spk_ref_mutex_);
        } else {
          memset(this->aec_ref_, 0, ref_bytes_needed);
        }

        // Process AEC
        this->aec_->process(this->aec_mic_, this->aec_ref_, this->aec_out_, this->aec_frame_samples_);

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
            if (this->active_.load(std::memory_order_acquire)) {
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
        if (this->active_.load(std::memory_order_acquire)) {
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
      if (this->aec_enabled_ && this->spk_ref_buffer_ != nullptr) {
        if (xSemaphoreTake(this->spk_ref_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
          this->spk_ref_buffer_->write(audio_chunk, read);
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

    // Real error
    ESP_LOGW(TAG, "Send failed: errno=%d sent=%zd offset=%zu total=%zu", errno, sent, offset, total);
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

      ESP_LOGI(TAG, "Received START from client (auto_answer=%s, no_ring=%s, caller=%s)",
               this->auto_answer_ ? "ON" : "OFF", no_ring ? "YES" : "NO",
               caller_name.empty() ? "(none)" : caller_name.c_str());

      // Publish caller name (even if empty - clears previous)
      if (this->ptmp_mode_) {
        this->publish_caller_(caller_name);
      }

      if (this->auto_answer_ || no_ring) {
        // Auto-answer ON or NO_RING flag: start streaming immediately
        this->pending_incoming_call_ = false;
        this->set_active_(true);
        this->set_streaming_(true);
        this->send_message_(this->client_.socket.load(), MessageType::PONG);
      } else {
        // Auto-answer OFF: go to ringing state, wait for local answer
        this->state_ = ConnectionState::CONNECTED;  // Stay connected but not streaming
        this->pending_incoming_call_ = true;  // Mark as incoming call waiting for answer
        this->send_message_(this->client_.socket.load(), MessageType::RING);
        ESP_LOGI(TAG, "Auto-answer OFF - sending RING, waiting for local answer");
        this->ringing_start_time_ = millis();  // Start ringing timeout timer
        this->publish_state_();  // Publish "Ringing" state
        this->ringing_trigger_.trigger();
      }
      break;
    }

    case MessageType::STOP:
      ESP_LOGI(TAG, "Received STOP from client");
      this->pending_incoming_call_ = false;  // Clear incoming call flag
      // Clear caller name in PTMP mode
      if (this->ptmp_mode_) {
        this->publish_caller_("");
      }
      this->set_streaming_(false);
      this->set_active_(false);
      this->publish_state_();  // Update text sensor to "Idle"
      this->idle_trigger_.trigger();  // Fire on_idle automation
      this->call_end_trigger_.trigger();
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
      // Remote answer from HA (when user clicks Answer in card)
      ESP_LOGI(TAG, "Received ANSWER from client - remote answer");
      if (this->pending_incoming_call_) {
        this->pending_incoming_call_ = false;
        this->set_active_(true);
        this->set_streaming_(true);
        // Send PONG as acknowledgment (like local answer)
        this->send_message_(this->client_.socket.load(), MessageType::PONG);
      } else {
        ESP_LOGW(TAG, "ANSWER received but no pending call");
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
  this->server_running_.store(true, std::memory_order_release);
  return true;
}

void IntercomApi::close_server_socket_() {
  if (this->server_socket_ >= 0) {
    close(this->server_socket_);
    this->server_socket_ = -1;
    this->server_running_.store(false, std::memory_order_release);
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

  // Check if already have a client
  if (this->client_.socket.load() >= 0) {
    ESP_LOGW(TAG, "Rejecting connection - already have client");
    // Send ERROR
    MessageHeader header;
    header.type = static_cast<uint8_t>(MessageType::ERROR);
    header.flags = 0;
    header.length = 1;
    uint8_t error_code = static_cast<uint8_t>(ErrorCode::BUSY);
    uint8_t msg[HEADER_SIZE + 1];
    memcpy(msg, &header, HEADER_SIZE);
    msg[HEADER_SIZE] = error_code;
    send(client_sock, msg, sizeof(msg), 0);
    close(client_sock);
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

  // Handle based on mic_bits configuration
  if (this->mic_bits_ == 32) {
    // 32-bit mic (e.g., SPH0645) - convert to 16-bit
    const int32_t *samples_32 = reinterpret_cast<const int32_t *>(data);
    size_t num_samples = len / sizeof(int32_t);

    int16_t converted[256];
    if (num_samples > 256) num_samples = 256;

    for (size_t i = 0; i < num_samples; i++) {
      // Extract upper 16 bits
      int32_t sample = samples_32[i] >> 16;

      // Optional DC offset removal
      if (this->dc_offset_removal_) {
        this->dc_offset_ = ((this->dc_offset_ * 255) >> 8) + sample;
        sample -= (this->dc_offset_ >> 8);
      }

      // Apply mic gain
      sample = static_cast<int32_t>(sample * this->mic_gain_);

      // Clamp to int16_t range
      if (sample > 32767) sample = 32767;
      if (sample < -32768) sample = -32768;

      converted[i] = static_cast<int16_t>(sample);
    }

    if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
      this->mic_buffer_->write(converted, num_samples * sizeof(int16_t));
      xSemaphoreGive(this->mic_mutex_);
    } else {
      static uint32_t mic_drops_32 = 0;
      if (++mic_drops_32 <= 5 || mic_drops_32 % 100 == 0) {
        ESP_LOGW(TAG, "Mic data dropped (32-bit): %lu total", (unsigned long)mic_drops_32);
      }
    }
  } else {
    // 16-bit mic - apply gain and optional DC offset removal
    const int16_t *samples_16 = reinterpret_cast<const int16_t *>(data);
    size_t num_samples = len / sizeof(int16_t);

    // Only process if gain != 1.0 or dc_offset_removal is enabled
    if (this->mic_gain_ != 1.0f || this->dc_offset_removal_) {
      int16_t converted[512];
      if (num_samples > 512) num_samples = 512;

      for (size_t i = 0; i < num_samples; i++) {
        int32_t sample = samples_16[i];

        if (this->dc_offset_removal_) {
          this->dc_offset_ = ((this->dc_offset_ * 255) >> 8) + sample;
          sample -= (this->dc_offset_ >> 8);
        }

        // Apply mic gain
        sample = static_cast<int32_t>(sample * this->mic_gain_);

        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        converted[i] = static_cast<int16_t>(sample);
      }

      if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        this->mic_buffer_->write(converted, num_samples * sizeof(int16_t));
        xSemaphoreGive(this->mic_mutex_);
      } else {
        static uint32_t mic_drops_16g = 0;
        if (++mic_drops_16g <= 5 || mic_drops_16g % 100 == 0) {
          ESP_LOGW(TAG, "Mic data dropped (16-bit gain): %lu total", (unsigned long)mic_drops_16g);
        }
      }
    } else {
      // Direct passthrough (gain=1.0 and no DC offset removal)
      if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
        this->mic_buffer_->write((void *)data, len);
        xSemaphoreGive(this->mic_mutex_);
      } else {
        static uint32_t mic_drops_16 = 0;
        if (++mic_drops_16 <= 5 || mic_drops_16 % 100 == 0) {
          ESP_LOGW(TAG, "Mic data dropped (16-bit): %lu total", (unsigned long)mic_drops_16);
        }
      }
    }
  }
}

}  // namespace intercom_api
}  // namespace esphome

#endif  // USE_ESP32
