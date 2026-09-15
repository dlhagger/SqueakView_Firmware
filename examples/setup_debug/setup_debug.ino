#include <MouseHouse.h>
#include <ctype.h>
#include <string.h>

MouseHouse mh;

// Guided, nonblocking pre-deployment qualification. Sensors are verified by
// firmware; LEDs, motor motion, camera TTL, buzzer, and house light require an
// operator because the controller has no physical feedback for those loads.
constexpr MouseHouse::PelletSensorMode PELLET_SENSOR_MODE =
    MouseHouse::PELLET_SENSOR_LATCHED_PRESENCE;
constexpr unsigned long TEST_OBSERVATION_MS = 750;
constexpr unsigned long COLOR_TEST_STEP_MS = 600;
constexpr int GPIO13_TEST_PIN = 13;
constexpr size_t DEVICE_ID_SIZE = 32;
constexpr int QUALIFICATION_PROTOCOL_VERSION = 2;

enum QualificationStage {
  STAGE_DEVICE_ID, STAGE_RTC, STAGE_LEFT_POKE, STAGE_RIGHT_POKE,
  STAGE_PELLET_BLOCK, STAGE_PELLET_CLEAR, STAGE_LEFT_DRINK,
  STAGE_RIGHT_DRINK, STAGE_LED_SWEEP, STAGE_LED_CONFIRM,
  STAGE_FEED_DARK, STAGE_FEED_DARK_CONFIRM, STAGE_FEED_DARK_CLEAR,
  STAGE_FEED_LIGHT, STAGE_FEED_LIGHT_CONFIRM, STAGE_FEED_LIGHT_CLEAR,
  STAGE_JAM, STAGE_JAM_CLEAR,
  STAGE_REVERSAL_CONFIRM, STAGE_BUZZER_CONFIRM, STAGE_CAMERA_CYCLE,
  STAGE_CAMERA_CONFIRM, STAGE_HOUSE_LIGHT_CONFIRM, STAGE_COMPLETE,
  STAGE_ABORTED
};

enum OperatorResponse {
  RESPONSE_NONE, RESPONSE_YES, RESPONSE_NO, RESPONSE_RETRY, RESPONSE_ABORT
};

enum FeedTestMode {
  FEED_TEST_NONE, FEED_TEST_LEFT_BLUE, FEED_TEST_ALL_OFF, FEED_TEST_JAM
};

enum FeedTestPhase {
  FEED_PHASE_NONE, FEED_PHASE_PRE_FEED, FEED_PHASE_FEEDING,
  FEED_PHASE_POST_FEED_OFF
};

enum FeedResult {
  FEED_RESULT_NONE, FEED_RESULT_PELLET, FEED_RESULT_JAMMED,
  FEED_RESULT_STOPPED
};

enum ColorTestStep {
  COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_WHITE, COLOR_OFF, COLOR_DONE
};

struct QualificationChecks {
  bool rtc = false;
  bool leftPoke = false;
  bool rightPoke = false;
  bool leftDrink = false;
  bool rightDrink = false;
  bool pelletBlocked = false;
  bool pelletCleared = false;
  bool ledRgbw = false;
  bool feedDarkFirmware = false;
  bool feedDarkMotor = false;
  bool feedLightFirmware = false;
  bool feedLightMotor = false;
  bool jamLatched = false;
  bool jamCleared = false;
  bool reversal = false;
  bool buzzer = false;
  bool cameraCycle = false;
  bool cameraTtl = false;
  bool houseLight = false;
};

QualificationStage stage = STAGE_DEVICE_ID;
OperatorResponse pendingResponse = RESPONSE_NONE;
QualificationChecks checks;
char deviceId[DEVICE_ID_SIZE] = "UNSET";
unsigned int failedAttempts = 0;
bool checklistChanged = true;
bool lastPelletBlocked = false;
bool lastFeedActive = false;
bool lastFeedJammed = false;
bool lastRunning = false;
bool cameraRunObserved = false;

FeedTestMode feedTestMode = FEED_TEST_NONE;
FeedTestPhase feedTestPhase = FEED_PHASE_NONE;
FeedResult feedResult = FEED_RESULT_NONE;
unsigned long feedTestDueMs = 0;
ColorTestStep colorStep = COLOR_DONE;
unsigned long colorDueMs = 0;

void setStage(QualificationStage nextStage);
void printPrompt();
void printChecklist(unsigned long nowMs, bool force = false);
void printStatus();
void startColorTest();
void allPixelsOff();

