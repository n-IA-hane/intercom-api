#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/core/ring_buffer.h"

#ifdef USE_MICROPHONE
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/microphone/microphone_source.h"
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
#include <vector>

namespace esphome {
namespace intercom_api {

// TCP connection state (low-level)
enum class ConnectionState : uint8_t {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
  STREAMING,
};

// Call state (high-level FSM for display/triggers)
enum class CallState : uint8_t {
  IDLE,        // No call in progress
  OUTGOING,    // We initiated a call, waiting for remote to answer
  INCOMING,    // Someone is calling us (before ringing starts)
  RINGING,     // Actively ringing/notifying user
  ANSWERING,   // Answer accepted, setting up stream
  STREAMING,   // Audio active
};

// Hangup/failure reasons
enum class CallEndReason : uint8_t {
  NONE,
  LOCAL_HANGUP,
  REMOTE_HANGUP,
  DECLINED,
  TIMEOUT,
  BUSY,
  UNREACHABLE,
  PROTOCOL_ERROR,
  BRIDGE_ERROR,
};

inline const char *call_state_to_str(CallState state) {
  switch (state) {
    case CallState::IDLE: return "idle";
    case CallState::OUTGOING: return "outgoing";
    case CallState::INCOMING: return "incoming";
    case CallState::RINGING: return "ringing";
    case CallState::ANSWERING: return "answering";
    case CallState::STREAMING: return "streaming";
    default: return "unknown";
  }
}

inline const char *call_end_reason_to_str(CallEndReason reason) {
  switch (reason) {
    case CallEndReason::NONE: return "";
    case CallEndReason::LOCAL_HANGUP: return "local_hangup";
    case CallEndReason::REMOTE_HANGUP: return "remote_hangup";
    case CallEndReason::DECLINED: return "declined";
    case CallEndReason::TIMEOUT: return "timeout";
    case CallEndReason::BUSY: return "busy";
    case CallEndReason::UNREACHABLE: return "unreachable";
    case CallEndReason::PROTOCOL_ERROR: return "protocol_error";
    case CallEndReason::BRIDGE_ERROR: return "bridge_error";
    default: return "unknown";
  }
}

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

  // Call from YAML api: on_client_connected: to publish restored entity states to HA
  void publish_entity_states();

  // Configuration
#ifdef USE_MICROPHONE
  void set_microphone_source(microphone::MicrophoneSource *source) { this->microphone_source_ = source; }
#endif
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
#endif

  void set_dc_offset_removal(bool enabled) { this->dc_offset_removal_ = enabled; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }

#ifdef USE_ESP_AEC
  void set_aec(esp_aec::EspAec *aec) { this->aec_ = aec; }
  void set_aec_enabled(bool enabled);
  bool is_aec_enabled() const { return this->aec_enabled_; }
#endif

  // Runtime control
  void start();
  void stop();
  bool is_active() const {
    // Use FSM state instead of atomic flag for more accurate state
    CallState cs = this->call_state_.load(std::memory_order_acquire);
    return cs == CallState::STREAMING || cs == CallState::ANSWERING || cs == CallState::OUTGOING;
  }
  bool is_connected() const {
    ConnectionState s = this->state_.load(std::memory_order_acquire);
    return s == ConnectionState::CONNECTED || s == ConnectionState::STREAMING;
  }

  // Volume control
  void set_volume(float volume);
  float get_volume() const { return this->volume_; }

  // Auto-answer control (for incoming calls)
  void set_auto_answer(bool enabled);
  bool is_auto_answer() const { return this->auto_answer_; }

  // Manual answer for incoming call (when auto_answer is OFF)
  void answer_call();
  void decline_call();
  bool is_ringing() const { return this->call_state_.load(std::memory_order_acquire) == CallState::RINGING; }
  bool is_idle() const { return this->call_state_.load(std::memory_order_acquire) == CallState::IDLE; }
  bool is_streaming() const { return this->call_state_.load(std::memory_order_acquire) == CallState::STREAMING; }

  // Smart call toggle: ringing → answer, active → hangup, idle → start
  void call_toggle();

