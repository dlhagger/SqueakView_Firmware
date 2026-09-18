#include "MouseHouse.h"
#include "camera_ttl.pio.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hardware/clocks.h>
#include <hardware/irq.h>
#include <hardware/sync.h>
#include <pico/rand.h>

#ifndef MPR121_TOUCHTH_0
#define MPR121_TOUCHTH_0 0x41
#define MPR121_RELEASETH_0 0x42
#define MPR121_DEBOUNCE 0x5B
#define MPR121_CONFIG1 0x5C
#define MPR121_CONFIG2 0x5D
#define MPR121_ECR 0x5E
#endif

extern "C" uint64_t time_us_64();

MouseHouse* MouseHouse::cameraPioOwner_ = nullptr;

MouseHouse::MouseHouse()
    : cap_(),
      strip_(kNumPixels, kNeoPixelPin, NEO_GRBW + NEO_KHZ800),
      stepper_(kMotorStepsPerRevolution, kAIN1, kAIN2, kBIN1, kBIN2) {}

uint64_t MouseHouse::getTimestampUs() const {
  uint64_t elapsedUs = time_us_64() - baseUs_;
  return baseUnixUs_ + elapsedUs;
}

bool MouseHouse::establishRtcAnchor() {
  DateTime previous = rtc_.now();
  if (previous.year() < 2020 || previous.year() > 2099) return false;

  uint64_t deadlineUs = time_us_64() + 1500000ULL;
  while (time_us_64() < deadlineUs) {
    uint64_t beforeUs = time_us_64();
    DateTime sample = rtc_.now();
    uint64_t afterUs = time_us_64();
    if (sample.unixtime() != previous.unixtime()) {
      baseUs_ = beforeUs + ((afterUs - beforeUs) / 2ULL);
      baseUnixUs_ = (uint64_t)sample.unixtime() * 1000000ULL;
      rtcAnchorUncertaintyUs_ = ((afterUs - beforeUs) / 2ULL) + 1ULL;
      return true;
    }
    delay(1);
  }
  return false;
}

void MouseHouse::logClockStatus(const char* reason) {
  queueText(MouseHouseProtocolV2::MESSAGE_DIAGNOSTIC,
            SerialTransport::PRIORITY_DIAGNOSTIC,
            "CLOCK_STATUS,%s,%llu,%llu,%llu,%s",
            rtcValid_ ? "VALID" : "INVALID",
            (unsigned long long)baseUnixUs_,
            (unsigned long long)baseUs_,
            (unsigned long long)rtcAnchorUncertaintyUs_,
            reason ? reason : kNanString);
}

void MouseHouse::setHouseLightSchedule(uint8_t onHour, uint8_t onMinute,
                                       uint8_t offHour, uint8_t offMinute,
                                       HouseLightTimeBasis timeBasis) {
  if (onHour >= 24 || offHour >= 24 || onMinute >= 60 || offMinute >= 60) {
    return;
  }

  houseLightOnMinuteOfDay_ = ((uint16_t)onHour * 60U) + onMinute;
  houseLightOffMinuteOfDay_ = ((uint16_t)offHour * 60U) + offMinute;
  houseLightTimeBasis_ = timeBasis;
  // Force the next update() call to apply the newly selected schedule.
  lastHouseLightCheck_ = millis() - kHouseLightCheckIntervalMs;
}

uint32_t MouseHouse::currentDayNumber(HouseLightTimeBasis timeBasis) const {
  uint32_t utcSeconds = latestRtcUnixSeconds_;
  if (utcSeconds == 0) {
    utcSeconds = (uint32_t)(getTimestampUs() / 1000000ULL);
  }
  return timeForBasis(DateTime(utcSeconds), timeBasis).unixtime() / 86400UL;
}

void MouseHouse::robustShow() {
  strip_.begin();
  strip_.show();
}

void MouseHouse::logEvent(const char* eventType,
                          uint64_t unixTime,
                          uint64_t rp2040Time,
                          const char* side,
                          unsigned long count,
                          uint64_t duration,
                          uint64_t latency,
                          long value,
                          const char* context,
                          const char* reason) {
  SerialTransport::EventFields event{};
  strncpy(event.eventType, eventType ? eventType : kNanString,
          sizeof(event.eventType) - 1U);
  event.unixTime = unixTime;
  event.rp2040Time = rp2040Time;
  strncpy(event.side, side ? side : kNanString, sizeof(event.side) - 1U);
  event.count = count;
  event.duration = duration;
  event.latency = latency;
  event.value = value;
  strncpy(event.context, context ? context : kNanString,
          sizeof(event.context) - 1U);
  strncpy(event.reason, reason ? reason : kNanString,
          sizeof(event.reason) - 1U);

  SerialTransport::Priority priority = SerialTransport::PRIORITY_EVENT;
  if (strcmp(event.eventType, "FEED_JAM") == 0
      || strcmp(event.eventType, "ACK_STOP") == 0) {
    priority = SerialTransport::PRIORITY_SAFETY;
  }
  uint32_t recordSession = (running_ || strcmp(event.eventType, "ACK_STOP") == 0)
                               ? sessionId_ : 0U;
  if (!transport_.enqueueEvent(event, priority, recordSession)) {
    noteRequiredRecordFailure(event.eventType, rp2040Time);
  }
}

bool MouseHouse::emitLegacyLine(const char* line) {
  if (transport_.enqueueText(line, MouseHouseProtocolV2::MESSAGE_EVENT,
                             SerialTransport::PRIORITY_EVENT,
                             running_ ? sessionId_ : 0U,
                             time_us_64())) {
    return true;
  }
  noteRequiredRecordFailure("TASK_EVENT", time_us_64());
  return false;
}

bool MouseHouse::queueText(uint8_t messageType,
                           SerialTransport::Priority priority,
                           const char* format, ...) {
  char line[SerialTransport::kLegacyLineSize];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (length < 0 || (size_t)length >= sizeof(line)) return false;
  return transport_.enqueueText(line, messageType, priority,
                                running_ ? sessionId_ : 0U,
                                time_us_64());
}

void MouseHouse::noteRequiredRecordFailure(const char* type,
                                            uint64_t timestampUs) {
  transport_.latchOverflow(type, timestampUs);
  integrityFailSafePending_ = true;
}

const char* MouseHouse::getContext() const {
  if (taskContext_[0] != '\0') {
    return taskContext_;
  }
  if (timeoutActive_) {
    return "Timeout";
  }
  if (pelletAvailable_) {
    return "Pellet_Available";
  }
  if (feedActive_) {
    return "Feeding";
  }
  return "Eligible";
}

void MouseHouse::setCompatibilitySerialMode(bool enabled) {
  compatibilitySerialMode_ = enabled;
}

void MouseHouse::setSerialCommandHandler(SerialCommandHandler handler) {
  serialCommandHandler_ = handler;
}

void MouseHouse::setTaskContext(const char* context) {
  if (context == nullptr) {
    taskContext_[0] = '\0';
    return;
  }

  strncpy(taskContext_, context, sizeof(taskContext_) - 1);
  taskContext_[sizeof(taskContext_) - 1] = '\0';
}

void MouseHouse::resetSessionEventCounts() {
  leftPokeEvent_.count = 0;
  rightPokeEvent_.count = 0;
  leftDrinkEvent_.count = 0;
  rightDrinkEvent_.count = 0;
  pelletDeliveryCount_ = 0;
  pelletRetrievalCount_ = 0;
  timeoutCount_ = 0;
}

void MouseHouse::resetSerialCommandBuffer() {
  serialCmdLength_ = 0;
  serialCmdOverflow_ = false;
  serialCmdBuffer_[0] = '\0';
}

void MouseHouse::sendCommandError(const char* command, const char* reason) {
  if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                 SerialTransport::PRIORITY_SAFETY, "NACK,%s,%s",
                 command ? command : "UNKNOWN",
                 reason ? reason : "INVALID")) {
    noteRequiredRecordFailure("COMMAND_RESULT", time_us_64());
  }
}