const char* stageName() {
  switch (stage) {
    case STAGE_DEVICE_ID: return "DEVICE_ID";
    case STAGE_RTC: return "RTC";
    case STAGE_LEFT_POKE: return "LEFT_POKE";
    case STAGE_RIGHT_POKE: return "RIGHT_POKE";
    case STAGE_PELLET_BLOCK: return "PELLET_BLOCK";
    case STAGE_PELLET_CLEAR: return "PELLET_CLEAR";
    case STAGE_LEFT_DRINK: return "LEFT_DRINK";
    case STAGE_RIGHT_DRINK: return "RIGHT_DRINK";
    case STAGE_LED_SWEEP: return "LED_SWEEP";
    case STAGE_LED_CONFIRM: return "LED_CONFIRM";
    case STAGE_FEED_DARK: return "FEED_DARK";
    case STAGE_FEED_DARK_CONFIRM: return "FEED_DARK_CONFIRM";
    case STAGE_FEED_DARK_CLEAR: return "FEED_DARK_CLEAR";
    case STAGE_FEED_LIGHT: return "FEED_LIGHT";
    case STAGE_FEED_LIGHT_CONFIRM: return "FEED_LIGHT_CONFIRM";
    case STAGE_FEED_LIGHT_CLEAR: return "FEED_LIGHT_CLEAR";
    case STAGE_JAM: return "JAM";
    case STAGE_JAM_CLEAR: return "JAM_CLEAR";
    case STAGE_REVERSAL_CONFIRM: return "REVERSAL_CONFIRM";
    case STAGE_BUZZER_CONFIRM: return "BUZZER_CONFIRM";
    case STAGE_CAMERA_CYCLE: return "CAMERA_CYCLE";
    case STAGE_CAMERA_CONFIRM: return "CAMERA_CONFIRM";
    case STAGE_HOUSE_LIGHT_CONFIRM: return "HOUSE_LIGHT_CONFIRM";
    case STAGE_COMPLETE: return "COMPLETE";
    case STAGE_ABORTED: return "ABORTED";
  }
  return "UNKNOWN";
}

const char* feedModeName() {
  if (feedTestMode == FEED_TEST_LEFT_BLUE) return "LEFT_BLUE";
  if (feedTestMode == FEED_TEST_ALL_OFF) return "ALL_OFF";
  if (feedTestMode == FEED_TEST_JAM) return "JAM";
  return "NONE";
}

const char* feedPhaseName() {
  if (feedTestPhase == FEED_PHASE_PRE_FEED) return "PRE_FEED";
  if (feedTestPhase == FEED_PHASE_FEEDING) return "FEEDING";
  if (feedTestPhase == FEED_PHASE_POST_FEED_OFF) return "POST_FEED_OFF";
  return "NONE";
}

void passCheck(bool& check, const char* name) {
  if (check) return;
  check = true;
  checklistChanged = true;
  Serial.print("SETUP_STEP_RESULT,step=");
  Serial.print(name);
  Serial.println(",result=PASS");
}

void logFailure(const char* reason) {
  failedAttempts++;
  Serial.print("SETUP_STEP_RESULT,step=");
  Serial.print(stageName());
  Serial.print(",result=FAIL,reason=");
  Serial.print(reason);
  Serial.print(",failed_attempts=");
  Serial.println(failedAttempts);
}

bool qualificationPassed() {
  return checks.rtc && checks.leftPoke && checks.rightPoke
         && checks.leftDrink && checks.rightDrink
         && checks.pelletBlocked && checks.pelletCleared && checks.ledRgbw
         && checks.feedDarkFirmware && checks.feedDarkMotor
         && checks.feedLightFirmware && checks.feedLightMotor
         && checks.jamLatched && checks.jamCleared && checks.reversal
         && checks.buzzer && checks.cameraCycle && checks.cameraTtl
         && checks.houseLight;
}

