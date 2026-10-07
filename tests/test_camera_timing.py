"""Cycle-check the committed camera PIO instructions with firmware FIFO values.

This covers digital instruction timing, not oscillator accuracy, IRQ latency,
or electrical behavior; those still require a physical measurement.
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def simulate_camera(fifo, edges=4):
    header = (ROOT / "src/camera_ttl.pio.h").read_text()
    instructions = header.split("camera_ttl_program_instructions[] = {", 1)[1]
    instructions = instructions.split("};", 1)[0]
    program = [int(word, 16) for word in re.findall(r"0x[0-9a-f]{4}", instructions)]
    wrap = int(re.search(r"#define camera_ttl_wrap (\d+)", header)[1])
    wrap_target = int(re.search(r"#define camera_ttl_wrap_target (\d+)", header)[1])
    registers = {1: 0, 2: 0, 6: 0, 7: 0}  # X, Y, ISR, OSR
    fifo = iter(fifo)
    pc = tick = pin = 0
    rises, falls = [], []
    for _ in range(1000):
        instruction = program[pc]
        opcode = instruction >> 13
        next_pc = pc + 1 if pc != wrap else wrap_target
        cycles = 1 + ((instruction >> 8) & 31)
        if opcode == 4:  # PULL BLOCK, FIFO is preloaded before enabling the SM.
            assert instruction == 0x80A0
            registers[7] = next(fifo)
        elif opcode == 5:  # MOV register, register (no inversion/reversal).
            assert instruction & 0x18 == 0
            registers[(instruction >> 5) & 7] = registers[instruction & 7]
        elif opcode == 7:  # SET PINS, immediate.
            assert (instruction >> 5) & 7 == 0
            value = instruction & 31
            if value != pin:
                (rises if value else falls).append(tick)
            pin = value
        elif opcode == 6:  # IRQ NOWAIT: one cycle regardless of CPU service.
            assert instruction == 0xC000
        elif opcode == 0:  # JMP X--/Y-- to itself, count + 1 iterations.
            condition = (instruction >> 5) & 7
            assert condition in (2, 4) and instruction & 31 == pc
            register = 1 if condition == 2 else 2
            cycles *= registers[register] + 1
            registers[register] = 0xFFFFFFFF
        else:
            raise AssertionError(f"Unsupported instruction: {instruction:04x}")
        tick += cycles
        pc = next_pc
        if len(falls) == edges:
            return rises, falls
    raise AssertionError("Camera program did not produce the requested pulses")


class CameraTimingTests(unittest.TestCase):
    def test_fifo_values_produce_advertised_pulse_period_and_start_delay(self):
        source = (ROOT / "src/MouseHouse.cpp").read_text()
        start = source.split("void MouseHouse::startCameraPio()", 1)[1]
        start = start.split("void MouseHouse::snapshotCameraIrq", 1)[0]
        expressions = re.findall(
            r"pio_sm_put_blocking\(cameraPio_, \(uint\)cameraSm_, ([^;]+)\);", start
        )
        self.assertEqual(len(expressions), 3)
        header = (ROOT / "src/MouseHouse.h").read_text()
        pulse = int(re.search(r"kPulseWidthUs = (\d+)ULL", header)[1])
        for fps in (1, 24, 30, 60, 120):
            with self.subTest(fps=fps):
                period = 1_000_000 // fps
                values = dict(highTicks=pulse, periodTicks=period,
                              lowTicks=period - pulse)
                # Evaluate only the firmware's integer FIFO expressions.
                fifo = [eval(re.sub(r"(\d+)U\b", r"\1", expression),
                             {"__builtins__": {}}, values)
                        for expression in expressions]
                rises, falls = simulate_camera(fifo)
                self.assertEqual(rises[0], period)
                self.assertEqual([fall - rise for rise, fall in zip(rises, falls)],
                                 [pulse] * len(falls))
                self.assertEqual([b - a for a, b in zip(rises, rises[1:])],
                                 [period] * (len(rises) - 1))
