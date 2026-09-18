# SqueakView controller protocol v2

Protocol v2 is an opt-in, reliable RAM-buffered transport for the Feather
RP2040 controller. Protocol v1 remains the boot default. The transport does not
make RAM records durable: the host must durably store and acknowledge them.

## Activation and lifecycle

The host sends the existing newline-delimited ASCII command `PROTO,2\n` while
no run or feed is active. The controller queues exactly `ACK_PROTO,2\n` as a v1
line. It remains in the pending state until all 12 bytes have been accepted by
USB through the incremental writer, then switches outbound records to v2.
Inbound commands remain newline-delimited ASCII and use the single existing
parser. A request during a run/feed returns `NACK,PROTO,DEVICE_BUSY`; an
unsupported version returns `NACK,PROTO,UNSUPPORTED_VERSION`. Repeating the
command while the acknowledgement is pending does not queue another response;
repeating it after activation produces a framed v2 `ACK_PROTO,2` command result.
There is no switch back to v1 without reboot.

USB disconnect/reconnect does not change the mode, boot ID, sequence state,
queue, acknowledgement state, active session, or integrity latch. If disconnect
happens during `ACK_PROTO,2\n` or a v2 frame, transmission resumes at the first
unaccepted byte. Reboot returns to v1 and loses unacknowledged RAM records.

At boot the firmware obtains a 64-bit ID from the RP2040 hardware random-number
generator (`get_rand_64()`). A changed boot ID tells the host that a reboot and
possible loss of unacknowledged RAM records occurred. Random collision
probability is 1 in 2^64 per independent draw (birthday effects apply across
large fleets).

Session ID 0 is the control/no-run session. Each accepted `START` increments a
32-bit counter (skipping zero) and all records produced by that run carry it.
The counter persists across STOP and resets only at boot. Sequence numbers are
one boot-global unsigned 64-bit space, start at 1, are assigned only after a
reliable record is admitted, and do not reset at START. Retransmission preserves
the original number. At even 1,000 records/second, wrap takes over 584 million
years; wrap is therefore treated as unreachable within one powered boot.

## Decoded record layout

All multibyte integers are unsigned little-endian unless a payload definition
says otherwise. The fixed header is 34 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | protocol version, always `2` |
| 1 | 1 | message type |
| 2 | 2 | flags |
| 4 | 8 | boot ID |
| 12 | 4 | session ID; zero outside a run |
| 16 | 8 | reliable sequence number |
| 24 | 8 | RP2040 monotonic timestamp, microseconds |
| 32 | 2 | payload length `N` |
| 34 | `N` | bounded UTF-8 payload, without newline or NUL |
| 34+`N` | 4 | CRC32 |

The payload maximum is 384 bytes. Maximum decoded size is 422 bytes and maximum
COBS-encoded size, including its delimiter, is 425 bytes. COBS is applied to the
entire decoded header + payload + CRC. Exactly one zero byte terminates each
encoded frame; there are no other zero bytes in the encoded body.

CRC is the common reflected CRC-32/ISO-HDLC form: polynomial `0x04C11DB7`
(`0xEDB88320` in the reflected implementation), initial value `0xFFFFFFFF`,
input and output reflected, final XOR `0xFFFFFFFF`, no augmentation beyond the
algorithm. It covers every byte from version through the final payload byte and
does not cover the CRC field or COBS encoding. The `123456789` check value is
`0xCBF43926`.

Message types:

| ID | Name | Payload |
|---:|---|---|
| 1 | `EVENT` | canonical v1 event fields |
| 2 | `COMMAND_RESULT` | existing ACK/NACK/result text |
| 3 | `CAMERA_EPOCH` | camera summary text below |
| 4 | `CAMERA_CHECKPOINT` | camera summary text below |
| 5 | `CAMERA_STOP` | camera summary text below |
| 6 | `TRANSPORT_STATUS` | transport counters text below |
| 7 | `INTEGRITY_FAULT` | latched fault text |
| 8 | `DIAGNOSTIC` | bounded diagnostic text |

Flags are `0x0001 RELIABLE`, `0x0002 RETRANSMISSION`, and
`0x0004 INTEGRITY_LATCHED`. Unknown flags must be preserved in stored raw frames
and ignored when their meaning is not known.

