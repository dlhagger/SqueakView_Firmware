#include <cassert>
#include <cstdlib>
#include <algorithm>
#include "../../examples/go_nogo_automated/go_nogo_automated.ino"
#undef Serial

int main(int argc, char** argv) {
  assert(argc == 2);
  int scenario = std::atoi(argv[1]);
  if (scenario == 16) {
    setup();
    Serial.input = "STOP\nSTART,30\n";
    loop();
    assert(!mh.isRunning());
    assert(std::count(mh.loggedEvents.begin(), mh.loggedEvents.end(), "ACK_STOP") == 1);
    loop();
    assert(mh.isRunning() && sessionWasRunning);
    trialCount = 17;
    trialActive = true;
    Serial.input = "STOP\nSTART_BEHAVIOR\nSTOP\nSTOP\n";
    loop();
    assert(!mh.isRunning() && !trialActive && !sessionWasRunning);
    loop();
    assert(mh.isRunning() && trialCount == 0 && sessionWasRunning);
    loop();
    loop();
    assert(std::count(mh.loggedEvents.begin(), mh.loggedEvents.end(), "ACK_STOP") == 4);
    assert(std::count(mh.loggedEvents.begin(), mh.loggedEvents.end(), "ACK_START") == 1);
    assert(std::count(mh.loggedEvents.begin(), mh.loggedEvents.end(), "ACK_START_BEHAVIOR") == 1);
    return 0;
  }
  if (scenario >= 4) {
    int variant = scenario - 4;
    bool held = (variant % 6) >= 3;
    int offsetMs = variant % 3 - 1;
    mh.running = sessionWasRunning = phaseInitialized = true;
    enterPhase(variant < 6 ? PHASE_3_NOGO : PHASE_4_GO_MIX, "Test");
    currentActiveSide = SIDE_LEFT;
    currentTrialType = TRIAL_NOGO;
    trialActive = true;
    trialCueStart_us = 1000000;
    leftPokeStartUnix = 800000;
    prevLeftPokeActive = true;
    mh.left = held;
    testMillis = 1000 + NOGO_TIME_MS + offsetMs;
    loop();
    if (offsetMs >= 0) {
      assert(!trialActive && mh.feedStarts == 1 && noGoCorrectCount == 1);
      assert(noGoTrialCount == 1 && waitingForRelease == held);
      assert(mh.eventValues.back() == 1); // TRIAL_RESULT is last logged event here.
      mh.left = false;
      loop();
      assert(!waitingForRelease && mh.feedStarts == 1 && noGoCorrectCount == 1);
    } else if (held) {
      assert(trialActive && mh.feedStarts == 0 && noGoTrialCount == 0);
    } else {
      assert(!trialActive && mh.feedStarts == 0 && noGoCorrectCount == 0);
      assert(noGoTrialCount == 1 && mh.isTimeoutActive());
      assert(mh.eventValues.back() == 0);
    }
    return 0;
  }
  mh.running = true;
  trainingPhase = PHASE_1_FR1;
  rewardFeedCount = PHASE_1_PELLET_THRESH - 1;
  if (scenario == 0) {
    mh.jammed = true;
    prevFeedActive = true;
    handleFeedStateTransitions();
    assert(!trialAvailabilityLit);
    assert(!isInitiationEligible());
    startTrialIfEligible(currentActiveSide, true, 0, 1000000, 1000000);
    assert(!trialActive && trialCount == 0);
    registerRewardFeedStart();
    assert(rewardFeedCount == PHASE_1_PELLET_THRESH - 1);
    assert(trainingPhase == PHASE_1_FR1 && mh.feedStarts == 0);
    mh.jammed = false;  // Operator CLEAR_JAM.
    handleFeedStateTransitions();
    assert(isInitiationEligible() && trialAvailabilityLit);
    registerRewardFeedStart();
    assert(mh.feedStarts == 1);
    assert(rewardFeedCount == PHASE_1_PELLET_THRESH);
    assert(trainingPhase == PHASE_2_HOLD_CUE);
  } else if (scenario == 1) {
    mh.rejectFeed = true;  // E.g. an integrity latch rejects a feed request.
    registerRewardFeedStart();
    assert(rewardFeedCount == PHASE_1_PELLET_THRESH - 1);
    assert(trainingPhase == PHASE_1_FR1 && mh.feedStarts == 0);
    assert(!rewardFeedStarting);
  } else {
    mh.cameraTriggers = scenario == 3;
    handleSessionStart();
    bool legacyAck = false;
    for (const auto& event : mh.loggedEvents) {
      if (event == "ACK_START") legacyAck = true;
    }
    assert(legacyAck == mh.cameraTriggers);
    assert(sessionWasRunning);
  }
}
