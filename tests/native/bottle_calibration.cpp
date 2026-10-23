// Execute production command/calibration methods against fault-injectable MPR
// register boundaries. No simulated control-flow copy of calibration lives here.
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include "BottleSwapState.h"
uint64_t testUs = 100;
uint64_t time_us_64() { return testUs++; }
unsigned long millis() { return testUs / 1000; }
namespace MouseHouseProtocolV2 {
constexpr uint8_t MESSAGE_COMMAND_RESULT=2, MESSAGE_DIAGNOSTIC=6, MESSAGE_CAMERA_STOP=5;
}
struct SerialTransport {
  enum Priority { PRIORITY_SAFETY };
  enum ProtocolMode { PROTOCOL_V2_ACTIVE };
  ProtocolMode mode() { return PROTOCOL_V2_ACTIVE; }
  bool overflowLatched() { return false; }
  bool v2EnabledOrPending() { return true; }
};
constexpr uint8_t MPR121_ECR=0x5E, MPR121_TOUCHTH_0=0x41, MPR121_DEBOUNCE=0x5B;
class MouseHouse {
public:
  BottleSwapState bottleSwap_;
  SerialTransport transport_;
  bool running_=true, leftDrinkNeedsRelease_=false, rightDrinkNeedsRelease_=false;
  bool cameraTriggersEnabled_=false, feedActive_=false, compatibilitySerialMode_=false;
  int feedRetryCount_=0;
  uint64_t frameCounter_=0;
  static constexpr const char* kNanString="nan";
  void stopSession();
  void stopCameraPio() {}
  void snapshotCameraIrq(uint32_t&, uint64_t&, uint64_t&) {}
  void feedStop(const char*) { feedActive_=false; }
  void clearTimeoutState() {}
  template<typename... Args> void queueCameraRecord(Args...) {}
  uint8_t bottleCalibrationRegister_=0;
  static constexpr uint8_t kBottleCalibrationMaxAttempts=3;
  uint8_t bottleCalibrationAttempt_=0;
  bool bottleCalibrationRetryPending_=false;
  uint64_t nextBottleCalibrationCheckUs_=0;
  bool bottleCalibrationSampled_=false;
  uint8_t bottleCalibrationEcr_=0, bottleCalibrationOorL_=0, bottleCalibrationOorH_=0;
  uint16_t bottleCalibrationTouched_=0;
  unsigned long lastMpr121Check_=0;
  static constexpr uint64_t kNotApplicable=69420;
  struct TimedBinaryEvent {
    bool previousState=false, justEnded=false;
    unsigned long count=0;
  } leftDrinkEvent_, rightDrinkEvent_;
  struct Cap {
    uint8_t regs[256]{};
    bool failStop=false;
    unsigned stopWriteFailures=0, ecrReadFailures=0, transientBadReads=0;
    int badReg=-1;
    unsigned reads[256]{};
    uint16_t touches=0;
    std::vector<std::pair<uint8_t,uint8_t>> writes;
    Cap() { regs[MPR121_ECR]=0x8C; }
    uint8_t readRegister8(uint8_t reg) {
      ++reads[reg];
      if (reg==MPR121_ECR && ecrReadFailures) {
        --ecrReadFailures;
        return 0xFF;
      }
      if (reg==badReg && transientBadReads) {
        --transientBadReads;
        uint8_t actual=regs[reg]^0xFF;
        if (!transientBadReads) badReg=-1;
        return actual;
      }
      return reg==badReg ? uint8_t(regs[reg]^0xFF) : regs[reg];
    }
    void writeRegister(uint8_t reg, uint8_t value) {
      writes.emplace_back(reg,value);
      if (reg==MPR121_ECR && value==0) {
        if (failStop) return;
        if (stopWriteFailures) { --stopWriteFailures; return; }
      }
      regs[reg]=value;
    }
    uint16_t touched() { return touches; }
  } cap_;
  struct Record { uint8_t type; std::string line; };
  std::vector<Record> records;
  unsigned integrityFailures=0;
  void handleBottleCommand(const char* command);
  void serviceBottleCalibration();
  void queueBottleState();
  void muteDrinkEvents(const char* reason);
  void failBottleCalibration(const char*, const char*, uint8_t, uint16_t, uint16_t);
  bool queueText(uint8_t type, SerialTransport::Priority priority, const char* format, ...) {
    assert(priority==SerialTransport::PRIORITY_SAFETY);
    char text[384]; va_list args; va_start(args,format);
    int length=vsnprintf(text,sizeof(text),format,args); va_end(args);
    assert(length>0 && size_t(length)<sizeof(text));
    records.push_back({type,text}); return true;
  }
  void noteRequiredRecordFailure(const char*, uint64_t) { ++integrityFailures; }
  void sendCommandError(const char*,const char*) { assert(false); }
  uint64_t getTimestampUs() { return testUs; }
  const char* getContext() { return "Test"; }
  template<typename... Args> void logEvent(Args...) {}
  void primeTimedBinaryEvent(TimedBinaryEvent& e,bool state,uint64_t) {
    e.previousState=state; e.justEnded=false;
  }
};
#include "calibration_methods.inc"
const char* id="0123456789abcdef0123456789abcdef";
void begin(MouseHouse& m) {
  m.leftDrinkEvent_.count=21; m.rightDrinkEvent_.count=16;
  m.leftDrinkEvent_.previousState=m.rightDrinkEvent_.previousState=true;
  m.leftDrinkEvent_.justEnded=m.rightDrinkEvent_.justEnded=true;
  std::string pause=std::string("BOTTLE_PAUSE,")+id;
  m.handleBottleCommand(pause.c_str());
  assert(m.bottleSwap_.state==BottleSwapState::PAUSED);
  assert(!m.leftDrinkEvent_.previousState && !m.rightDrinkEvent_.previousState);
  assert(!m.leftDrinkEvent_.justEnded && !m.rightDrinkEvent_.justEnded);
  std::string resume=std::string("BOTTLE_RESUME,")+id;
  m.handleBottleCommand(resume.c_str());
  assert(m.bottleSwap_.state==BottleSwapState::CALIBRATING);
}
void program(MouseHouse& m) {
  size_t base=m.cap_.writes.size();
  for (int i=0;i<30;++i) m.serviceBottleCalibration();
  assert(m.bottleSwap_.state==BottleSwapState::CALIBRATING);
  assert(m.cap_.writes.size()==base+30);
  for (int i=0;i<24;++i) {
    assert(m.cap_.writes[base+i].first==0x41+i);
    assert(m.cap_.writes[base+i].second==(i%2?0:2));
  }
  const uint8_t regs[]={0x7B,0x7D,0x7F,0x7E,0x5B,0x5E};
  const uint8_t vals[]={0x0B,200,180,130,0,0x8C};
  for(int i=0;i<6;++i) assert(m.cap_.writes[base+i+24]==std::make_pair(regs[i],vals[i]));
}
void ready(MouseHouse& m) {
  testUs=m.nextBottleCalibrationCheckUs_;
  m.serviceBottleCalibration();
  testUs+=BottleSwapState::kReleasedUs;
  m.serviceBottleCalibration();
  assert(m.bottleSwap_.state==BottleSwapState::ACTIVE);
  assert(!m.leftDrinkNeedsRelease_ && !m.rightDrinkNeedsRelease_);
  assert(m.bottleSwap_.left==21 && m.bottleSwap_.right==16);
}
void failed(MouseHouse& m,const char* reason,const char* detail) {
  assert(m.bottleSwap_.state==BottleSwapState::FAILED);
  assert(std::string(m.bottleSwap_.reason)==reason);
  assert(m.leftDrinkNeedsRelease_ && m.rightDrinkNeedsRelease_);
  assert(m.records.back().type==MouseHouseProtocolV2::MESSAGE_DIAGNOSTIC);
  assert(m.records.back().line.find(detail)!=std::string::npos);
  auto count=m.records.size();
  m.serviceBottleCalibration();
  assert(m.records.size()==count); // One diagnostic per failure only.
  assert(m.leftDrinkEvent_.count==21 && m.rightDrinkEvent_.count==16);
}
void exhaust(MouseHouse& m) {
  for (int i=0;i<200 && m.bottleSwap_.state==BottleSwapState::CALIBRATING;++i)
    m.serviceBottleCalibration();
  assert(m.bottleCalibrationAttempt_==3);
}
int main(int,char**) {
  // Both observed candidate faults recover within one Resume request.
  for (int fault=0;fault<3;++fault) {
    MouseHouse m;
    if (fault==0) m.cap_.stopWriteFailures=1;
    begin(m);
    if (fault==1) m.cap_.ecrReadFailures=1;
    if (fault==2) { m.cap_.badReg=0x50; m.cap_.transientBadReads=1; }
    uint64_t boundary=m.bottleSwap_.boundaryUs;
    for(int i=0;i<30 && !m.bottleCalibrationRetryPending_;++i)
      m.serviceBottleCalibration();
    assert(m.bottleSwap_.state==BottleSwapState::CALIBRATING);
    assert(m.bottleCalibrationRetryPending_ && m.bottleCalibrationAttempt_==1);
    assert(m.records.back().line.find("attempt=1,retrying=1")!=std::string::npos);
    if (fault==1) assert(m.records.back().line.find("actual=0x00ff")!=std::string::npos);
    assert(m.leftDrinkNeedsRelease_ && m.rightDrinkNeedsRelease_);
    assert(m.bottleSwap_.left==21 && m.bottleSwap_.right==16);
    // Duplicate command cannot restart attempts or alter the boundary.
    m.handleBottleCommand((std::string("BOTTLE_RESUME,")+id).c_str());
    assert(m.bottleCalibrationAttempt_==1 && m.bottleCalibrationRetryPending_);
    size_t writes=m.cap_.writes.size();
    m.serviceBottleCalibration();
    assert(m.cap_.writes.size()==writes+1 && m.bottleCalibrationAttempt_==2);
    assert(m.bottleSwap_.boundaryUs==boundary);
    program(m); ready(m);
    assert(std::string(m.bottleSwap_.id)==id);
  }
  {
    MouseHouse m; m.cap_.failStop=true; begin(m); exhaust(m);
    failed(m,"CAL_STOP_VERIFY_FAILED","STOP_VERIFY,register=0x5e,expected=0x0000,actual=0x008c,cursor=1");
    assert(m.cap_.reads[0x5E]==3); // No diagnostic reread of failed stops.
    unsigned diagnostics=0;
    for(const auto& r:m.records) if(r.type==MouseHouseProtocolV2::MESSAGE_DIAGNOSTIC) ++diagnostics;
    assert(diagnostics==3);
    assert(m.records.back().line.find("attempt=3,retrying=0")!=std::string::npos);
    m.cap_.failStop=false;
    m.handleBottleCommand((std::string("BOTTLE_RESUME,")+id).c_str());
    assert(m.bottleCalibrationAttempt_==1);
    program(m); ready(m); // A new manual request gets a fresh bounded budget.
  }
  for(int reg : {0x41,0x50}) {
    MouseHouse m; begin(m); m.cap_.badReg=reg; exhaust(m);
    const char* detail=reg==0x41
      ? "REGISTER_VERIFY,register=0x41,expected=0x0002,actual=0x00fd,cursor=1"
      : "REGISTER_VERIFY,register=0x50,expected=0x0000,actual=0x00ff,cursor=16";
    failed(m,"CAL_REGISTER_VERIFY_FAILED",detail);
    assert(m.cap_.reads[reg]==3);
  }
  {
    MouseHouse m; begin(m); program(m); ready(m);
    assert(m.bottleCalibrationAttempt_==1);
  }
  for (int fault=0;fault<3;++fault) {
    MouseHouse m; begin(m); program(m);
    testUs=m.nextBottleCalibrationCheckUs_;
    if (fault==0) m.cap_.touches=1<<1;
    if (fault==1) m.cap_.regs[0x02]=0x40;
    if (fault==2) m.cap_.regs[0x5E]=0;
    m.serviceBottleCalibration();
    testUs=m.bottleSwap_.boundaryUs+BottleSwapState::kDeadlineUs;
    m.serviceBottleCalibration();
    failed(m,fault==0?"CAL_CHANNELS_NOT_RELEASED":"CAL_SENSOR_HEALTH_TIMEOUT", "READINESS");
    assert(m.bottleCalibrationAttempt_==1); // Readiness never bypasses checks.
  }
  {
    MouseHouse m; m.cap_.failStop=true; begin(m); m.serviceBottleCalibration();
    testUs=m.bottleSwap_.boundaryUs+BottleSwapState::kDeadlineUs;
    size_t writes=m.cap_.writes.size(); m.serviceBottleCalibration();
    failed(m,"CAL_READINESS_TIMEOUT","READINESS");
    assert(m.cap_.writes.size()==writes && m.bottleCalibrationAttempt_==1);
  }
  {
    MouseHouse m; m.cap_.failStop=true; begin(m); m.serviceBottleCalibration();
    // Stop cancels transaction state; a queued retry must never produce late ACTIVE.
    m.stopSession();
    assert(!m.running_ && m.bottleSwap_.id[0]=='\0');
    size_t writes=m.cap_.writes.size(), records=m.records.size();
    m.serviceBottleCalibration();
    assert(m.cap_.writes.size()==writes && m.records.size()==records);
  }
}