`EVENT` payloads retain all ten canonical fields in their existing order:
`type,unix_us,rp2040_us,side,count,duration_us,latency_us,value,context,reason`.
Text payload numbers use decimal ASCII. All v2 records currently use the same
outer reliable queue, including diagnostics and command results.

Camera payloads are:

`CAMERA_EPOCH,count=C,timestamp_us=T,period_us=P,pulse_us=W,health=0xHHHHHHHH,queue=Q/H,suppressed=S,reason=R`

`CAMERA_CHECKPOINT` and `CAMERA_STOP` use the same fields and their respective
labels. Health bit 0 means PIO initialized, bit 1 means the run was active when
the record was constructed, and bit 2 means transport integrity is latched.
Epoch count/timestamp identify the first actual rising edge using the RP2040
timestamp captured when its PIO IRQ is serviced (there is bounded IRQ-entry
latency after the physical edge). Checkpoints report
the latest already-counted IRQ edge approximately once per second. STOP reports
the final count and last rising-edge timestamp; when no edge occurred its
timestamp is the STOP handling time. Falling edges are reconstructed from the
configured pulse width.

`TRANSPORT_STATUS\n` is an ASCII request accepted in either mode. In v2 its one
record reports boot ID, header session ID, mode, queue used/capacity/high-water,
oldest retained, highest assigned/transmitted/acknowledged sequences, overflow
and first-loss details, suppressed checkpoints, partial/zero-capacity writes,
retransmissions, command overflows/malformed commands, and maximum observed
service duration. It is only emitted on request and therefore cannot flood the
link. To remain within the strict payload bound its keys are compact:
`b,m,n,q,h,o,a,t,k,x,l,u,c,p,z,r,v,f,d` in exactly that order for boot, mode,
USB connected, queue, high-water, oldest, assigned, transmitted, acknowledged, overflow,
first-lost type/time, suppressed checkpoints, partial writes, zero-capacity
calls, retransmissions, command-buffer overflows, malformed commands, and
maximum service microseconds.

Example: message type 2, flags 1, boot `0x0102030405060708`, session 7,
sequence 42, monotonic time 123456789, payload `ACK_FEED` decodes according to
the table above. Its complete COBS frame is:

```text
04 02 02 01 0a 08 07 06 05 04 03 02 01 07 01 01
02 2a 01 01 01 01 01 01 05 15 cd 5b 07 01 01 01
02 08 0d 41 43 4b 5f 46 45 45 44 49 38 a8 0f 00
```

## Acknowledgement and replay

After a frame has been validated and durably stored, the host sends:

```text
ACK_EVENTS,<boot_id>,<highest_durably_stored_sequence>\n
```

Acknowledgement is cumulative. The boot ID and sequence are unsigned decimal.
The controller releases records through that sequence only if the boot matches
and the sequence is no higher than the highest completely transmitted record.
Duplicates and stale ACKs are harmless. Invalid/wrong-boot/beyond-emitted ACKs
are rejected and cannot release data. ACK handling queues no success response.

To replay, send:

```text
RESEND_EVENTS,<boot_id>,<from_sequence>\n
```

This only sets a cursor; it never sends synchronously. A partially transmitted
frame finishes first, then retained frames through the highest transmitted
sequence are replayed incrementally with their original sequence and the
retransmission flag. `ACK_RESEND_EVENTS` confirms acceptance. A request before
the oldest retained sequence returns `NACK,RESEND_EVENTS,TOO_OLD,oldest=N`;
wrong-boot and not-yet-emitted requests have distinct NACK reasons. Duplicate
frames are normal and hosts must deduplicate by `(boot_id, sequence)`.

## Queue, scheduling, and failure behavior

The queue has 64 fixed slots of 432 bytes: 27,648 bytes. Four slots are reserved
from ordinary records for safety records. Diagnostics stop at 40 occupied slots
and optional camera checkpoints at 48. Records are never overwritten. The
transport also owns 422 decoded and 425 encoded scratch bytes. Command storage
is 96 bytes; the task-line builder is 384 bytes. There is no heap allocation,
filesystem access, SD logging, waiting, or retry loop in acquisition/transport.
For the largest `go_nogo_automated` image, PlatformIO reports 10,192 bytes RAM
(3.9%) at the unchanged baseline commit and 39,312 bytes (15.0%) after this
transport, an increase of 29,120 bytes. The resulting image leaves 222,832
bytes (about 217.6 KiB) of the RP2040's 262,144-byte SRAM budget unused.

