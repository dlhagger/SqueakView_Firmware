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

// Choose which nose poke is active for this task.
// Examples:
//   ACTIVE_POKE = LEFT   -> left poke earns reward
//   ACTIVE_POKE = RIGHT  -> right poke earns reward
enum ActivePoke {
  LEFT,
  RIGHT
};
constexpr ActivePoke ACTIVE_POKE = LEFT;

// Counts completed active pokes toward the next reward.
uint8_t activePokesTowardReward = 0;

// If the active poke overlaps with feeder movement, ignore that poke when it
// ends so it does not count toward the FR requirement.
bool ignoreCurrentActivePoke = false;

void setup() {
  mh.begin();
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

  // Clear task-level state and task lights whenever the session is not running.
  if (!mh.isRunning()) {
    activePokesTowardReward = 0;
    ignoreCurrentActivePoke = false;
    mh.clearMainStrip();
    mh.leftPokeLightOff();
    mh.rightPokeLightOff();
    return;
  }

  // Example session lights for this FR task.
  // mh.setMainStrip(0, 0, 0, 1);

  if (ACTIVE_POKE == LEFT) {
    mh.leftPokeLightOn(0, 0, 10, 0);
    mh.rightPokeLightOff();
  } else {
    mh.rightPokeLightOn(0, 0, 10, 0);
    mh.leftPokeLightOff();
  }

  bool activePokeActive = (ACTIVE_POKE == LEFT) ? mh.leftPokeActive() : mh.rightPokeActive();
  if (mh.isFeedActive() && activePokeActive) {
    ignoreCurrentActivePoke = true;
  }

  // Read each poke-ended event once per loop. The library consumes these
  // flags when you call leftPokeEnded() / rightPokeEnded().
  bool leftPokeEnded = mh.leftPokeEnded();
  bool rightPokeEnded = mh.rightPokeEnded();
  bool activePokeEnded = (ACTIVE_POKE == LEFT) ? leftPokeEnded : rightPokeEnded;

  // FR contingency:
  // 1. Wait for the selected poke to end.
  // 2. Ignore it if it overlapped with motor movement.
  // 3. Count it toward the ratio.
  // 4. Deliver reward when the ratio is reached.
  if (activePokeEnded) {
    if (ignoreCurrentActivePoke || mh.isFeedActive()) {
      ignoreCurrentActivePoke = false;
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

        // Edit this reward block to change what happens when the FR
        // requirement is met.
        mh.feed();
        mh.playTone(10000, 250);
      }
    }
  }

  // Inactive-poke hooks. Add logic here if the inactive poke should have a
  // consequence in a future protocol.
  if (ACTIVE_POKE == LEFT && rightPokeEnded) {
    // Reserved for future inactive-poke contingencies.
  }

  if (ACTIVE_POKE == RIGHT && leftPokeEnded) {
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