bool MouseHouse::parseLongStrict(const char* text,
                                 long minimum,
                                 long maximum,
                                 long& value) const {
  if (text == nullptr || text[0] == '\0') return false;
  errno = 0;
  char* end = nullptr;
  long parsed = strtol(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0') return false;
  if (parsed < minimum || parsed > maximum) return false;
  value = parsed;
  return true;
}

bool MouseHouse::parseUint64Strict(const char* text,
                                   uint64_t minimum,
                                   uint64_t maximum,
                                   uint64_t& value) const {
  if (text == nullptr || text[0] == '\0' || text[0] == '-') return false;
  errno = 0;
  char* end = nullptr;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (errno == ERANGE || end == text || *end != '\0') return false;
  uint64_t converted = (uint64_t)parsed;
  if (converted < minimum || converted > maximum) return false;
  value = converted;
  return true;
}

void MouseHouse::handleTimeSyncCommand(const char* arguments, uint64_t receivedUs) {
  char copy[kSerialCmdBufferSize];
  strncpy(copy, arguments ? arguments : "", sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';

  char* separator = strchr(copy, ',');
  if (separator == nullptr) {
    sendCommandError("TIME_SYNC", "EXPECTED_SEQUENCE_AND_JETSON_NS");
    return;
  }
  *separator = '\0';

  uint64_t sequence = 0;
  uint64_t jetsonSendNs = 0;
  if (!parseUint64Strict(copy, 0, UINT64_MAX, sequence)
      || !parseUint64Strict(separator + 1, 0, UINT64_MAX, jetsonSendNs)) {
    sendCommandError("TIME_SYNC", "INVALID_ARGUMENT");
    return;
  }

  uint64_t transmitUs = time_us_64();
  uint64_t controllerUnixUs = baseUnixUs_ + (transmitUs - baseUs_);
  if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                 SerialTransport::PRIORITY_SAFETY,
                 "CLOCK_SYNC,%llu,%llu,%llu,%llu,%llu,%s",
                 (unsigned long long)sequence,
                 (unsigned long long)jetsonSendNs,
                 (unsigned long long)receivedUs,
                 (unsigned long long)transmitUs,
                 (unsigned long long)controllerUnixUs,
                 rtcValid_ ? "RTC_VALID" : "RTC_INVALID")) {
    noteRequiredRecordFailure("CLOCK_SYNC", transmitUs);
  }
}

void MouseHouse::handleSetRtcCommand(const char* arguments) {
  if (running_ || feedActive_) {
    sendCommandError("SET_RTC", "DEVICE_BUSY");
    return;
  }

  uint64_t epochSeconds = 0;
  if (!parseUint64Strict(arguments, 1577836800ULL, 4102444799ULL, epochSeconds)) {
    sendCommandError("SET_RTC", "INVALID_UNIX_SECONDS");
    return;
  }

  rtc_.adjust(DateTime((uint32_t)epochSeconds));
  rtcValid_ = establishRtcAnchor();
  if (!rtcValid_) {
    sendCommandError("SET_RTC", "ANCHOR_FAILED");
    return;
  }

  if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                 SerialTransport::PRIORITY_SAFETY,
                 "ACK_SET_RTC,%llu,%llu,%llu",
                 (unsigned long long)epochSeconds,
                 (unsigned long long)baseUs_,
                 (unsigned long long)rtcAnchorUncertaintyUs_)) {
    noteRequiredRecordFailure("ACK_SET_RTC", time_us_64());
  }
  logClockStatus("JetsonSet");
}

void MouseHouse::queueTransportStatus() {
  SerialTransport::Diagnostics d = transport_.diagnostics();
  if (!queueText(MouseHouseProtocolV2::MESSAGE_TRANSPORT_STATUS,
                 SerialTransport::PRIORITY_SAFETY,
                 "TRANSPORT_STATUS,b=%llu,m=%u,n=%u,q=%u/%u,h=%u,o=%llu,a=%llu,"
                 "t=%llu,k=%llu,x=%u,l=%s,u=%llu,c=%lu,p=%lu,z=%lu,r=%lu,"
                 "v=%lu,f=%lu,d=%lu",
                 (unsigned long long)transport_.bootId(),
                 (unsigned)transport_.mode(), d.connected ? 1U : 0U,
                 d.queueUsed, d.queueCapacity,
                 d.queueHighWater, (unsigned long long)d.oldestSequence,
                 (unsigned long long)d.highestAssignedSequence,
                 (unsigned long long)d.highestTransmittedSequence,
                 (unsigned long long)d.highestAcknowledgedSequence,
                 d.overflowLatched ? 1U : 0U, d.firstLostType,
                 (unsigned long long)d.firstLostTimestampUs,
                 (unsigned long)d.suppressedCheckpoints,
                 (unsigned long)d.partialWrites,
                 (unsigned long)d.zeroCapacityServices,
                 (unsigned long)d.retransmissions,
                 (unsigned long)d.commandBufferOverflows,
                 (unsigned long)d.malformedCommands,
                 (unsigned long)d.maxServiceDurationUs)) {
    noteRequiredRecordFailure("TRANSPORT_STATUS", time_us_64());
  }
}

void MouseHouse::handleSerialCommand(const char* cmd) {
  uint64_t receivedUs = time_us_64();
  if (strcmp(cmd, "PROTO,2") == 0) {
    if (running_ || feedActive_) {
      sendCommandError("PROTO", "DEVICE_BUSY");
    } else if (transport_.mode() == SerialTransport::PROTOCOL_V2_ACTIVE) {
      if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                     SerialTransport::PRIORITY_SAFETY, "ACK_PROTO,2")) {
        noteRequiredRecordFailure("ACK_PROTO", receivedUs);
      }
    } else if (!transport_.requestProtocolV2()) {
      noteRequiredRecordFailure("ACK_PROTO", receivedUs);
    }
  } else if (strncmp(cmd, "PROTO,", 6) == 0) {
    sendCommandError("PROTO", "UNSUPPORTED_VERSION");
  } else if (strncmp(cmd, "ACK_EVENTS,", 11) == 0) {
    char copy[kSerialCmdBufferSize];
    strncpy(copy, cmd + 11, sizeof(copy) - 1U);
    copy[sizeof(copy) - 1U] = '\0';
    char* separator = strchr(copy, ',');
    uint64_t boot = 0;
    uint64_t sequence = 0;
    if (separator == nullptr) {
      transport_.noteMalformedCommand();
      sendCommandError("ACK_EVENTS", "EXPECTED_BOOT_AND_SEQUENCE");
    } else {
      *separator = '\0';
      if (!parseUint64Strict(copy, 0, UINT64_MAX, boot)
          || !parseUint64Strict(separator + 1, 0, UINT64_MAX, sequence)) {
        transport_.noteMalformedCommand();
        sendCommandError("ACK_EVENTS", "INVALID_ARGUMENT");
      } else if (!transport_.acknowledge(boot, sequence)) {
        sendCommandError("ACK_EVENTS", "WRONG_BOOT_OR_NOT_EMITTED");
      }
    }
  } else if (strncmp(cmd, "RESEND_EVENTS,", 14) == 0) {
    char copy[kSerialCmdBufferSize];
    strncpy(copy, cmd + 14, sizeof(copy) - 1U);
    copy[sizeof(copy) - 1U] = '\0';
    char* separator = strchr(copy, ',');
    uint64_t boot = 0;
    uint64_t sequence = 0;
    if (separator == nullptr) {
      transport_.noteMalformedCommand();
      sendCommandError("RESEND_EVENTS", "EXPECTED_BOOT_AND_SEQUENCE");
    } else {
      *separator = '\0';
      if (!parseUint64Strict(copy, 0, UINT64_MAX, boot)
          || !parseUint64Strict(separator + 1, 1, UINT64_MAX, sequence)) {
        transport_.noteMalformedCommand();
        sendCommandError("RESEND_EVENTS", "INVALID_ARGUMENT");
      } else {
        SerialTransport::ResendResult result = transport_.requestResend(boot, sequence);
        if (result == SerialTransport::RESEND_ACCEPTED) {
          if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                         SerialTransport::PRIORITY_SAFETY,
                         "ACK_RESEND_EVENTS,%llu,%llu",
                         (unsigned long long)boot,
                         (unsigned long long)sequence)) {
            noteRequiredRecordFailure("ACK_RESEND_EVENTS", receivedUs);
          }
        } else if (result == SerialTransport::RESEND_WRONG_BOOT) {
          sendCommandError("RESEND_EVENTS", "WRONG_BOOT");
        } else if (result == SerialTransport::RESEND_NOT_EMITTED) {
          sendCommandError("RESEND_EVENTS", "NOT_EMITTED");
        } else {
          if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                         SerialTransport::PRIORITY_SAFETY,
                         "NACK,RESEND_EVENTS,TOO_OLD,oldest=%llu",
                         (unsigned long long)transport_.oldestRetainedSequence())) {
            noteRequiredRecordFailure("NACK_RESEND_EVENTS", receivedUs);
          }
        }
      }
    }
  } else if (strcmp(cmd, "TRANSPORT_STATUS") == 0) {
    queueTransportStatus();
  } else if (strncmp(cmd, "START,", 6) == 0) {
    long fps = 0;
    if (!parseLongStrict(cmd + 6, 1, 120, fps)) {
      sendCommandError("START", "INVALID_FPS");
    } else if (transport_.overflowLatched()) {
      sendCommandError("START", "INTEGRITY_LATCHED");
    } else if (!rtcValid_) {
      sendCommandError("START", "RTC_INVALID");
    } else if (!cameraPioReady_) {
      sendCommandError("START", "CAMERA_PIO_UNAVAILABLE");
    } else if (running_) {
      sendCommandError("START", "ALREADY_RUNNING");
    } else {
      startSession((uint32_t)fps);
    }
  } else if (strcmp(cmd, "STOP") == 0) {
    stopSession();
  } else if (strncmp(cmd, "FEED,", 5) == 0) {
    long steps = 0;
    if (!parseLongStrict(cmd + 5, 1, kMaxFeedCommandSteps, steps)) {
      sendCommandError("FEED", "INVALID_STEPS");
    } else if (transport_.overflowLatched()) {
      sendCommandError("FEED", "INTEGRITY_LATCHED");
    } else if (feedJammed_) {
      sendCommandError("FEED", "JAMMED");
    } else if (feedActive_) {
      sendCommandError("FEED", "ALREADY_ACTIVE");
    } else {
      feed((int)steps);
      if (!compatibilitySerialMode_) {
        if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                       SerialTransport::PRIORITY_SAFETY, "ACK_FEED")) {
          noteRequiredRecordFailure("ACK_FEED", receivedUs);
        }
      }
    }
  } else if (strcmp(cmd, "CLEAR_JAM") == 0) {
    if (feedActive_) {
      sendCommandError("CLEAR_JAM", "FEED_ACTIVE");
    } else if (!feedJammed_) {
      sendCommandError("CLEAR_JAM", "NOT_JAMMED");
    } else {
      clearFeedJam();
      if (!queueText(MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                     SerialTransport::PRIORITY_SAFETY, "ACK_CLEAR_JAM")) {
        noteRequiredRecordFailure("ACK_CLEAR_JAM", receivedUs);
      }
    }
  } else if (strncmp(cmd, "TIME_SYNC,", 10) == 0) {
    if (running_ || feedActive_) {
      sendCommandError("TIME_SYNC", "DEVICE_BUSY");
    } else {
      handleTimeSyncCommand(cmd + 10, receivedUs);
    }
  } else if (strncmp(cmd, "SET_RTC,", 8) == 0) {
    handleSetRtcCommand(cmd + 8);
  } else if (serialCommandHandler_ != nullptr && serialCommandHandler_(cmd)) {
    // The active sketch accepted an application-specific command.
  } else {
    sendCommandError("UNKNOWN", "UNKNOWN_COMMAND");
  }
}

