#pragma once
#include "Arduino.h"
#include <string>

class MouseHouse {
public:
  enum PelletSensorMode { PELLET_SENSOR_LATCHED_PRESENCE };
  enum TimeoutSignalMode { TIMEOUT_CLICK_PATTERN };
  bool running = false, feeding = false, jammed = false, rejectFeed = false;
  bool cameraTriggers = true;
  std::vector<std::string> loggedEvents;
  bool pellet = false, blocked = false, timeout = false;
  bool left = false, right = false, mainLight = false;
  bool leftLight = false, rightLight = false;
  unsigned feedStarts = 0, stops = 0;

  void begin() {}
  void update() {}
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
  bool leftPokeEnded() { return false; }
  bool rightPokeEnded() { return false; }
  bool leftDrinkEnded() { return false; }
  bool rightDrinkEnded() { return false; }
  bool pelletRetrieved() { return false; }
  bool timeoutEnded() { return false; }
  bool rtcValid() const { return true; }
  uint64_t timestampUs() const { return time_us_64(); }
  unsigned fps() const { return 30; }
  uint64_t framePeriodUs() const { return 33333; }
  uint64_t frameCounter() const { return 0; }
  void feed(int = 1200) {
    if (feeding || jammed || rejectFeed) return;
    feeding = true;
    ++feedStarts;
  }
  void stopSession() { running = feeding = timeout = false; ++stops; }
  template <typename... Args> void setMainStrip(Args...) { mainLight = true; }
  template <typename... Args> void leftPokeLightOn(Args...) { leftLight = true; }
  template <typename... Args> void rightPokeLightOn(Args...) { rightLight = true; }
  void clearMainStrip() { mainLight = false; }
  void leftPokeLightOff() { leftLight = false; }
  void rightPokeLightOff() { rightLight = false; }
  template <typename... Args> void logEvent(const char* event, Args...) {
    loggedEvents.emplace_back(event);
  }
  template <typename... Args> void playTone(Args...) {}
  template <typename... Args> void startTimeout(Args...) { timeout = true; }
  void setCompatibilitySerialMode(bool) {}
  void setTaskContext(const char*) {}
  void setPelletSensorMode(PelletSensorMode) {}
  void setSensorPollingWhileStopped(bool) {}
  void setSerialCommandHandler(bool (*)(const char*)) {}
};
