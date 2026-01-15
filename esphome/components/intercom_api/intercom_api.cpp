#include "intercom_api.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <errno.h>
#include <fcntl.h>

namespace esphome {
namespace intercom_api {

static const char *TAG = "intercom_api";

void IntercomApi::setup() {
  ESP_LOGI(TAG, "Setting up Intercom API...");

  // Create mutexes
  this->client_mutex_ = xSemaphoreCreateMutex();
  this->mic_mutex_ = xSemaphoreCreateMutex();
  this->speaker_mutex_ = xSemaphoreCreateMutex();

  if (!this->client_mutex_ || !this->mic_mutex_ || !this->speaker_mutex_) {
    ESP_LOGE(TAG, "Failed to create mutexes");
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

  if (!this->tx_buffer_ || !this->rx_buffer_) {
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

  // Create server task
  BaseType_t ok = xTaskCreatePinnedToCore(
      IntercomApi::server_task,
      "intercom_srv",
      4096,
      this,
      5,
      &this->server_task_handle_,
      1  // Core 1
  );

  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create server task");
    this->mark_failed();
    return;
  }

  // Create audio task
  ok = xTaskCreatePinnedToCore(
      IntercomApi::audio_task,
      "intercom_audio",
      8192,
      this,
      6,  // Higher priority than server
      &this->audio_task_handle_,
      0  // Core 0
  );

  if (ok != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio task");
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "Intercom API ready on port %d", INTERCOM_PORT);
}

void IntercomApi::loop() {
  // Main loop - mostly handled by tasks
  // Just update state for HA
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
}

void IntercomApi::start() {
  if (this->active_.load(std::memory_order_acquire)) {
    ESP_LOGW(TAG, "Already active");
    return;
  }

  ESP_LOGI(TAG, "Starting intercom");
  this->active_.store(true, std::memory_order_release);

  // Start microphone
#ifdef USE_MICROPHONE
  if (this->microphone_ != nullptr) {
    this->microphone_->start();
  }
#endif

  // Start speaker
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->speaker_->start();
  }
#endif

  // Notify server task
  if (this->server_task_handle_) {
    xTaskNotifyGive(this->server_task_handle_);
  }

  this->start_trigger_.trigger();
}

void IntercomApi::stop() {
  if (!this->active_.load(std::memory_order_acquire)) {
    return;
  }

  ESP_LOGI(TAG, "Stopping intercom");
  this->active_.store(false, std::memory_order_release);

  // Close client connection
  this->close_client_socket_();

  // Clear buffers
  if (this->mic_buffer_) this->mic_buffer_->reset();
  if (this->speaker_buffer_) this->speaker_buffer_->reset();

  // Stop microphone
#ifdef USE_MICROPHONE
  if (this->microphone_ != nullptr) {
    this->microphone_->stop();
  }
#endif

  // Stop speaker
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->speaker_->stop();
  }
#endif

  this->state_ = ConnectionState::DISCONNECTED;
  this->stop_trigger_.trigger();
}

void IntercomApi::set_volume(float volume) {
  this->volume_ = std::max(0.0f, std::min(1.0f, volume));
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    this->speaker_->set_volume(this->volume_);
  }
#endif
}

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
  switch (this->state_) {
    case ConnectionState::DISCONNECTED: return "Disconnected";
    case ConnectionState::CONNECTING: return "Connecting";
    case ConnectionState::CONNECTED: return "Connected";
    case ConnectionState::STREAMING: return "Streaming";
    default: return "Unknown";
  }
}

// === Server Task ===

void IntercomApi::server_task(void *param) {
  static_cast<IntercomApi *>(param)->server_task_();
}

