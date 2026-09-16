#include <MouseHouse.h>

MouseHouse mh;

// ----------------------
// Task configuration
// ----------------------
// Set the fixed-ratio requirement here.
// Examples:
//   FR = 1  -> FR1
//   FR = 3  -> FR3
//   FR = 5  -> FR5
constexpr uint8_t FR = 1;  // Set to 1-5 for FR1-FR5.

// Choose which pellet-sensor faceplate is installed for this task.
// Examples:
//   PELLET_MODE = LATCHED_PRESENCE   -> sensed pellet well
//   PELLET_MODE = TRANSIENT_DELIVERY -> delivery-confirmation beam only
enum PelletMode {
  LATCHED_PRESENCE,
  TRANSIENT_DELIVERY
};
constexpr PelletMode PELLET_MODE = TRANSIENT_DELIVERY;

// The active nose poke alternates at midnight in America/New_York. The
// reference date determines the sequence and makes it stable across resets.
enum ActivePoke {
  LEFT,
  RIGHT
};
constexpr uint16_t ACTIVE_POKE_REFERENCE_YEAR = 2026;
constexpr uint8_t ACTIVE_POKE_REFERENCE_MONTH = 9;
constexpr uint8_t ACTIVE_POKE_REFERENCE_DAY = 15;
constexpr ActivePoke ACTIVE_POKE_ON_REFERENCE_DAY = LEFT;

// Match the reversed room light cycle. The house light is on from 7:33 PM
// through 7:32:59 AM, and off from 7:33 AM through 7:32:59 PM.
constexpr uint8_t HOUSE_LIGHT_ON_HOUR = 19;
constexpr uint8_t HOUSE_LIGHT_ON_MINUTE = 33;
constexpr uint8_t HOUSE_LIGHT_OFF_HOUR = 7;
constexpr uint8_t HOUSE_LIGHT_OFF_MINUTE = 33;

// Counts completed active pokes toward the next reward.
uint8_t activePokesTowardReward = 0;

// If the active poke overlaps with feeder movement, ignore that poke when it
// ends so it does not count toward the FR requirement.
bool ignoreCurrentActivePoke = false;

ActivePoke activePoke = ACTIVE_POKE_ON_REFERENCE_DAY;
uint32_t activePokeDayNumber = UINT32_MAX;
bool activePokeScheduleChangedThisLoop = false;
bool activePokeSwitchPending = false;
ActivePoke pendingActivePoke = ACTIVE_POKE_ON_REFERENCE_DAY;
uint32_t pendingActivePokeDayNumber = UINT32_MAX;
bool finishRatioBeforeSwitch = false;
bool pendingRewardFeedStarted = false;
bool pendingRewardDelivered = false;
bool feedWasActive = false;
bool taskRewardFeedInProgress = false;
bool taskRewardFailedThisLoop = false;

ActivePoke oppositePoke(ActivePoke poke) {
  return poke == LEFT ? RIGHT : LEFT;
}

ActivePoke scheduledPokeForDay(uint32_t localDayNumber) {
  static const uint32_t referenceDayNumber =
      DateTime(ACTIVE_POKE_REFERENCE_YEAR,
               ACTIVE_POKE_REFERENCE_MONTH,
               ACTIVE_POKE_REFERENCE_DAY).unixtime() / 86400UL;
  int64_t daysFromReference = (int64_t)localDayNumber - (int64_t)referenceDayNumber;
  return (daysFromReference % 2LL == 0)
             ? ACTIVE_POKE_ON_REFERENCE_DAY
             : oppositePoke(ACTIVE_POKE_ON_REFERENCE_DAY);
}

