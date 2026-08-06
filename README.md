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

`MouseHouse` is meant to separate reusable rig control from experiment-specific
task logic:

- the library owns hardware, logging, session control, and sensor state
- the sketch owns contingencies like "when a left poke ends, deliver a pellet"

The library owns:

- session control and serial `START,<fps>` / `STOP` / `FEED,<steps>` commands
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

`TIME_SYNC` is read-only and may be used during a session to estimate clock
offset and drift. `SET_RTC` is accepted only while the session and feeder are
stopped. If the DS3231 reports lost power or an implausible date, `START` is
rejected with `NACK,START,RTC_INVALID` until the RTC is set. Event time remains
based on the RP2040 monotonic clock throughout a running session.

The Jetson utility records synchronization samples and estimates clock drift:

```sh
python3 -m pip install -r tools/requirements.txt
python3 tools/mousehouse_clock_sync.py /dev/ttyACM0 --interval 30
```

Use `--set-rtc` only while the controller is stopped and reports `RTC_INVALID`.

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

The `fixed_ratio` example is intended to be compiled as a normal installed Arduino library example with `#include <MouseHouse.h>`.

Included now:

- `src/MouseHouse.h`
- `src/MouseHouse.cpp`
- `examples/free_feeding/free_feeding.ino`
- `examples/fixed_ratio/fixed_ratio.ino`
- `examples/go_nogo_automated/go_nogo_automated.ino`
- `examples/setup_debug/setup_debug.ino`