void MouseHouse::checkSerialCommands() {
  uint8_t bytesRead = 0;
  uint8_t commandsDispatched = 0;
  while (Serial.available() > 0
         && bytesRead < SerialTransport::kInputByteBudget
         && commandsDispatched < SerialTransport::kCommandDispatchBudget) {
    char ch = (char)Serial.read();
    bytesRead++;

    if (ch == '\r') continue;

    if (ch == '\n') {
      if (serialCmdOverflow_) {
        transport_.noteCommandBufferOverflow();
        sendCommandError("UNKNOWN", "COMMAND_TOO_LONG");
      } else if (serialCmdLength_ > 0) {
        serialCmdBuffer_[serialCmdLength_] = '\0';
        handleSerialCommand(serialCmdBuffer_);
        commandsDispatched++;
      }
      resetSerialCommandBuffer();
      continue;
    }

    if (serialCmdOverflow_) continue;

    if (serialCmdLength_ < (kSerialCmdBufferSize - 1)) {
      serialCmdBuffer_[serialCmdLength_++] = ch;
    } else {
      serialCmdOverflow_ = true;
    }
  }
}

void MouseHouse::setDebounce(uint8_t dt, uint8_t dr) {
  dt = dt & 0x07;
  dr = dr & 0x07;
  uint8_t savedEcr = cap_.readRegister8(MPR121_ECR);
  cap_.writeRegister(MPR121_ECR, 0x00);
  cap_.writeRegister(MPR121_DEBOUNCE, (dr << 4) | dt);
  cap_.writeRegister(MPR121_ECR, savedEcr);
}

void MouseHouse::dumpRegs() {
  uint8_t db = cap_.readRegister8(MPR121_DEBOUNCE);
  queueText(MouseHouseProtocolV2::MESSAGE_DIAGNOSTIC,
            SerialTransport::PRIORITY_DIAGNOSTIC,
            "Debounce DT=%u DR=%u", (unsigned)(db & 0x07),
            (unsigned)((db >> 4) & 0x07));
  queueText(MouseHouseProtocolV2::MESSAGE_DIAGNOSTIC,
            SerialTransport::PRIORITY_DIAGNOSTIC,
            "TTH/RTH electrode 1 = %u/%u",
            (unsigned)cap_.readRegister8(MPR121_TOUCHTH_0 + 2),
            (unsigned)cap_.readRegister8(MPR121_RELEASETH_0 + 2));
}

void MouseHouse::configureMPR121() {
  cap_.setAutoconfig(true);
  cap_.setThresholds(2, 0);
  setDebounce(0, 0);
  dumpRegs();
}

void MouseHouse::configureMPR121Silent() {
  cap_.setAutoconfig(true);
  cap_.setThresholds(2, 0);
  setDebounce(0, 0);
}

void MouseHouse::resetMPR121() {
  emitLegacyLine("MPR121_RESET_BEGIN");
  cap_.writeRegister(0x80, 0x63);
  delay(1);
  delay(2);

  if (!cap_.begin(0x5A)) {
    emitLegacyLine("MPR121_RESET_FAIL: begin()");
    return;
  }

  configureMPR121();
  dumpRegs();
  emitLegacyLine("MPR121_RESET_OK");
}

bool MouseHouse::mpr121Faulted() {
  uint8_t ecr = cap_.readRegister8(MPR121_ECR);
  if (ecr == 0x00) return true;

  uint8_t oorL = cap_.readRegister8(0x02);
  uint8_t oorH = cap_.readRegister8(0x03);
  return (oorL | oorH) != 0;
}

void MouseHouse::checkMPR121Health() {
  unsigned long nowMs = millis();
  if (nowMs - lastMpr121Check_ >= kMpr121CheckIntervalMs) {
    lastMpr121Check_ = nowMs;

    if (mpr121Faulted()) {
      emitLegacyLine("MPR121_FAULT_DETECTED");
      resetMPR121();
    }
  }
}

void MouseHouse::updateTimedBinaryEvent(TimedBinaryEvent& event,
                                        bool currentState,
                                        const char* startEventType,
                                        const char* endEventType,
                                        const char* side,
                                        uint64_t nowUnix,
                                        uint64_t nowUs) {
  if (currentState && !event.previousState) {
    event.startTime = nowUnix;
    event.count++;

    logEvent(startEventType,
             nowUnix, nowUs,
             side,
             event.count,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             getContext(),
             kNanString);
  }

  if (!currentState && event.previousState) {
    event.endTime = nowUnix;
    uint64_t duration = event.endTime - event.startTime;
    if (event.count > 0 && duration > 0) {
      event.justEnded = true;

      logEvent(endEventType,
               event.endTime, nowUs,
               side,
               event.count,
               duration,
               kNotApplicable,
               kNotApplicable,
               getContext(),
               kNanString);
    }
  }

  event.previousState = currentState;
}

void MouseHouse::primeTimedBinaryEvent(TimedBinaryEvent& event,
                                       bool currentState,
                                       uint64_t nowUnix) {
  event.previousState = currentState;
  event.startTime = currentState ? nowUnix : 0;
  event.endTime = 0;
  event.justEnded = false;
}

void MouseHouse::syncBehavioralStateToSensors(uint64_t nowUnix) {
  uint16_t touched = cap_.touched();
  bool pelletNow = feedPelletSensorTriggered();

  primeTimedBinaryEvent(leftPokeEvent_, digitalRead(kLeftPokePin) == LOW, nowUnix);
  primeTimedBinaryEvent(rightPokeEvent_, digitalRead(kRightPokePin) == LOW, nowUnix);
  primeTimedBinaryEvent(leftDrinkEvent_, (touched & (1 << 1)) != 0, nowUnix);
  primeTimedBinaryEvent(rightDrinkEvent_, (touched & (1 << 2)) != 0, nowUnix);

  prevPelletState_ = pelletNow;
  pelletJustRetrieved_ = false;
  pelletRetrievalTime_ = 0;
  pelletRetrievalLatency_ = 0;
  pelletArrivalTime_ = 0;
  wellCheckActive_ = false;
  wellCheckStartTime_ = 0;
  wellCheckEndTime_ = 0;
  ignoreNextPelletSensorClear_ = false;

  if (pelletSensorMode_ == PELLET_SENSOR_LATCHED_PRESENCE) {
    pelletAvailable_ = pelletNow;
  } else {
    pelletAvailable_ = false;
  }
}

