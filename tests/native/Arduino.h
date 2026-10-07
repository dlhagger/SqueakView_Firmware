#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Host-side boundaries only; the tests compile the production transport/sketches.
inline unsigned long testMillis = 0;
inline unsigned long millis() { return testMillis; }
inline int digitalRead(int) { return 0; }
inline void randomSeed(uint32_t) {}
inline long random(long) { return 0; }
extern "C" inline uint64_t time_us_64() { return testMillis * 1000ULL; }

struct FakeSerial {
  std::vector<uint8_t> output;
  int capacity = 32;
  explicit operator bool() const { return true; }
  int availableForWrite() const { return capacity; }
  size_t write(const uint8_t* bytes, size_t size) {
    output.insert(output.end(), bytes, bytes + size);
    return size;
  }
};
inline FakeSerial Serial;