void IntercomApi::server_task_() {
  ESP_LOGI(TAG, "Server task started");

  while (true) {
    // Wait for activation or check periodically
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

    if (!this->active_.load(std::memory_order_acquire)) {
      // Not active - ensure server socket is closed
      if (this->server_socket_ >= 0) {
        this->close_server_socket_();
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Client mode - connect to remote
    if (this->client_mode_) {
      if (this->client_.socket < 0) {
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

        ret = select(sock + 1, nullptr, &write_fds, nullptr, &tv);
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
        this->client_.socket = sock;
        this->client_.streaming = false;
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
      if (this->client_.socket < 0) {
        this->accept_client_();
      }
    }

    // Handle existing client
    if (this->client_.socket >= 0) {
      // Check for incoming data
      fd_set read_fds;
      FD_ZERO(&read_fds);
      FD_SET(this->client_.socket, &read_fds);
      struct timeval tv = {.tv_sec = 0, .tv_usec = 10000};  // 10ms

      int ret = select(this->client_.socket + 1, &read_fds, nullptr, nullptr, &tv);
      if (ret > 0 && FD_ISSET(this->client_.socket, &read_fds)) {
        MessageHeader header;
        if (this->receive_message_(this->client_.socket, header, this->rx_buffer_, MAX_MESSAGE_SIZE)) {
          this->handle_message_(header, this->rx_buffer_ + HEADER_SIZE);
        } else {
          // Connection closed or error
          ESP_LOGI(TAG, "Client disconnected");
          this->close_client_socket_();
          this->state_ = ConnectionState::DISCONNECTED;
          this->disconnect_trigger_.trigger();
        }
      }

      // Send ping if needed
      if (millis() - this->client_.last_ping > PING_INTERVAL_MS) {
        this->send_message_(this->client_.socket, MessageType::PING);
        this->client_.last_ping = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1));  // Yield
  }
}

// === Audio Task ===

void IntercomApi::audio_task(void *param) {
  static_cast<IntercomApi *>(param)->audio_task_();
}

void IntercomApi::audio_task_() {
  ESP_LOGI(TAG, "Audio task started");

  uint8_t audio_chunk[AUDIO_CHUNK_SIZE];

  while (true) {
    if (!this->active_.load(std::memory_order_acquire) || this->client_.socket < 0) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // === TX: Mic buffer → Network ===
    if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
      while (this->mic_buffer_->available() >= AUDIO_CHUNK_SIZE) {
        size_t read = this->mic_buffer_->read(audio_chunk, AUDIO_CHUNK_SIZE, 0);
        xSemaphoreGive(this->mic_mutex_);

        if (read == AUDIO_CHUNK_SIZE && this->client_.socket >= 0) {
          xSemaphoreTake(this->client_mutex_, portMAX_DELAY);
          this->send_message_(this->client_.socket, MessageType::AUDIO, MessageFlags::NONE,
                              audio_chunk, AUDIO_CHUNK_SIZE);
          xSemaphoreGive(this->client_mutex_);
        }

        if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(1)) != pdTRUE) {
          break;
        }
      }
      xSemaphoreGive(this->mic_mutex_);
    }

    // === RX: Speaker buffer → Speaker ===
#ifdef USE_SPEAKER
    if (this->speaker_ != nullptr) {
      if (xSemaphoreTake(this->speaker_mutex_, pdMS_TO_TICKS(5)) == pdTRUE) {
        while (this->speaker_buffer_->available() >= AUDIO_CHUNK_SIZE) {
          size_t read = this->speaker_buffer_->read(audio_chunk, AUDIO_CHUNK_SIZE, 0);
          xSemaphoreGive(this->speaker_mutex_);

          if (read == AUDIO_CHUNK_SIZE && this->volume_ > 0.001f) {
            this->speaker_->play(audio_chunk, AUDIO_CHUNK_SIZE, pdMS_TO_TICKS(10));
          }

          if (xSemaphoreTake(this->speaker_mutex_, pdMS_TO_TICKS(1)) != pdTRUE) {
            break;
          }
        }
        xSemaphoreGive(this->speaker_mutex_);
      }
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// === Protocol ===

bool IntercomApi::send_message_(int socket, MessageType type, MessageFlags flags,
                                 const uint8_t *data, size_t len) {
  if (socket < 0) return false;

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
  ssize_t sent = send(socket, this->tx_buffer_, total, 0);

  if (sent != (ssize_t)total) {
    ESP_LOGW(TAG, "Send failed: %d (expected %zu)", errno, total);
    return false;
  }

  return true;
}

bool IntercomApi::receive_message_(int socket, MessageHeader &header, uint8_t *buffer, size_t buffer_size) {
  // Read header
  ssize_t received = recv(socket, buffer, HEADER_SIZE, MSG_WAITALL);
  if (received != HEADER_SIZE) {
    return false;
  }

  memcpy(&header, buffer, HEADER_SIZE);

  // Validate length
  if (header.length > buffer_size - HEADER_SIZE) {
    ESP_LOGW(TAG, "Message too large: %d", header.length);
    return false;
  }

  // Read payload
  if (header.length > 0) {
    received = recv(socket, buffer + HEADER_SIZE, header.length, MSG_WAITALL);
    if (received != header.length) {
      return false;
    }
  }

  return true;
}

void IntercomApi::handle_message_(const MessageHeader &header, const uint8_t *data) {
  MessageType type = static_cast<MessageType>(header.type);

  switch (type) {
    case MessageType::AUDIO:
      // Write to speaker buffer
      if (xSemaphoreTake(this->speaker_mutex_, pdMS_TO_TICKS(1)) == pdTRUE) {
        this->speaker_buffer_->write((void *)data, header.length);
        xSemaphoreGive(this->speaker_mutex_);
      }
      if (this->state_ != ConnectionState::STREAMING) {
        this->state_ = ConnectionState::STREAMING;
      }
      break;

    case MessageType::START:
      ESP_LOGI(TAG, "Received START");
      this->client_.streaming = true;
      this->state_ = ConnectionState::STREAMING;
      // Send PONG as ACK
      this->send_message_(this->client_.socket, MessageType::PONG);
      break;

    case MessageType::STOP:
      ESP_LOGI(TAG, "Received STOP");
      this->client_.streaming = false;
      this->state_ = ConnectionState::CONNECTED;
      break;

    case MessageType::PING:
      this->send_message_(this->client_.socket, MessageType::PONG);
      break;

    case MessageType::PONG:
      this->client_.last_ping = millis();
      if (this->client_mode_ && this->state_ == ConnectionState::CONNECTED) {
        // ACK for START - begin streaming
        this->client_.streaming = true;
        this->state_ = ConnectionState::STREAMING;
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
  xSemaphoreTake(this->client_mutex_, portMAX_DELAY);
  if (this->client_.socket >= 0) {
    // Send STOP if streaming
    if (this->client_.streaming) {
      this->send_message_(this->client_.socket, MessageType::STOP);
    }
    close(this->client_.socket);
    this->client_.socket = -1;
    this->client_.streaming = false;
  }
  xSemaphoreGive(this->client_mutex_);
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
  if (this->client_.socket >= 0) {
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

  // Set non-blocking
  int flags = fcntl(client_sock, F_GETFL, 0);
  fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
  ESP_LOGI(TAG, "Client connected from %s", ip_str);

  xSemaphoreTake(this->client_mutex_, portMAX_DELAY);
  this->client_.socket = client_sock;
  this->client_.addr = client_addr;
  this->client_.last_ping = millis();
  this->client_.streaming = false;
  xSemaphoreGive(this->client_mutex_);

  this->state_ = ConnectionState::CONNECTED;
  this->connect_trigger_.trigger();
}

// === Microphone Callback ===

void IntercomApi::on_microphone_data_(const uint8_t *data, size_t len) {
  if (!this->active_.load(std::memory_order_acquire)) {
    return;
  }

  if (this->client_.socket < 0 || !this->client_.streaming) {
    return;
  }

  if (xSemaphoreTake(this->mic_mutex_, pdMS_TO_TICKS(1)) == pdTRUE) {
    this->mic_buffer_->write((void *)data, len);
    xSemaphoreGive(this->mic_mutex_);
  }
}

}  // namespace intercom_api
}  // namespace esphome

#endif  // USE_ESP32
