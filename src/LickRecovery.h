#ifndef MOUSEHOUSE_LICK_RECOVERY_H
#define MOUSEHOUSE_LICK_RECOVERY_H
#include <stdint.h>

// Hardware-independent policy and bounded register sequencer. A reset here is
// a nonblocking software reset followed by verified Stop -> Run autoconfiguration.
class LickRecovery {
public:
  enum State { ACTIVE, WAITING, CALIBRATING, FAILED };
  enum Change { NONE, ATTEMPT, RETRY, READY, EXHAUSTED };
  static constexpr uint64_t kSettleUs = 500000ULL, kReleaseUs = 250000ULL;
  static constexpr uint64_t kDeadlineUs = 10000000ULL, kCooldownUs = 5000000ULL;
  static constexpr uint64_t kBudgetWindowUs = 60000000ULL;
  static constexpr uint8_t kMaxAttempts = 3;
  State state = ACTIVE;
  const char* reason = "READY";
  const char* cause = "READY";
  uint32_t id = 0;
  uint64_t gapStart = 0, gapEnd = 0;
  uint8_t attempts = 0, cursor = 0, failedRegister = 0;
  uint16_t expected = 0, actual = 0;

  void reset() { *this = LickRecovery(); }
  bool available() const { return state == ACTIVE; }
  bool busy() const { return state == WAITING || state == CALIBRATING; }
  bool request(uint64_t now, const char* why) {
    if (state != ACTIVE) return false;
    state = WAITING; cause = reason = why; ++id;
    gapStart = now; gapEnd = 0;
    return true;
  }
  void manualReady(uint64_t now) {
    state = ACTIVE; reason = "MANUAL_READY"; gapEnd = now;
    // Explicit operator recalibration grants a fresh recovery budget.
    attempts = 0; windowSet_ = false; nextAttempt_ = now + kCooldownUs;
  }
  template<class Sensor> Change service(Sensor& cap, uint64_t now) {
    if (state == WAITING) {
      if (now < nextAttempt_) return NONE;
      if (!windowSet_ || now - windowStart_ >= kBudgetWindowUs) {
        windowSet_ = true; windowStart_ = now; attempts = 0;
      }
      if (attempts >= kMaxAttempts) {
        state = FAILED; reason = "RESET_BUDGET_EXHAUSTED"; return EXHAUSTED;
      }
      ++attempts; state = CALIBRATING; attemptStart_ = now;
      runStarted_ = released_ = false; cursor = 0;
      failedRegister = 0; expected = actual = 0;
      cap.writeRegister(0x5E, 0);
      actual = cap.readRegister8(0x5E);
      if (actual != 0) return fail(now, "STOP_VERIFY_FAILED", 0x5E, 0, actual);
      // Stop first: the Adafruit register writer restores the previous ECR
      // after a reset write. Its saved value must therefore already be zero.
      cap.writeRegister(0x80, 0x63);
      resetPending_ = true; resetReadyAt_ = now + 3000ULL;
      return ATTEMPT;
    }
    if (state != CALIBRATING) return NONE;
    if (now - attemptStart_ >= kDeadlineUs)
      return fail(now, "READINESS_TIMEOUT", failedRegister, expected, actual);
    if (resetPending_) {
      if (now < resetReadyAt_) return NONE;
      resetPending_ = false;
      uint8_t config = cap.readRegister8(0x5D);
      if (config != 0x24) return fail(now, "RESET_VERIFY_FAILED", 0x5D, 0x24, config);
      uint8_t ecr = cap.readRegister8(0x5E);
      if (ecr != 0) return fail(now, "STOP_VERIFY_FAILED", 0x5E, 0, ecr);
      return NONE;
    }
    if (cursor < kRegisterCount) {
      uint8_t reg, value; setting(cursor++, reg, value);
      cap.writeRegister(reg, value);
      uint8_t readback = cap.readRegister8(reg);
      if (readback != value) return fail(now, "REGISTER_VERIFY_FAILED", reg, value, readback);
      if (reg == 0x5E) { runStarted_ = true; runAt_ = now; nextRead_ = now + kSettleUs; }
      return NONE;
    }
    if (!runStarted_ || now < nextRead_) return NONE;
    nextRead_ = now + 10000ULL;
    uint8_t ecr = cap.readRegister8(0x5E);
    uint8_t low = cap.readRegister8(0x02), high = cap.readRegister8(0x03);
    uint16_t touch = cap.touched();
    bool healthy = ecr == 0x8C && !(low | high);
    bool clear = !(touch & 0x06);
    if (!healthy || !clear) {
      released_ = false;
      failedRegister = !healthy ? (ecr != 0x8C ? 0x5E : (low ? 0x02 : 0x03)) : 0x00;
      expected = failedRegister == 0x5E ? 0x8C : 0;
      actual = failedRegister == 0x5E ? ecr : failedRegister == 0x02 ? low : failedRegister == 0x03 ? high : touch;
      reason = !healthy ? "SENSOR_NOT_HEALTHY" : "CHANNELS_NOT_RELEASED";
      return NONE;
    }
    if (!released_) { released_ = true; releasedAt_ = now; }
    if (now - runAt_ >= kSettleUs && now - releasedAt_ >= kReleaseUs) {
      state = ACTIVE; reason = "READY"; gapEnd = now; nextAttempt_ = now + kCooldownUs;
      return READY;
    }
    return NONE;
  }

  // Preserve the driver's established baseline/filter settings as well as the
  // requested thresholds 2/0, debounce 0/0, and autoconfig + autoreconfig 0x0B.
  static constexpr uint8_t kRegisterCount = 43;
  static void setting(uint8_t i, uint8_t& reg, uint8_t& value) {
    static const uint8_t regs[] = {0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,0x34,0x35,0x5C,0x5D};
    static const uint8_t vals[] = {1,1,14,0,1,5,1,0,0,0,0,0x10,0x20};
    if (i < 13) { reg = regs[i]; value = vals[i]; return; }
    i -= 13;
    if (i < 24) { reg = 0x41 + i; value = i % 2 ? 0 : 2; return; }
    static const uint8_t tailRegs[] = {0x7B,0x7D,0x7F,0x7E,0x5B,0x5E};
    static const uint8_t tailVals[] = {0x0B,200,180,130,0,0x8C};
    reg = tailRegs[i-24]; value = tailVals[i-24];
  }
private:
  bool windowSet_ = false, runStarted_ = false, released_ = false, resetPending_ = false;
  uint64_t windowStart_ = 0, nextAttempt_ = 0, attemptStart_ = 0;
  uint64_t runAt_ = 0, releasedAt_ = 0, nextRead_ = 0;
  uint64_t resetReadyAt_ = 0;
  Change fail(uint64_t now, const char* why, uint8_t reg, uint16_t wanted, uint16_t got) {
    reason = why; failedRegister = reg; expected = wanted; actual = got;
    nextAttempt_ = now + kCooldownUs;
    state = attempts >= kMaxAttempts ? FAILED : WAITING;
    return state == FAILED ? EXHAUSTED : RETRY;
  }
};
#endif