  // Ringing timeout configuration (auto-hangup if not answered)
  void set_ringing_timeout(uint32_t timeout_ms) { this->ringing_timeout_ms_ = timeout_ms; }

  // Mic gain control (dB scale: -20 to +20)
  void set_mic_gain_db(float db);
  float get_mic_gain() const { return this->mic_gain_; }

  // State getters
  ConnectionState get_state() const { return this->state_.load(std::memory_order_acquire); }
  const char *get_state_str() const;

  // Mode setting
  void set_full_mode(bool full) { this->full_mode_ = full; }
  bool is_full_mode() const { return this->full_mode_; }

  // Sensor registration
  void set_state_sensor(text_sensor::TextSensor *sensor) { this->state_sensor_ = sensor; }
  void set_destination_sensor(text_sensor::TextSensor *sensor) { this->destination_sensor_ = sensor; }
  void set_caller_sensor(text_sensor::TextSensor *sensor) { this->caller_sensor_ = sensor; }
  void set_contacts_sensor(text_sensor::TextSensor *sensor) { this->contacts_sensor_ = sensor; }

  // Entity registration (for state sync after boot)
  void register_auto_answer_switch(switch_::Switch *sw) { this->auto_answer_switch_ = sw; }
  void register_volume_number(number::Number *num) { this->volume_number_ = num; }
  void register_mic_gain_number(number::Number *num) { this->mic_gain_number_ = num; }
#ifdef USE_ESP_AEC
  void register_aec_switch(switch_::Switch *sw) { this->aec_switch_ = sw; }
#endif

  // Contacts management (full mode only)
  void set_contacts(const std::string &contacts_csv);
  void next_contact();
  void prev_contact();
  const std::string &get_current_destination() const;
  std::string get_caller() const { return this->caller_sensor_ ? this->caller_sensor_->state : ""; }
  std::string get_contacts_csv() const;

  // Legacy triggers (backward compatible)
  Trigger<> *get_connect_trigger() { return &this->connect_trigger_; }
  Trigger<> *get_disconnect_trigger() { return &this->disconnect_trigger_; }
  Trigger<> *get_start_trigger() { return &this->start_trigger_; }
  Trigger<> *get_stop_trigger() { return &this->stop_trigger_; }
  Trigger<> *get_ringing_trigger() { return &this->ringing_trigger_; }
  Trigger<> *get_streaming_trigger() { return &this->streaming_trigger_; }
  Trigger<> *get_idle_trigger() { return &this->idle_trigger_; }
  // New FSM triggers
  Trigger<> *get_outgoing_call_trigger() { return &this->outgoing_call_trigger_; }
  Trigger<> *get_answered_trigger() { return &this->answered_trigger_; }
  Trigger<std::string> *get_hangup_trigger() { return &this->hangup_trigger_; }
  Trigger<std::string> *get_call_failed_trigger() { return &this->call_failed_trigger_; }

  // Call state getter
  CallState get_call_state() const { return this->call_state_.load(std::memory_order_acquire); }
  const char *get_call_state_str() const { return call_state_to_str(this->call_state_.load(std::memory_order_acquire)); }

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

  // Publish sensor values
  void publish_state_();
  void publish_destination_();
  void publish_caller_(const std::string &caller_name);
  void publish_contacts_();

  // Call state FSM
  void set_call_state_(CallState new_state);
  void end_call_(CallEndReason reason);

#ifdef USE_ESP_AEC
  // AEC helper
  void reset_aec_buffers_();
#endif

  // Components
#ifdef USE_MICROPHONE
  microphone::MicrophoneSource *microphone_source_{nullptr};
#endif
#ifdef USE_SPEAKER
  speaker::Speaker *speaker_{nullptr};
#endif

  // Mode and state
  bool full_mode_{false};  // simple (false) or full (true) mode
  std::atomic<bool> active_{false};
  std::atomic<ConnectionState> state_{ConnectionState::DISCONNECTED};
  std::atomic<CallState> call_state_{CallState::IDLE};  // High-level FSM state (source of truth)

