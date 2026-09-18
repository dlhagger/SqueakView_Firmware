#include <MouseHouse.h>
#include <TaskSerialProxy.h>

MouseHouse mh;
TaskSerialProxy taskSerial(&mh);
#define Serial taskSerial

// ----------------------
// Task configuration
// ----------------------
// When a pellet is retrieved, wait this long before dispensing the next one.
constexpr unsigned long PELLET_REPLACEMENT_DELAY_MS = 5000;
// If a feed completes without leaving a pellet in the well, wait this long
// before trying again.
constexpr unsigned long FAILED_FEED_RETRY_DELAY_MS = 2000;

bool sessionWasRunning = false;
bool pelletNeeded = false;
bool retryFeedPending = false;
unsigned long nextFeedAtMs = 0;
bool feedWasActive = false;
bool feedWasJammed = false;

void resetTaskState() {
  pelletNeeded = false;
  retryFeedPending = false;
  nextFeedAtMs = 0;
  feedWasActive = false;
  feedWasJammed = false;
}

void schedulePelletDelivery(unsigned long dueAtMs, bool isRetry) {
  pelletNeeded = true;
  retryFeedPending = isRetry;
  nextFeedAtMs = dueAtMs;
}

void updatePelletDeliveryState(unsigned long nowMs) {
  bool pelletAvailable = mh.isPelletAvailable();
  bool feedActive = mh.isFeedActive();
  bool feedJammed = mh.isFeedJammed();

  if (feedJammed) {
    pelletNeeded = false;
    retryFeedPending = false;
    feedWasActive = feedActive;
    feedWasJammed = true;
    return;
  }

  if (feedWasJammed && !pelletAvailable) {
    schedulePelletDelivery(nowMs, false);
  }
  feedWasJammed = false;

  if (pelletAvailable) {
    pelletNeeded = false;
    retryFeedPending = false;
  }

  // If the feeder just stopped and the well is still empty, keep trying until
  // a pellet is actually detected.
  if (feedWasActive && !feedActive && !pelletAvailable) {
    schedulePelletDelivery(nowMs + FAILED_FEED_RETRY_DELAY_MS, true);
  }

  if (pelletNeeded && !feedActive && (long)(nowMs - nextFeedAtMs) >= 0) {
    mh.feed();
  }

  feedWasActive = feedActive;
}

void updateRunningLights() {
  if (mh.isFeedActive()) {
    // Amber while the feeder is moving.
    mh.setMainStrip(20, 8, 0, 0);
  } else if (mh.isPelletAvailable()) {
    // Green when a pellet is available in the well.
    mh.setMainStrip(0, 20, 0, 0);
  } else if (pelletNeeded && retryFeedPending) {
    // Blue while waiting to retry after a failed feed.
    mh.setMainStrip(0, 0, 20, 0);
  } else if (pelletNeeded) {
    // Dim white while waiting to replace a retrieved pellet.
    mh.setMainStrip(0, 0, 0, 8);
  } else {
    // Red means the well is empty but no dispense is currently scheduled.
    mh.setMainStrip(20, 0, 0, 0);
  }

  mh.leftPokeLightOff();
  mh.rightPokeLightOff();
}

void setup() {
  mh.begin();
  mh.setPelletSensorMode(MouseHouse::PELLET_SENSOR_LATCHED_PRESENCE);
}

void loop() {
  // Always update the rig first so serial commands, feeder state, sensor
  // polling, logging, and session state stay current.
  mh.update();

  unsigned long nowMs = millis();
  bool running = mh.isRunning();

  if (!running) {
    if (sessionWasRunning) {
      sessionWasRunning = false;
      resetTaskState();
    }

    mh.clearMainStrip();
    mh.leftPokeLightOff();
    mh.rightPokeLightOff();
    return;
  }

  if (!sessionWasRunning) {
    sessionWasRunning = true;
    resetTaskState();

    // When the task starts, make sure a pellet is available immediately.
    schedulePelletDelivery(nowMs, false);
  }

  // When a pellet is taken, wait 5 seconds and then replace it.
  if (mh.pelletRetrieved()) {
    schedulePelletDelivery(nowMs + PELLET_REPLACEMENT_DELAY_MS, false);
  }

  updatePelletDeliveryState(nowMs);

  updateRunningLights();

  // Consume unused events so the sketch stays clean if you add contingencies later.
  if (mh.leftPokeEnded()) {}
  if (mh.rightPokeEnded()) {}
  if (mh.leftDrinkEnded()) {}
  if (mh.rightDrinkEnded()) {}
}
