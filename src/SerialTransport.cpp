#include "SerialTransport.h"

#include <stdio.h>
#include <string.h>

extern "C" uint64_t time_us_64();

namespace {
template <size_t N>
void copyBounded(char (&destination)[N], const char* source) {
  strncpy(destination, source ? source : "", N - 1U);
  destination[N - 1U] = '\0';
}
}  // namespace

void SerialTransport::begin(uint64_t bootId) {
  bootId_ = bootId;
  head_ = 0;
  count_ = 0;
  highWater_ = 0;
  nextSequence_ = 1;
  highestTransmittedSequence_ = 0;
  highestAcknowledgedSequence_ = 0;
  mode_ = PROTOCOL_V1;
  connected_ = false;
  frameActive_ = false;
  resendActive_ = false;
}

uint8_t SerialTransport::physicalIndex(uint8_t logicalIndex) const {
  return (uint8_t)((head_ + logicalIndex) % kQueueCapacity);
}

bool SerialTransport::hasCapacity(Priority priority) const {
  if (count_ >= kQueueCapacity) return false;
  if (priority == PRIORITY_SAFETY) return true;
  if (priority == PRIORITY_DIAGNOSTIC) return count_ < kDiagnosticHighWater;
  if (priority == PRIORITY_CAMERA_CHECKPOINT) {
    return count_ < kCheckpointHighWater;
  }
  return count_ < (kQueueCapacity - kSafetyReserve);
}

SerialTransport::Slot* SerialTransport::allocateSlot(
    Priority priority, uint8_t messageType, uint32_t sessionId,
    uint64_t monotonicUs) {
  if (!hasCapacity(priority)) return nullptr;
  Slot& slot = queue_[physicalIndex(count_)];
  memset(&slot, 0, sizeof(slot));
  slot.priority = priority;
  slot.messageType = messageType;
  slot.sessionId = sessionId;
  slot.monotonicUs = monotonicUs;
  slot.reliable = v2EnabledOrPending();
  slot.flags = slot.reliable ? MouseHouseProtocolV2::kFlagReliable : 0;
  if (overflowLatched_) slot.flags |= MouseHouseProtocolV2::kFlagIntegrityLatched;
  if (slot.reliable) slot.sequence = nextSequence_++;
  count_++;
  if (count_ > highWater_) highWater_ = count_;
  return &slot;
}

bool SerialTransport::enqueueEvent(const EventFields& event, Priority priority,
                                   uint32_t sessionId) {
  Slot* slot = allocateSlot(priority, MouseHouseProtocolV2::MESSAGE_EVENT,
                            sessionId, event.rp2040Time);
  if (slot == nullptr) return false;
  slot->kind = SLOT_EVENT;
  slot->data.event = event;
  return true;
}

bool SerialTransport::enqueueText(const char* line, uint8_t messageType,
                                  Priority priority, uint32_t sessionId,
                                  uint64_t monotonicUs,
                                  bool activateV2After) {
  if (line == nullptr) return false;
  size_t length = strnlen(line, kLegacyLineSize);
  if (length >= kLegacyLineSize) return false;
  Slot* slot = allocateSlot(priority, messageType, sessionId, monotonicUs);
  if (slot == nullptr) return false;
  slot->kind = SLOT_TEXT;
  slot->activateV2After = activateV2After;
  memcpy(slot->data.text.bytes, line, length);
  if (length == 0 || line[length - 1U] != '\n') {
    if (length + 1U >= kLegacyLineSize) {
      count_--;
      if (slot->reliable) nextSequence_--;
      return false;
    }
    slot->data.text.bytes[length++] = '\n';
  }
  slot->data.text.bytes[length] = '\0';
  slot->data.text.length = (uint16_t)length;
  return true;
}

bool SerialTransport::enqueueCamera(uint8_t messageType,
                                    const CameraFields& camera,
                                    Priority priority, uint32_t sessionId,
                                    uint64_t monotonicUs) {
  Slot* slot = allocateSlot(priority, messageType, sessionId, monotonicUs);
  if (slot == nullptr) return false;
  slot->kind = SLOT_CAMERA;
  slot->data.camera = camera;
  return true;
}