void emitRecord(const char* result) {
  // TODO(sd-logging): persist this exact record to the Adalogger SD card.
  Serial.print("SETUP_QUALIFICATION_RECORD,device=");
  Serial.print(deviceId);
  Serial.print(",result=");
  Serial.print(result);
  Serial.print(",timestamp_us=");
  Serial.print(mh.timestampUs());
  Serial.print(",firmware_date=");
  Serial.print(__DATE__);
  Serial.print(",firmware_time=");
  Serial.print(__TIME__);
  Serial.print(",failed_attempts=");
  Serial.print(failedAttempts);
  Serial.print(",rtc="); Serial.print(checks.rtc);
  Serial.print(",pokes="); Serial.print(checks.leftPoke && checks.rightPoke);
  Serial.print(",drinks="); Serial.print(checks.leftDrink && checks.rightDrink);
  Serial.print(",pellet_sensor="); Serial.print(checks.pelletBlocked && checks.pelletCleared);
  Serial.print(",led_rgbw="); Serial.print(checks.ledRgbw);
  Serial.print(",feed_dark="); Serial.print(checks.feedDarkFirmware && checks.feedDarkMotor);
  Serial.print(",feed_light="); Serial.print(checks.feedLightFirmware && checks.feedLightMotor);
  Serial.print(",jam_recovery="); Serial.print(checks.jamLatched && checks.jamCleared);
  Serial.print(",reversal="); Serial.print(checks.reversal);
  Serial.print(",buzzer="); Serial.print(checks.buzzer);
  Serial.print(",camera="); Serial.print(checks.cameraCycle && checks.cameraTtl);
  Serial.print(",house_light="); Serial.print(checks.houseLight);
  Serial.println(",storage=SERIAL_ONLY,sd_copy=TODO");
}

void printChecklist(unsigned long nowMs, bool force) {
  (void)nowMs;
  if (!force && !checklistChanged) return;
  checklistChanged = false;
  Serial.print("SETUP_CHECKLIST,device="); Serial.print(deviceId);
  Serial.print(",stage="); Serial.print(stageName());
  Serial.print(",rtc="); Serial.print(checks.rtc);
  Serial.print(",left_poke="); Serial.print(checks.leftPoke);
  Serial.print(",right_poke="); Serial.print(checks.rightPoke);
  Serial.print(",left_drink="); Serial.print(checks.leftDrink);
  Serial.print(",right_drink="); Serial.print(checks.rightDrink);
  Serial.print(",pellet_blocked="); Serial.print(checks.pelletBlocked);
  Serial.print(",pellet_cleared="); Serial.print(checks.pelletCleared);
  Serial.print(",led_rgbw="); Serial.print(checks.ledRgbw);
  Serial.print(",feed_dark_fw="); Serial.print(checks.feedDarkFirmware);
  Serial.print(",feed_dark_motor="); Serial.print(checks.feedDarkMotor);
  Serial.print(",feed_light_fw="); Serial.print(checks.feedLightFirmware);
  Serial.print(",feed_light_motor="); Serial.print(checks.feedLightMotor);
  Serial.print(",jam_latched="); Serial.print(checks.jamLatched);
  Serial.print(",jam_cleared="); Serial.print(checks.jamCleared);
  Serial.print(",reversal="); Serial.print(checks.reversal);
  Serial.print(",buzzer="); Serial.print(checks.buzzer);
  Serial.print(",camera_cycle="); Serial.print(checks.cameraCycle);
  Serial.print(",camera_ttl="); Serial.print(checks.cameraTtl);
  Serial.print(",house_light="); Serial.print(checks.houseLight);
  Serial.print(",result=");
  Serial.println(qualificationPassed() ? "PASS" : "INCOMPLETE");
}

