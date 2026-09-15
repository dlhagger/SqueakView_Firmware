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

Useful wizard commands are:

```text
TEST,STATUS
TEST,YES
TEST,NO
TEST,RETRY
TEST,ABORT
TEST,RESTART
```

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
acquisition. If the DS3231 reports lost power or an implausible date, `START`
is rejected with `NACK,START,RTC_INVALID` until the RTC is set. Event time
remains based on the RP2040 monotonic clock throughout a running session.

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

Startup intentionally waits for a USB serial connection before initializing the
rig. A missing DS3231 RTC or MPR121 sensor then prints `ERROR_NO_RTC` or
`ERROR_NO_MPR121` and halts in a safe state. An RTC with lost power or an
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

Included now:

- `src/MouseHouse.h`
- `src/MouseHouse.cpp`
- `examples/free_feeding/free_feeding.ino`
- `examples/fixed_ratio/fixed_ratio.ino`
- `examples/go_nogo_automated/go_nogo_automated.ino`
- `examples/setup_debug/setup_debug.ino`
