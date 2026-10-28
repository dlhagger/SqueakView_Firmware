// Compile production session-control method bodies against observable hardware
// boundaries. This verifies mode changes without duplicating their decisions.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "BottleSwapState.h"
#include "LickRecovery.h"

uint64_t testUs = 100;
uint64_t time_us_64() { return testUs++; }
namespace MouseHouseProtocolV2 {
constexpr uint8_t MESSAGE_CAMERA_EPOCH=3, MESSAGE_CAMERA_CHECKPOINT=4, MESSAGE_CAMERA_STOP=5;
}
struct SerialTransport {
  enum Priority { PRIORITY_CAMERA_CRITICAL, PRIORITY_CAMERA_CHECKPOINT, PRIORITY_SAFETY };
  static constexpr int kCheckpointHighWater=48;
  bool overflow=false;
  bool overflowLatched() const { return overflow; }
  bool v2EnabledOrPending() const { return true; }
  struct Diagnostics { int queueUsed=0; };
  Diagnostics diagnostics() { return {}; }
  void noteSuppressedCheckpoint() {}
  void serviceOutput() {}
};
class MouseHouse {
public:
  void startSession(uint32_t fps, bool triggerCamera=true);
  void stopSession();
  void update();
  void drainCameraEvents();
  bool cameraTriggersEnabled() const;
  bool running_=false, cameraTriggersEnabled_=false, rtcValid_=true, cameraPioReady_=true;
  bool compatibilitySerialMode_=false, sensorPollingWhileStopped_=false, feedActive_=false;
  bool cameraEpochQueued_=false, leftDrinkNeedsRelease_=false, rightDrinkNeedsRelease_=false;
  uint32_t fps_=30, sessionId_=0, frameCounter_=0, cameraIrqFrameCount_=0, cameraLoggedFrameCount_=0;
  uint64_t cameraFirstIrqUs_=0, cameraLastIrqUs_=0, cameraFirstFrameUs_=0;
  uint64_t framePeriodUs_=0, nextCameraCheckpointUs_=0, baseUs_=0, baseUnixUs_=0;
  int feedRetryCount_=0, sensorPolls=0, cameraStarts=0, hardwareStops=0, mprWrites=0;
  static constexpr int MPR121_ECR=0x5E;
  static constexpr uint64_t kCameraCheckpointIntervalUs=1000000ULL, kPulseWidthUs=1000ULL;
  static constexpr int kNotApplicable=69420;
  static constexpr const char* kNanString="nan";
  struct Event { unsigned long count=0; } leftDrinkEvent_, rightDrinkEvent_;
  struct Cap { void writeRegister(int, int) {} } cap_;
  BottleSwapState bottleSwap_;
  LickRecovery lickRecovery_;
  void beginLickRecovery(const char*) {}
  void cancelLickRecovery() {}
  SerialTransport transport_;
  std::vector<std::string> events;
  std::vector<uint32_t> eventCounts;
  std::vector<int> cameraRecords;
  void stopCameraPio() { ++hardwareStops; }
  void startCameraPio() { assert(cameraTriggersEnabled_); ++cameraStarts; }
  void configureMPR121Silent() {}
  void clearTimeoutState() {}
  void resetSessionEventCounts() { leftDrinkEvent_.count=rightDrinkEvent_.count=0; }
  void syncBehavioralStateToSensors(uint64_t) {}
  uint64_t getTimestampUs() { return testUs; }
  const char* getContext() { return "Test"; }
  void feedStop(const char*) { feedActive_=false; }
  void muteDrinkEvents(const char*) {}
  void queueBottleState() {}
  void updateBackgroundServices() {}
  void pollBehavioralSensors() { ++sensorPolls; }
  void snapshotCameraIrq(uint32_t& count, uint64_t& first, uint64_t& last) {
    count=cameraIrqFrameCount_; first=cameraFirstIrqUs_; last=cameraLastIrqUs_;
  }
  void queueCameraRecord(uint8_t type, const char*, SerialTransport::Priority, uint32_t, uint64_t) {
    cameraRecords.push_back(type);
  }
  void logEvent(const char* type, uint64_t, uint64_t, const char*, unsigned long count,
                uint64_t, uint64_t, long, const char*, const char*) {
    events.emplace_back(type); eventCounts.push_back(count);
  }
};
#include "session_methods.inc"

int main(int, char**) {
  MouseHouse m;
  m.startSession(30);
  assert(m.running_ && m.cameraTriggersEnabled() && m.cameraStarts==1);
  assert(m.events.back()=="ACK_START" && m.framePeriodUs_==33333);
  m.cameraIrqFrameCount_=7; m.cameraFirstIrqUs_=10; m.cameraLastIrqUs_=200;
  m.update();
  assert(m.sensorPolls==1 && m.frameCounter_==7 && m.cameraRecords.size()==1);
  m.stopSession();
  assert(!m.running_ && !m.cameraTriggersEnabled());
  assert(m.events.back()=="ACK_STOP" && m.eventCounts.back()==7);
  assert(m.cameraRecords.back()==MouseHouseProtocolV2::MESSAGE_CAMERA_STOP);
  size_t records=m.cameraRecords.size();
  m.cameraPioReady_=false;
  m.startSession(0, false);
  assert(m.running_ && !m.cameraTriggersEnabled() && m.cameraStarts==1);
  assert(m.fps_==0 && m.framePeriodUs_==0 && m.frameCounter_==0);
  assert(m.cameraIrqFrameCount_==0 && m.events.back()=="ACK_START_BEHAVIOR");
  m.leftDrinkEvent_.count=10; m.rightDrinkEvent_.count=15;
  assert(m.bottleSwap_.pause("0123456789abcdef0123456789abcdef",1000,10,15)==BottleSwapState::TRANSITION);
  m.cameraIrqFrameCount_=99; // Stale/fault-injected IRQ state must never leak.
  m.update();
  assert(m.sensorPolls==2 && m.cameraRecords.size()==records && m.frameCounter_==0);
  m.startSession(30); // An active behavior session cannot turn triggers on.
  assert(m.cameraStarts==1 && !m.cameraTriggersEnabled());
  m.feedActive_=true;
  m.stopSession();
  assert(!m.running_ && !m.feedActive_ && !m.cameraTriggersEnabled());
  assert(m.events.back()=="ACK_STOP" && m.eventCounts.back()==0);
  assert(m.cameraRecords.size()==records);
  assert(m.bottleSwap_.id[0]=='\0');
  m.startSession(30); // Triggered sessions still require available PIO.
  assert(!m.running_);
  m.rtcValid_=false;
  m.startSession(0,false);
  assert(!m.running_);
  m.rtcValid_=true; m.cameraPioReady_=true;
  m.startSession(0);
  assert(!m.running_);
  m.startSession(60);
  assert(m.running_ && m.cameraTriggersEnabled() && m.cameraStarts==2);
  assert(m.framePeriodUs_==16666 && m.events.back()=="ACK_START");
}