  // Sensors (state is always present, others only in full mode)
  text_sensor::TextSensor *state_sensor_{nullptr};
  text_sensor::TextSensor *destination_sensor_{nullptr};  // full: selected contact
  text_sensor::TextSensor *caller_sensor_{nullptr};       // full: who is calling
  text_sensor::TextSensor *contacts_sensor_{nullptr};     // full: CSV of contacts

  // Registered entities (for state sync after boot)
  switch_::Switch *auto_answer_switch_{nullptr};
  number::Number *volume_number_{nullptr};
  number::Number *mic_gain_number_{nullptr};
#ifdef USE_ESP_AEC
  switch_::Switch *aec_switch_{nullptr};
#endif

  // Contacts management (full mode only)
  std::vector<std::string> contacts_{"Home Assistant"};  // Default contact
  size_t contact_index_{0};
  std::string device_name_;  // This device's friendly name (to exclude from contacts)

  // Sockets
  int server_socket_{-1};
  ClientInfo client_;
  SemaphoreHandle_t client_mutex_{nullptr};

  // Buffers
  std::unique_ptr<RingBuffer> mic_buffer_;
  std::unique_ptr<RingBuffer> speaker_buffer_;


  // Pre-allocated frame buffers
  uint8_t *tx_buffer_{nullptr};      // Used by server_task for control messages
  uint8_t *rx_buffer_{nullptr};      // Used by server_task for receiving
  uint8_t *audio_tx_buffer_{nullptr}; // Used by tx_task for audio (no mutex needed)
  SemaphoreHandle_t send_mutex_{nullptr};  // Protects tx_buffer_ during send

  // Task handles
  TaskHandle_t server_task_handle_{nullptr};
  TaskHandle_t tx_task_handle_{nullptr};
  TaskHandle_t speaker_task_handle_{nullptr};

  // Speaker single-owner: only speaker_task_ touches speaker hardware
  // This prevents race conditions between play() and stop()
  std::atomic<bool> speaker_stop_requested_{false};
  SemaphoreHandle_t speaker_stopped_sem_{nullptr};  // Signaled when speaker has stopped

  // Volume
  float volume_{1.0f};

  // Auto-answer (default true for backward compatibility)
  bool auto_answer_{true};

  // Call timeout (0 = disabled, otherwise auto-hangup after this many ms)
  uint32_t ringing_timeout_ms_{0};
  uint32_t ringing_start_time_{0};
  uint32_t outgoing_start_time_{0};

  // Mic gain (applied before sending to network)
  float mic_gain_{1.0f};
  float mic_gain_db_{0.0f};  // UI-friendly value (dB) for persistence

  // === Settings persistence (local flash) ===
  static constexpr uint8_t SETTINGS_VERSION = 1;

  struct StoredSettings {
    uint8_t version{SETTINGS_VERSION};
    uint8_t volume_pct{100};   // 0..100
    int8_t mic_gain_db{0};     // -20..+20
    uint8_t reserved{0};       // Padding for alignment
  };

  ESPPreferenceObject settings_pref_{};
  bool suppress_save_{false};
  bool save_scheduled_{false};

  void load_settings_();
  void schedule_save_settings_();
  void save_settings_();

  // Mic configuration
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
#endif

  // Legacy triggers (backward compatible)
  Trigger<> connect_trigger_;
  Trigger<> disconnect_trigger_;
  Trigger<> start_trigger_;
  Trigger<> stop_trigger_;
  Trigger<> ringing_trigger_;
  Trigger<> streaming_trigger_;
  Trigger<> idle_trigger_;
  // New FSM triggers
  Trigger<> outgoing_call_trigger_;   // We initiated a call
  Trigger<> answered_trigger_;        // Call was answered (local or remote)
  Trigger<std::string> hangup_trigger_;      // Call ended normally (reason string)
  Trigger<std::string> call_failed_trigger_; // Call failed (reason string)
};

// Switch for on/off control (simple - ESPHome handles restore)
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

// Number for volume control (simple - parent syncs after boot)
class IntercomApiVolume : public number::Number, public Parented<IntercomApi> {
 public:
  void control(float value) override {
    this->parent_->set_volume(value / 100.0f);
    this->publish_state(value);
  }
};

// Number for mic gain control (dB scale, simple - parent syncs after boot)
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

// Note: State and Destination sensors are plain TextSensor* created via new_text_sensor()
// No custom classes needed - the parent IntercomApi just holds pointers to them

// === Actions for YAML automation syntax ===
// Usage: intercom_api.next_contact:, intercom_api.start:, etc.

template<typename... Ts>
class NextContactAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  void play(const Ts &...x) override { this->parent_->next_contact(); }
};

