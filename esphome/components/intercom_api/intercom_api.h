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
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_ESP_AEC
#include "esphome/components/esp_aec/esp_aec.h"
#endif

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

// Client info - socket and streaming are atomic for thread safety
struct ClientInfo {
  std::atomic<int> socket{-1};
  struct sockaddr_in addr{};
  uint32_t last_ping{0};
  std::atomic<bool> streaming{false};
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
  void set_mic_bits(int bits) { this->mic_bits_ = bits; }
  void set_dc_offset_removal(bool enabled) { this->dc_offset_removal_ = enabled; }

#ifdef USE_ESP_AEC
  void set_aec(esp_aec::EspAec *aec) { this->aec_ = aec; }
  void set_aec_enabled(bool enabled);
  bool is_aec_enabled() const { return this->aec_enabled_; }
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

  // Auto-answer control (for incoming calls)
  void set_auto_answer(bool enabled);
  bool is_auto_answer() const { return this->auto_answer_; }

  // Manual answer for incoming call (when auto_answer is OFF)
  void answer_call();
  void decline_call();
  bool is_ringing() const { return this->state_ == ConnectionState::CONNECTED &&
                                   this->client_.socket.load() >= 0 &&
                                   !this->client_.streaming.load(); }

  // Mic gain control (dB scale: -20 to +20)
  void set_mic_gain_db(float db);
  float get_mic_gain() const { return this->mic_gain_; }

  // Client mode (for ESP→ESP direct - legacy)
  void connect_to(const std::string &host, uint16_t port = INTERCOM_PORT);
  void disconnect();

  // State getters
  ConnectionState get_state() const { return this->state_; }
  const char *get_state_str() const;

  // State sensor registration
  void set_state_sensor(text_sensor::TextSensor *sensor) { this->state_sensor_ = sensor; }
  void publish_state_();

#ifdef USE_INTERCOM_BROKER
  // Broker configuration
  void set_broker_host(const std::string &host) { this->broker_host_ = host; }
  void set_broker_port(uint16_t port) { this->broker_port_ = port; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }

  // Broker call control
  void broker_call(const std::string &target_device);
  void broker_answer();
  void broker_decline();
  void broker_hangup();

  // Broker state
  CallState get_call_state() const { return this->call_state_; }
  const char *get_call_state_str() const;
  const std::string &get_caller_name() const { return this->caller_name_; }
  const std::string &get_target_name() const { return this->target_name_; }
  bool is_broker_connected() const { return this->broker_connected_.load(); }

  // Contact list
  size_t get_contact_count() const { return this->contact_count_; }
  const char *get_contact_name(size_t index) const;
  bool is_contact_busy(size_t index) const;
#endif

  // Triggers
  Trigger<> *get_connect_trigger() { return &this->connect_trigger_; }
  Trigger<> *get_disconnect_trigger() { return &this->disconnect_trigger_; }
  Trigger<> *get_start_trigger() { return &this->start_trigger_; }
  Trigger<> *get_stop_trigger() { return &this->stop_trigger_; }
  Trigger<> *get_ringing_trigger() { return &this->ringing_trigger_; }
  Trigger<> *get_call_end_trigger() { return &this->call_end_trigger_; }

 protected:
  // Server task - handles incoming connections and receiving data
  static void server_task(void *param);
  void server_task_();

  // TX task - handles mic capture and sending to network (Core 0)
  static void tx_task(void *param);
  void tx_task_();

  // Speaker task - handles playback from speaker buffer (Core 0)
  static void speaker_task(void *param);
  void speaker_task_();

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

  // State helpers - consolidate duplicated start/stop logic
  void set_active_(bool on);
  void set_streaming_(bool on);

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
  text_sensor::TextSensor *state_sensor_{nullptr};

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
  uint8_t *tx_buffer_{nullptr};      // Used by server_task for control messages
  uint8_t *rx_buffer_{nullptr};      // Used by server_task for receiving
  uint8_t *audio_tx_buffer_{nullptr}; // Used by tx_task for audio (no mutex needed)
  SemaphoreHandle_t send_mutex_{nullptr};  // Protects tx_buffer_ during send

