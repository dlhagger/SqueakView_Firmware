#pragma once
#include <cstdint>

struct DateTime {
  uint32_t seconds;
  explicit DateTime(uint32_t value) : seconds(value) {}
  DateTime(int, int, int) : seconds(0) {}
  uint32_t unixtime() const { return seconds; }
};
struct RtcCalls {
  unsigned begins = 0, reads = 0, powerReads = 0, adjustments = 0;
  bool present = true, lost = true;
  uint32_t seconds = 0;
};
inline RtcCalls pcfCalls, dsCalls;

class RTC_PCF8523 {
public:
  bool begin() { ++pcfCalls.begins; return pcfCalls.present; }
  bool lostPower() { ++pcfCalls.powerReads; return pcfCalls.lost; }
  DateTime now() { ++pcfCalls.reads; return DateTime(pcfCalls.seconds); }
  void adjust(const DateTime& value) {
    ++pcfCalls.adjustments;
    pcfCalls.seconds = value.unixtime();
    pcfCalls.lost = false;
  }
};
class RTC_DS3231 {
public:
  bool begin() { ++dsCalls.begins; return dsCalls.present; }
  bool lostPower() { ++dsCalls.powerReads; return dsCalls.lost; }
  DateTime now() { ++dsCalls.reads; return DateTime(dsCalls.seconds); }
  void adjust(const DateTime& value) {
    ++dsCalls.adjustments;
    dsCalls.seconds = value.unixtime();
    dsCalls.lost = false;
  }
};
