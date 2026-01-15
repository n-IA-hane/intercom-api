#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/ring_buffer.h"

#ifdef USE_MICROPHONE
#include "esphome/components/microphone/microphone.h"
#endif
#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#include "esphome/components/switch/switch.h"
#include "esphome/components/number/number.h"

#include "intercom_protocol.h"

#include <lwip/sockets.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <atomic>
#include <memory>
#include <string>

namespace esphome {
namespace intercom_api {

// Connection state
enum class ConnectionState : uint8_t {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  STREAMING,
};

// Client info
struct ClientInfo {
  int socket{-1};
  struct sockaddr_in addr{};
  uint32_t last_ping{0};
  bool streaming{false};
};

class IntercomApi : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Configuration
#ifdef USE_MICROPHONE
  void set_microphone(microphone::Microphone *mic) { this->microphone_ = mic; }
#endif
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
#endif

  // Runtime control
  void start();
  void stop();
  bool is_active() const { return this->active_.load(std::memory_order_acquire); }
  bool is_connected() const { return this->state_ == ConnectionState::CONNECTED ||
                                     this->state_ == ConnectionState::STREAMING; }

  // Volume control
  void set_volume(float volume);
  float get_volume() const { return this->volume_; }

  // Client mode (for ESP→ESP)
  void connect_to(const std::string &host, uint16_t port = INTERCOM_PORT);
  void disconnect();

  // State getters
  ConnectionState get_state() const { return this->state_; }
  const char *get_state_str() const;

  // Triggers
  Trigger<> *get_connect_trigger() { return &this->connect_trigger_; }
  Trigger<> *get_disconnect_trigger() { return &this->disconnect_trigger_; }
  Trigger<> *get_start_trigger() { return &this->start_trigger_; }
  Trigger<> *get_stop_trigger() { return &this->stop_trigger_; }

 protected:
  // Server task - handles incoming connections
  static void server_task(void *param);
  void server_task_();

  // Audio task - handles mic capture and playback
  static void audio_task(void *param);
  void audio_task_();

  // Protocol handling
  bool send_message_(int socket, MessageType type, MessageFlags flags = MessageFlags::NONE,
                     const uint8_t *data = nullptr, size_t len = 0);
  bool receive_message_(int socket, MessageHeader &header, uint8_t *buffer, size_t buffer_size);
  void handle_message_(const MessageHeader &header, const uint8_t *data);

  // Socket helpers
  bool setup_server_socket_();
  void close_server_socket_();
  void close_client_socket_();
  void accept_client_();

  // Microphone callback
  void on_microphone_data_(const uint8_t *data, size_t len);

  // Components
#ifdef USE_MICROPHONE
  microphone::Microphone *microphone_{nullptr};
#endif
#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};
#endif

  // State
  std::atomic<bool> active_{false};
  std::atomic<bool> server_running_{false};
  ConnectionState state_{ConnectionState::DISCONNECTED};

  // Sockets
  int server_socket_{-1};
  ClientInfo client_;
  SemaphoreHandle_t client_mutex_{nullptr};

  // Client mode (ESP→ESP)
  bool client_mode_{false};
  std::string remote_host_;
  uint16_t remote_port_{INTERCOM_PORT};

  // Buffers
  std::unique_ptr<RingBuffer> mic_buffer_;
  std::unique_ptr<RingBuffer> speaker_buffer_;
  SemaphoreHandle_t mic_mutex_{nullptr};
  SemaphoreHandle_t speaker_mutex_{nullptr};

  // Pre-allocated frame buffers
  uint8_t *tx_buffer_{nullptr};
  uint8_t *rx_buffer_{nullptr};

  // Task handles
  TaskHandle_t server_task_handle_{nullptr};
  TaskHandle_t audio_task_handle_{nullptr};

  // Volume
  float volume_{1.0f};

  // Triggers
  Trigger<> connect_trigger_;
  Trigger<> disconnect_trigger_;
  Trigger<> start_trigger_;
  Trigger<> stop_trigger_;
};

// Switch for on/off control
class IntercomApiSwitch : public switch_::Switch, public Parented<IntercomApi> {
 public:
  void write_state(bool state) override {
    if (state) {
      this->parent_->start();
    } else {
      this->parent_->stop();
    }
    this->publish_state(state);
  }
};

// Number for volume control
class IntercomApiVolume : public number::Number, public Parented<IntercomApi> {
 public:
  void control(float value) override {
    this->parent_->set_volume(value / 100.0f);
    this->publish_state(value);
  }
};

}  // namespace intercom_api
}  // namespace esphome

#endif  // USE_ESP32
