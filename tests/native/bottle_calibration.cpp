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
#include "LickRecovery.h"
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
  LickRecovery lickRecovery_;
  unsigned long lickRecoveryEndCounts_[2]={0,0};
  uint64_t lickTouchSinceUs_[2]={0,0}, lickRecoveryTouchTimeoutUs_=5000000;
  bool mprHealthScanning_=false;
  uint8_t mprHealthCursor_=0;
  static constexpr unsigned long kMpr121CheckIntervalMs=1000;
  void beginLickRecovery(const char*,uint8_t=0,uint16_t=0,uint16_t=0);
  void serviceLickRecovery();
  void reportLickRecovery(const char*,const char*);
  void reportLickRecoveryDiagnostic(const char*);
  void cancelLickRecovery();
  void checkMPR121Health();
  void setLickRecoveryTouchTimeoutMs(uint32_t);
  void updateLeftDrink(); void updateRightDrink();
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
    uint64_t startTime=0, endTime=0;
    unsigned long count=0;
  } leftDrinkEvent_, rightDrinkEvent_;
  struct Cap {
    uint8_t regs[256]{};
    bool failStop=false;
    uint8_t stopReadback=0, resetEcrReadback=0;
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
        regs[reg]=stopReadback;
        return;
      }
      if(reg==0x80 && value==0x63) {
        for(auto& entry:regs) entry=0;
        regs[0x5D]=0x24;
        regs[MPR121_ECR]=resetEcrReadback;
      } else regs[reg]=value;
    }
    uint16_t touched() { return touches; }
    uint16_t filteredData(uint8_t) { return 500; }
    uint16_t baselineData(uint8_t) { return 504; }
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
  std::vector<std::string> errors;
  void sendCommandError(const char*,const char* reason) { errors.emplace_back(reason); }
  uint64_t getTimestampUs() { return testUs; }
  const char* getContext() { return "Test"; }
  struct Event { std::string type, side, reason; unsigned long count; uint64_t gap; long id; };
  std::vector<Event> events;
  void logEvent(const char* type,uint64_t,uint64_t,const char* side,unsigned long count,
                uint64_t gap,uint64_t,long id,const char*,const char* reason) {
    events.push_back({type,side,reason,count,gap,id});
  }
  void updateTimedBinaryEvent(TimedBinaryEvent&,bool,const char*,const char*,const char*,uint64_t,uint64_t);
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
  for (int i=0;i<LickRecovery::kRegisterCount;++i) m.serviceBottleCalibration();
  assert(m.bottleSwap_.state==BottleSwapState::CALIBRATING);
  assert(m.cap_.writes.size()==base+LickRecovery::kRegisterCount);
  for (int i=0;i<24;++i) {
    assert(m.cap_.writes[base+i+13].first==0x41+i);
    assert(m.cap_.writes[base+i+13].second==(i%2?0:2));
  }
  const uint8_t regs[]={0x7B,0x7D,0x7F,0x7E,0x5B,0x5E};
  const uint8_t vals[]={0x0B,200,180,130,0,0x8C};
  for(int i=0;i<6;++i) assert(m.cap_.writes[base+i+37]==std::make_pair(regs[i],vals[i]));
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
  // Exercise every ECR byte in both production Stop-verification paths.
  // Only 00/40/80/C0 mean stopped; any electrode/proximity bit must fail.
  for (unsigned raw=0;raw<256;++raw) {
    bool stopped=(raw==0 || raw==0x40 || raw==0x80 || raw==0xC0);
    {
      MouseHouse m; m.cap_.stopReadback=uint8_t(raw);
      begin(m);
      if(stopped) { program(m); ready(m); assert(m.bottleCalibrationAttempt_==1); }
      else {
        m.serviceBottleCalibration();
        assert(m.bottleCalibrationRetryPending_);
        assert(m.records.back().line.find("STOP_VERIFY")!=std::string::npos);
        assert(m.leftDrinkNeedsRelease_ && m.rightDrinkNeedsRelease_);
      }
    }
    {
      MouseHouse m; m.cap_.stopReadback=uint8_t(raw);
      m.beginLickRecovery("SESSION_START"); m.serviceLickRecovery();
      assert(m.lickRecovery_.attempts==1);
      if(stopped) {
        assert(m.lickRecovery_.state==LickRecovery::CALIBRATING);
        for(int tick=0;tick<200 && !m.lickRecovery_.available();++tick) {
          testUs+=10000; m.serviceLickRecovery();
        }
        assert(m.lickRecovery_.available() && m.lickRecovery_.attempts==1);
      } else {
        assert(m.lickRecovery_.state==LickRecovery::WAITING);
        assert(std::string(m.lickRecovery_.reason)=="STOP_VERIFY_FAILED");
        assert(m.lickRecovery_.actual==raw);
        assert(m.cap_.writes.size()==1); // No reset/configuration while enabled.
      }
    }
    {
      MouseHouse m; m.cap_.resetEcrReadback=uint8_t(raw);
      m.beginLickRecovery("SESSION_START"); m.serviceLickRecovery();
      testUs+=3000; m.serviceLickRecovery();
      assert(m.lickRecovery_.state==(stopped ? LickRecovery::CALIBRATING : LickRecovery::WAITING));
      if(stopped) {
        for(int tick=0;tick<200 && !m.lickRecovery_.available();++tick) {
          testUs+=10000; m.serviceLickRecovery();
        }
        assert(m.lickRecovery_.available() && m.lickRecovery_.attempts==1);
      } else {
        assert(std::string(m.lickRecovery_.reason)=="STOP_VERIFY_FAILED");
        assert(m.lickRecovery_.actual==raw);
        assert(m.cap_.writes.size()==2); // Stop/reset only, no register program.
      }
    }
  }
  // Upper bits are significant in Run mode: an enabled sensor with the
  // wrong baseline calibration mode must remain unavailable.
  for(uint8_t ecr : {0x0C,0x4C,0xCC,0x8D,0xFF}) {
    MouseHouse m; begin(m); program(m);
    m.cap_.regs[MPR121_ECR]=ecr;
    testUs=m.nextBottleCalibrationCheckUs_; m.serviceBottleCalibration();
    assert(m.bottleSwap_.state==BottleSwapState::CALIBRATING);
    assert(m.leftDrinkNeedsRelease_ && m.rightDrinkNeedsRelease_);
    testUs=m.bottleSwap_.boundaryUs+BottleSwapState::kDeadlineUs;
    m.serviceBottleCalibration();
    failed(m,"CAL_SENSOR_HEALTH_TIMEOUT","READINESS");
    MouseHouse automatic;
    automatic.beginLickRecovery("SESSION_START"); automatic.serviceLickRecovery();
    testUs+=3000; automatic.serviceLickRecovery();
    for(unsigned index=0;index<LickRecovery::kRegisterCount;++index) automatic.serviceLickRecovery();
    automatic.cap_.regs[MPR121_ECR]=ecr;
    testUs+=LickRecovery::kSettleUs; automatic.serviceLickRecovery();
    assert(!automatic.lickRecovery_.available());
    assert(automatic.lickRecovery_.failedRegister==MPR121_ECR);
    assert(automatic.lickRecovery_.expected==0x8C && automatic.lickRecovery_.actual==ecr);
    testUs+=LickRecovery::kDeadlineUs; automatic.serviceLickRecovery();
    assert(automatic.lickRecovery_.state==LickRecovery::WAITING);
    assert(std::string(automatic.lickRecovery_.reason)=="READINESS_TIMEOUT");
    automatic.cap_.touches=2; automatic.updateLeftDrink();
    assert(automatic.leftDrinkEvent_.count==0);
  }
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
      ? "REGISTER_VERIFY,register=0x41,expected=0x0002,actual=0x00fd,cursor=14"
      : "REGISTER_VERIFY,register=0x50,expected=0x0000,actual=0x00ff,cursor=29";
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

  // Actual automatic service: preserved counters, complete verified settings,
  // settling measured from Run, release gating, and no bottle transaction.
  {
    MouseHouse m; m.leftDrinkEvent_.count=21; m.rightDrinkEvent_.count=16;
    m.beginLickRecovery("SESSION_START");
    m.handleBottleCommand((std::string("BOTTLE_PAUSE,")+id).c_str());
    assert(m.errors.back()=="SENSOR_RECOVERY_ACTIVE");
    assert(m.bottleSwap_.state==BottleSwapState::ACTIVE && m.bottleSwap_.id[0]=='\0');
    m.updateLeftDrink(); assert(m.leftDrinkEvent_.count==21);
    assert(m.bottleSwap_.id[0]=='\0');
    m.serviceLickRecovery();
    size_t resetWrites=m.cap_.writes.size();
    m.serviceLickRecovery(); assert(m.cap_.writes.size()==resetWrites);
    testUs+=3000; m.serviceLickRecovery();
    for (unsigned i=0;i<LickRecovery::kRegisterCount;++i) m.serviceLickRecovery();
    assert(m.lickRecovery_.state==LickRecovery::CALIBRATING);
    assert(m.cap_.regs[0x43]==2 && m.cap_.regs[0x44]==0 && m.cap_.regs[0x5B]==0);
    assert(m.cap_.regs[0x7B]==0x0B && m.cap_.regs[0x5E]==0x8C);
    testUs+=LickRecovery::kSettleUs;
    m.cap_.touches=2; m.serviceLickRecovery();
    testUs+=300000; m.serviceLickRecovery();
    assert(!m.lickRecovery_.available());
    m.cap_.touches=0; testUs+=10000; m.serviceLickRecovery();
    testUs+=LickRecovery::kReleaseUs; m.serviceLickRecovery();
    assert(m.lickRecovery_.available());
    assert(m.events.back().type=="LICK_RECOVERY_READY" && m.events.back().count==16);
    assert(m.events.back().gap>=750000);
    assert(m.leftDrinkEvent_.count==21 && m.rightDrinkEvent_.count==16);
    m.cap_.touches=2; m.updateLeftDrink();
    assert(m.leftDrinkEvent_.count==22); // First real edge after verified release.
    testUs+=5000000; m.checkMPR121Health();
    assert(m.lickRecovery_.busy()); // Sustained touch automatically recovers.
    assert(std::string(m.lickRecovery_.cause)=="LEFT_TOUCH_TIMEOUT");
    assert(m.bottleSwap_.id[0]=='\0' && m.bottleSwap_.state==BottleSwapState::ACTIVE);
    m.stopSession();
    assert(m.lickRecovery_.available() && !m.running_);
    size_t writes=m.cap_.writes.size(); m.serviceLickRecovery();
    assert(m.cap_.writes.size()==writes);
  }
  {
    MouseHouse m; m.cap_.failStop=true;
    m.beginLickRecovery("CONFIG_MISMATCH");
    for (int i=0;i<3;++i) {
      m.serviceLickRecovery();
      if(i<2) {
        assert(m.lickRecovery_.state==LickRecovery::WAITING);
        size_t writes=m.cap_.writes.size();
        m.serviceLickRecovery(); assert(m.cap_.writes.size()==writes);
        testUs+=LickRecovery::kCooldownUs;
      }
    }
    assert(m.lickRecovery_.state==LickRecovery::FAILED);
    assert(m.events.back().type=="LICK_RECOVERY_FAILED");
    size_t writes=m.cap_.writes.size(); testUs+=120000000;
    m.serviceLickRecovery(); assert(m.cap_.writes.size()==writes); // No reset storm.
    m.cap_.touches=2; m.updateLeftDrink(); assert(m.leftDrinkEvent_.count==0);
    m.cap_.failStop=false; m.cap_.touches=0;
    begin(m); program(m); ready(m); // Explicit bottle recalibration recovers FAILED.
    assert(m.lickRecovery_.available() && m.lickRecovery_.attempts==0);
  }
  {
    MouseHouse m;
    // Populate the expected configuration, then corrupt a lick threshold.
    for(uint8_t i=0;i<LickRecovery::kRegisterCount;++i) {
      uint8_t reg,value; LickRecovery::setting(i,reg,value); m.cap_.regs[reg]=value;
    }
    m.cap_.regs[0x43]=9;
    testUs+=1000000;
    for(int i=0;i<50 && m.lickRecovery_.available();++i) m.checkMPR121Health();
    assert(m.lickRecovery_.busy() && std::string(m.lickRecovery_.cause)=="CONFIG_MISMATCH");
    m.setLickRecoveryTouchTimeoutMs(10000); assert(m.lickRecoveryTouchTimeoutUs_==10000000);
    m.setLickRecoveryTouchTimeoutMs(0); assert(m.lickRecoveryTouchTimeoutUs_==10000000);
  }

  // Repeated successful resets are also bounded, not only failed attempts.
  {
    MouseHouse m;
    for(int attempt=0;attempt<3;++attempt) {
      m.beginLickRecovery("CONFIG_MISMATCH");
      for(int tick=0;tick<1000 && !m.lickRecovery_.available();++tick) {
        testUs+=10000; m.serviceLickRecovery();
      }
      assert(m.lickRecovery_.available());
      assert(m.lickRecovery_.attempts==attempt+1);
    }
    m.beginLickRecovery("CONFIG_MISMATCH");
    testUs+=LickRecovery::kCooldownUs;
    size_t writes=m.cap_.writes.size(); m.serviceLickRecovery();
    assert(m.lickRecovery_.state==LickRecovery::FAILED);
    assert(m.cap_.writes.size()==writes);
  }
  // Readiness failures never resume counting; reset verification is explicit.
  for(int fault=0;fault<3;++fault) {
    MouseHouse m; m.leftDrinkEvent_.count=7; m.rightDrinkEvent_.count=11;
    if(fault==0) m.cap_.touches=2;
    if(fault==1) m.cap_.badReg=0x02;
    if(fault==2) m.cap_.badReg=0x5D;
    m.beginLickRecovery("OUT_OF_RANGE");
    for(int tick=0;tick<500 && m.lickRecovery_.state!=LickRecovery::FAILED;++tick) {
      testUs+=100000; m.serviceLickRecovery();
    }
    assert(m.lickRecovery_.state==LickRecovery::FAILED);
    assert(m.leftDrinkEvent_.count==7 && m.rightDrinkEvent_.count==11);
    assert(m.bottleSwap_.id[0]=='\0');
  }
}