  // Task handles
  TaskHandle_t server_task_handle_{nullptr};
  TaskHandle_t tx_task_handle_{nullptr};
  TaskHandle_t speaker_task_handle_{nullptr};

  // Volume
  float volume_{1.0f};

  // Auto-answer (default true for backward compatibility)
  bool auto_answer_{true};

  // Mic gain (applied before sending to network)
  float mic_gain_{1.0f};

  // Mic configuration
  int mic_bits_{16};              // 16 or 32 bit mic
  bool dc_offset_removal_{false}; // Enable for mics with DC bias (SPH0645)
  int32_t dc_offset_{0};          // Running DC offset value

#ifdef USE_ESP_AEC
  // AEC (Acoustic Echo Cancellation)
  esp_aec::EspAec *aec_{nullptr};
  bool aec_enabled_{false};

  // Speaker reference buffer for AEC (fed by speaker_task)
  std::unique_ptr<RingBuffer> spk_ref_buffer_;
  SemaphoreHandle_t spk_ref_mutex_{nullptr};

  // AEC frame accumulation (frame_size = 512 samples = 32ms at 16kHz)
  int aec_frame_samples_{0};
  int16_t *aec_mic_{nullptr};   // Accumulated mic samples (frame_size)
  int16_t *aec_ref_{nullptr};   // Speaker reference samples (frame_size)
  int16_t *aec_out_{nullptr};   // AEC output samples (frame_size)
  size_t aec_mic_fill_{0};      // Current fill level in aec_mic_
  size_t aec_ref_fill_{0};      // Current fill level in aec_ref_
#endif

  // Triggers
  Trigger<> connect_trigger_;
  Trigger<> disconnect_trigger_;
  Trigger<> start_trigger_;
  Trigger<> stop_trigger_;
  Trigger<> ringing_trigger_;
  Trigger<> call_end_trigger_;  // Fires when call ends (hangup, decline, or connection lost)

#ifdef USE_INTERCOM_BROKER
  // Broker task
  static void broker_task(void *param);
  void broker_task_();

  // Broker protocol handling
  bool send_broker_message_(BrokerMsgType type, uint32_t call_id, uint32_t seq,
                            const uint8_t *data = nullptr, size_t len = 0);
  void handle_broker_message_(const BrokerHeader &header, const uint8_t *data);

  // Broker config
  std::string broker_host_;
  uint16_t broker_port_{BROKER_PORT};
  std::string device_name_;

  // Broker connection
  std::atomic<bool> broker_connected_{false};
  int broker_socket_{-1};
  TaskHandle_t broker_task_handle_{nullptr};
  uint8_t *broker_rx_buffer_{nullptr};
  uint8_t *broker_tx_buffer_{nullptr};

  // Call state
  CallState call_state_{CallState::IDLE};
  uint32_t current_call_id_{0};
  std::string caller_name_;      // Who's calling us (when RINGING)
  std::string target_name_;      // Who we're calling (when CALLING)
  uint32_t broker_audio_seq_{0}; // Sequence number for outgoing audio

  // Contact list (received from broker)
  struct Contact {
    char name[MAX_DEVICE_ID_LEN];
    bool busy;
  };
  Contact contacts_[MAX_CONTACTS];
  size_t contact_count_{0};
#endif
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

// Number for mic gain control (dB scale)
class IntercomApiMicGain : public number::Number, public Parented<IntercomApi> {
 public:
  void control(float value) override {
    this->parent_->set_mic_gain_db(value);
    this->publish_state(value);
  }
};

// Switch for auto-answer control
class IntercomApiAutoAnswer : public switch_::Switch, public Parented<IntercomApi> {
 public:
  void write_state(bool state) override {
    this->parent_->set_auto_answer(state);
    this->publish_state(state);
  }
};

// Text sensor for intercom state (Idle, Ringing, Streaming, etc.)
class IntercomApiStateSensor : public text_sensor::TextSensor, public Component, public Parented<IntercomApi> {
 public:
  void setup() override {
    this->parent_->set_state_sensor(this);
  }

  float get_setup_priority() const override { return setup_priority::DATA; }
};

}  // namespace intercom_api
}  // namespace esphome

#endif  // USE_ESP32
