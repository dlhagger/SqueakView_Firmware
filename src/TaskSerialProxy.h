#ifndef MOUSEHOUSE_TASK_SERIAL_PROXY_H
#define MOUSEHOUSE_TASK_SERIAL_PROXY_H

#include <Arduino.h>
#include "MouseHouse.h"

class TaskSerialProxy {
public:
  explicit TaskSerialProxy(MouseHouse* owner) : owner_(owner) {}

  size_t print(const char* value) { return append(value ? value : ""); }
  size_t print(char value) { return appendChar(value); }
  size_t print(unsigned char value) { return appendUnsigned(value); }
  size_t print(int value) { return appendSigned(value); }
  size_t print(unsigned int value) { return appendUnsigned(value); }
  size_t print(long value) { return appendSigned(value); }
  size_t print(unsigned long value) { return appendUnsigned(value); }
  size_t print(long long value) { return appendSigned(value); }
  size_t print(unsigned long long value) { return appendUnsigned(value); }
  size_t print(bool value) { return append(value ? "1" : "0"); }

  size_t println() { return finishLine(); }
  template <typename T> size_t println(T value) {
    size_t written = print(value);
    return written + finishLine();
  }

private:
  static constexpr size_t kBufferSize = SerialTransport::kLegacyLineSize;
  MouseHouse* owner_;
  char buffer_[kBufferSize] = "";
  size_t length_ = 0;
  bool overflow_ = false;

  size_t appendChar(char value) {
    if (length_ + 1U >= kBufferSize) {
      overflow_ = true;
      return 0;
    }
    buffer_[length_++] = value;
    buffer_[length_] = '\0';
    return 1;
  }

  size_t append(const char* value) {
    size_t added = 0;
    while (*value != '\0') {
      if (appendChar(*value++) == 0) break;
      added++;
    }
    return added;
  }

  size_t appendUnsigned(unsigned long long value) {
    char reversed[24];
    size_t count = 0;
    do {
      reversed[count++] = (char)('0' + (value % 10ULL));
      value /= 10ULL;
    } while (value != 0 && count < sizeof(reversed));
    size_t written = 0;
    while (count > 0) written += appendChar(reversed[--count]);
    return written;
  }

  size_t appendSigned(long long value) {
    size_t written = 0;
    unsigned long long magnitude;
    if (value < 0) {
      written += appendChar('-');
      magnitude = (unsigned long long)(-(value + 1LL)) + 1ULL;
    } else {
      magnitude = (unsigned long long)value;
    }
    return written + appendUnsigned(magnitude);
  }

  size_t finishLine() {
    bool accepted = false;
    if (owner_ != nullptr) {
      accepted = overflow_ ? owner_->emitLegacyLine(nullptr)
                           : owner_->emitLegacyLine(buffer_);
    }
    length_ = 0;
    overflow_ = false;
    buffer_[0] = '\0';
    return accepted ? 1U : 0U;
  }
};

#endif