void printPrompt() {
  Serial.print("SETUP_PROMPT,stage="); Serial.print(stageName());
  Serial.print(",action=");
  switch (stage) {
    case STAGE_DEVICE_ID: Serial.println("ENTER_DEVICE_ID"); break;
    case STAGE_RTC: Serial.println("VALIDATE_RTC_AGAINST_JETSON"); break;
    case STAGE_LEFT_POKE: Serial.println("ENTER_AND_LEAVE_LEFT_POKE"); break;
    case STAGE_RIGHT_POKE: Serial.println("ENTER_AND_LEAVE_RIGHT_POKE"); break;
    case STAGE_PELLET_BLOCK: Serial.println("BLOCK_PELLET_BEAM"); break;
    case STAGE_PELLET_CLEAR: Serial.println("CLEAR_PELLET_BEAM"); break;
    case STAGE_LEFT_DRINK: Serial.println("ACTIVATE_AND_RELEASE_LEFT_DRINK"); break;
    case STAGE_RIGHT_DRINK: Serial.println("ACTIVATE_AND_RELEASE_RIGHT_DRINK"); break;
    case STAGE_LED_SWEEP: Serial.println("WATCH_ALL_PIXELS_RED_GREEN_BLUE_WHITE_OFF"); break;
    case STAGE_LED_CONFIRM: Serial.println("CONFIRM_LED_RGBW"); break;
    case STAGE_FEED_DARK: Serial.println("RIGHT_POKE_TO_FEED_DARK_THEN_TRIGGER_PELLET_BEAM"); break;
    case STAGE_FEED_DARK_CONFIRM: Serial.println("CONFIRM_FEED_DARK_MOTOR"); break;
    case STAGE_FEED_DARK_CLEAR: Serial.println("REMOVE_PELLET_AND_CLEAR_BEAM"); break;
    case STAGE_FEED_LIGHT: Serial.println("LEFT_POKE_TO_FEED_WITH_BLUE_LIGHT_THEN_TRIGGER_PELLET_BEAM"); break;
    case STAGE_FEED_LIGHT_CONFIRM: Serial.println("CONFIRM_FEED_LIGHT_MOTOR"); break;
    case STAGE_FEED_LIGHT_CLEAR: Serial.println("REMOVE_PELLET_AND_CLEAR_BEAM"); break;
    case STAGE_JAM: Serial.println("RIGHT_POKE_THEN_LEAVE_PELLET_BEAM_CLEAR_UNTIL_JAM"); break;
    case STAGE_JAM_CLEAR: Serial.println("PHYSICALLY_CLEAR_FEEDER_THEN_SEND CLEAR_JAM"); break;
    case STAGE_REVERSAL_CONFIRM: Serial.println("CONFIRM_RETRY_REVERSAL"); break;
    case STAGE_BUZZER_CONFIRM: Serial.println("CONFIRM_BUZZER"); break;
    case STAGE_CAMERA_CYCLE: Serial.println("RUN_CAMERA_START_STOP"); break;
    case STAGE_CAMERA_CONFIRM: Serial.println("CONFIRM_CAMERA_TTL"); break;
    case STAGE_HOUSE_LIGHT_CONFIRM: Serial.println("CONFIRM_HOUSE_LIGHT"); break;
    case STAGE_COMPLETE: Serial.println("QUALIFICATION_COMPLETE"); break;
    case STAGE_ABORTED: Serial.println("RESTART_QUALIFICATION"); break;
  }
}

void setStage(QualificationStage nextStage) {
  stage = nextStage;
  pendingResponse = RESPONSE_NONE;
  checklistChanged = true;
  Serial.print("SETUP_STAGE,"); Serial.println(stageName());
  if (stage == STAGE_LED_SWEEP) startColorTest();
  if (stage == STAGE_BUZZER_CONFIRM) mh.playTone(2000, 300);
  if (stage == STAGE_COMPLETE) emitRecord(qualificationPassed() ? "PASS" : "FAIL");
  if (stage == STAGE_ABORTED) emitRecord("ABORTED");
  printPrompt();
}

bool validDeviceId(const char* value) {
  if (value == nullptr || value[0] == '\0' || strlen(value) >= DEVICE_ID_SIZE) return false;
  for (const char* p = value; *p != '\0'; ++p) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return false;
  }
  return true;
}

bool handleSetupCommand(const char* command) {
  if (strcmp(command, "TEST,HELLO") == 0) {
    Serial.print("SETUP_PROTOCOL,version=");
    Serial.print(QUALIFICATION_PROTOCOL_VERSION);
    Serial.println(",mode=GUIDED_QUALIFICATION");
    printStatus();
    printChecklist(millis(), true);
    printPrompt();
    return true;
  }
  if (strncmp(command, "TEST,DEVICE,", 12) == 0) {
    const char* value = command + 12;
    if (stage != STAGE_DEVICE_ID) Serial.println("NACK,TEST,DEVICE_ID_NOT_EXPECTED");
    else if (!validDeviceId(value)) Serial.println("NACK,TEST,INVALID_DEVICE_ID");
    else {
      strncpy(deviceId, value, sizeof(deviceId) - 1);
      deviceId[sizeof(deviceId) - 1] = '\0';
      Serial.print("ACK_TEST_DEVICE,"); Serial.println(deviceId);
      setStage(STAGE_RTC);
    }
    return true;
  }
  if (strcmp(command, "TEST,RTC_VERIFIED") == 0) {
    if (stage != STAGE_RTC) Serial.println("NACK,TEST,RTC_VERIFICATION_NOT_EXPECTED");
    else if (!mh.rtcValid()) Serial.println("NACK,TEST,RTC_INVALID");
    else {
      passCheck(checks.rtc, "RTC_JETSON_VALIDATION");
      setStage(STAGE_LEFT_POKE);
    }
    return true;
  }
  if (strcmp(command, "TEST,YES") == 0) pendingResponse = RESPONSE_YES;
  else if (strcmp(command, "TEST,NO") == 0) pendingResponse = RESPONSE_NO;
  else if (strcmp(command, "TEST,RETRY") == 0) pendingResponse = RESPONSE_RETRY;
  else if (strcmp(command, "TEST,ABORT") == 0) pendingResponse = RESPONSE_ABORT;
  else if (strcmp(command, "TEST,STATUS") == 0) {
    printStatus(); printChecklist(millis(), true); printPrompt(); return true;
  } else if (strcmp(command, "TEST,RESTART") == 0) {
    if (mh.isFeedActive() || mh.isRunning() || mh.isFeedJammed()) {
      Serial.println("NACK,TEST,STOP_SESSION_AND_CLEAR_JAM_FIRST");
    }
    else {
      checks = QualificationChecks();
      strcpy(deviceId, "UNSET");
      failedAttempts = 0;
      cameraRunObserved = false;
      colorStep = COLOR_DONE;
      feedTestMode = FEED_TEST_NONE;
      feedTestPhase = FEED_PHASE_NONE;
      allPixelsOff();
      setStage(STAGE_DEVICE_ID);
    }
    return true;
  } else return false;
  Serial.print("ACK_TEST_RESPONSE,"); Serial.println(command + 5);
  return true;
}

