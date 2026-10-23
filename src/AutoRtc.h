#ifndef MOUSEHOUSE_AUTO_RTC_H
#define MOUSEHOUSE_AUTO_RTC_H

#include <RTClib.h>
#include <Wire.h>

// Startup-only, read-only identification of the two supported RTCs. Their
// register pointers wrap at different addresses during sequential reads:
// DS3231: 0x12 -> 0x00; PCF8523: 0x13 -> 0x00.
// Sources: DS3231 datasheet, Address Map; PCF8523 datasheet, section 8.1.
// https://www.analog.com/media/en/technical-documentation/data-sheets/ds3231.pdf
// https://www.nxp.com/docs/en/data-sheet/PCF8523.pdf
class AutoRtc {
public:
  enum Type { UNKNOWN, PCF8523, DS3231 };

  bool begin() {
    type_ = UNKNOWN;
    Wire.begin();
    Type previous = UNKNOWN;
    // Two consecutive matching probes are required. Retry transient changes
    // (second rollover, temperature conversion, timer flags) only at startup.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      Type candidate = probe();
      if (candidate != UNKNOWN && candidate == previous) {
        bool ready = candidate == DS3231 ? ds3231_.begin() : pcf8523_.begin();
        if (ready) type_ = candidate;
        return ready;
      }
      previous = candidate;
    }
    return false;
  }

  const char* typeName() const {
    return type_ == DS3231 ? "DS3231" : type_ == PCF8523 ? "PCF8523" : "UNKNOWN";
  }

  bool lostPower() {
    if (type_ == DS3231) return ds3231_.lostPower();
    if (type_ == PCF8523) return pcf8523_.lostPower();
    return true;
  }

  DateTime now() {
    if (type_ == DS3231) return ds3231_.now();
    if (type_ == PCF8523) return pcf8523_.now();
    return DateTime(2000, 1, 1);
  }

  void adjust(const DateTime& value) {
    if (type_ == DS3231) ds3231_.adjust(value);
    else if (type_ == PCF8523) pcf8523_.adjust(value);
  }

private:
  static constexpr uint8_t kAddress = 0x68;
  static constexpr size_t kProbeBytes = 40;
  Type type_ = UNKNOWN;
  RTC_PCF8523 pcf8523_;
  RTC_DS3231 ds3231_;

  Type probe() {
    // The single transmitted byte sets only the register pointer. No register
    // data, clock value, oscillator setting, or status flag is written.
    Wire.beginTransmission(kAddress);
    if (Wire.write((uint8_t)0) != 1) {
      Wire.endTransmission();
      return UNKNOWN;
    }
    if (Wire.endTransmission(false) != 0) return UNKNOWN;
    if (Wire.requestFrom(kAddress, kProbeBytes) != kProbeBytes) {
      while (Wire.available()) Wire.read();
      return UNKNOWN;
    }
    uint8_t bytes[kProbeBytes];
    for (size_t i = 0; i < kProbeBytes; ++i) {
      int value = Wire.read();
      if (value < 0) return UNKNOWN;
      bytes[i] = (uint8_t)value;
    }
    bool ds3231 = repeats(bytes, 19);
    bool pcf8523 = repeats(bytes, 20);
    // Uniform/ambiguous data and unsupported maps must never choose a driver.
    if (ds3231 == pcf8523) return UNKNOWN;
    return ds3231 ? DS3231 : PCF8523;
  }

  static bool repeats(const uint8_t* bytes, size_t period) {
    for (size_t i = period; i < kProbeBytes; ++i) {
      if (bytes[i] != bytes[i - period]) return false;
    }
    return true;
  }
};

#endif
