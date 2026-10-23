# MouseHouse

First-pass Arduino library for the MouseHouse behavioral rig.

## PlatformIO

This repository is also a ready-to-build PlatformIO project. It targets the
Adafruit Feather RP2040 and uses the Arduino-Pico core because the firmware
calls RP2040 SDK timing functions directly.

The `fixed_ratio` task is the default environment:

```sh
pio run
pio run --target upload
pio device monitor
```

Its active poke alternates at America/New_York midnight using the validated
RTC calendar. The reference date and starting side are configured near the
top of `examples/fixed_ratio/fixed_ratio.ino`; September 15, 2026 is currently
a left-poke day, followed by a right-poke day. The selection is deterministic
across resets. A midnight change waits for any active poke or feed to finish,
and emits `ACTIVE_POKE_SWITCH_PENDING`. If the old side has partial ratio
progress, or its poke is active at midnight, that side remains active until the
animal completes the ratio and its reward feed succeeds. The firmware then
applies the new side and emits `ACTIVE_POKE_SCHEDULE`. With no ratio in
progress, the handoff occurs as soon as the poke sensors and feeder are idle.
Stopping a session abandons partial task progress, as it does at other times,
and permits the pending calendar side to be applied.

The other included tasks can be built or uploaded by selecting their
environment:

```sh
pio run -e free_feeding
pio run -e go_nogo_automated
pio run -e setup_debug
pio run -e setup_debug --target upload
```

The environments share the reusable code in `src/MouseHouse.*`. The small
`src/main.cpp` dispatcher includes the corresponding original sketch from
`examples/`, so the Arduino IDE examples and PlatformIO builds use the same
task source.

### Automatic RTC detection

The same firmware automatically detects an installed **PCF8523** or **DS3231**
at startup, in both PlatformIO and Arduino builds. No RTC build flag or user
selection is needed. Startup reports `RTC_TYPE,PCF8523` or `RTC_TYPE,DS3231`.