void allPixelsOff() {
  mh.clearMainStrip(); mh.rightPokeLightOff(); mh.leftPokeLightOff();
}

void allPixels(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  mh.setMainStrip(r, g, b, w);
  mh.rightPokeLightOn(r, g, b, w);
  mh.leftPokeLightOn(r, g, b, w);
}

void applyColorStep() {
  if (colorStep == COLOR_RED) { allPixels(12, 0, 0, 0); Serial.println("SETUP_LED_TEST,RED"); }
  else if (colorStep == COLOR_GREEN) { allPixels(0, 12, 0, 0); Serial.println("SETUP_LED_TEST,GREEN"); }
  else if (colorStep == COLOR_BLUE) { allPixels(0, 0, 12, 0); Serial.println("SETUP_LED_TEST,BLUE"); }
  else if (colorStep == COLOR_WHITE) { allPixels(0, 0, 0, 8); Serial.println("SETUP_LED_TEST,WHITE"); }
  else if (colorStep == COLOR_OFF) { allPixelsOff(); Serial.println("SETUP_LED_TEST,OFF"); }
}

void startColorTest() {
  colorStep = COLOR_RED;
  applyColorStep();
  colorDueMs = millis() + COLOR_TEST_STEP_MS;
}

bool serviceColorTest(unsigned long nowMs) {
  if (colorStep == COLOR_DONE) return false;
  if ((long)(nowMs - colorDueMs) < 0) return true;
  colorStep = (ColorTestStep)((int)colorStep + 1);
  if (colorStep == COLOR_DONE) { setStage(STAGE_LED_CONFIRM); return false; }
  applyColorStep();
  colorDueMs = nowMs + COLOR_TEST_STEP_MS;
  return true;
}

void applyFeedLights() {
  mh.clearMainStrip();
  mh.rightPokeLightOff();
  if (feedTestMode == FEED_TEST_LEFT_BLUE) mh.leftPokeLightOn(0, 0, 10, 0);
  else mh.leftPokeLightOff();
}

void printGpio13(const char* event, const char* lights) {
  Serial.print("GPIO13_TEST,event="); Serial.print(event);
  Serial.print(",mode="); Serial.print(feedModeName());
  Serial.print(",gpio13_control="); Serial.print(digitalRead(GPIO13_TEST_PIN));
  Serial.print(",feed_active="); Serial.print(mh.isFeedActive());
  Serial.print(",lights="); Serial.println(lights);
}

void startFeedTest(FeedTestMode mode) {
  if (feedTestMode != FEED_TEST_NONE || mh.isFeedActive() || mh.isFeedJammed()) return;
  feedTestMode = mode;
  feedTestPhase = FEED_PHASE_PRE_FEED;
  feedResult = FEED_RESULT_NONE;
  feedTestDueMs = millis() + TEST_OBSERVATION_MS;
  applyFeedLights();
  printGpio13("PRE_FEED", mode == FEED_TEST_LEFT_BLUE ? "LEFT_BLUE" : "ALL_OFF");
}

