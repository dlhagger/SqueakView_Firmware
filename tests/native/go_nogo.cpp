#include <cassert>
#include <cstdlib>
#include "../../examples/go_nogo_automated/go_nogo_automated.ino"

int main(int argc, char** argv) {
  assert(argc == 2);
  int scenario = std::atoi(argv[1]);
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
