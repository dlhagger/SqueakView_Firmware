#include "AutoRtc.h"
#include <cassert>
#include <cstdlib>
#include <cstring>

const std::vector<uint8_t> pcf = {
    0x80, 0, 0xe0, 0x80, 0, 0, 1, 0, 1, 0,
    0x80, 0x80, 0x80, 0x80, 0, 0x38, 7, 0, 7, 0};
const std::vector<uint8_t> ds = {
    0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x1c, 0x88, 0, 25, 0};

int main(int argc, char** argv) {
  assert(argc == 2);
  int scenario = std::atoi(argv[1]);
  AutoRtc rtc;
  if (scenario == 13) {
    for (bool useDs : {false, true}) {
      for (unsigned second = 0; second < 60; ++second) {
        Wire = FakeWire();
        Wire.registers = useDs ? ds : pcf;
        Wire.registers[useDs ? 0 : 3] = (second / 10) * 16 + second % 10;
        // Exercise a set, healthy clock as well as the factory/lost-power maps.
        Wire.registers[useDs ? 6 : 9] = 0x26;
        Wire.registers[useDs ? 15 : 2] = 0;
        assert(rtc.begin());
        assert(std::strcmp(rtc.typeName(), useDs ? "DS3231" : "PCF8523") == 0);
      }
    }
    return 0;
  }
  if (scenario == 0 || scenario == 1 || scenario == 9) {
    bool useDs = scenario == 1;
    Wire.registers = useDs ? ds : pcf;
    // Type detection must not depend on a plausible or running calendar.
    if (scenario == 9) {
      for (unsigned i = 3; i <= 9; ++i) Wire.registers[i] = 0;
    }
    auto before = Wire.registers;
    assert(rtc.begin());
    assert(Wire.requests == 2 && Wire.registers == before);
    assert(std::strcmp(rtc.typeName(), useDs ? "DS3231" : "PCF8523") == 0);
    assert(rtc.lostPower());
    rtc.adjust(DateTime(1790000000));
    assert(!rtc.lostPower());
    assert(rtc.now().unixtime() == 1790000000);
    RtcCalls& selected = useDs ? dsCalls : pcfCalls;
    RtcCalls& other = useDs ? pcfCalls : dsCalls;
    assert(selected.begins == 1 && selected.adjustments == 1);
    assert(other.begins == 0 && other.reads == 0 && other.powerReads == 0
           && other.adjustments == 0);
    // A subsequent failed begin must not retain the old driver selection.
    Wire.nack = 2;
    assert(!rtc.begin());
    assert(rtc.lostPower());
    rtc.adjust(DateTime(1));
    assert(selected.adjustments == 1);
    return 0;
  }
  Wire.registers = pcf;
  if (scenario == 2) Wire.nack = 2;
  if (scenario == 3) {
    Wire.registers.resize(64);
    for (unsigned i = 0; i < 64; ++i) Wire.registers[i] = i;
  }
  if (scenario == 4) Wire.registers.assign(20, 0);
  if (scenario == 5) Wire.shortRead = true;
  if (scenario == 6) Wire.snapshots = {pcf, ds, pcf, ds};
  if (scenario == 7) {
    std::vector<uint8_t> unstable;
    for (unsigned i = 0; i < 40; ++i) unstable.push_back(ds[i % ds.size()]);
    unstable[19] = 1; // Seconds change at the DS3231 pointer wrap.
    Wire.snapshots = {unstable, ds, ds};
    assert(rtc.begin());
    assert(Wire.requests == 3 && dsCalls.begins == 1 && pcfCalls.begins == 0);
    return 0;
  }
  if (scenario == 8) Wire.registers.assign(20, 0xff);
  if (scenario == 10) pcfCalls.present = false;
  if (scenario == 11) Wire.failWrite = true;
  if (scenario == 12) Wire.failRead = true;
  assert(!rtc.begin());
  assert(std::strcmp(rtc.typeName(), "UNKNOWN") == 0);
  assert(Wire.requests <= 4 && Wire.transmissions <= 4);
  assert(rtc.lostPower());
  rtc.adjust(DateTime(1));
  assert(pcfCalls.adjustments == 0 && dsCalls.adjustments == 0);
}