void MouseHouse::updateLeftPoke() {
  bool currentState = (digitalRead(kLeftPokePin) == LOW);
  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();

  updateTimedBinaryEvent(leftPokeEvent_, currentState,
                         "POKE_START", "POKE_END", "L",
                         nowUnix, nowUs);
}

void MouseHouse::updateRightPoke() {
  bool currentState = (digitalRead(kRightPokePin) == LOW);
  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();

  updateTimedBinaryEvent(rightPokeEvent_, currentState,
                         "POKE_START", "POKE_END", "R",
                         nowUnix, nowUs);
}

void MouseHouse::updatePelletWell() {
  bool pelletNow = (digitalRead(kPelletSensorPin) == LOW);
  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();

  if (feedActive_) {
    // While the feeder is running, the pellet beam belongs to feed
    // confirmation only. Do not generate well checks during this window.
    wellCheckActive_ = false;
    prevPelletState_ = pelletNow;
    return;
  }

  if (pelletSensorMode_ == PELLET_SENSOR_TRANSIENT_DELIVERY) {
    if (!pelletNow && prevPelletState_ && ignoreNextPelletSensorClear_) {
      ignoreNextPelletSensorClear_ = false;
    }

    wellCheckActive_ = false;
    prevPelletState_ = pelletNow;
    return;
  }

  if (pelletNow && !prevPelletState_) {
    if (!pelletAvailable_) {
      wellCheckStartTime_ = nowUnix;
      wellCheckCount_++;
      wellCheckActive_ = true;

      logEvent("WELL_CHECK_START",
               nowUnix, nowUs,
               kNanString,
               wellCheckCount_,
               kNotApplicable, kNotApplicable, kNotApplicable,
               getContext(),
               kNanString);
    }
  }

  if (!pelletNow && prevPelletState_) {
    if (wellCheckActive_) {
      wellCheckEndTime_ = nowUnix;
      uint64_t duration = wellCheckEndTime_ - wellCheckStartTime_;
      wellCheckActive_ = false;

      logEvent("WELL_CHECK_END",
               wellCheckEndTime_, nowUs,
               kNanString,
               wellCheckCount_,
               duration,
               kNotApplicable,
               kNotApplicable,
               getContext(),
               kNanString);
    } else if (pelletAvailable_) {
      recordPelletRetrieved(nowUnix, nowUs, kNanString);
    }
  }

  // In latched-presence mode, the beam staying clear means the pellet is gone.
  // Recover here as well so a missed falling edge cannot leave pelletAvailable_
  // stuck true indefinitely.
  if (pelletAvailable_ && !pelletNow) {
    recordPelletRetrieved(nowUnix, nowUs, "Sensor clear");
  }

  prevPelletState_ = pelletNow;
}

void MouseHouse::updateLeftDrink() {
  bool currentState = cap_.touched() & (1 << 1);
  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();

  updateTimedBinaryEvent(leftDrinkEvent_, currentState,
                         "DRINK_START", "DRINK_END", "LD",
                         nowUnix, nowUs);
}

void MouseHouse::updateRightDrink() {
  bool currentState = cap_.touched() & (1 << 2);
  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();

  updateTimedBinaryEvent(rightDrinkEvent_, currentState,
                         "DRINK_START", "DRINK_END", "RD",
                         nowUnix, nowUs);
}

void MouseHouse::pollBehavioralSensors() {
  updateLeftPoke();
  updateRightPoke();
  updatePelletWell();
  updateLeftDrink();
  updateRightDrink();
}

void MouseHouse::disableFeederOutputs() {
  digitalWrite(kAIN1, LOW);
  digitalWrite(kAIN2, LOW);
  digitalWrite(kBIN1, LOW);
  digitalWrite(kBIN2, LOW);
}

bool MouseHouse::feedPelletSensorTriggered() const {
  return digitalRead(kPelletSensorPin) == LOW;
}

void MouseHouse::logFeedPelletArrival(uint64_t nowUs, const char* reason) {
  logEvent("PELLET_ARRIVAL",
           getTimestampUs(), nowUs,
           kNanString,
           pelletDeliveryCount_,
           kNotApplicable, kNotApplicable, kNotApplicable,
           getContext(),
           reason);
}

void MouseHouse::recordPelletRetrieved(uint64_t nowUnix,
                                       uint64_t nowUs,
                                       const char* reason) {
  if (!pelletAvailable_) {
    return;
  }

  pelletRetrievalTime_ = nowUnix;
  pelletRetrievalCount_++;

  uint64_t duration = kNotApplicable;
  uint64_t latency = kNotApplicable;
  if (pelletArrivalTime_ > 0) {
    duration = pelletRetrievalTime_ - pelletArrivalTime_;
    latency = duration;
  }

  pelletRetrievalLatency_ = latency;
  pelletJustRetrieved_ = true;
  const char* context = getContext();
  pelletAvailable_ = false;
  ignoreNextPelletSensorClear_ = false;

  logEvent("PELLET_RETRIEVAL",
           pelletRetrievalTime_, nowUs,
           kNanString,
           pelletRetrievalCount_,
           duration,
           latency,
           kNotApplicable,
           context,
           reason);

  pelletArrivalTime_ = 0;
}
void MouseHouse::startFeedRun(int steps, uint64_t nowUs) {
  feedActive_ = true;
  feedRequestedSteps_ = steps;
  feedStepDirection_ = feedPreferredStepDirection_;
  feedStepDelayUs_ = kFeedStepDelayUs;
  feedStepsLeft_ = feedRequestedSteps_;
  feedCurrentStep_ = 0;
  feedNextStepTime_ = nowUs;
  feedRetryCount_ = 0;

  feedStartTime_ = nowUs;
  feedStartCount_++;

  uint64_t nowUnix = getTimestampUs();

  logEvent("FEED_START",
           nowUnix, nowUs,
           kNanString,
           feedStartCount_,
           kNotApplicable,
           kNotApplicable,
           steps,
           getContext(),
           kNanString);
  digitalWrite(kMotorEnablePin, HIGH);
}

void MouseHouse::queueFeedRetry(uint64_t nowUs) {
  feedRetryCount_++;
  feedPreferredStepDirection_ = -feedPreferredStepDirection_;
  feedStepDirection_ = feedPreferredStepDirection_;
  feedStepDelayUs_ = kFeedRetryStepDelayUs;
  feedStepsLeft_ = feedRequestedSteps_;
  feedCurrentStep_ = 0;
  feedNextStepTime_ = nowUs + feedStepDelayUs_;

  logEvent("FEED_RETRY",
           getTimestampUs(), nowUs,
           kNanString,
           feedRetryCount_,
           kNotApplicable,
           kNotApplicable,
           feedRequestedSteps_,
           getContext(),
           "Pellet not detected, flipping retry direction");
}

void MouseHouse::logFeedJam(uint64_t nowUs) {
  logEvent("FEED_JAM",
           getTimestampUs(), nowUs,
           kNanString,
           feedStopCount_ + 1,
           kNotApplicable,
           kNotApplicable,
           kNotApplicable,
           getContext(),
           "Pellet did not trigger sensor");
}

void MouseHouse::advanceFeedMotorStep(uint64_t nowUs) {
  stepper_.step(feedStepDirection_);

  feedCurrentStep_++;
  feedStepsLeft_--;
  feedNextStepTime_ = nowUs + feedStepDelayUs_;
}

void MouseHouse::handleFeedPelletArrival(uint64_t nowUs,
                                         const char* arrivalReason,
                                         const char* stopReason) {
  pelletArrivalTime_ = getTimestampUs();
  pelletDeliveryCount_++;
  pelletAvailable_ = (pelletSensorMode_ == PELLET_SENSOR_LATCHED_PRESENCE);
  ignoreNextPelletSensorClear_ = (pelletSensorMode_ == PELLET_SENSOR_TRANSIENT_DELIVERY);
  logFeedPelletArrival(nowUs, arrivalReason);
  feedStop(stopReason);
  feedRetryCount_ = 0;
}

void MouseHouse::handleFeedPassComplete(uint64_t nowUs) {
  if (feedPelletSensorTriggered()) {
    handleFeedPelletArrival(nowUs, "Pellet after full pass", "Feed complete");
  } else if (feedRetryCount_ < kMaxFeedRetries) {
    queueFeedRetry(nowUs);
  } else {
    feedJammed_ = true;
    logFeedJam(nowUs);
    feedStop("Feeder Jammed: User Check");
    feedRetryCount_ = 0;
  }
}

void MouseHouse::feedStop(const char* reason) {
  feedActive_ = false;
  disableFeederOutputs();
  digitalWrite(kMotorEnablePin, LOW);
  indicators_.refreshNeeded = indicators_.mainStrip.isOn
                              || indicators_.rightPoke.isOn
                              || indicators_.leftPoke.isOn;
  indicatorRefreshDueUs_ = 0;

  feedStopCount_++;

  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();
  uint64_t duration = (feedStartTime_ > 0) ? (nowUs - feedStartTime_) : kNotApplicable;

  logEvent("FEED_STOP",
           nowUnix, nowUs,
           kNanString,
           feedStopCount_,
           duration,
           kNotApplicable,
           kNotApplicable,
           getContext(),
           reason);

  robustShow();
}