bool SerialTransport::requestProtocolV2() {
  if (mode_ == PROTOCOL_V2_ACTIVE) return true;
  if (mode_ == PROTOCOL_V2_PENDING) return true;
  Slot* slot = allocateSlot(PRIORITY_SAFETY,
                            MouseHouseProtocolV2::MESSAGE_COMMAND_RESULT,
                            0, time_us_64());
  if (slot == nullptr) return false;
  slot->kind = SLOT_TEXT;
  const char response[] = "ACK_PROTO,2\n";
  memcpy(slot->data.text.bytes, response, sizeof(response));
  slot->data.text.length = sizeof(response) - 1U;
  slot->reliable = false;
  slot->sequence = 0;
  slot->flags = 0;
  slot->activateV2After = true;
  mode_ = PROTOCOL_V2_PENDING;
  return true;
}

uint64_t SerialTransport::oldestRetainedSequence() const {
  for (uint8_t index = 0; index < count_; ++index) {
    const Slot& slot = queue_[physicalIndex(index)];
    if (slot.reliable) return slot.sequence;
  }
  return nextSequence_;
}

int SerialTransport::findSequence(uint64_t sequence) const {
  for (uint8_t index = 0; index < count_; ++index) {
    uint8_t physical = physicalIndex(index);
    if (queue_[physical].reliable && queue_[physical].sequence == sequence) {
      return physical;
    }
  }
  return -1;
}

int SerialTransport::findNextUntransmitted() const {
  for (uint8_t index = 0; index < count_; ++index) {
    uint8_t physical = physicalIndex(index);
    if (!queue_[physical].transmitted) return physical;
  }
  return -1;
}

int SerialTransport::findNextResend() const {
  for (uint8_t index = 0; index < count_; ++index) {
    uint8_t physical = physicalIndex(index);
    const Slot& slot = queue_[physical];
    if (slot.reliable && slot.transmitted
        && slot.sequence >= resendNextSequence_
        && slot.sequence <= resendThroughSequence_) {
      return physical;
    }
  }
  return -1;
}

bool SerialTransport::acknowledge(uint64_t bootId, uint64_t highestSequence) {
  if (bootId != bootId_ || highestSequence > highestTransmittedSequence_) {
    return false;
  }
  if (highestSequence <= highestAcknowledgedSequence_) return true;
  highestAcknowledgedSequence_ = highestSequence;
  removeAcknowledged();
  return true;
}

void SerialTransport::removeAcknowledged() {
  while (count_ > 0) {
    Slot& slot = queue_[head_];
    if (!slot.reliable || !slot.transmitted
        || slot.sequence > highestAcknowledgedSequence_) {
      break;
    }
    head_ = (uint8_t)((head_ + 1U) % kQueueCapacity);
    count_--;
  }
}

SerialTransport::ResendResult SerialTransport::requestResend(
    uint64_t bootId, uint64_t fromSequence) {
  if (bootId != bootId_) return RESEND_WRONG_BOOT;
  uint64_t oldest = oldestRetainedSequence();
  if (fromSequence < oldest) return RESEND_TOO_OLD;
  if (fromSequence > highestTransmittedSequence_) return RESEND_NOT_EMITTED;
  if (findSequence(fromSequence) < 0) return RESEND_TOO_OLD;
  resendActive_ = true;
  resendNextSequence_ = fromSequence;
  resendThroughSequence_ = highestTransmittedSequence_;
  return RESEND_ACCEPTED;
}