void finishFeedTest() {
  FeedTestMode completedMode = feedTestMode;
  FeedResult completedResult = feedResult;
  feedTestMode = FEED_TEST_NONE;
  feedTestPhase = FEED_PHASE_NONE;
  feedTestDueMs = 0;

  if (completedMode == FEED_TEST_ALL_OFF && stage == STAGE_FEED_DARK) {
    if (completedResult == FEED_RESULT_PELLET) {
      passCheck(checks.feedDarkFirmware, "FEED_DARK_FIRMWARE");
      setStage(STAGE_FEED_DARK_CONFIRM);
    } else {
      logFailure(completedResult == FEED_RESULT_JAMMED ? "UNEXPECTED_JAM_CLEAR_AND_RETRY" : "NO_PELLET");
      if (completedResult == FEED_RESULT_JAMMED) Serial.println("SETUP_RECOVERY,PHYSICALLY_CLEAR_FEEDER_THEN_SEND CLEAR_JAM");
      printPrompt();
    }
  } else if (completedMode == FEED_TEST_LEFT_BLUE && stage == STAGE_FEED_LIGHT) {
    if (completedResult == FEED_RESULT_PELLET) {
      passCheck(checks.feedLightFirmware, "FEED_LIGHT_FIRMWARE");
      setStage(STAGE_FEED_LIGHT_CONFIRM);
    } else {
      logFailure(completedResult == FEED_RESULT_JAMMED ? "UNEXPECTED_JAM_CLEAR_AND_RETRY" : "NO_PELLET");
      if (completedResult == FEED_RESULT_JAMMED) Serial.println("SETUP_RECOVERY,PHYSICALLY_CLEAR_FEEDER_THEN_SEND CLEAR_JAM");
      printPrompt();
    }
  } else if (completedMode == FEED_TEST_JAM && stage == STAGE_JAM) {
    if (completedResult == FEED_RESULT_JAMMED) {
      passCheck(checks.jamLatched, "JAM_LATCHED");
      if (mh.isFeedJammed()) {
        setStage(STAGE_JAM_CLEAR);
      } else {
        passCheck(checks.jamCleared, "JAM_CLEARED");
        setStage(STAGE_REVERSAL_CONFIRM);
      }
    } else { logFailure("PELLET_DETECTED_EXPECTED_JAM"); printPrompt(); }
  }
}

bool serviceFeedTest(unsigned long nowMs) {
  if (feedTestMode == FEED_TEST_NONE) return false;
  if (feedTestPhase == FEED_PHASE_PRE_FEED) {
    applyFeedLights();
    if ((long)(nowMs - feedTestDueMs) >= 0) {
      mh.feed();
      feedTestPhase = FEED_PHASE_FEEDING;
      printGpio13("FEED_STARTED", feedTestMode == FEED_TEST_LEFT_BLUE ? "LEFT_BLUE" : "ALL_OFF");
    }
    return true;
  }
  if (feedTestPhase == FEED_PHASE_FEEDING) {
    if (mh.isFeedActive()) { applyFeedLights(); return true; }
    if (mh.isFeedJammed()) feedResult = FEED_RESULT_JAMMED;
    else if (mh.isPelletAvailable()) feedResult = FEED_RESULT_PELLET;
    else feedResult = FEED_RESULT_STOPPED;
    allPixelsOff();
    feedTestPhase = FEED_PHASE_POST_FEED_OFF;
    feedTestDueMs = nowMs + TEST_OBSERVATION_MS;
    printGpio13("FEED_STOPPED_ALL_OFF", "ALL_OFF");
    return true;
  }
  if (feedTestPhase == FEED_PHASE_POST_FEED_OFF) {
    allPixelsOff();
    if ((long)(nowMs - feedTestDueMs) >= 0) {
      printGpio13("POST_FEED_ALL_OFF", "ALL_OFF");
      finishFeedTest();
      return false;
    }
    return true;
  }
  return true;
}