void MouseHouse::feed(int steps) {
  uint64_t nowUs = time_us_64();

  if (steps > 0 && !feedActive_ && !feedJammed_
      && !transport_.overflowLatched()) {
    startFeedRun(steps, nowUs);
  }
}

void MouseHouse::serviceFeed() {
  uint64_t nowUs = time_us_64();

  if (!feedActive_) return;

  if (feedPelletSensorTriggered()) {
    handleFeedPelletArrival(time_us_64(), kNanString, "Pellet detected mid-feed");
    return;
  }

  if (nowUs < feedNextStepTime_) return;

  advanceFeedMotorStep(nowUs);

  if (feedStepsLeft_ <= 0) {
    handleFeedPassComplete(time_us_64());
  }
}

bool MouseHouse::anyIndicatorsOn() const {
  return indicators_.mainStrip.isOn
         || indicators_.rightPoke.isOn
         || indicators_.leftPoke.isOn;
}

void MouseHouse::renderIndicators() {
  uint32_t stripColor = indicators_.mainStrip.isOn ? indicators_.mainStrip.color : 0;
  uint32_t rightPokeColor = indicators_.rightPoke.isOn ? indicators_.rightPoke.color : 0;
  uint32_t leftPokeColor = indicators_.leftPoke.isOn ? indicators_.leftPoke.color : 0;

  for (int i = 0; i < kActivePixels; i++) {
    strip_.setPixelColor(i, stripColor);
  }

  strip_.setPixelColor(kRightPokeLedIndex, rightPokeColor);
  strip_.setPixelColor(kLeftPokeLedIndex, leftPokeColor);
}

void MouseHouse::scheduleIndicatorRefresh() {
  // GPIO 13 switches power to the pixels. Schedule the transmission after the
  // original FED3 hardware's 2 ms power-settling interval without blocking
  // feeder, sensor, serial, or camera services.
  if (!indicators_.refreshNeeded || indicatorRefreshDueUs_ == 0) {
    indicatorRefreshDueUs_ = time_us_64() + kNeoPixelPowerSettleUs;
  }
  indicators_.refreshNeeded = true;
}

void MouseHouse::serviceIndicatorRefresh() {
  if (!indicators_.refreshNeeded || indicatorRefreshDueUs_ == 0) return;
  if (time_us_64() < indicatorRefreshDueUs_) return;

  renderIndicators();
  robustShow();
  indicators_.refreshNeeded = false;
  indicatorRefreshDueUs_ = 0;
}

void MouseHouse::turnIndicatorOn(IndicatorChannel& channel,
                                 uint32_t colorVal,
                                 const char* eventType) {
  bool wasOn = channel.isOn;
  bool colorChanged = (!channel.isOn || channel.color != colorVal);

  digitalWrite(kMotorEnablePin, HIGH);

  channel.isOn = true;
  channel.color = colorVal;

  if (colorChanged || indicators_.refreshNeeded) {
    scheduleIndicatorRefresh();
  }

  bool suppressEvent = compatibilitySerialMode_
                       && (strcmp(eventType, "LEFT_POKE_LIGHT_ON") == 0
                           || strcmp(eventType, "RIGHT_POKE_LIGHT_ON") == 0);

  if ((!wasOn || colorChanged) && !suppressEvent) {
    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent(eventType,
             nowUnix, nowUs,
             kNanString,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             colorVal,
             getContext(),
             kNanString);
  }

  channel.lastColor = colorVal;
}

void MouseHouse::turnIndicatorOff(IndicatorChannel& channel,
                                  const char* eventType) {
  if (!channel.isOn) {
    if (!feedActive_ && !anyIndicatorsOn() && !indicators_.refreshNeeded) {
      digitalWrite(kMotorEnablePin, LOW);
    }
    return;
  }

  channel.isOn = false;
  channel.color = 0;
  renderIndicators();
  robustShow();

  if (!anyIndicatorsOn() && !feedActive_) {
    digitalWrite(kMotorEnablePin, LOW);
    indicators_.refreshNeeded = false;
    indicatorRefreshDueUs_ = 0;
  }

  bool suppressEvent = compatibilitySerialMode_
                       && (strcmp(eventType, "LEFT_POKE_LIGHT_OFF") == 0
                           || strcmp(eventType, "RIGHT_POKE_LIGHT_OFF") == 0);

  if (!suppressEvent) {
    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent(eventType,
             nowUnix, nowUs,
             kNanString,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             channel.lastColor,
             getContext(),
             kNanString);
  }
}

void MouseHouse::setMainStrip(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  turnIndicatorOn(indicators_.mainStrip,
                  strip_.Color(r, g, b, w),
                  "STRIP_ON");
}

void MouseHouse::clearMainStrip() {
  turnIndicatorOff(indicators_.mainStrip, "STRIP_OFF");
}

void MouseHouse::rightPokeLightOn(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  turnIndicatorOn(indicators_.rightPoke,
                  strip_.Color(r, g, b, w),
                  "RIGHT_POKE_LIGHT_ON");
}

void MouseHouse::rightPokeLightOff() {
  turnIndicatorOff(indicators_.rightPoke, "RIGHT_POKE_LIGHT_OFF");
}

void MouseHouse::leftPokeLightOn(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  turnIndicatorOn(indicators_.leftPoke,
                  strip_.Color(r, g, b, w),
                  "LEFT_POKE_LIGHT_ON");
}

void MouseHouse::leftPokeLightOff() {
  turnIndicatorOff(indicators_.leftPoke, "LEFT_POKE_LIGHT_OFF");
}

void MouseHouse::flashMainStrip(uint8_t r, uint8_t g, uint8_t b, uint64_t durationMs) {
  uint32_t colorVal = strip_.Color(r, g, b, 0);

  setMainStrip(r, g, b);
  ledFlashActive_ = true;
  ledFlashEndTime_ = time_us_64() + (durationMs * 1000ULL);

  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();

  logEvent("STRIP_FLASH_START",
           nowUnix, nowUs,
           kNanString,
           kNotApplicable,
           durationMs * 1000ULL,
           kNotApplicable,
           colorVal,
           getContext(),
           kNanString);
}

void MouseHouse::updateLEDFlash() {
  if (ledFlashActive_ && time_us_64() > ledFlashEndTime_) {
    clearMainStrip();
    ledFlashActive_ = false;

    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent("STRIP_FLASH_END",
             nowUnix, nowUs,
             kNanString,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             indicators_.mainStrip.lastColor,
             getContext(),
             kNanString);
  }
}

void MouseHouse::playTone(unsigned int freq, uint64_t durationMs) {
  uint64_t nowUnix = getTimestampUs();
  uint64_t nowUs = time_us_64();
  uint64_t durationUs = durationMs * 1000ULL;

  logEvent("TONE_START",
           nowUnix, nowUs,
           kNanString,
           kNotApplicable,
           durationUs,
           kNotApplicable,
           freq,
           getContext(),
           kNanString);

  tone(kBuzzerPin, freq);
  toneActive_ = true;
  toneEndTime_ = nowUs + durationUs;
  lastToneFreq_ = freq;
}

void MouseHouse::updateTone() {
  if (toneActive_ && time_us_64() > toneEndTime_) {
    noTone(kBuzzerPin);
    toneActive_ = false;

    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent("TONE_END",
             nowUnix, nowUs,
             kNanString,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             lastToneFreq_,
             getContext(),
             kNanString);
  }
}

void MouseHouse::startClickPattern(int clicks,
                                   unsigned int freq,
                                   uint32_t clickOnMs,
                                   uint32_t clickOffMs) {
  if (clicks <= 0) return;

  clickPatternActive_ = true;
  clickPatternEndPending_ = false;
  clickPatternRemaining_ = clicks;
  nextClickTime_ = time_us_64();
  clickPatternFreq_ = freq;
  clickPatternOnMs_ = clickOnMs;
  clickPatternOffMs_ = clickOffMs;

  logEvent("CLICK_PATTERN_START",
           getTimestampUs(), time_us_64(),
           kNanString,
           clicks,
           kNotApplicable,
           kNotApplicable,
           clickPatternFreq_,
           getContext(),
           kNanString);
}

void MouseHouse::clearClickPattern() {
  clickPatternActive_ = false;
  clickPatternEndPending_ = false;
  clickPatternRemaining_ = 0;
  nextClickTime_ = 0;
}

