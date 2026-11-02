#include <algorithm>
#include <cassert>
#include <cstdlib>
#include "rtc/RTClib.h"
#ifndef FIXED_RATIO_SKETCH
#define FIXED_RATIO_SKETCH "../../examples/fixed_ratio/fixed_ratio.ino"
#endif
#include FIXED_RATIO_SKETCH

unsigned eventCount(const char* name) {
  return std::count(mh.loggedEvents.begin(), mh.loggedEvents.end(), name);
}

void checkLatchedPresence(bool initiallyBlocked) {
  mh.blocked = mh.pellet = initiallyBlocked;
  setup();
  loop();
  mh.running = true;
  loop();
  mh.blocked = mh.pellet = true;
  loop();
  testMillis += 600000;
  mh.leftEnded = true;
  loop();
  assert(mh.feedStarts == 0);  // An occupied latched well still defers feeding.
  mh.blocked = mh.pellet = false;
  loop();
  mh.leftEnded = true;
  loop();
  assert(mh.feedStarts == 1 && !mh.jammed);
  mh.stopSession();
  loop();
  mh.blocked = mh.pellet = true;
  mh.running = true;
  loop();
  testMillis += 600000;
  loop();
  assert(!pelletWellWarningActive);
  assert(eventCount("WELL_SENSOR_BLOCKED") == 0);
  assert(eventCount("WELL_SENSOR_CLEAR") == 0);
}

void checkStartup(bool initiallyBlocked) {
  mh.blocked = initiallyBlocked;
  setup();
  assert(pelletWellWarningActive == initiallyBlocked);
  assert(eventCount(initiallyBlocked ? "WELL_SENSOR_BLOCKED" : "WELL_SENSOR_CLEAR") == 1);
  loop();  // Stopped; do not keep emitting the boot status.
  mh.running = true;
  loop();
  assert(eventCount(initiallyBlocked ? "WELL_SENSOR_BLOCKED" : "WELL_SENSOR_CLEAR") == 2);
  for (int i = 0; i < 20; ++i) {
    mh.leftEnded = initiallyBlocked;
    loop();
  }
  assert(mh.feedStarts == 0 && !mh.jammed);
  testMillis += 600000;
  loop();  // An existing startup warning must not repeat at the timeout.
  assert(eventCount(initiallyBlocked ? "WELL_SENSOR_BLOCKED" : "WELL_SENSOR_CLEAR") == 2);

  if (initiallyBlocked) {
    mh.blocked = false;
    loop();
    assert(!pelletWellWarningActive);
    assert(eventCount("WELL_SENSOR_CLEAR") == 1);
  }
  unsigned reports = mh.loggedEvents.size();
  mh.leftEnded = true;
  loop();
  assert(mh.feedStarts == 1 && mh.feeding && !mh.jammed);
  // A normal pellet passage during a feed must not become a startup warning.
  mh.blocked = true;
  loop();
  assert(mh.loggedEvents.size() == reports);

  mh.stopSession();
  loop();
  mh.running = true;
  loop();
  assert(pelletWellWarningActive);
  assert(eventCount("WELL_SENSOR_BLOCKED") == (initiallyBlocked ? 3U : 1U));
  assert(mh.feedStarts == 1 && !mh.jammed);
}

void checkWatchdog() {
  setup();
  mh.running = true;
  loop();
  assert(eventCount("WELL_SENSOR_CLEAR") == 2);

  // The first blocked sample can occur at monotonic time zero.
  mh.blocked = true;
  loop();
  testMillis += 299999;
  loop();
  assert(!pelletWellWarningActive && eventCount("WELL_SENSOR_BLOCKED") == 0);
  testMillis += 1;
  loop();
  assert(pelletWellWarningActive && eventCount("WELL_SENSOR_BLOCKED") == 1);
  for (int i = 0; i < 10; ++i) {
    testMillis += 300000;
    loop();
  }
  assert(eventCount("WELL_SENSOR_BLOCKED") == 1);
  assert(mh.feedStarts == 0 && !mh.jammed);

  mh.blocked = false;
  loop();
  assert(!pelletWellWarningActive && eventCount("WELL_SENSOR_CLEAR") == 3);
  loop();
  assert(eventCount("WELL_SENSOR_CLEAR") == 3);

  // Brief passages and interrupted blockages must not accumulate time.
  for (unsigned duration : {10U, 299999U}) {
    mh.blocked = true;
    loop();
    testMillis += duration;
    loop();
    mh.blocked = false;
    loop();
  }
  assert(eventCount("WELL_SENSOR_BLOCKED") == 1);
  assert(eventCount("WELL_SENSOR_CLEAR") == 3);
  mh.blocked = true;
  loop();
  testMillis += 299999;
  loop();
  assert(eventCount("WELL_SENSOR_BLOCKED") == 1);
  testMillis += 1;
  loop();
  assert(eventCount("WELL_SENSOR_BLOCKED") == 2);
  mh.blocked = false;
  loop();

  // Time spent stopped must not count toward a new session's watchdog.
  mh.blocked = true;
  loop();
  testMillis += 299999;
  loop();
  mh.stopSession();
  loop();
  testMillis += 600000;
  loop();
  assert(eventCount("WELL_SENSOR_BLOCKED") == 2);
  mh.blocked = false;
  mh.running = true;
  loop();
  mh.blocked = true;
  loop();
  testMillis += 1;
  loop();
  assert(eventCount("WELL_SENSOR_BLOCKED") == 2);
  testMillis += 299999;
  loop();
  assert(eventCount("WELL_SENSOR_BLOCKED") == 3);
  assert(mh.feedStarts == 0 && !mh.jammed);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  int scenario = std::atoi(argv[1]);
  if (PELLET_MODE == LATCHED_PRESENCE) {
    checkLatchedPresence(scenario != 0);
  } else if (scenario == 2) {
    checkWatchdog();
  } else {
    checkStartup(scenario != 0);
  }
}
