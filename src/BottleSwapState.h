#ifndef MOUSEHOUSE_BOTTLE_SWAP_STATE_H
#define MOUSEHOUSE_BOTTLE_SWAP_STATE_H

#include <stdint.h>
#include <string.h>

// Hardware-independent transaction state. Counts always remain session-global.
class BottleSwapState {
public:
  enum State { ACTIVE, PAUSED, CALIBRATING, FAILED };
  enum Request { REJECT, REPEAT, TRANSITION };
  static constexpr uint64_t kSettleUs = 500000ULL;
  static constexpr uint64_t kReleasedUs = 250000ULL;
  static constexpr uint64_t kDeadlineUs = 10000000ULL;

  State state = ACTIVE;
  char id[33] = "";
  const char* reason = "READY";
  uint64_t boundaryUs = 0;
  uint32_t left = 0, right = 0;

  static bool validId(const char* value) {
    if (!value || strlen(value) != 32) return false;
    for (unsigned i = 0; i < 32; ++i) {
      if (!((value[i] >= '0' && value[i] <= '9')
            || (value[i] >= 'a' && value[i] <= 'f'))) return false;
    }
    return true;
  }
  void reset(uint64_t now, uint32_t l, uint32_t r) {
    id[0] = '\0';
    set(ACTIVE, "READY", now, l, r);
    releasedSince_ = 0;
  }
  Request pause(const char* value, uint64_t now, uint32_t l, uint32_t r) {
    if (!validId(value)) return REJECT;
    if (strcmp(id, value) == 0) return REPEAT;
    if (state != ACTIVE) return REJECT;
    strcpy(id, value);
    set(PAUSED, "PAUSED", now, l, r);
    return TRANSITION;
  }
  Request resume(const char* value, uint64_t now, uint32_t l, uint32_t r) {
    if (!validId(value) || strcmp(id, value) != 0) return REJECT;
    if (state == ACTIVE || state == CALIBRATING) return REPEAT;
    calibrationStart_ = now;
    releasedSince_ = 0;
    set(CALIBRATING, "CALIBRATING", now, l, r);
    return TRANSITION;
  }
  void fail(uint64_t now, uint32_t l, uint32_t r,
            const char* why = "CALIBRATION_FAILED") {
    set(FAILED, why, now, l, r);
  }
  bool service(uint64_t now, bool healthy, bool released,
               uint32_t l, uint32_t r) {
    if (state != CALIBRATING) return false;
    if (now - calibrationStart_ >= kDeadlineUs) {
      set(FAILED, "CALIBRATION_FAILED", now, l, r);
      return true;
    }
    if (now - calibrationStart_ >= kSettleUs && healthy && released) {
      if (!releasedSince_) releasedSince_ = now;
      if (now - releasedSince_ >= kReleasedUs) {
        set(ACTIVE, "READY", now, l, r);
        return true;
      }
    } else {
      releasedSince_ = 0;
    }
    return false;
  }
  const char* name() const {
    switch (state) {
      case PAUSED: return "PAUSED";
      case CALIBRATING: return "CALIBRATING";
      case FAILED: return "FAILED";
      default: return "ACTIVE";
    }
  }
private:
  uint64_t calibrationStart_ = 0, releasedSince_ = 0;
  void set(State next, const char* why, uint64_t now, uint32_t l, uint32_t r) {
    state = next; reason = why; boundaryUs = now; left = l; right = r;
  }
};
#endif