bool updateActivePokeSchedule() {
  if (!mh.rtcValid()) return false;

  uint32_t today = mh.currentDayNumber(MouseHouse::HOUSE_LIGHT_US_EASTERN);
  ActivePoke scheduledPoke = scheduledPokeForDay(today);
  bool applySchedule = false;

  // Initialize directly from the calendar so resets do not alter the
  // left/right sequence.
  if (activePokeDayNumber == UINT32_MAX) {
    activePoke = scheduledPoke;
    activePokeDayNumber = today;
    applySchedule = true;
  } else if (today != activePokeDayNumber
             && (!activePokeSwitchPending
                 || today != pendingActivePokeDayNumber)) {
    activePokeSwitchPending = true;
    pendingActivePoke = scheduledPoke;
    pendingActivePokeDayNumber = today;

    bool activePokeInProgress = (activePoke == LEFT)
                                    ? mh.leftPokeActive()
                                    : mh.rightPokeActive();
    if (activePokesTowardReward > 0 || activePokeInProgress
        || taskRewardFeedInProgress || taskRewardFailedThisLoop) {
      finishRatioBeforeSwitch = true;
    }
    if (taskRewardFeedInProgress) pendingRewardFeedStarted = true;

    Serial.print("ACTIVE_POKE_SWITCH_PENDING,");
    Serial.print(mh.timestampUs());
    Serial.print(",local_day=");
    Serial.print(today);
    Serial.print(",current_side=");
    Serial.print(activePoke == LEFT ? "LEFT" : "RIGHT");
    Serial.print(",next_side=");
    Serial.print(pendingActivePoke == LEFT ? "LEFT" : "RIGHT");
    Serial.print(",progress=");
    Serial.print(activePokesTowardReward);
    Serial.print(",finish_ratio=");
    Serial.println(finishRatioBeforeSwitch ? 1 : 0);
  }

  if (activePokeSwitchPending) {
    bool activePokeInProgress = (activePoke == LEFT)
                                    ? mh.leftPokeActive()
                                    : mh.rightPokeActive();
    if (activePokeInProgress) finishRatioBeforeSwitch = true;

    if (finishRatioBeforeSwitch && !pendingRewardDelivered) return false;

    bool pokeInProgress = mh.isRunning()
                          && (mh.leftPokeActive() || mh.rightPokeActive());
    if (mh.isFeedActive() || pokeInProgress) return false;

    activePoke = pendingActivePoke;
    activePokeDayNumber = pendingActivePokeDayNumber;
    activePokeSwitchPending = false;
    pendingActivePokeDayNumber = UINT32_MAX;
    finishRatioBeforeSwitch = false;
    pendingRewardFeedStarted = false;
    pendingRewardDelivered = false;
    applySchedule = true;
  }

  if (!applySchedule) return false;

  activePokesTowardReward = 0;
  ignoreCurrentActivePoke = false;

  Serial.print("ACTIVE_POKE_SCHEDULE,");
  Serial.print(mh.timestampUs());
  Serial.print(",local_day=");
  Serial.print(today);
  Serial.print(",side=");
  Serial.println(activePoke == LEFT ? "LEFT" : "RIGHT");
  return true;
}

void setup() {
  mh.begin();
  mh.setHouseLightSchedule(HOUSE_LIGHT_ON_HOUR, HOUSE_LIGHT_ON_MINUTE,
                           HOUSE_LIGHT_OFF_HOUR, HOUSE_LIGHT_OFF_MINUTE,
                           MouseHouse::HOUSE_LIGHT_US_EASTERN);
  if (PELLET_MODE == LATCHED_PRESENCE) {
    mh.setPelletSensorMode(MouseHouse::PELLET_SENSOR_LATCHED_PRESENCE);
  } else {
    mh.setPelletSensorMode(MouseHouse::PELLET_SENSOR_TRANSIENT_DELIVERY);
  }
}