size_t SerialTransport::serializePayload(const Slot& slot, char* output,
                                         size_t capacity) const {
  int length = -1;
  if (slot.kind == SLOT_EVENT) {
    const EventFields& event = slot.data.event;
    length = snprintf(output, capacity,
                      "%s,%llu,%llu,%s,%lu,%llu,%llu,%ld,%s,%s",
                      event.eventType,
                      (unsigned long long)event.unixTime,
                      (unsigned long long)event.rp2040Time,
                      event.side, (unsigned long)event.count,
                      (unsigned long long)event.duration,
                      (unsigned long long)event.latency,
                      (long)event.value, event.context, event.reason);
  } else if (slot.kind == SLOT_CAMERA) {
    const CameraFields& camera = slot.data.camera;
    const char* label = slot.messageType == MouseHouseProtocolV2::MESSAGE_CAMERA_EPOCH
                            ? "CAMERA_EPOCH"
                        : slot.messageType == MouseHouseProtocolV2::MESSAGE_CAMERA_STOP
                            ? "CAMERA_STOP"
                            : "CAMERA_CHECKPOINT";
    length = snprintf(output, capacity,
                      "%s,count=%lu,timestamp_us=%llu,period_us=%lu,"
                      "pulse_us=%lu,health=0x%08lx,queue=%u/%u,suppressed=%lu,reason=%s",
                      label, (unsigned long)camera.triggerCount,
                      (unsigned long long)camera.triggerTimestampUs,
                      (unsigned long)camera.framePeriodUs,
                      (unsigned long)camera.pulseWidthUs,
                      (unsigned long)camera.healthFlags,
                      camera.queueDepth, camera.queueHighWater,
                      (unsigned long)camera.suppressedCheckpoints,
                      camera.reason);
  } else {
    size_t lengthWithoutNewline = slot.data.text.length;
    if (lengthWithoutNewline > 0
        && slot.data.text.bytes[lengthWithoutNewline - 1U] == '\n') {
      lengthWithoutNewline--;
    }
    if (lengthWithoutNewline >= capacity) return 0;
    memcpy(output, slot.data.text.bytes, lengthWithoutNewline);
    output[lengthWithoutNewline] = '\0';
    return lengthWithoutNewline;
  }
  if (length < 0 || (size_t)length >= capacity) return 0;
  return (size_t)length;
}

bool SerialTransport::prepareLegacyFrame(const Slot& slot) {
  if (slot.kind == SLOT_TEXT) {
    if (slot.data.text.length > sizeof(encoded_)) return false;
    memcpy(encoded_, slot.data.text.bytes, slot.data.text.length);
    encodedLength_ = slot.data.text.length;
    return true;
  }
  size_t payloadLength = serializePayload(
      slot, reinterpret_cast<char*>(encoded_), sizeof(encoded_) - 1U);
  if (payloadLength == 0 || payloadLength + 1U > sizeof(encoded_)) return false;
  encoded_[payloadLength] = '\n';
  encodedLength_ = (uint16_t)(payloadLength + 1U);
  return true;
}

bool SerialTransport::prepareV2Frame(const Slot& slot, bool retransmission) {
  char payload[MouseHouseProtocolV2::kMaxPayloadSize + 1U];
  size_t payloadLength = serializePayload(slot, payload, sizeof(payload));
  if (payloadLength == 0 || payloadLength > MouseHouseProtocolV2::kMaxPayloadSize) {
    return false;
  }

  uint16_t flags = slot.flags;
  if (retransmission) flags |= MouseHouseProtocolV2::kFlagRetransmission;
  uint8_t* cursor = decoded_;
  *cursor++ = MouseHouseProtocolV2::kVersion;
  *cursor++ = slot.messageType;
  MouseHouseProtocolV2::putLe16(cursor, flags); cursor += 2;
  MouseHouseProtocolV2::putLe64(cursor, bootId_); cursor += 8;
  MouseHouseProtocolV2::putLe32(cursor, slot.sessionId); cursor += 4;
  MouseHouseProtocolV2::putLe64(cursor, slot.sequence); cursor += 8;
  MouseHouseProtocolV2::putLe64(cursor, slot.monotonicUs); cursor += 8;
  MouseHouseProtocolV2::putLe16(cursor, (uint16_t)payloadLength); cursor += 2;
  memcpy(cursor, payload, payloadLength); cursor += payloadLength;
  uint32_t crc = MouseHouseProtocolV2::crc32(decoded_, cursor - decoded_);
  MouseHouseProtocolV2::putLe32(cursor, crc); cursor += 4;

  size_t encodedLength = MouseHouseProtocolV2::cobsEncode(
      decoded_, cursor - decoded_, encoded_, sizeof(encoded_));
  if (encodedLength == 0) return false;
  encodedLength_ = (uint16_t)encodedLength;
  return true;
}

