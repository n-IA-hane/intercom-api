#pragma once

#include <cstdint>

namespace esphome {
namespace intercom_api {

// TCP port for audio streaming
static constexpr uint16_t INTERCOM_PORT = 6054;

// Protocol version
static constexpr uint8_t PROTOCOL_VERSION = 1;

// Message types
enum class MessageType : uint8_t {
  AUDIO = 0x01,   // PCM audio data
  START = 0x02,   // Start streaming request
  STOP = 0x03,    // Stop streaming
  PING = 0x04,    // Keep-alive ping
  PONG = 0x05,    // Keep-alive response
  ERROR = 0x06,   // Error response
  RING = 0x07,    // ESP→HA: auto_answer OFF, waiting for local answer
  ANSWER = 0x08,  // ESP→HA: call answered locally, start stream
};

// Message flags
enum class MessageFlags : uint8_t {
  NONE = 0x00,
  END = 0x01,     // Last packet of stream
};

// Error codes
enum class ErrorCode : uint8_t {
  OK = 0x00,
  BUSY = 0x01,           // Already streaming with another client
  INVALID_MSG = 0x02,    // Invalid message format
  NOT_READY = 0x03,      // Component not ready
  INTERNAL = 0xFF,       // Internal error
};

// Audio format constants
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr uint8_t BITS_PER_SAMPLE = 16;
static constexpr uint8_t CHANNELS = 1;
static constexpr size_t AUDIO_CHUNK_SIZE = 512;      // bytes per chunk
static constexpr size_t SAMPLES_PER_CHUNK = 256;     // 512 bytes / 2 bytes per sample
static constexpr uint32_t CHUNK_DURATION_MS = 16;    // 256 samples at 16kHz

// Protocol header
struct __attribute__((packed)) MessageHeader {
  uint8_t type;      // MessageType
  uint8_t flags;     // MessageFlags
  uint16_t length;   // Payload length (little-endian)
};

static constexpr size_t HEADER_SIZE = sizeof(MessageHeader);
static constexpr size_t MAX_AUDIO_CHUNK = 2048;  // Browser may send larger chunks
static constexpr size_t MAX_MESSAGE_SIZE = HEADER_SIZE + MAX_AUDIO_CHUNK + 64;

// Buffer sizes
static constexpr size_t RX_BUFFER_SIZE = 8192;       // ~256ms - fits 4 browser chunks
static constexpr size_t TX_BUFFER_SIZE = 2048;       // ~64ms of audio
static constexpr size_t SOCKET_BUFFER_SIZE = 4096;

// Timeouts
static constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
static constexpr uint32_t PING_INTERVAL_MS = 5000;
static constexpr uint32_t PING_TIMEOUT_MS = 10000;

// ============================================================================
// Broker Protocol (ESP↔ESP via HA relay) - Port 6060
// ============================================================================

static constexpr uint16_t BROKER_PORT = 6060;

// Broker message types (0x10-0x1F range)
enum class BrokerMsgType : uint8_t {
  REGISTER = 0x10,     // ESP→HA: device registration
  INVITE = 0x11,       // ESP→HA: initiate call to target
  RING = 0x12,         // HA→ESP: incoming call notification
  ANSWER = 0x13,       // ESP→HA: accept incoming call
  DECLINE = 0x14,      // ESP→HA: reject incoming call
  HANGUP = 0x15,       // Both: end call
  BYE = 0x16,          // HA→ESP: call ended by peer
  AUDIO = 0x17,        // Both: audio data during call
  CONTACTS = 0x18,     // HA→ESP: list of available devices
  PING = 0x19,         // Both: keepalive
  PONG = 0x1A,         // Both: keepalive response
  ERROR = 0x1B,        // HA→ESP: error notification
};

// Broker error codes
enum class BrokerError : uint8_t {
  NOT_FOUND = 0x01,    // Target device not connected
  BUSY = 0x02,         // Target device already in call
  TIMEOUT = 0x03,      // Call timeout (no answer)
  PROTOCOL = 0x04,     // Protocol error
};

// Decline reasons
enum class DeclineReason : uint8_t {
  BUSY = 0x00,
  REJECT = 0x01,
};

// Call states
enum class CallState : uint8_t {
  IDLE = 0,
  CALLING = 1,         // Outgoing call waiting for answer
  RINGING = 2,         // Incoming call waiting for user
  IN_CALL = 3,         // Active bidirectional audio
};

// Broker header (12 bytes)
struct __attribute__((packed)) BrokerHeader {
  uint8_t type;        // BrokerMsgType
  uint8_t flags;       // Reserved
  uint16_t length;     // Payload length (little-endian)
  uint32_t call_id;    // Call identifier (little-endian)
  uint32_t seq;        // Sequence number for audio (little-endian)
};

static constexpr size_t BROKER_HEADER_SIZE = sizeof(BrokerHeader);

// Broker timeouts
static constexpr uint32_t BROKER_CALL_TIMEOUT_MS = 30000;
static constexpr uint32_t BROKER_RECONNECT_MS = 5000;
static constexpr uint32_t BROKER_PING_INTERVAL_MS = 10000;

// Max contacts
static constexpr size_t MAX_CONTACTS = 16;
static constexpr size_t MAX_DEVICE_ID_LEN = 32;

}  // namespace intercom_api
}  // namespace esphome
