# SqueakView Firmware Guardrails

These instructions apply to the entire repository. Read them before changing
firmware, build configuration, or hardware-facing code.

## Hardware invariants

This firmware targets the existing Adafruit Feather RP2040 MouseHouse control
board. Pin assignments and electrical behavior are hardware constraints, not
cleanup opportunities. Do not rename, remap, invert, combine, or abstract
hardware-control paths without explicit approval and a test on the physical rig.

The critical assignments in `src/MouseHouse.h` are:

- GPIO 13: shared feeder motor-enable and NeoPixel circuitry control
- A3, A2, GPIO 24, GPIO 25: feeder stepper inputs, in that order
- A1: NeoPixel data
- A0: camera TTL pulse
- GPIO 0: active-low pellet sensor
- GPIO 5 and GPIO 6: poke inputs
- GPIO 1: buzzer
- GPIO 9: house light

### GPIO 13 is shared and sequencing-sensitive

GPIO 13 controls circuitry used by both the feeder and the LEDs. The known-good
behavior is intentionally direct and must remain as follows:

1. Initialize GPIO 13 as an output and drive it `LOW` in `MouseHouse::begin()`.
2. Drive GPIO 13 `HIGH` at the end of `startFeedRun()`.
3. Do not rewrite or recompute GPIO 13 inside `serviceFeed()` between steps.
4. In `feedStop()`, first mark feeding inactive, disable all four stepper inputs,
   and then drive GPIO 13 `LOW`.
5. `robustShow()` may call only the NeoPixel initialization/show operations. It
   must not drive GPIO 13, add a settling delay, or restore a derived enable
   state.
6. Turning an indicator on drives GPIO 13 `HIGH` before updating/showing pixels.
7. Turning an indicator off shows the updated pixels first, then drives GPIO 13
   `LOW` only when no indicator channels remain on.

Do not replace this with a cached shared-enable flag or an expression such as
`feedActive || anyIndicatorsOn()`. That abstraction was tested on this hardware
and broke feeder operation. In particular, do not make `feedStop()` leave GPIO
13 high merely because an indicator is logically on.

If feeding regresses after an LED or feeder change, inspect GPIO 13 sequencing
first. Do not immediately change the motor direction, retry logic, or pellet
sensor polarity.

## Feeder behavior that is intentional

- The `Stepper` constructor pin order is A3, A2, GPIO 24, GPIO 25.
- The preferred initial step direction is `-1`.
- Feeding is serviced non-blockingly, one step at a time, by `serviceFeed()`.
- A pass with no pellet reverses the preferred direction and retries.
- Retry direction flipping and serial `FEED_RETRY` messages are expected.
- The pellet sensor is active low.
- A feed stops immediately when the pellet sensor triggers.
- After the configured retry limit, the firmware logs a jam and stops safely.

Preserve these behaviors unless the user explicitly requests a behavioral
change and confirms it against the rig.

## USB and serial behavior

- Startup intentionally waits for the USB serial connection before completing
  rig initialization. Do not remove this wait as a generic startup cleanup.
- Normal USB upload uses the CDC serial device, typically `/dev/ttyACM0`, to
  request the RP2040 bootloader. A board running compatible firmware can
  normally be flashed over USB without manually holding BOOTSEL.
- The serial device disappears and may return under a different name when the
  RP2040 reboots. If upload and verification finish with `OK` but the subsequent
  monitor cannot reopen the old port, the flash succeeded; start the monitor as
  a separate step on the re-enumerated port.
- Prefer separate PlatformIO upload and monitor commands. Do not treat monitor
  failure after a verified flash as firmware upload failure.
- A blank board or firmware without working USB reset support may still require
  one manual BOOTSEL recovery flash.
- Preserve the existing serial command and event formats unless a coordinated
  host-side protocol change is part of the task.

## Timing and startup safety

- Camera TTL generation uses RP2040 PIO to avoid timing disruption. Preserve
  that implementation unless hardware timing is measured after a replacement.
- `src/camera_ttl.pio` is the editable PIO source;
  `src/camera_ttl.pio.h` is committed for Arduino builds and must be regenerated
  when the PIO source changes.
- Event time during a session is anchored to the RP2040 monotonic clock. Do not
  replace it with repeated live RTC reads.
- Missing RTC or MPR121 hardware intentionally leaves the controller in a safe
  halted or command-responsive state as documented in `README.md`.
- An invalid RTC intentionally blocks `START` until `SET_RTC` succeeds.

## Required process for hardware-facing changes

Before editing feeder, LED, sensor, camera, startup, or serial code:

1. Read the complete affected path, including initialization, update/service,
   shutdown, serial command handling, and the relevant example sketches.
2. Preserve the invariants above unless the requested change explicitly
   supersedes them.
3. Keep changes surgical. Do not mix hardware behavior changes with broad
   refactors.
4. Review the diff specifically for pin writes, write ordering, polarity,
   blocking delays, and changes to repeated service calls.
5. Run `git diff --check`.
6. Compile every PlatformIO environment:

   ```sh
   pio run -e fixed_ratio
   pio run -e free_feeding
   pio run -e go_nogo_automated
   pio run -e setup_debug
   ```

7. State clearly that compilation does not validate electrical behavior.
8. For any change touching the critical paths, request or perform physical-rig
   checks before calling the behavioral fix confirmed.

The minimum physical test matrix for feeder/LED changes is:

- feeder with all indicators off
- feeder while the main strip is on
- feeder while each poke indicator is on
- successful pellet detection and feed stop
- missing-pellet retry with direction reversal
- jam stop after the retry limit
- LED updates before and after a feed
- USB upload followed by a separately started serial monitor

## Source-control hygiene

- Do not restore old files wholesale over `src/MouseHouse.cpp` or
  `src/MouseHouse.h`; doing so can discard newer RTC, PIO camera, and serial
  safety work. Port only the required known-good behavior.
- Do not commit `.pio/`, `.venv/`, device logs, generated runtime data, or local
  editor state.
- The Python host utility uses UV. Keep `pyproject.toml` and `uv.lock` together;
  do not recreate `requirements.txt` unless the project deliberately changes
  dependency-management strategy.
- Preserve unrelated user changes in a dirty worktree.