void MouseHouse::updateClickPattern() {
  if (!clickPatternActive_) return;

  uint64_t nowUs = time_us_64();

  if (clickPatternEndPending_) {
    if (nowUs >= nextClickTime_) {
      clearClickPattern();

      logEvent("CLICK_PATTERN_END",
               getTimestampUs(), nowUs,
               kNanString,
               kNotApplicable,
               kNotApplicable,
               kNotApplicable,
               clickPatternFreq_,
               getContext(),
               kNanString);
    }
    return;
  }

  if (nowUs < nextClickTime_) return;

  if (clickPatternRemaining_ <= 0) {
    clickPatternEndPending_ = true;
    nextClickTime_ = nowUs;
    return;
  }

  playTone(clickPatternFreq_, clickPatternOnMs_);
  clickPatternRemaining_--;
  nextClickTime_ = nowUs + (clickPatternOnMs_ + clickPatternOffMs_) * 1000ULL;

  if (clickPatternRemaining_ == 0) {
    clickPatternEndPending_ = true;
  }
}

void MouseHouse::clearTimeoutState() {
  bool timeoutWasSignaling = timeoutOwnsTone_ || clickPatternActive_ || clickPatternEndPending_;

  timeoutActive_ = false;
  timeoutEndTime_ = 0;
  timeoutJustEnded_ = false;
  timeoutSignalMode_ = TIMEOUT_SILENT;
  timeoutOwnsTone_ = false;

  if (timeoutWasSignaling && toneActive_) {
    noTone(kBuzzerPin);
    toneActive_ = false;

    logEvent("TONE_END",
             getTimestampUs(), time_us_64(),
             kNanString,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             lastToneFreq_,
             getContext(),
             kNanString);
  }

  clearClickPattern();
}

void MouseHouse::startTimeout(uint64_t durationMs,
                              const char* reason,
                              TimeoutSignalMode mode,
                              unsigned int clickFreq,
                              uint32_t clickOnMs,
                              uint32_t clickOffMs,
                              int clickCount) {
  if (timeoutActive_) return;

  timeoutActive_ = true;
  timeoutEndTime_ = time_us_64() + (durationMs * 1000ULL);
  timeoutJustEnded_ = false;
  timeoutCount_++;
  timeoutSignalMode_ = mode;
  timeoutOwnsTone_ = false;

  clearMainStrip();
  rightPokeLightOff();
  leftPokeLightOff();

  if (timeoutSignalMode_ == TIMEOUT_CLICK_PATTERN) {
    startClickPattern(clickCount, clickFreq, clickOnMs, clickOffMs);
  } else if (timeoutSignalMode_ == TIMEOUT_STEADY_TONE) {
    playTone(clickFreq, durationMs);
    timeoutOwnsTone_ = true;
  }

  logEvent("TIMEOUT_START",
           getTimestampUs(), time_us_64(),
           kNanString,
           timeoutCount_,
           durationMs * 1000ULL,
           kNotApplicable,
           kNotApplicable,
           getContext(),
           reason ? reason : kNanString);
}

void MouseHouse::updateTimeout() {
  if (!timeoutActive_) return;

  if (time_us_64() >= timeoutEndTime_) {
    bool timeoutWasSignaling = timeoutOwnsTone_
                               || clickPatternActive_
                               || clickPatternEndPending_;

    if (timeoutWasSignaling && toneActive_) {
      noTone(kBuzzerPin);
      toneActive_ = false;

      logEvent("TONE_END",
               getTimestampUs(), time_us_64(),
               kNanString,
               kNotApplicable,
               kNotApplicable,
               kNotApplicable,
               lastToneFreq_,
               getContext(),
               kNanString);
    }

    clearClickPattern();
    timeoutActive_ = false;
    timeoutEndTime_ = 0;
    timeoutJustEnded_ = true;
    timeoutSignalMode_ = TIMEOUT_SILENT;
    timeoutOwnsTone_ = false;

    logEvent("TIMEOUT_END",
             getTimestampUs(), time_us_64(),
             kNanString,
             timeoutCount_,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             getContext(),
             kNanString);
  }
}

void MouseHouse::houseLightOn() {
  if (!houseLightIsOn_) {
    digitalWrite(kHouseLightPin, LOW);
    houseLightIsOn_ = true;

    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent("HOUSELIGHT_ON",
             nowUnix, nowUs,
             kNanString,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             1,
             getContext(),
             kNanString);
  }
}

void MouseHouse::houseLightOff() {
  if (houseLightIsOn_) {
    digitalWrite(kHouseLightPin, HIGH);
    houseLightIsOn_ = false;

    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent("HOUSELIGHT_OFF",
             nowUnix, nowUs,
             kNanString,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             0,
             getContext(),
             kNanString);
  }
}

DateTime MouseHouse::timeForBasis(const DateTime& rtcTime,
                                  HouseLightTimeBasis timeBasis) const {
  if (timeBasis != HOUSE_LIGHT_US_EASTERN) return rtcTime;

  int year = rtcTime.year();
  uint8_t marchFirstWeekday = DateTime(year, 3, 1).dayOfTheWeek();
  uint8_t novemberFirstWeekday = DateTime(year, 11, 1).dayOfTheWeek();
  uint8_t secondSundayMarch = 8U + ((7U - marchFirstWeekday) % 7U);
  uint8_t firstSundayNovember = 1U + ((7U - novemberFirstWeekday) % 7U);

  // US Eastern DST begins at 02:00 EST (07:00 UTC) on the second Sunday in
  // March and ends at 02:00 EDT (06:00 UTC) on the first Sunday in November.
  uint32_t dstStartUtc = DateTime(year, 3, secondSundayMarch, 7, 0, 0).unixtime();
  uint32_t dstEndUtc = DateTime(year, 11, firstSundayNovember, 6, 0, 0).unixtime();
  uint32_t utcSeconds = rtcTime.unixtime();
  uint32_t offsetSeconds = (utcSeconds >= dstStartUtc && utcSeconds < dstEndUtc)
                               ? 4UL * 60UL * 60UL
                               : 5UL * 60UL * 60UL;
  return DateTime(utcSeconds - offsetSeconds);
}

void MouseHouse::updateHouseLight() {
  unsigned long nowMs = millis();
  if ((unsigned long)(nowMs - lastHouseLightCheck_) < kHouseLightCheckIntervalMs) return;
  lastHouseLightCheck_ = nowMs;
  if (!rtcValid_) return;

  DateTime rtcNow = rtc_.now();
  latestRtcUnixSeconds_ = rtcNow.unixtime();
  DateTime now = timeForBasis(rtcNow, houseLightTimeBasis_);
  uint16_t minuteOfDay = ((uint16_t)now.hour() * 60U) + now.minute();
  bool shouldBeOn = false;

  if (houseLightOnMinuteOfDay_ < houseLightOffMinuteOfDay_) {
    shouldBeOn = minuteOfDay >= houseLightOnMinuteOfDay_
                 && minuteOfDay < houseLightOffMinuteOfDay_;
  } else if (houseLightOnMinuteOfDay_ > houseLightOffMinuteOfDay_) {
    // An on-time later than the off-time describes a schedule spanning
    // midnight, such as 19:33 through 07:33.
    shouldBeOn = minuteOfDay >= houseLightOnMinuteOfDay_
                 || minuteOfDay < houseLightOffMinuteOfDay_;
  }

  if (shouldBeOn) {
    houseLightOn();
  } else {
    houseLightOff();
  }
}