void serviceResponse() {
  if (pendingResponse == RESPONSE_NONE) return;
  OperatorResponse response = pendingResponse;
  pendingResponse = RESPONSE_NONE;
  if (response == RESPONSE_ABORT) { setStage(STAGE_ABORTED); return; }
  bool yes = response == RESPONSE_YES;
  if (response == RESPONSE_NO) logFailure("OPERATOR_REPORTED_FAILURE");

  switch (stage) {
    case STAGE_LED_CONFIRM:
      if (yes) { passCheck(checks.ledRgbw, "LED_RGBW"); setStage(STAGE_FEED_DARK); }
      else if (response == RESPONSE_NO || response == RESPONSE_RETRY) setStage(STAGE_LED_SWEEP);
      break;
    case STAGE_FEED_DARK_CONFIRM:
      if (yes) { passCheck(checks.feedDarkMotor, "FEED_DARK_MOTOR"); setStage(STAGE_FEED_DARK_CLEAR); }
      else if (response == RESPONSE_NO || response == RESPONSE_RETRY) setStage(STAGE_FEED_DARK);
      break;
    case STAGE_FEED_LIGHT_CONFIRM:
      if (yes) { passCheck(checks.feedLightMotor, "FEED_LIGHT_MOTOR"); setStage(STAGE_FEED_LIGHT_CLEAR); }
      else if (response == RESPONSE_NO || response == RESPONSE_RETRY) setStage(STAGE_FEED_LIGHT);
      break;
    case STAGE_REVERSAL_CONFIRM:
      if (yes) { passCheck(checks.reversal, "RETRY_DIRECTION_REVERSAL"); setStage(STAGE_BUZZER_CONFIRM); }
      else if (response == RESPONSE_NO || response == RESPONSE_RETRY) setStage(STAGE_JAM);
      break;
    case STAGE_BUZZER_CONFIRM:
      if (yes) { passCheck(checks.buzzer, "BUZZER"); cameraRunObserved = false; setStage(STAGE_CAMERA_CYCLE); }
      else if (response == RESPONSE_NO || response == RESPONSE_RETRY) setStage(STAGE_BUZZER_CONFIRM);
      break;
    case STAGE_CAMERA_CONFIRM:
      if (yes) { passCheck(checks.cameraTtl, "CAMERA_TTL_PHYSICAL"); setStage(STAGE_HOUSE_LIGHT_CONFIRM); }
      else if (response == RESPONSE_NO || response == RESPONSE_RETRY) {
        checks.cameraCycle = false; cameraRunObserved = false; setStage(STAGE_CAMERA_CYCLE);
      }
      break;
    case STAGE_HOUSE_LIGHT_CONFIRM:
      if (yes) { passCheck(checks.houseLight, "HOUSE_LIGHT"); setStage(STAGE_COMPLETE); }
      else printPrompt();
      break;
    default:
      Serial.println("NACK,TEST,YES_NO_NOT_EXPECTED_AT_THIS_STAGE");
      printPrompt();
      break;
  }
}

void processTransitions() {
  bool pelletBlocked = mh.pelletSensorBlocked();
  if (pelletBlocked != lastPelletBlocked) {
    lastPelletBlocked = pelletBlocked;
    Serial.print("DEBUG_EVENT,"); Serial.println(pelletBlocked ? "PELLET_BEAM_BLOCKED" : "PELLET_BEAM_CLEARED");
    if (stage == STAGE_PELLET_BLOCK && pelletBlocked) {
      passCheck(checks.pelletBlocked, "PELLET_BLOCKED"); setStage(STAGE_PELLET_CLEAR);
    } else if (stage == STAGE_PELLET_CLEAR && !pelletBlocked) {
      passCheck(checks.pelletCleared, "PELLET_CLEARED"); setStage(STAGE_LEFT_DRINK);
    } else if (stage == STAGE_FEED_DARK_CLEAR && !pelletBlocked) {
      setStage(STAGE_FEED_LIGHT);
    } else if (stage == STAGE_FEED_LIGHT_CLEAR && !pelletBlocked) {
      setStage(STAGE_JAM);
    }
  }
  bool feedActive = mh.isFeedActive();
  if (feedActive != lastFeedActive) {
    lastFeedActive = feedActive;
    Serial.print("DEBUG_EVENT,"); Serial.println(feedActive ? "FEED_ACTIVE" : "FEED_IDLE");
  }
  bool feedJammed = mh.isFeedJammed();
  if (feedJammed != lastFeedJammed) {
    lastFeedJammed = feedJammed;
    Serial.print("DEBUG_EVENT,"); Serial.println(feedJammed ? "FEED_JAM_LATCHED" : "FEED_JAM_CLEARED");
    if (!feedJammed && stage == STAGE_JAM_CLEAR) {
      passCheck(checks.jamCleared, "JAM_CLEARED"); setStage(STAGE_REVERSAL_CONFIRM);
    }
  }
  bool running = mh.isRunning();
  if (running != lastRunning) {
    lastRunning = running;
    Serial.print("DEBUG_EVENT,"); Serial.println(running ? "SESSION_RUNNING" : "SESSION_STOPPED");
    if (stage == STAGE_CAMERA_CYCLE && running) cameraRunObserved = true;
    else if (stage == STAGE_CAMERA_CYCLE && !running && cameraRunObserved) {
      passCheck(checks.cameraCycle, "CAMERA_SESSION_CYCLE"); setStage(STAGE_CAMERA_CONFIRM);
    }
  }
}

