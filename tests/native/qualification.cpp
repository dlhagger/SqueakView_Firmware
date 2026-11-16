#include <cassert>
#include <cstdlib>
#include "../../examples/setup_debug/setup_debug.ino"
#undef Serial

int main(int argc, char** argv) {
  assert(argc == 2);
  int scenario = std::atoi(argv[1]);
  setup();
  if (scenario == 14) {
    stage = STAGE_CAMERA_CYCLE;
    Serial.input = "START,30\nSTOP\nSTART,30\n";
    loop();
    assert(mh.isRunning() && cameraRunObserved);
    loop();
    assert(!mh.isRunning() && checks.cameraCycle && stage == STAGE_CAMERA_CONFIRM);
    loop();
    assert(mh.isRunning());
    return 0;
  }
  if (scenario >= 5) {
    int variant = scenario - 5;
    int phase = variant % 3; // pre-feed, feeding, post-feed
    FeedTestMode mode = variant / 3 == 0 ? FEED_TEST_ALL_OFF
                       : variant / 3 == 1 ? FEED_TEST_LEFT_BLUE : FEED_TEST_JAM;
    stage = mode == FEED_TEST_ALL_OFF ? STAGE_FEED_DARK
            : mode == FEED_TEST_LEFT_BLUE ? STAGE_FEED_LIGHT : STAGE_JAM;
    startFeedTest(mode);
    if (phase >= 1) {
      testMillis = TEST_OBSERVATION_MS;
      serviceFeedTest(testMillis);
      assert(mh.isFeedActive());
    }
    if (phase == 2) {
      mh.feeding = false;
      serviceFeedTest(testMillis);
    }
    unsigned starts = mh.feedStarts;
    // Include a simultaneous poke and repeated idle STOP in the same buffer.
    mh.leftEnded = mh.rightEnded = true;
    Serial.input = "STOP\nSTOP\n";
    loop();
    assert(stage == STAGE_ABORTED && !mh.isFeedActive());
    for (unsigned i = 0; i < 10; ++i) {
      testMillis += 1000;
      loop();
      assert(!mh.isFeedActive() && mh.feedStarts == starts);
    }
    assert(handleSetupCommand("TEST,RESTART"));
    assert(stage == STAGE_DEVICE_ID);
    return 0;
  }
  if (scenario == 0 || scenario == 1) {
    stage = STAGE_FEED_DARK;
    startFeedTest(FEED_TEST_ALL_OFF);
    if (scenario == 1) {
      testMillis = TEST_OBSERVATION_MS;
      serviceFeedTest(testMillis);
      assert(mh.isFeedActive());
    }
  } else if (scenario == 2) {
    stage = STAGE_CAMERA_CYCLE;
    mh.running = true;
    cameraRunObserved = true;
    lastRunning = true;
  } else if (scenario == 3) {
    setStage(STAGE_LED_SWEEP);
    assert(mh.mainLight);
  } else {
    // A simultaneous sensor transition must not consume the Abort response.
    stage = STAGE_PELLET_BLOCK;
    mh.blocked = true;
  }
  unsigned startsBeforeAbort = mh.feedStarts;
  assert(handleSetupCommand("TEST,ABORT"));
  loop();
  assert(stage == STAGE_ABORTED);
  assert(!mh.isRunning() && !mh.isFeedActive());
  for (unsigned i = 0; i < 10; ++i) {
    testMillis += 1000;
    loop();
    assert(stage == STAGE_ABORTED);
    assert(!mh.isRunning() && !mh.isFeedActive());
    assert(!mh.mainLight && !mh.leftLight && !mh.rightLight);
  }
  assert(mh.feedStarts == startsBeforeAbort);
  assert(taskSerial.output.find("result=ABORTED") != std::string::npos);
  assert(handleSetupCommand("TEST,RESTART"));
  assert(stage == STAGE_DEVICE_ID);
}