bool MouseHouse::initializeCameraPio() {
  cameraSm_ = pio_claim_unused_sm(pio0, false);
  cameraPio_ = pio0;
  if (cameraSm_ < 0) {
    cameraSm_ = pio_claim_unused_sm(pio1, false);
    cameraPio_ = pio1;
  }
  if (cameraSm_ < 0) return false;

  cameraProgramOffset_ = pio_add_program(cameraPio_, &camera_ttl_program);
  float divider = (float)clock_get_hz(clk_sys) / 1000000.0f;
  camera_ttl_program_init(cameraPio_, (uint)cameraSm_, cameraProgramOffset_,
                          (uint)kPulsePin, divider);

  cameraPioOwner_ = this;
  cameraIrq_ = (cameraPio_ == pio0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
  irq_add_shared_handler((uint)cameraIrq_, cameraPioIrqHandler,
                         PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
  pio_set_irq0_source_enabled(cameraPio_, pis_interrupt0, true);
  irq_set_enabled((uint)cameraIrq_, true);
  return true;
}

void MouseHouse::cameraPioIrqHandler() {
  MouseHouse* owner = cameraPioOwner_;
  if (owner == nullptr || owner->cameraPio_ == nullptr) return;
  if (pio_interrupt_get(owner->cameraPio_, 0)) {
    pio_interrupt_clear(owner->cameraPio_, 0);
    uint64_t irqUs = time_us_64();
    uint32_t nextCount = owner->cameraIrqFrameCount_ + 1U;
    if (nextCount == 1U) owner->cameraFirstIrqUs_ = irqUs;
    owner->cameraLastIrqUs_ = irqUs;
    owner->cameraIrqFrameCount_ = nextCount;
  }
}

void MouseHouse::startCameraPio() {
  if (!cameraPioReady_) return;

  stopCameraPio();
  cameraIrqFrameCount_ = 0;
  cameraFirstIrqUs_ = 0;
  cameraLastIrqUs_ = 0;
  cameraLoggedFrameCount_ = 0;
  frameCounter_ = 0;

  uint32_t highTicks = (uint32_t)kPulseWidthUs;
  uint32_t periodTicks = (uint32_t)framePeriodUs_;
  uint32_t lowTicks = periodTicks - highTicks;

  pio_sm_clear_fifos(cameraPio_, (uint)cameraSm_);
  pio_sm_restart(cameraPio_, (uint)cameraSm_);
  pio_sm_clkdiv_restart(cameraPio_, (uint)cameraSm_);
  pio_sm_exec(cameraPio_, (uint)cameraSm_,
              pio_encode_jmp(cameraProgramOffset_));

  pio_sm_put_blocking(cameraPio_, (uint)cameraSm_, highTicks - 3U);
  pio_sm_put_blocking(cameraPio_, (uint)cameraSm_, periodTicks - 2U);
  pio_sm_put_blocking(cameraPio_, (uint)cameraSm_, lowTicks - 2U);

  cameraFirstFrameUs_ = time_us_64() + framePeriodUs_;
  pio_interrupt_clear(cameraPio_, 0);
  pio_sm_set_enabled(cameraPio_, (uint)cameraSm_, true);
}

void MouseHouse::snapshotCameraIrq(uint32_t& count, uint64_t& firstUs,
                                   uint64_t& lastUs) const {
  uint32_t interruptState = save_and_disable_interrupts();
  count = cameraIrqFrameCount_;
  firstUs = cameraFirstIrqUs_;
  lastUs = cameraLastIrqUs_;
  restore_interrupts(interruptState);
}

void MouseHouse::stopCameraPio() {
  if (!cameraPioReady_) {
    digitalWrite(kPulsePin, LOW);
    return;
  }
  pio_sm_set_enabled(cameraPio_, (uint)cameraSm_, false);
  pio_interrupt_clear(cameraPio_, 0);
  pio_sm_set_pins_with_mask(cameraPio_, (uint)cameraSm_, 0,
                            1u << (uint)kPulsePin);
}

void MouseHouse::drainCameraEvents() {
  if (!running_) return;

  uint32_t observedFrames = 0;
  uint64_t firstIrqUs = 0;
  uint64_t lastIrqUs = 0;
  snapshotCameraIrq(observedFrames, firstIrqUs, lastIrqUs);
  frameCounter_ = observedFrames;
  if (transport_.v2EnabledOrPending()) {
    if (observedFrames == 0) return;
    uint64_t latestUs = lastIrqUs;
    if (!cameraEpochQueued_) {
      queueCameraRecord(MouseHouseProtocolV2::MESSAGE_CAMERA_EPOCH, "Start",
                        SerialTransport::PRIORITY_CAMERA_CRITICAL,
                        1U, firstIrqUs);
      cameraEpochQueued_ = true;
      nextCameraCheckpointUs_ = firstIrqUs
          + kCameraCheckpointIntervalUs;
    }
    if (latestUs >= nextCameraCheckpointUs_) {
      SerialTransport::Diagnostics d = transport_.diagnostics();
      if (d.queueUsed < SerialTransport::kCheckpointHighWater) {
        queueCameraRecord(MouseHouseProtocolV2::MESSAGE_CAMERA_CHECKPOINT,
                          "Periodic",
                          SerialTransport::PRIORITY_CAMERA_CHECKPOINT,
                          observedFrames, latestUs);
      } else {
        transport_.noteSuppressedCheckpoint();
      }
      nextCameraCheckpointUs_ = latestUs + kCameraCheckpointIntervalUs;
    }
    cameraLoggedFrameCount_ = observedFrames;
    return;
  }

  uint32_t emitted = 0;
  while (cameraLoggedFrameCount_ < observedFrames
         && emitted < 8U) {
    uint32_t frame = ++cameraLoggedFrameCount_;
    uint64_t riseUs = cameraFirstFrameUs_ + ((uint64_t)(frame - 1U) * framePeriodUs_);
    uint64_t riseUnix = baseUnixUs_ + (riseUs - baseUs_);

    logEvent("CAMERA_HIGH", riseUnix, riseUs,
             kNanString, frame,
             kNotApplicable, kNotApplicable, kNotApplicable,
             getContext(), kNanString);
    logEvent("CAMERA_LOW", riseUnix + kPulseWidthUs, riseUs + kPulseWidthUs,
             kNanString, frame,
             kNotApplicable, kNotApplicable, kNotApplicable,
             getContext(), kNanString);
    emitted++;
  }
}

void MouseHouse::queueCameraRecord(uint8_t messageType, const char* reason,
                                   SerialTransport::Priority priority,
                                   uint32_t triggerCount,
                                   uint64_t triggerTimestampUs) {
  SerialTransport::Diagnostics d = transport_.diagnostics();
  SerialTransport::CameraFields camera{};
  camera.triggerCount = triggerCount;
  camera.triggerTimestampUs = triggerTimestampUs;
  camera.framePeriodUs = (uint32_t)framePeriodUs_;
  camera.pulseWidthUs = (uint32_t)kPulseWidthUs;
  camera.healthFlags = (cameraPioReady_ ? 0x01UL : 0UL)
      | (running_ ? 0x02UL : 0UL)
      | (transport_.overflowLatched() ? 0x04UL : 0UL);
  camera.suppressedCheckpoints = d.suppressedCheckpoints;
  camera.queueDepth = d.queueUsed;
  camera.queueHighWater = d.queueHighWater;
  strncpy(camera.reason, reason ? reason : kNanString,
          sizeof(camera.reason) - 1U);
  if (!transport_.enqueueCamera(messageType, camera, priority, sessionId_,
                                triggerTimestampUs)) {
    if (priority == SerialTransport::PRIORITY_CAMERA_CHECKPOINT) {
      transport_.noteSuppressedCheckpoint();
    } else {
      noteRequiredRecordFailure("CAMERA_RECORD", triggerTimestampUs);
    }
  }
}

void MouseHouse::applyIntegrityFailSafe() {
  if (!integrityFailSafePending_ || integrityFailSafeApplied_) return;
  integrityFailSafePending_ = false;
  integrityFailSafeApplied_ = true;

  bool cameraWasRunning = running_;
  running_ = false;
  stopCameraPio();
  uint32_t finalFrames = 0;
  uint64_t firstFrameUs = 0;
  uint64_t finalFrameUs = 0;
  snapshotCameraIrq(finalFrames, firstFrameUs, finalFrameUs);
  if (finalFrames == 0) finalFrameUs = time_us_64();
  frameCounter_ = finalFrames;

  if (feedActive_) {
    feedActive_ = false;
    disableFeederOutputs();
    digitalWrite(kMotorEnablePin, LOW);
  }
  clearTimeoutState();

  integrityReportPending_ = true;
  integrityCameraStopPending_ = cameraWasRunning
      && transport_.v2EnabledOrPending();
  integrityFinalFrameCount_ = finalFrames;
  integrityFinalFrameUs_ = finalFrameUs;
}

void MouseHouse::serviceIntegrityReports() {
  if (integrityReportPending_) {
    SerialTransport::Diagnostics d = transport_.diagnostics();
    if (d.queueUsed < SerialTransport::kQueueCapacity
        && queueText(MouseHouseProtocolV2::MESSAGE_INTEGRITY_FAULT,
                     SerialTransport::PRIORITY_SAFETY,
                     "INTEGRITY_FAULT,QUEUE_OVERFLOW,first_lost=%s,first_lost_us=%llu",
                     d.firstLostType,
                     (unsigned long long)d.firstLostTimestampUs)) {
      integrityReportPending_ = false;
    }
  } else if (integrityCameraStopPending_) {
    SerialTransport::Diagnostics d = transport_.diagnostics();
    if (d.queueUsed < SerialTransport::kQueueCapacity) {
      queueCameraRecord(MouseHouseProtocolV2::MESSAGE_CAMERA_STOP,
                        "IntegrityFailSafe", SerialTransport::PRIORITY_SAFETY,
                        integrityFinalFrameCount_, integrityFinalFrameUs_);
      integrityCameraStopPending_ = false;
    }
  }
}

void MouseHouse::updateBackgroundServices() {
  checkSerialCommands();
  if (transport_.overflowLatched() && !integrityFailSafeApplied_) {
    integrityFailSafePending_ = true;
  }
  applyIntegrityFailSafe();
  serviceIntegrityReports();
  serviceFeed();
  serviceIndicatorRefresh();
  checkMPR121Health();
  updateTone();
  updateClickPattern();
  updateTimeout();
  updateLEDFlash();
  updateHouseLight();
}

bool MouseHouse::consumeEventFlag(TimedBinaryEvent& event) {
  bool triggered = event.justEnded;
  event.justEnded = false;
  return triggered;
}

bool MouseHouse::consumeFlag(bool& flag) {
  bool triggered = flag;
  flag = false;
  return triggered;
}

void MouseHouse::begin() {
  Serial.begin(115200);
  transport_.begin(get_rand_64());

  if (!rtc_.begin()) {
    emitLegacyLine("ERROR_NO_RTC");
    while (1) {
      transport_.serviceOutput();
    }
  }

  bool rtcLostPower = rtc_.lostPower();
  if (rtcLostPower) {
    emitLegacyLine("RTC_LOST_POWER");
  }

  baseUs_ = time_us_64();
  baseUnixUs_ = 0;
  rtcAnchorUncertaintyUs_ = 0;
  DateTime rtcNow = rtc_.now();
  latestRtcUnixSeconds_ = rtcNow.unixtime();
  bool rtcDatePlausible = rtcNow.year() >= 2020 && rtcNow.year() <= 2099;
  rtcValid_ = !rtcLostPower && rtcDatePlausible && establishRtcAnchor();

  pinMode(kPulsePin, OUTPUT);
  digitalWrite(kPulsePin, LOW);
  cameraPioReady_ = initializeCameraPio();

  pinMode(kLeftPokePin, INPUT_PULLUP);
  pinMode(kRightPokePin, INPUT_PULLUP);
  pinMode(kPelletSensorPin, INPUT_PULLUP);

  pinMode(kAIN1, OUTPUT);
  pinMode(kAIN2, OUTPUT);
  pinMode(kBIN1, OUTPUT);
  pinMode(kBIN2, OUTPUT);
  pinMode(kMotorEnablePin, OUTPUT);
  digitalWrite(kMotorEnablePin, LOW);
  stepper_.setSpeed(kFeedMotorRpm);

  framePeriodUs_ = 1000000ULL / fps_;
  if (!cap_.begin(0x5A)) {
    emitLegacyLine("ERROR_NO_MPR121");
    while (1) {
      transport_.serviceOutput();
    }
  }

  configureMPR121();
  dumpRegs();

  pinMode(kBuzzerPin, OUTPUT);
  digitalWrite(kBuzzerPin, LOW);

  strip_.begin();
  strip_.clear();
  robustShow();

  pinMode(kHouseLightPin, OUTPUT);
  digitalWrite(kHouseLightPin, HIGH);

  queueText(MouseHouseProtocolV2::MESSAGE_DIAGNOSTIC,
            SerialTransport::PRIORITY_DIAGNOSTIC,
            "SYSTEM_START,%llu,%llu",
            (unsigned long long)getTimestampUs(),
            (unsigned long long)time_us_64());
  if (!compatibilitySerialMode_) {
    queueText(MouseHouseProtocolV2::MESSAGE_DIAGNOSTIC,
              SerialTransport::PRIORITY_DIAGNOSTIC,
              "MOUSEHOUSE_BUILD,%s,%s", __DATE__, __TIME__);
  }
  logClockStatus(rtcLostPower ? "LostPower" :
                 (rtcDatePlausible ? "Boot" : "ImplausibleDate"));
}

void MouseHouse::startSession(uint32_t fps) {
  if (fps == 0 || fps > 120 || !rtcValid_ || !cameraPioReady_
      || transport_.overflowLatched()) return;

  configureMPR121Silent();
  clearTimeoutState();

  fps_ = fps;
  sessionId_++;
  if (sessionId_ == 0) sessionId_ = 1;
  running_ = true;
  frameCounter_ = 0;
  cameraEpochQueued_ = false;
  nextCameraCheckpointUs_ = 0;
  resetSessionEventCounts();
  syncBehavioralStateToSensors(getTimestampUs());

  framePeriodUs_ = 1000000ULL / fps_;
  startCameraPio();

  if (!compatibilitySerialMode_) {
    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent("ACK_START",
             nowUnix, nowUs,
             kNanString,
             fps_,
             framePeriodUs_,
             kNotApplicable,
             kNotApplicable,
             getContext(),
             kNanString);
  }
}

void MouseHouse::stopSession() {
  bool wasRunning = running_;
  if (feedActive_) {
    feedStop("StopCommand");
    feedRetryCount_ = 0;
  }

  running_ = false;
  stopCameraPio();
  uint32_t finalFrames = 0;
  uint64_t firstFrameUs = 0;
  uint64_t lastFrameUs = 0;
  snapshotCameraIrq(finalFrames, firstFrameUs, lastFrameUs);
  frameCounter_ = finalFrames;
  if (finalFrames == 0) lastFrameUs = time_us_64();
  clearTimeoutState();

  if (wasRunning && transport_.v2EnabledOrPending()) {
    queueCameraRecord(MouseHouseProtocolV2::MESSAGE_CAMERA_STOP,
                      "StopCommand", SerialTransport::PRIORITY_SAFETY,
                      (uint32_t)frameCounter_, lastFrameUs);
  }

  if (!compatibilitySerialMode_) {
    uint64_t nowUnix = getTimestampUs();
    uint64_t nowUs = time_us_64();

    logEvent("ACK_STOP",
             nowUnix, nowUs,
             kNanString,
             frameCounter_,
             kNotApplicable,
             kNotApplicable,
             kNotApplicable,
             getContext(),
             kNanString);
  }
}

void MouseHouse::update() {
  updateBackgroundServices();

  if (running_) {
    drainCameraEvents();
  }

  if (running_ || sensorPollingWhileStopped_) {
    pollBehavioralSensors();
  }

  transport_.serviceOutput();
}

bool MouseHouse::isRunning() const {
  return running_;
}

uint32_t MouseHouse::fps() const {
  return fps_;
}

uint64_t MouseHouse::framePeriodUs() const {
  return framePeriodUs_;
}

uint64_t MouseHouse::frameCounter() const {
  return frameCounter_;
}

uint64_t MouseHouse::timestampUs() const {
  return getTimestampUs();
}

bool MouseHouse::rtcValid() const {
  return rtcValid_;
}

uint64_t MouseHouse::rtcAnchorUncertaintyUs() const {
  return rtcAnchorUncertaintyUs_;
}

bool MouseHouse::isTimeoutActive() const {
  return timeoutActive_;
}

bool MouseHouse::timeoutEnded() {
  return consumeFlag(timeoutJustEnded_);
}

bool MouseHouse::leftPokeEnded() {
  return consumeEventFlag(leftPokeEvent_);
}

bool MouseHouse::rightPokeEnded() {
  return consumeEventFlag(rightPokeEvent_);
}

bool MouseHouse::leftDrinkEnded() {
  return consumeEventFlag(leftDrinkEvent_);
}

bool MouseHouse::rightDrinkEnded() {
  return consumeEventFlag(rightDrinkEvent_);
}

bool MouseHouse::pelletRetrieved() {
  return consumeFlag(pelletJustRetrieved_);
}

void MouseHouse::markPelletRetrieved() {
  recordPelletRetrieved(getTimestampUs(), time_us_64(), "Manual");
}

bool MouseHouse::leftPokeActive() const {
  return leftPokeEvent_.previousState;
}

bool MouseHouse::rightPokeActive() const {
  return rightPokeEvent_.previousState;
}

bool MouseHouse::leftDrinkActive() const {
  return leftDrinkEvent_.previousState;
}

bool MouseHouse::rightDrinkActive() const {
  return rightDrinkEvent_.previousState;
}

bool MouseHouse::pelletSensorBlocked() const {
  return prevPelletState_;
}

bool MouseHouse::isPelletAvailable() const {
  return pelletAvailable_;
}

bool MouseHouse::isFeedActive() const {
  return feedActive_;
}

bool MouseHouse::isFeedJammed() const {
  return feedJammed_;
}

bool MouseHouse::clearFeedJam() {
  if (feedActive_ || !feedJammed_) return false;

  feedJammed_ = false;
  feedRetryCount_ = 0;
  feedPreferredStepDirection_ = kFeedStepDirection;
  return true;
}

void MouseHouse::setPelletSensorMode(PelletSensorMode mode) {
  pelletSensorMode_ = mode;
  syncBehavioralStateToSensors(getTimestampUs());
}

MouseHouse::PelletSensorMode MouseHouse::pelletSensorMode() const {
  return pelletSensorMode_;
}

void MouseHouse::setSensorPollingWhileStopped(bool enabled) {
  sensorPollingWhileStopped_ = enabled;
}

bool MouseHouse::sensorPollingWhileStopped() const {
  return sensorPollingWhileStopped_;
}
