#ifndef MOUSEHOUSE_PROTOCOL_V2_H
#define MOUSEHOUSE_PROTOCOL_V2_H

#include <stddef.h>
#include <stdint.h>

namespace MouseHouseProtocolV2 {

constexpr uint8_t kVersion = 2;
constexpr uint8_t kFrameDelimiter = 0;
constexpr uint16_t kHeaderSize = 34;
constexpr uint16_t kMaxPayloadSize = 384;
constexpr uint16_t kMaxDecodedFrameSize =
    kHeaderSize + kMaxPayloadSize + sizeof(uint32_t);
constexpr uint16_t kMaxEncodedFrameSize =
    kMaxDecodedFrameSize + (kMaxDecodedFrameSize / 254U) + 2U;

constexpr uint16_t kFlagReliable = 0x0001;
constexpr uint16_t kFlagRetransmission = 0x0002;
constexpr uint16_t kFlagIntegrityLatched = 0x0004;

enum MessageType : uint8_t {
  MESSAGE_EVENT = 1,
  MESSAGE_COMMAND_RESULT = 2,
  MESSAGE_CAMERA_EPOCH = 3,
  MESSAGE_CAMERA_CHECKPOINT = 4,
  MESSAGE_CAMERA_STOP = 5,
  MESSAGE_TRANSPORT_STATUS = 6,
  MESSAGE_INTEGRITY_FAULT = 7,
  MESSAGE_DIAGNOSTIC = 8,
};

inline void putLe16(uint8_t* destination, uint16_t value) {
  destination[0] = (uint8_t)value;
  destination[1] = (uint8_t)(value >> 8U);
}

inline void putLe32(uint8_t* destination, uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) {
    destination[index] = (uint8_t)(value >> (index * 8U));
  }
}

inline void putLe64(uint8_t* destination, uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    destination[index] = (uint8_t)(value >> (index * 8U));
  }
}

inline uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320UL : 0UL);
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

inline size_t cobsEncode(const uint8_t* input, size_t length,
                         uint8_t* output, size_t capacity) {
  if (capacity == 0) return 0;
  size_t readIndex = 0;
  size_t writeIndex = 1;
  size_t codeIndex = 0;
  uint8_t code = 1;

  while (readIndex < length) {
    if (input[readIndex] == 0) {
      if (codeIndex >= capacity) return 0;
      output[codeIndex] = code;
      codeIndex = writeIndex++;
      code = 1;
      readIndex++;
    } else {
      if (writeIndex >= capacity) return 0;
      output[writeIndex++] = input[readIndex++];
      code++;
      if (code == 0xFF) {
        if (codeIndex >= capacity) return 0;
        output[codeIndex] = code;
        codeIndex = writeIndex++;
        code = 1;
      }
    }
  }

  if (codeIndex >= capacity || writeIndex >= capacity) return 0;
  output[codeIndex] = code;
  output[writeIndex++] = kFrameDelimiter;
  return writeIndex;
}

}  // namespace MouseHouseProtocolV2

#endif
