#pragma once
#include "Arduino.h"
#include <string>
#include <cstring>
#include "SerialTransport.h"

class MouseHouse {
public:
  enum PelletSensorMode { PELLET_SENSOR_LATCHED_PRESENCE, PELLET_SENSOR_TRANSIENT_DELIVERY };
  enum TimeoutSignalMode { TIMEOUT_CLICK_PATTERN };
  enum HouseLightTimeBasis { HOUSE_LIGHT_US_EASTERN };
  bool leftEnded = false, rightEnded = false;
  bool running = false, feeding = false, jammed = false, rejectFeed = false;
  bool& running_ = running;
  bool compatibility = false;
  void (*stopHandler_)() = nullptr;
  bool (*commandHandler_)(const char*) = nullptr;
  SerialTransport transport_;
  static constexpr size_t kSerialCmdBufferSize = 96;
  char serialCmdBuffer_[kSerialCmdBufferSize]{};
  size_t serialCmdLength_ = 0;
  bool serialCmdOverflow_ = false;
  void checkSerialCommands();
  void resetSerialCommandBuffer();
  void sendCommandError(const char*, const char*) { loggedEvents.emplace_back("NACK"); }
  void handleSerialCommand(const char* command) {
    if (strcmp(command, "STOP") == 0) stopSession();
    else if (strncmp(command, "START,", 6) == 0 || strcmp(command, "START_BEHAVIOR") == 0) {
      if (running) { loggedEvents.emplace_back("NACK"); return; }
      running = true;
      cameraTriggers = strncmp(command, "START,", 6) == 0;
      if (!compatibility || !cameraTriggers)
        loggedEvents.emplace_back(cameraTriggers ? "ACK_START" : "ACK_START_BEHAVIOR");
    } else if (commandHandler_) commandHandler_(command);
  }
  bool cameraTriggers = true;
  std::vector<std::string> loggedEvents;
  std::vector<long> eventValues;
  bool pellet = false, blocked = false, timeout = false;
  bool left = false, right = false, mainLight = false;
  bool leftLight = false, rightLight = false;
  unsigned feedStarts = 0, stops = 0;

  void begin() {}
  void update() { checkSerialCommands(); }
  bool isRunning() const { return running; }
  bool cameraTriggersEnabled() const { return running && cameraTriggers; }
  bool isFeedActive() const { return feeding; }
  bool isFeedJammed() const { return jammed; }
  bool isPelletAvailable() const { return pellet; }
  bool pelletSensorBlocked() const { return blocked; }
  bool isTimeoutActive() const { return timeout; }
  bool leftPokeActive() const { return left; }
  bool rightPokeActive() const { return right; }
  bool leftDrinkActive() const { return false; }
  bool rightDrinkActive() const { return false; }
  bool leftPokeEnded() { bool value = leftEnded; leftEnded = false; return value; }
  bool rightPokeEnded() { bool value = rightEnded; rightEnded = false; return value; }
  bool leftDrinkEnded() { return false; }
  bool rightDrinkEnded() { return false; }
  bool pelletRetrieved() { return false; }
  bool timeoutEnded() { return false; }
  bool rtcValid() const { return true; }
  uint32_t currentDayNumber(HouseLightTimeBasis) const { return 0; }
  template <typename... Args> void setHouseLightSchedule(Args...) {}
  uint64_t timestampUs() const { return time_us_64(); }
  unsigned fps() const { return 30; }
  uint64_t framePeriodUs() const { return 33333; }
  uint64_t frameCounter() const { return 0; }
  void feed(int = 1200) {
    if (feeding || jammed || rejectFeed) return;
    feeding = true;
    ++feedStarts;
  }
  void stopSession() {
    running = feeding = timeout = jammed = false;
    ++stops;
    if (stopHandler_) stopHandler_();
    loggedEvents.emplace_back("ACK_STOP");
  }
  void setStopHandler(void (*handler)()) { stopHandler_ = handler; }
  template <typename... Args> void setMainStrip(Args...) { mainLight = true; }
  template <typename... Args> void leftPokeLightOn(Args...) { leftLight = true; }
  template <typename... Args> void rightPokeLightOn(Args...) { rightLight = true; }
  void clearMainStrip() { mainLight = false; }
  void leftPokeLightOff() { leftLight = false; }
  void rightPokeLightOff() { rightLight = false; }
  void logEvent(const char* event, uint64_t, uint64_t, const char*, unsigned long,
                uint64_t, uint64_t, long value, const char*, const char*) {
    loggedEvents.emplace_back(event);
    eventValues.push_back(value);
  }
  template <typename... Args> void playTone(Args...) {}
  template <typename... Args> void startTimeout(Args...) { timeout = true; }
  void setCompatibilitySerialMode(bool enabled) { compatibility = enabled; }
  void setTaskContext(const char*) {}
  void setPelletSensorMode(PelletSensorMode) {}
  void setSensorPollingWhileStopped(bool) {}
  void setSerialCommandHandler(bool (*handler)(const char*)) { commandHandler_ = handler; }
};

// These are extracted verbatim from production; only command effects/hardware
// are stubbed above, so task tests exercise the actual batching boundaries.
#include "command_methods.inc"
