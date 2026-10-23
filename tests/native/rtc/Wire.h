#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

struct FakeWire {
  std::vector<uint8_t> registers;
  std::vector<std::vector<uint8_t>> snapshots;
  std::vector<uint8_t> rx, tx;
  size_t offset = 0;
  unsigned requests = 0, transmissions = 0;
  int nack = 0;
  bool shortRead = false, failWrite = false, failRead = false;
  void begin() {}
  void beginTransmission(uint8_t address) {
    assert(address == 0x68);
    tx.clear();
  }
  size_t write(uint8_t value) {
    if (failWrite) return 0;
    tx.push_back(value);
    return 1;
  }
  int endTransmission(bool = true) {
    // Identification may set only the address pointer, never register data.
    assert(tx.empty() || (tx.size() == 1 && tx[0] == 0));
    ++transmissions;
    return nack;
  }
  size_t requestFrom(uint8_t address, size_t count) {
    assert(address == 0x68);
    const auto& values = snapshots.empty() ? registers
        : snapshots[std::min<size_t>(requests, snapshots.size() - 1)];
    ++requests;
    rx.clear();
    offset = 0;
    if (values.empty()) return 0;
    for (size_t i = 0; i < count; ++i) rx.push_back(values[i % values.size()]);
    if (shortRead) rx.pop_back();
    return rx.size();
  }
  int available() { return (int)(rx.size() - offset); }
  int read() {
    if (failRead || offset >= rx.size()) return -1;
    return rx[offset++];
  }
};
inline FakeWire Wire;