bool SerialTransport::prepareFrame(uint8_t slotIndex, bool retransmission) {
  Slot& slot = queue_[slotIndex];
  bool prepared = slot.reliable
                      ? prepareV2Frame(slot, retransmission)
                      : prepareLegacyFrame(slot);
  if (!prepared) {
    latchOverflow("SERIALIZATION_FAILURE", time_us_64());
    return false;
  }
  activeSlotIndex_ = slotIndex;
  activeRetransmission_ = retransmission;
  encodedOffset_ = 0;
  frameActive_ = true;
  return true;
}

void SerialTransport::finishFrame() {
  Slot& slot = queue_[activeSlotIndex_];
  if (activeRetransmission_) {
    retransmissions_++;
    resendNextSequence_ = slot.sequence + 1ULL;
    if (resendNextSequence_ > resendThroughSequence_
        || findNextResend() < 0) {
      resendActive_ = false;
    }
  } else {
    slot.transmitted = true;
    if (slot.reliable && slot.sequence > highestTransmittedSequence_) {
      highestTransmittedSequence_ = slot.sequence;
    }
    bool activate = slot.activateV2After;
    if (!slot.reliable) {
      if (activeSlotIndex_ == head_) {
        head_ = (uint8_t)((head_ + 1U) % kQueueCapacity);
        count_--;
      }
    }
    if (activate) mode_ = PROTOCOL_V2_ACTIVE;
  }
  frameActive_ = false;
  encodedLength_ = 0;
  encodedOffset_ = 0;
}

void SerialTransport::serviceOutput() {
  uint64_t startedUs = time_us_64();
  connected_ = (bool)Serial;
  if (!frameActive_) {
    int slotIndex = resendActive_ ? findNextResend() : -1;
    bool retransmission = slotIndex >= 0;
    if (slotIndex < 0) slotIndex = findNextUntransmitted();
    if (slotIndex < 0 || !prepareFrame((uint8_t)slotIndex, retransmission)) {
      uint32_t duration = (uint32_t)(time_us_64() - startedUs);
      if (duration > maxServiceDurationUs_) maxServiceDurationUs_ = duration;
      return;
    }
  }

  int available = Serial.availableForWrite();
  if (available <= 0) {
    zeroCapacityServices_++;
    uint32_t duration = (uint32_t)(time_us_64() - startedUs);
    if (duration > maxServiceDurationUs_) maxServiceDurationUs_ = duration;
    return;
  }
  size_t remaining = encodedLength_ - encodedOffset_;
  size_t requested = remaining;
  if (requested > (size_t)available) requested = (size_t)available;
  if (requested > kWriteBudget) requested = kWriteBudget;
  size_t written = Serial.write(encoded_ + encodedOffset_, requested);
  if (written < requested) partialWrites_++;
  if (written > requested) written = requested;
  encodedOffset_ += (uint16_t)written;
  if (encodedOffset_ == encodedLength_) finishFrame();

  uint32_t duration = (uint32_t)(time_us_64() - startedUs);
  if (duration > maxServiceDurationUs_) maxServiceDurationUs_ = duration;
}

void SerialTransport::latchOverflow(const char* lostType,
                                    uint64_t timestampUs) {
  if (overflowLatched_) return;
  overflowLatched_ = true;
  copyBounded(firstLostType_, lostType ? lostType : "UNKNOWN");
  firstLostTimestampUs_ = timestampUs;
}

SerialTransport::Diagnostics SerialTransport::diagnostics() const {
  Diagnostics result{};
  result.queueUsed = count_;
  result.queueCapacity = kQueueCapacity;
  result.queueHighWater = highWater_;
  result.connected = connected_;
  result.oldestSequence = oldestRetainedSequence();
  result.highestAssignedSequence = highestAssignedSequence();
  result.highestTransmittedSequence = highestTransmittedSequence_;
  result.highestAcknowledgedSequence = highestAcknowledgedSequence_;
  result.overflowLatched = overflowLatched_;
  copyBounded(result.firstLostType, firstLostType_);
  result.firstLostTimestampUs = firstLostTimestampUs_;
  result.suppressedCheckpoints = suppressedCheckpoints_;
  result.partialWrites = partialWrites_;
  result.zeroCapacityServices = zeroCapacityServices_;
  result.retransmissions = retransmissions_;
  result.commandBufferOverflows = commandBufferOverflows_;
  result.malformedCommands = malformedCommands_;
  result.maxServiceDurationUs = maxServiceDurationUs_;
  return result;
}