void loop() {
  // Always update the rig first so serial commands, feeder state, sensor
  // polling, logging, and session state stay current.
  mh.update();

  bool running = mh.isRunning();
  bool feedActive = mh.isFeedActive();
  taskRewardFailedThisLoop = false;

  // A stopped session abandons an unfinished ratio. Keep a pending calendar
  // change, but no longer require the old side to complete a reward first.
  if (!running) {
    activePokesTowardReward = 0;
    ignoreCurrentActivePoke = false;
    finishRatioBeforeSwitch = false;
    pendingRewardFeedStarted = false;
    pendingRewardDelivered = false;
    taskRewardFeedInProgress = false;
  } else if (feedWasActive && !feedActive && taskRewardFeedInProgress) {
    if (activePokeSwitchPending && pendingRewardFeedStarted) {
      if (mh.isFeedJammed()) {
        // After CLEAR_JAM, the old side must earn another complete ratio and
        // successful reward before the pending handoff can occur.
        pendingRewardFeedStarted = false;
      } else {
        pendingRewardDelivered = true;
      }
    }
    taskRewardFailedThisLoop = mh.isFeedJammed();
    taskRewardFeedInProgress = false;
  }
  feedWasActive = feedActive;

  activePokeScheduleChangedThisLoop = updateActivePokeSchedule();

  // Clear task-level state and task lights whenever the session is not running.
  if (!running) {
    mh.clearMainStrip();
    mh.leftPokeLightOff();
    mh.rightPokeLightOff();
    return;
  }

  // Example session lights for this FR task.
  // mh.setMainStrip(0, 0, 0, 1);

  if (activePoke == LEFT) {
    mh.leftPokeLightOn(0, 0, 10, 0);
    mh.rightPokeLightOff();
  } else {
    mh.rightPokeLightOn(0, 0, 10, 0);
    mh.leftPokeLightOff();
  }

  bool activePokeActive = (activePoke == LEFT) ? mh.leftPokeActive() : mh.rightPokeActive();
  if (mh.isFeedActive() && activePokeActive) {
    ignoreCurrentActivePoke = true;
  }

  // Read each poke-ended event once per loop. The library consumes these
  // flags when you call leftPokeEnded() / rightPokeEnded().
  bool leftPokeEnded = mh.leftPokeEnded();
  bool rightPokeEnded = mh.rightPokeEnded();
  bool activePokeEnded = !activePokeScheduleChangedThisLoop
                         && ((activePoke == LEFT) ? leftPokeEnded : rightPokeEnded);

  // FR contingency:
  // 1. Wait for the selected poke to end.
  // 2. Ignore it if it overlapped with motor movement.
  // 3. Count it toward the ratio.
  // 4. Deliver reward when the ratio is reached.
  if (activePokeEnded) {
    if (ignoreCurrentActivePoke || mh.isFeedActive()) {
      ignoreCurrentActivePoke = false;
    } else if (mh.isFeedJammed()) {
      // Require an operator CLEAR_JAM before accepting another reward.
    } else if (PELLET_MODE == LATCHED_PRESENCE && mh.isPelletAvailable()) {
      // In a latched-well setup, do not count responses toward the next ratio
      // while the previously earned pellet is still sitting in the well.
    } else if (PELLET_MODE == TRANSIENT_DELIVERY && mh.pelletSensorBlocked()) {
      // In a transient-delivery setup, a blocked pellet beam after feeding
      // means the chute is still occupied, so do not count new responses yet.
    } else {
      activePokesTowardReward++;

      if (activePokesTowardReward >= FR) {
        activePokesTowardReward = 0;

        if (activePokeSwitchPending && finishRatioBeforeSwitch) {
          pendingRewardFeedStarted = true;
        }
        taskRewardFeedInProgress = true;

        // Edit this reward block to change what happens when the FR
        // requirement is met.
        mh.feed();
        mh.playTone(10000, 250);
      }
    }
  }

  // Inactive-poke hooks. Add logic here if the inactive poke should have a
  // consequence in a future protocol.
  if (activePoke == LEFT && rightPokeEnded) {
    // Reserved for future inactive-poke contingencies.
  }

  if (activePoke == RIGHT && leftPokeEnded) {
    // Reserved for future inactive-poke contingencies.
  }

  if (mh.leftDrinkEnded()) {
    // Reserved for future drink contingencies.
  }

  if (mh.rightDrinkEnded()) {
    // Reserved for future drink contingencies.
  }

  if (mh.pelletRetrieved()) {
    // Reserved for future pellet contingencies. In TRANSIENT_DELIVERY mode,
    // this will not fire automatically because retrieval is not sensed.
  }
}
