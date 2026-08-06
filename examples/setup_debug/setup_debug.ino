#include <MouseHouse.h>

MouseHouse mh;

// ----------------------
// Setup / debug behavior
// ----------------------
// This example is intended for cage setup and hardware debugging.
//
// What it does:
// 1. Enables sensor polling even when no session is running.
// 2. Shows poke beams on LED 9 / LED 10 in real time.
// 3. Uses the main strip to show drink and pellet sensor state.
// 4. Prints a compact DEBUG_STATUS line to Serial at a steady interval.
// 5. Leaves the library's normal serial commands available:
//    START,<fps>
//    STOP
//    FEED,<steps>
//
// Notes:
// - This sketch does not auto-start a session, so camera TTLs stay off unless
//   you explicitly send START,<fps>.
// - Library event logs still print to Serial as sensors change state.
// - This version also auto-runs the feeder once per minute.

constexpr MouseHouse::PelletSensorMode PELLET_SENSOR_MODE =
    MouseHouse::PELLET_SENSOR_LATCHED_PRESENCE;
constexpr unsigned long STATUS_INTERVAL_MS = 250;
constexpr unsigned long AUTO_FEED_INTERVAL_MS = 60000;

unsigned long lastStatusMs = 0;
unsigned long lastAutoFeedMs = 0;
bool lastPelletSensorBlocked = false;
bool lastFeedActive = false;

const char* pelletModeName() {
  return (mh.pelletSensorMode() == MouseHouse::PELLET_SENSOR_LATCHED_PRESENCE)
             ? "LATCHED_PRESENCE"
             : "TRANSIENT_DELIVERY";
}

void updateDebugLights() {
  // LED 10 follows the left poke beam.
  if (mh.leftPokeActive()) {
    mh.leftPokeLightOn(0, 0, 10, 0);
  } else {
    mh.leftPokeLightOff();
  }

  // LED 9 follows the right poke beam.
  if (mh.rightPokeActive()) {
    mh.rightPokeLightOn(0, 0, 10, 0);
  } else {
    mh.rightPokeLightOff();
  }

  // Main strip shows the higher-priority current state:
  // feeder moving > pellet available > raw pellet beam blocked > both drinks
  // > left drink > right drink > idle
  if (mh.isFeedActive()) {
    mh.setMainStrip(20, 8, 0, 0);
  } else if (mh.isPelletAvailable()) {
    mh.setMainStrip(0, 20, 0, 0);
  } else if (mh.pelletSensorBlocked()) {
    mh.setMainStrip(20, 0, 20, 0);
  } else if (mh.leftDrinkActive() && mh.rightDrinkActive()) {
    mh.setMainStrip(0, 0, 20, 0);
  } else if (mh.leftDrinkActive()) {
    mh.setMainStrip(0, 20, 0, 0);
  } else if (mh.rightDrinkActive()) {
    mh.setMainStrip(0, 10, 20, 0);
  } else {
    mh.setMainStrip(0, 0, 0, 6);
  }
}

void printStatusIfDue(unsigned long nowMs) {
  if ((long)(nowMs - lastStatusMs) < (long)STATUS_INTERVAL_MS) {
    return;
  }

  lastStatusMs = nowMs;

  Serial.print("DEBUG_STATUS,");
  Serial.print("running=");
  Serial.print(mh.isRunning());
  Serial.print(",left_poke=");
  Serial.print(mh.leftPokeActive());
  Serial.print(",right_poke=");
  Serial.print(mh.rightPokeActive());
  Serial.print(",left_drink=");
  Serial.print(mh.leftDrinkActive());
  Serial.print(",right_drink=");
  Serial.print(mh.rightDrinkActive());
  Serial.print(",pellet_sensor=");
  Serial.print(mh.pelletSensorBlocked());
  Serial.print(",pellet_available=");
  Serial.print(mh.isPelletAvailable());
  Serial.print(",feed_active=");
  Serial.print(mh.isFeedActive());
  Serial.print(",pellet_mode=");
  Serial.println(pelletModeName());
}

void printTransitionEvents() {
  bool pelletSensorBlocked = mh.pelletSensorBlocked();
  if (pelletSensorBlocked != lastPelletSensorBlocked) {
    Serial.print("DEBUG_EVENT,");
    Serial.println(pelletSensorBlocked ? "PELLET_BEAM_BLOCKED" : "PELLET_BEAM_CLEARED");
    lastPelletSensorBlocked = pelletSensorBlocked;
  }

  bool feedActive = mh.isFeedActive();
  if (feedActive != lastFeedActive) {
    Serial.print("DEBUG_EVENT,");
    Serial.println(feedActive ? "FEED_ACTIVE" : "FEED_IDLE");
    lastFeedActive = feedActive;
  }
}

void setup() {
  mh.begin();
  mh.setPelletSensorMode(PELLET_SENSOR_MODE);
  mh.setSensorPollingWhileStopped(true);
  lastAutoFeedMs = millis();
  lastPelletSensorBlocked = mh.pelletSensorBlocked();
  lastFeedActive = mh.isFeedActive();

  Serial.println("SETUP_DEBUG_READY");
  Serial.println("SETUP_DEBUG_COMMANDS,START,<fps>|STOP|FEED,<steps>");
  Serial.println("SETUP_DEBUG_LIGHTS,LED10=LEFT_POKE,LED9=RIGHT_POKE,STRIP=FEED_OR_PELLET_OR_DRINK");
  Serial.print("SETUP_DEBUG_PELLET_MODE,");
  Serial.println(pelletModeName());
  Serial.print("SETUP_DEBUG_AUTO_FEED,steps=300,interval_ms=");
  Serial.println(AUTO_FEED_INTERVAL_MS);
  if (mh.pelletSensorMode() == MouseHouse::PELLET_SENSOR_TRANSIENT_DELIVERY) {
    Serial.println("SETUP_DEBUG_NOTE,TRANSIENT_DELIVERY_HAS_NO_WELL_CHECK_OR_RETRIEVAL");
  }
}

void loop() {
  mh.update();

  unsigned long nowMs = millis();

  updateDebugLights();
  printStatusIfDue(nowMs);
  printTransitionEvents();

  if (!mh.isFeedActive() && (long)(nowMs - lastAutoFeedMs) >= (long)AUTO_FEED_INTERVAL_MS) {
    lastAutoFeedMs = nowMs;
    mh.feed();
    Serial.println("DEBUG_EVENT,AUTO_FEED");
  }

  if (mh.leftPokeEnded()) {
    Serial.println("DEBUG_EVENT,LEFT_POKE_END");
  }

  if (mh.rightPokeEnded()) {
    Serial.println("DEBUG_EVENT,RIGHT_POKE_END");
  }

  if (mh.leftDrinkEnded()) {
    Serial.println("DEBUG_EVENT,LEFT_DRINK_END");
  }

  if (mh.rightDrinkEnded()) {
    Serial.println("DEBUG_EVENT,RIGHT_DRINK_END");
  }

  if (mh.pelletRetrieved()) {
    Serial.println("DEBUG_EVENT,PELLET_RETRIEVED");
  }
}