template<typename... Ts>
class PrevContactAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  void play(const Ts &...x) override { this->parent_->prev_contact(); }
};

template<typename... Ts>
class StartAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  void play(const Ts &...x) override { this->parent_->start(); }
};

template<typename... Ts>
class StopAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  void play(const Ts &...x) override { this->parent_->stop(); }
};

template<typename... Ts>
class AnswerCallAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  void play(const Ts &...x) override { this->parent_->answer_call(); }
};

template<typename... Ts>
class DeclineCallAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  void play(const Ts &...x) override { this->parent_->decline_call(); }
};

// === Parameterized actions ===

template<typename... Ts>
class SetVolumeAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  TEMPLATABLE_VALUE(float, volume)
  void play(const Ts &...x) override {
    this->parent_->set_volume(this->volume_.value(x...));
  }
};

template<typename... Ts>
class SetMicGainDbAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  TEMPLATABLE_VALUE(float, gain_db)
  void play(const Ts &...x) override {
    this->parent_->set_mic_gain_db(this->gain_db_.value(x...));
  }
};

template<typename... Ts>
class SetContactsAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  TEMPLATABLE_VALUE(std::string, contacts_csv)
  void play(const Ts &...x) override {
    this->parent_->set_contacts(this->contacts_csv_.value(x...));
  }
};

template<typename... Ts>
class CallToggleAction : public Action<Ts...>, public Parented<IntercomApi> {
 public:
  void play(const Ts &...x) override { this->parent_->call_toggle(); }
};

// === Switch platform classes with restore support ===

// AEC switch (only available when USE_ESP_AEC is defined)
#ifdef USE_ESP_AEC
class IntercomAecSwitch : public switch_::Switch, public Parented<IntercomApi> {
 public:
  void write_state(bool state) override {
    this->parent_->set_aec_enabled(state);
    // Publish ACTUAL state (set_aec_enabled may refuse if AEC not initialized)
    this->publish_state(this->parent_->is_aec_enabled());
  }
};
#endif

// === Condition classes for ESPHome automation ===

template<typename... Ts>
class IntercomIsIdleCondition : public Condition<Ts...>, public Parented<IntercomApi> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_idle(); }
};

template<typename... Ts>
class IntercomIsRingingCondition : public Condition<Ts...>, public Parented<IntercomApi> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_ringing(); }
};

template<typename... Ts>
class IntercomIsStreamingCondition : public Condition<Ts...>, public Parented<IntercomApi> {
 public:
  bool check(const Ts &...x) override { return this->parent_->is_streaming(); }
};

template<typename... Ts>
class IntercomIsCallingCondition : public Condition<Ts...>, public Parented<IntercomApi> {
 public:
  bool check(const Ts &...x) override { return this->parent_->get_call_state() == CallState::OUTGOING; }
};

template<typename... Ts>
class IntercomIsIncomingCondition : public Condition<Ts...>, public Parented<IntercomApi> {
 public:
  bool check(const Ts &...x) override {
    CallState cs = this->parent_->get_call_state();
    return cs == CallState::INCOMING || cs == CallState::RINGING;
  }
};

template<typename... Ts>
class IntercomIsAnsweringCondition : public Condition<Ts...>, public Parented<IntercomApi> {
 public:
  bool check(const Ts &...x) override {
    return this->parent_->get_call_state() == CallState::ANSWERING;
  }
};

template<typename... Ts>
class IntercomIsInCallCondition : public Condition<Ts...>, public Parented<IntercomApi> {
 public:
  bool check(const Ts &...x) override {
    auto state = this->parent_->get_call_state();
    return state == CallState::STREAMING || state == CallState::ANSWERING;
  }
};

}  // namespace intercom_api
}  // namespace esphome

#endif  // USE_ESP32
