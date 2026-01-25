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
  END = 0x01,      // Last packet of stream
  NO_RING = 0x02,  // START flag: skip ringing, start streaming directly (for caller in bridge)
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

// AEC reference delay: compensate for I2S DMA latency + acoustic path
// The mic captures echo from audio played ~60-100ms ago, but reference is "fresh".
// We delay the reference so it aligns with when the echo appears in mic.
// DMA latency: ~64ms typical (depends on buffer count/size)
// Acoustic delay: ~5ms (room dependent)
// Total: ~70ms, we use 80ms for safety margin
static constexpr size_t AEC_REF_DELAY_MS = 80;
static constexpr size_t AEC_REF_DELAY_SAMPLES = (SAMPLE_RATE * AEC_REF_DELAY_MS) / 1000;  // 1280 samples
static constexpr size_t AEC_REF_DELAY_BYTES = AEC_REF_DELAY_SAMPLES * sizeof(int16_t);   // 2560 bytes

// Timeouts
static constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
static constexpr uint32_t PING_INTERVAL_MS = 5000;
static constexpr uint32_t PING_TIMEOUT_MS = 10000;

}  // namespace intercom_api
}  // namespace esphome
