#ifndef MOUSEHOUSE_SERIAL_TRANSPORT_H
#define MOUSEHOUSE_SERIAL_TRANSPORT_H

#include <Arduino.h>
#include "ProtocolV2.h"

class SerialTransport {
public:
  static constexpr uint8_t kQueueCapacity = 64;
  static constexpr uint8_t kSafetyReserve = 4;
  static constexpr uint8_t kCheckpointHighWater = 48;
  static constexpr uint8_t kDiagnosticHighWater = 40;
  static constexpr uint8_t kWriteBudget = 32;
  static constexpr uint8_t kInputByteBudget = 64;
  static constexpr uint8_t kCommandDispatchBudget = 4;
  static constexpr size_t kLegacyLineSize = MouseHouseProtocolV2::kMaxPayloadSize;
  static constexpr uint8_t kEncodeBudget = 1;
  static constexpr uint32_t kTargetMaxServiceUs = 500;
  static constexpr size_t kExpectedSlotSize = 432;
  static constexpr size_t kQueueStorageBytes =
      kQueueCapacity * kExpectedSlotSize;

  enum Priority : uint8_t {
    PRIORITY_DIAGNOSTIC = 0,
    PRIORITY_CAMERA_CHECKPOINT = 1,
    PRIORITY_CAMERA_CRITICAL = 2,
    PRIORITY_EVENT = 3,
    PRIORITY_SAFETY = 4,
  };

  enum ProtocolMode : uint8_t {
    PROTOCOL_V1 = 1,
    PROTOCOL_V2_PENDING = 2,
    PROTOCOL_V2_ACTIVE = 3,
  };

  struct EventFields {
    char eventType[24];
    uint64_t unixTime;
    uint64_t rp2040Time;
    char side[8];
    uint32_t count;
    uint64_t duration;
    uint64_t latency;
    int32_t value;
    char context[32];
    char reason[56];
  };

  struct CameraFields {
    uint32_t triggerCount;
    uint64_t triggerTimestampUs;
    uint32_t framePeriodUs;
    uint32_t pulseWidthUs;
    uint32_t healthFlags;
    uint32_t suppressedCheckpoints;
    uint8_t queueDepth;
    uint8_t queueHighWater;
    char reason[24];
  };

  struct Diagnostics {
    uint8_t queueUsed;
    uint8_t queueCapacity;
    uint8_t queueHighWater;
    bool connected;
    uint64_t oldestSequence;
    uint64_t highestAssignedSequence;
    uint64_t highestTransmittedSequence;
    uint64_t highestAcknowledgedSequence;
    bool overflowLatched;
    char firstLostType[24];
    uint64_t firstLostTimestampUs;
    uint32_t suppressedCheckpoints;
    uint32_t partialWrites;
    uint32_t zeroCapacityServices;
    uint32_t retransmissions;
    uint32_t commandBufferOverflows;
    uint32_t malformedCommands;
    uint32_t maxServiceDurationUs;
  };

  void begin(uint64_t bootId);
  bool enqueueEvent(const EventFields& event, Priority priority,
                    uint32_t sessionId);
  bool enqueueText(const char* line, uint8_t messageType, Priority priority,
                   uint32_t sessionId, uint64_t monotonicUs,
                   bool activateV2After = false);
  bool enqueueCamera(uint8_t messageType, const CameraFields& camera,
                     Priority priority, uint32_t sessionId,
                     uint64_t monotonicUs);

  void serviceOutput();
  bool requestProtocolV2();
  bool acknowledge(uint64_t bootId, uint64_t highestSequence);
  enum ResendResult : uint8_t {
    RESEND_ACCEPTED,
    RESEND_WRONG_BOOT,
    RESEND_TOO_OLD,
    RESEND_NOT_EMITTED,
  };
  ResendResult requestResend(uint64_t bootId, uint64_t fromSequence);

  ProtocolMode mode() const { return mode_; }
  bool v2EnabledOrPending() const { return mode_ != PROTOCOL_V1; }
  uint64_t bootId() const { return bootId_; }
  uint64_t oldestRetainedSequence() const;
  uint64_t highestAssignedSequence() const { return nextSequence_ - 1ULL; }
  uint64_t highestTransmittedSequence() const {
    return highestTransmittedSequence_;
  }
  bool overflowLatched() const { return overflowLatched_; }
  void latchOverflow(const char* lostType, uint64_t timestampUs);
  void noteSuppressedCheckpoint() { suppressedCheckpoints_++; }
  void noteCommandBufferOverflow() { commandBufferOverflows_++; }
  void noteMalformedCommand() { malformedCommands_++; }
  Diagnostics diagnostics() const;

private:
  enum SlotKind : uint8_t { SLOT_EVENT, SLOT_TEXT, SLOT_CAMERA };

  struct Slot {
    SlotKind kind;
    Priority priority;
    uint8_t messageType;
    uint16_t flags;
    uint32_t sessionId;
    uint64_t sequence;
    uint64_t monotonicUs;
    bool reliable;
    bool transmitted;
    bool activateV2After;
    union {
      EventFields event;
      CameraFields camera;
      struct {
        uint16_t length;
        char bytes[kLegacyLineSize];
      } text;
    } data;
  };

  static_assert(sizeof(Slot) == kExpectedSlotSize,
                "Update the documented fixed queue SRAM budget");

  Slot queue_[kQueueCapacity];
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  uint8_t highWater_ = 0;
  uint64_t bootId_ = 0;
  uint64_t nextSequence_ = 1;
  uint64_t highestTransmittedSequence_ = 0;
  uint64_t highestAcknowledgedSequence_ = 0;
  ProtocolMode mode_ = PROTOCOL_V1;
  bool connected_ = false;

  bool frameActive_ = false;
  uint8_t activeSlotIndex_ = 0;
  bool activeRetransmission_ = false;
  uint8_t encoded_[MouseHouseProtocolV2::kMaxEncodedFrameSize];
  uint16_t encodedLength_ = 0;
  uint16_t encodedOffset_ = 0;
  uint8_t decoded_[MouseHouseProtocolV2::kMaxDecodedFrameSize];

  bool resendActive_ = false;
  uint64_t resendNextSequence_ = 0;
  uint64_t resendThroughSequence_ = 0;

  bool overflowLatched_ = false;
  char firstLostType_[24] = "";
  uint64_t firstLostTimestampUs_ = 0;
  uint32_t suppressedCheckpoints_ = 0;
  uint32_t partialWrites_ = 0;
  uint32_t zeroCapacityServices_ = 0;
  uint32_t retransmissions_ = 0;
  uint32_t commandBufferOverflows_ = 0;
  uint32_t malformedCommands_ = 0;
  uint32_t maxServiceDurationUs_ = 0;

  bool hasCapacity(Priority priority) const;
  Slot* allocateSlot(Priority priority, uint8_t messageType,
                     uint32_t sessionId, uint64_t monotonicUs);
  uint8_t physicalIndex(uint8_t logicalIndex) const;
  int findSequence(uint64_t sequence) const;
  int findNextUntransmitted() const;
  int findNextResend() const;
  void removeAcknowledged();
  bool prepareFrame(uint8_t slotIndex, bool retransmission);
  bool prepareLegacyFrame(const Slot& slot);
  bool prepareV2Frame(const Slot& slot, bool retransmission);
  size_t serializePayload(const Slot& slot, char* output, size_t capacity) const;
  void finishFrame();
};

#endif
