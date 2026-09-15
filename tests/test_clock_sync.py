import argparse
import unittest
from unittest.mock import patch

from squeakview_firmware.clock_sync import (
    collect,
    fit,
    non_negative_int,
    positive_float,
    wait_for,
)


class ResponsePort:
    def __init__(self, *responses: str):
        self.responses = [response.encode() for response in responses]

    def readline(self) -> bytes:
        return self.responses.pop(0) if self.responses else b""


class SyncPort:
    def __init__(self):
        self.command = ""

    def write(self, command: bytes) -> None:
        self.command = command.decode().strip()

    def readline(self) -> bytes:
        _, sequence, sent_ns = self.command.split(",")
        return f"CLOCK_SYNC,{sequence},{sent_ns},100,104,1700000000000000,RTC_VALID\n".encode()


class ClockSyncTests(unittest.TestCase):
    def test_fit_recovers_zero_drift_mapping(self):
        samples = [
            {"rp2040_midpoint_us": 10.0, "jetson_midpoint_ns": 11_000.0},
            {"rp2040_midpoint_us": 20.0, "jetson_midpoint_ns": 21_000.0},
            {"rp2040_midpoint_us": 30.0, "jetson_midpoint_ns": 31_000.0},
        ]

        intercept, slope, drift_ppm = fit(samples)

        self.assertAlmostEqual(intercept, 1_000.0)
        self.assertAlmostEqual(slope, 1_000.0)
        self.assertAlmostEqual(drift_ppm, 0.0)

    def test_fit_needs_two_distinct_controller_times(self):
        self.assertIsNone(fit([]))
        self.assertIsNone(
            fit(
                [
                    {"rp2040_midpoint_us": 1.0, "jetson_midpoint_ns": 1.0},
                    {"rp2040_midpoint_us": 1.0, "jetson_midpoint_ns": 2.0},
                ]
            )
        )

    @patch(
        "squeakview_firmware.clock_sync.time.time_ns",
        side_effect=[1_000_000, 1_004_000],
    )
    def test_collect_parses_a_matching_response(self, _time_ns):
        sample = collect(SyncPort(), sequence=7)

        self.assertEqual(sample["sequence"], 7)
        self.assertEqual(sample["round_trip_ns"], 4_000)
        self.assertEqual(sample["rp2040_midpoint_us"], 102.0)
        self.assertEqual(sample["rtc_status"], "RTC_VALID")

    def test_wait_for_raises_controller_nack(self):
        with self.assertRaisesRegex(RuntimeError, "INVALID_FPS"):
            wait_for(ResponsePort("NACK,START,INVALID_FPS\n"), "CLOCK_SYNC,")

    def test_numeric_argument_validation(self):
        self.assertEqual(positive_float("0.5"), 0.5)
        self.assertEqual(non_negative_int("0"), 0)
        with self.assertRaises(argparse.ArgumentTypeError):
            positive_float("0")
        with self.assertRaises(argparse.ArgumentTypeError):
            positive_float("nan")
        with self.assertRaises(argparse.ArgumentTypeError):
            positive_float("inf")
        with self.assertRaises(argparse.ArgumentTypeError):
            non_negative_int("-1")


if __name__ == "__main__":
    unittest.main()
