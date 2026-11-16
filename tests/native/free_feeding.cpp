#include <cassert>
#include "../../examples/free_feeding/free_feeding.ino"
#undef Serial

int main() {
  setup();
  Serial.input = "START,30\n";
  loop();
  assert(mh.isFeedActive() && sessionWasRunning);
  nextFeedAtMs = 999999;
  retryFeedPending = true;
  Serial.input = "STOP\nSTART,30\n";
  loop();
  assert(!mh.isRunning() && !mh.isFeedActive() && !sessionWasRunning);
  assert(!pelletNeeded && !retryFeedPending && nextFeedAtMs == 0);
  loop();
  assert(mh.isRunning() && sessionWasRunning && mh.feedStarts == 2);
}