The chips share I2C address `0x68`, so an address ACK alone cannot identify them.
`src/AutoRtc.h` compares sequential register reads: the DS3231 pointer wraps
after `0x12`, while the PCF8523 wraps after `0x13`. These behaviors are documented
in the [DS3231 address map](https://www.analog.com/media/en/technical-documentation/data-sheets/ds3231.pdf)
and [PCF8523 section 8.1](https://www.nxp.com/docs/en/data-sheet/PCF8523.pdf).
Detection sets only the read pointer and never writes register data, resets the
clock, or changes oscillator/calibration settings. It requires two consecutive
matching probes, within four attempts, before initializing the selected driver.
It does not rely on a valid calendar, so a lost-power RTC can still be identified
and corrected through `SET_RTC`.

Missing, failed, or ambiguous detection retains the safe `ERROR_NO_RTC` /
`STARTUP_FAULT,NO_RTC` behavior and prevents clock writes. It never guesses a
driver. This discriminates the two supported chips; it is not a universal RTC
identifier. Only one RTC may occupy `0x68`.

Both types use UTC, the same `TIME_SYNC` and `SET_RTC` commands, lost-power and
invalid-date checks, monotonic event-time anchoring, and house-light/day
scheduling. After changing the RTC, use the idle pre-run clock validation and
correction procedure below.

Software tests exercise detection, driver dispatch, failures, and the absence
of register-data writes using simulated I2C. They do not validate electrical
behavior. Before deployment, check automatic identification on both physical
RTC types, startup with valid, invalid/lost-power, and missing RTC states;
set and verify UTC against the Jetson; verify battery-backed time across power
cycles; confirm `TIME_SYNC`/`SET_RTC` reject active sessions and feeds; and check
house-light/day scheduling and continuous camera timing on the physical rig.

### Recover a missing controller on Jetson USB-C

If an upload fails because the board is missing from `lsusb` and
`/dev/serial/by-id/`, the repository includes an on-demand recovery command.
It uses the system Python standard library, requires no package installation,
and installs no service or boot configuration. Run it only with the rig idle.
This targets the tested Orin Nano USB-C role switch `usb2-0-role-switch` and
USB device path `1-1`; it is not a general reset for arbitrary USB ports.

From the repository directory, no arguments are needed:

```sh
# Optional read-only presence check (no sudo).
bash tools/recover_usb.sh --check

# If missing, recover the USB-C connection, then retry your selected upload.
sudo bash tools/recover_usb.sh
pio run -e fixed_ratio --target upload

# Start the monitor separately after the upload completes.
pio device monitor
```

The tool automatically looks for a Feather RP2040 Adalogger serial device.
It returns 0 when one is present and nonzero on failure. If multiple controllers
are visible, use the optional `--serial <serial>` flag to select one; the serial
number is in its `/dev/serial/by-id/` name. To upload only after a successful
check or recovery:

```sh
sudo bash tools/recover_usb.sh && pio run -e fixed_ratio --target upload
```

An already-present controller is left untouched. Otherwise the tool waits two
seconds, then makes at most three `host -> none -> host` recovery attempts,
waiting up to 15 seconds after each. It refuses to reset a port in another role
or occupied by another device. It does not open the serial port, issue firmware
commands, or reset the shared USB host controller. Presence means USB enumeration
succeeded, not that firmware initialization or pre-run checks passed. Use this
only for a missing device, while idle, not as an experiment-time watchdog.

The implementation is also importable as `squeakview_firmware.usb_recovery`;
privilege is still required for the host's USB role write. A manual role reset
recovered a real cold-start failure on the test Jetson without replugging.
Repeated cold-start validation of this packaged tool remains required before
fleet deployment.

### Guided pre-deployment qualification

Flash `setup_debug`. It runs an ordered qualification wizard and reports the
next required action as `SETUP_PROMPT`. You can operate it through a raw serial
monitor, although the PySide interface described below is recommended. In a
raw monitor, begin by assigning the unit an identifier:

```text
TEST,DEVICE,MH-012
```

After the device ID, the GUI requires the same NTP-backed clock validation used
by `mousehouse-clock-validate`. A plausible RTC date alone cannot pass. The GUI
collects seven samples, uses the three lowest-latency exchanges, explicitly
offers RTC correction when needed, verifies the result, and embeds the clock
evidence in the final JSON qualification record. The wizard then requests
complete left/right poke cycles, a pellet-beam block/clear cycle, and complete
left/right drink cycles. Those inputs advance automatically only after the
requested physical transition is observed. It then performs a nonblocking red,
green, blue, white, and off LED sweep and asks the operator to answer with
`TEST,YES`, `TEST,NO`, or `TEST,RETRY`.

NeoPixels remain off during sensor-only stages because the GUI provides sensor
feedback. They are energized only by the RGBW sweep and the explicit
feed-with-indicator test; the dark-feed and jam tests keep every pixel off.

Later stages guide the operator through successful feeds with all LEDs off and
with the left poke pixel blue. The well must be cleared between feeds. A jam
test exercises retries and the `CLEAR_JAM` command, followed by confirmation of
physical direction reversal. The wizard also sounds the buzzer, requests a
`START,30`/`STOP` camera cycle and physical TTL confirmation, and requests
house-light confirmation.

During the camera cycle, the GUI shows an authoritative running indicator,
controller TTL pulse count, timestamp-derived output rate, and a 60-second
measurement ramp. This is controller-output telemetry, not proof that the
physical camera received every pulse; compare it with the receiver, camera, or
test instrument before confirming the physical TTL check.

If a jam occurs unexpectedly during either successful-feed test, the GUI keeps
the current qualification stage and exposes **Clear Jam** immediately. Restore
motor power, inspect and physically clear the feeder, clear the firmware latch,
and then repeat the current prompted action.

Useful wizard commands are:

```text
TEST,STATUS
TEST,YES
TEST,NO
TEST,RETRY
TEST,ABORT
TEST,RESTART
```

Abort cancels pending LED/feed tests, stops the active feeder and camera session,
and turns the indicators off. Use `TEST,RESTART` to begin a new qualification;
an existing feeder jam must still be physically cleared and acknowledged first.

Live sensor and GPIO status is request-only: send `TEST,STATUS`. Normal serial
output is limited to prompts, state transitions, results, and qualification
records so the guided test does not continuously flood the monitor.

Each attempt is logged. Completion emits one machine-readable
`SETUP_QUALIFICATION_RECORD` containing the device ID, firmware build time,
test results, failure count, and overall result. The initial implementation
emits that record over serial for SqueakView or another host to save. SD-card
persistence is intentionally marked `sd_copy=TODO`; the record format and test
logic are kept independent of the future storage backend.

Operator confirmations are required because GPIO readback cannot prove that a
pixel emitted the right color, the motor produced torque, or a camera receiver
saw a pulse. Likewise, `gpio13_control` is not a VIN, battery-voltage, or
motor-current measurement.

The PySide operator interface provides serial-port discovery, the current
instruction in plain language, context-specific controls, checklist progress,
and automatic host-side transcript and result storage. Close PlatformIO's
serial monitor first, then launch it with:

```sh
uv sync --locked
uv run mousehouse-setup
```

The GUI performs a protocol-version handshake and enables qualification
controls only for the matching `setup_debug` firmware. Reflash the current
`setup_debug` build whenever the GUI reports a firmware/protocol mismatch.

By default, records are written beneath `~/MouseHouseQualificationLogs`; the
destination can be changed before connecting. A completed qualification saves
the raw record as text and a JSON file containing both the parsed result and
full timestamped transcript.

`MouseHouse` is meant to separate reusable rig control from experiment-specific
task logic:

- the library owns hardware, logging, session control, and sensor state
- the sketch owns contingencies like "when a left poke ends, deliver a pellet"

The library owns:

- session control and serial `START,<fps>` / `STOP` / `FEED,<steps>` /
  `CLEAR_JAM` commands
- camera sync pulses
- poke, drink, pellet, and well sensing
- feeder stepping and retry/jam handling
- tone, strip, and house-light control
- automatic event logging

Additional clock-management commands are available for the Jetson host:

```text
TIME_SYNC,<sequence>,<jetson_send_ns>
SET_RTC,<unix_seconds>
```

Both commands are pre-run operations and are rejected with `DEVICE_BUSY` while
the session or feeder is active. `TIME_SYNC` is read-only, but it is still kept
out of active experiments to avoid adding serial traffic during behavioral
acquisition. If the detected RTC reports lost power or an implausible date, `START`
is rejected with `NACK,START,RTC_INVALID` until the RTC is set. Event time
remains based on the RP2040 monotonic clock throughout a running session.

### Reliable serial protocol v2

The controller boots with the existing CSV protocol for deployed-host
compatibility. A stopped host may send `PROTO,2` to activate reliable COBS +
CRC32 framing after the exact legacy `ACK_PROTO,2` response has completely
left the controller. Protocol v2 retains records in a fixed RAM queue until the
host acknowledges durable storage, supports explicit replay, and replaces
per-edge camera messages with epoch/checkpoint/stop summaries. See
[PROTOCOL_V2.md](PROTOCOL_V2.md) for the byte layout, host requirements, failure
behavior, and hardware qualification plan.

After the feeder exhausts its retry limit, it emits `FEED_JAM` and rejects new
feed requests with `NACK,FEED,JAMMED`. After physically clearing the mechanism,
send `CLEAR_JAM`; the controller responds with `ACK_CLEAR_JAM` and permits the
next feed. Rebooting also clears the latch.

The Jetson utility records synchronization samples and estimates clock drift:

```sh
uv sync --locked
uv run mousehouse-clock-sync /dev/ttyACM0 --interval 30
```

Before a deployment, the lightweight validation gate confirms that the Jetson
reports NTP synchronization and that the idle controller clock is within 1.5
seconds of Jetson UTC. It uses the three lowest-latency exchanges from a
seven-sample burst and saves a machine-readable JSON record beneath
`~/MouseHouseClockValidationLogs`:

```sh
uv run mousehouse-clock-validate /dev/ttyACM0
```

That command is read-only. If it reports `CLOCK_CORRECTION_REQUIRED`, rerun it
with explicit permission to set and recheck the RTC:

```sh
uv run mousehouse-clock-validate /dev/ttyACM0 --correct
```

The gate exits successfully only after validation passes. Firmware rejects
both clock-measurement and clock-setting commands while a session or feeder is
active. Close the PlatformIO monitor and any other serial client before using
the gate.

The development Python version is recorded in `.python-version`; the utility
supports Python 3.10 and newer. Commit `uv.lock` when dependencies change. The
virtual environment created by UV remains local and is ignored by Git.

For compatibility with older deployment commands, the wrapper script remains
available after `uv sync`:

```sh
uv run python tools/mousehouse_clock_sync.py /dev/ttyACM0 --interval 30
```

Use `--set-rtc` only while the controller is stopped and reports `RTC_INVALID`.

## Startup and hardware failures

Startup waits for a USB serial connection before completing rig initialization.
After initialization, transport output is queued without blocking acquisition.
A missing/unidentified RTC or missing MPR121 sensor queues `ERROR_NO_RTC` or
`ERROR_NO_MPR121` and remains in a
safe halted state. That state accepts only the qualification `TEST,HELLO` and
`TEST,STATUS` diagnostics, repeats the fault as `STARTUP_FAULT,<reason>`, and
rejects every other command as `NACK,STARTUP,<reason>`. An RTC with lost power or an
implausible date leaves the controller responsive to serial commands, but
`START` returns `NACK,START,RTC_INVALID` until `SET_RTC` succeeds.

The library does not impose default experiment lights. Example sketches decide
when to call things like `setMainStrip()` and `leftPokeLightOn()`.

The intended sketch shape is:

```cpp
#include <MouseHouse.h>

MouseHouse mh;

void setup() {
  mh.begin();
}

void loop() {
  mh.update();  // handles serial commands, background services, and running-state polling

  if (!mh.isRunning()) {
    return;
  }

  mh.setMainStrip(0, 0, 0, 5);
  mh.leftPokeLightOn(0, 0, 20, 0);

  if (mh.leftPokeEnded()) {
    mh.feed();
    mh.playTone(10000, 250);
  }
}
```

The `fixed_ratio` example can also be compiled as a normal installed Arduino
library example with `#include <MouseHouse.h>`. The generated
`src/camera_ttl.pio.h` is committed so Arduino builds do not need to invoke
`pioasm`; the original `src/camera_ttl.pio` remains the editable source. Select
an RP2040 board using the Arduino-Pico core.

For software regression checks, run `uv run python -m unittest discover -s tests -v`.
With `g++` installed, this also compiles the production serial transport and task
sketches against simulated hardware to exercise replay/ACK overlap, abort, and
jam recovery. Camera tests check the committed PIO instructions against the
firmware's configured timing. These checks do not validate electrical behavior.
Follow the physical feeder/LED matrix in [AGENTS.md](AGENTS.md), and additionally
check abort during the pre-feed delay, active feeding, LED sweep, and camera
run; jam recovery in go/no-go; startup without a serial connection; and camera
pulse width/period with a logic analyzer. At 30 FPS, the configured values are
1,000 µs HIGH and a 33,333 µs period.

Included now:

- `src/MouseHouse.h`
- `src/MouseHouse.cpp`
- `examples/free_feeding/free_feeding.ino`
- `examples/fixed_ratio/fixed_ratio.ino`
- `examples/go_nogo_automated/go_nogo_automated.ino`
- `examples/setup_debug/setup_debug.ino`

### Bottle swaps during a reliable session

Firmware supports bottle-swap capability version 1. The host sends newline
terminated `BOTTLE_STATUS`, `BOTTLE_PAUSE,<swap_id>`, and
`BOTTLE_RESUME,<swap_id>` commands. IDs are 32 lowercase hexadecimal characters.
Pause/resume require an active protocol-v2 session; status is also available
before a session. Results and state transitions are reliable `COMMAND_RESULT`
records containing:

```
BOTTLE_STATE,1,<id-or-none>,<ACTIVE|PAUSED|CALIBRATING|FAILED>,<left>,<right>,<monotonic_us>,<reason>
```

Counts and monotonic time are snapshots of the reported transition, including
when retrieved through status queries or retries. Global lick totals continue
through normal `DRINK_START` records. Both drink channels are suppressed while
paused, calibrating, or failed; pokes, feeder, lights, session time, camera PIO,
and other task services continue. An already-active contact is logged as
`DRINK_INTERRUPTED`, with its existing global count, rather than a synthetic
completed lick. Neither side's session counter resets. The host owns selected
side, fluid, weights, bottle history, and per-bottle counter boundaries.

Only one swap may be pending. Repeating its pause, repeating its resume while
calibrating, or retrying the latest completed resume has no side effects. A new
ID while a swap is pending is rejected. The host must serialize swaps and must
never reuse a completed ID for a later swap. Resume from `FAILED` explicitly
starts another calibration attempt for the same ID. Stopping cancels pending
calibration, clears the ID, and emits `SESSION_STOPPED`; that response is never
proof that a resume succeeded. Starting a new session clears swap state and
session counters together.

Calibration uses the MPR121's existing autoconfiguration on a stop-to-run
transition. Threshold, autoconfiguration, and debounce configuration is written
one register per service iteration. It waits at least 500 ms after entering run
mode, then requires healthy register readback and both channels released for
250 ms. A 10-second overall request deadline leaves both channels muted on failure.
The periodic automatic health reset is suspended while a swap is pending.
Before accepting subsequent lick edges, both channels must each be observed
released, preventing stale contact edges from being counted.

Compilation and native state-machine tests do not establish sensor readiness or
electrical behavior. Physical acceptance requires left-only, right-only, and
both-bottle swaps; different fluids; touch during calibration; missing/faulted
MPR121; retry and stop during calibration; uninterrupted poke/feeding behavior;
and a logic-analyzer check of continuous camera pulse width and period. Follow
with a representative 24-hour run with repeated swaps. Do not use the initial
settling/readiness constants as experimentally validated sensor guarantees.

### Behavioral sessions with camera triggers disabled

`START_BEHAVIOR` starts a normal behavioral session without enabling the camera
TTL generator. It shares the RTC, startup-fault, integrity, and already-running
checks used by `START,<fps>`, but does not require an available camera PIO state
machine. Its canonical `ACK_START_BEHAVIOR` event has FPS/count and frame period
both zero. Pokes, licks, feeding, task logic, global drink counters, and reliable
bottle swaps operate normally. No `CAMERA_EPOCH`, `CAMERA_CHECKPOINT`, or
`CAMERA_STOP` records are emitted for this mode. `STOP` leaves camera TTL LOW,
shuts down any feed, and reports `ACK_STOP` with final frame count zero.

Existing `START,<fps>` behavior and acknowledgement remain unchanged. A session
cannot switch trigger modes while running; stop first, then start the requested
mode. Each start clears prior camera IRQ counts, event counts, and bottle-swap
state. Library users can call `startSession(0, false)` for behavioral mode and
inspect `cameraTriggersEnabled()`; `isRunning()` is true for either mode.

The native session test compiles the production start/stop/update/drain methods
against observable hardware boundaries and verifies triggered → behavioral →
triggered transitions, zero behavioral frame totals, no behavioral camera
records, continued sensor polling, bottle-pause eligibility, and startup guards.
Physical acceptance must additionally verify TTL remains LOW throughout a
behavioral session and bottle swaps, then resumes normal timing in a subsequent
triggered session. Compilation does not validate electrical behavior.

Calibration failures retain both-channel muting and report a specific reason:
`CAL_STOP_VERIFY_FAILED`, `CAL_REGISTER_VERIFY_FAILED`,
`CAL_CHANNELS_NOT_RELEASED`, `CAL_SENSOR_HEALTH_TIMEOUT`, or
`CAL_READINESS_TIMEOUT`. Each failure emits one reliable, safety-priority
`BOTTLE_CAL_DIAG,1,...` diagnostic with the swap ID, stage, failing register,
expected/actual value, configuration cursor, elapsed microseconds, and captured
ECR/out-of-range/touch readings. Diagnostic values come from the original
failing read or latest readiness sample; logging does not reread hardware.
A Resume request allows up to three configuration attempts within the existing
10-second overall deadline. Stop/register verification failures retry on a later
service pass, with both lick channels muted and global counts unchanged. Each
attempt logs its captured failure, attempt number, and whether it will retry.
Persistent faults fail after the third attempt; held channels and readiness
failures retain their original deadline and never bypass checks. A later manual
resume starts a fresh bounded request.