One service call encodes at most one bounded record, queries current USB
capacity once, makes at most one write, and offers at most 32 bytes. It advances
only by the actual returned count. Input consumes at most 64 available bytes and
dispatches at most four complete commands per loop. Target transport service
time is 500 microseconds and maximum observed time is instrumented. Arduino-Pico
USB can wait internally when asked for more data than endpoint capacity; this
implementation first queries `availableForWrite()` and limits its small write
to that reported capacity, remaining frame bytes, and the 32-byte budget.

The only interrupt producer is the PIO camera IRQ. It clears the IRQ, captures
the first/latest monotonic timestamp, and increments a volatile counter; it never touches the queue, formats, calculates
CRC/COBS, or accesses USB. All queue producers run in the main loop, so no queue
critical section is needed. Main-loop snapshots of the 32/64-bit IRQ fields use
only a few loads inside a very short interrupt-masked section. PIO generates
both edges independently of the main loop and its program/timing values are
unchanged.

If a required record cannot enter the queue, the controller atomically preserves
the first lost type/time in dedicated state and latches scientific integrity
invalid until reboot. It stops the run/PIO, disables an active feeder using the
existing safe output order, clears automatic timeout behavior, rejects later
START/FEED, and continues servicing STOP, commands, transport ACK/replay/status,
and `CLEAR_JAM`. It retries only the reserved integrity-fault and camera-stop
*enqueue* once ACKs create capacity, one bounded attempt per main-loop call; it
never waits. This fail-safe can end a run and is intentionally not recoverable
without reboot.

Checkpoints are optional above 48 occupied slots, allocate no sequence when
suppressed, increment a counter, and are tried again on a later interval. Epoch,
STOP, behavioral, feeder/jam, command result, and integrity records are required.
Protocol v1 uses the same bounded incremental writer but releases a line only
after its newline is fully accepted. Its formats and per-frame `CAMERA_HIGH` /
`CAMERA_LOW` records remain unchanged.

## Host requirements

The host must retain an input buffer across reads, split only on zero, COBS
decode, enforce all sizes, verify version/length/CRC before interpreting a
record, durably store before ACK, ACK only the highest contiguous stored
sequence, deduplicate replayed `(boot, sequence)` pairs, and treat boot-ID change
as a reboot boundary. It must tolerate USB reads splitting or combining frames,
unknown future message types/flags, retransmissions, and long intervals with no
checkpoint. It must not infer durability from successful USB receipt alone.

## Hardware qualification plan

1. Activate v2, START at 30 FPS, and verify epoch/checkpoints at the host.
2. Measure A0 with a logic analyzer before and during the remaining tests;
   confirm the established frequency tolerance and 1,000 us high pulse.
3. Stop host reads while camera triggering continues and watch queue/status
   counters grow without trigger changes or main-loop starvation.
4. During backpressure send STOP and confirm safe physical stop and eventual
   reliable STOP result.
5. Reconnect, request `RESEND_EVENTS` from the oldest retained sequence, durably
   store/ACK it, and prove sequence continuity and absence of silent overwrite.
6. Repeat with short reads/disconnects during frames; confirm partial-write,
   zero-capacity, high-water, and retransmission diagnostics.
7. Exercise pokes, pellet detection, successful feed, retry reversal, jam, and
   exact `CLEAR_JAM` behavior before/during congestion.
8. Withhold reads and ACKs long enough to fill the queue. Confirm the integrity
   latch, stopped PIO/run, disabled feeder, rejected START/FEED, responsive STOP
   and `CLEAR_JAM`, and delayed fault/STOP reporting after capacity returns.
9. Confirm no reset/watchdog event occurred and that a deliberate reboot changes
   boot ID and returns to protocol v1.

Compilation and software fake-endpoint tests cannot establish electrical timing;
the logic-analyzer test is required before qualifying the behavioral change.