void processEvents() {
  bool leftPokeEnded = mh.leftPokeEnded();
  bool rightPokeEnded = mh.rightPokeEnded();
  if (leftPokeEnded) {
    Serial.println("DEBUG_EVENT,LEFT_POKE_END");
    if (stage == STAGE_LEFT_POKE) { passCheck(checks.leftPoke, "LEFT_POKE"); setStage(STAGE_RIGHT_POKE); }
    else if (stage == STAGE_FEED_LIGHT) startFeedTest(FEED_TEST_LEFT_BLUE);
  }
  if (rightPokeEnded) {
    Serial.println("DEBUG_EVENT,RIGHT_POKE_END");
    if (stage == STAGE_RIGHT_POKE) { passCheck(checks.rightPoke, "RIGHT_POKE"); setStage(STAGE_PELLET_BLOCK); }
    else if (stage == STAGE_FEED_DARK) startFeedTest(FEED_TEST_ALL_OFF);
    else if (stage == STAGE_JAM) startFeedTest(FEED_TEST_JAM);
  }
  if (mh.leftDrinkEnded()) {
    Serial.println("DEBUG_EVENT,LEFT_DRINK_END");
    if (stage == STAGE_LEFT_DRINK) { passCheck(checks.leftDrink, "LEFT_DRINK"); setStage(STAGE_RIGHT_DRINK); }
  }
  if (mh.rightDrinkEnded()) {
    Serial.println("DEBUG_EVENT,RIGHT_DRINK_END");
    if (stage == STAGE_RIGHT_DRINK) { passCheck(checks.rightDrink, "RIGHT_DRINK"); setStage(STAGE_LED_SWEEP); }
  }
  if (mh.pelletRetrieved()) Serial.println("DEBUG_EVENT,PELLET_RETRIEVED");
}

void printStatus() {
  Serial.print("DEBUG_STATUS,stage="); Serial.print(stageName());
  Serial.print(",left_poke="); Serial.print(mh.leftPokeActive());
  Serial.print(",right_poke="); Serial.print(mh.rightPokeActive());
  Serial.print(",left_drink="); Serial.print(mh.leftDrinkActive());
  Serial.print(",right_drink="); Serial.print(mh.rightDrinkActive());
  Serial.print(",pellet_sensor="); Serial.print(mh.pelletSensorBlocked());
  Serial.print(",feed_active="); Serial.print(mh.isFeedActive());
  Serial.print(",feed_jammed="); Serial.print(mh.isFeedJammed());
  Serial.print(",gpio13_control="); Serial.print(digitalRead(GPIO13_TEST_PIN));
  Serial.print(",feed_test_mode="); Serial.print(feedModeName());
  Serial.print(",feed_test_phase="); Serial.println(feedPhaseName());
}

void setup() {
  mh.setSerialCommandHandler(handleSetupCommand);
  mh.begin();
  mh.setPelletSensorMode(PELLET_SENSOR_MODE);
  mh.setSensorPollingWhileStopped(true);
  lastPelletBlocked = mh.pelletSensorBlocked();
  lastFeedActive = mh.isFeedActive();
  lastFeedJammed = mh.isFeedJammed();
  lastRunning = mh.isRunning();
  checks.rtc = false;
  Serial.println("SETUP_DEBUG_READY,mode=GUIDED_QUALIFICATION");
  Serial.print("SETUP_PROTOCOL,version=");
  Serial.print(QUALIFICATION_PROTOCOL_VERSION);
  Serial.println(",mode=GUIDED_QUALIFICATION");
  Serial.println("SETUP_COMMANDS,TEST,DEVICE,<id>|TEST,RTC_VERIFIED|TEST,YES|TEST,NO|TEST,RETRY|TEST,STATUS|TEST,ABORT|TEST,RESTART");
  Serial.println("SETUP_STORAGE,serial=ACTIVE,sd=TODO");
  Serial.println("SETUP_SAFETY,MOTOR_STARTS_ONLY_AFTER_PROMPTED_POKE");
  setStage(STAGE_DEVICE_ID);
}

void loop() {
  mh.update();
  unsigned long nowMs = millis();
  if (stage == STAGE_FEED_DARK_CLEAR && !mh.pelletSensorBlocked()) {
    setStage(STAGE_FEED_LIGHT);
  } else if (stage == STAGE_FEED_LIGHT_CLEAR && !mh.pelletSensorBlocked()) {
    setStage(STAGE_JAM);
  }
  processTransitions();
  processEvents();
  serviceResponse();
  bool colorActive = serviceColorTest(nowMs);
  bool feedActive = serviceFeedTest(nowMs);
  if (!colorActive && !feedActive) allPixelsOff();
  printChecklist(nowMs);
}
