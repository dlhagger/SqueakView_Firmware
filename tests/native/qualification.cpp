#include <cassert>
#include <cstdlib>
#include "../../examples/setup_debug/setup_debug.ino"

int main(int argc, char** argv) {
  assert(argc == 2);
  int scenario = std::atoi(argv[1]);
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
