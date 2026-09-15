import argparse
import unittest
from unittest.mock import patch

from squeakview_firmware.clock_sync import (
    collect,
    fit,
    non_negative_int,
    parse_clock_response,
    positive_float,
    wait_for,
)
from squeakview_firmware.clock_validation import (
    host_time_status,
    sample_offset_ns,
    summarize_samples,
    validate_clock,
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

    def test_parse_clock_response_rejects_wrong_sequence(self):
        with self.assertRaisesRegex(ValueError, "mismatched"):
            parse_clock_response(
                "CLOCK_SYNC,8,1000000,100,104,1700000000000000,RTC_VALID",
                sequence=7,
                sent_ns=1_000_000,
                received_ns=1_004_000,
            )

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


class ClockValidationTests(unittest.TestCase):
    @staticmethod
    def sample(sequence, offset_ns=0, round_trip_ns=2_000_000):
        jetson_midpoint_ns = 1_000_000_000.0 + sequence * 100_000_000.0
        rp2040_midpoint_us = 100.0 + sequence * 100_000.0
        rp2040_send_us = rp2040_midpoint_us + 4.0
        controller_midpoint_ns = jetson_midpoint_ns + offset_ns
        controller_unix_us = int(controller_midpoint_ns / 1000.0 + 4.0)
        return {
            "sequence": sequence,
            "jetson_send_ns": int(jetson_midpoint_ns - round_trip_ns / 2),
            "jetson_receive_ns": int(jetson_midpoint_ns + round_trip_ns / 2),
            "round_trip_ns": round_trip_ns,
            "rp2040_receive_us": int(rp2040_midpoint_us - 4.0),
            "rp2040_send_us": int(rp2040_send_us),
            "rp2040_midpoint_us": rp2040_midpoint_us,
            "jetson_midpoint_ns": jetson_midpoint_ns,
            "controller_unix_us": controller_unix_us,
            "rtc_status": "RTC_VALID",
        }

    def test_host_time_status_requires_ntp_yes(self):
        values = {"NTPSynchronized": "yes", "Timezone": "America/New_York"}
        status = host_time_status(values.__getitem__)
        self.assertTrue(status["ntp_synchronized"])
        self.assertEqual(status["timezone"], "America/New_York")

    def test_sample_offset_aligns_controller_send_time_to_midpoint(self):
        self.assertEqual(sample_offset_ns(self.sample(0)), 0.0)
        self.assertEqual(sample_offset_ns(self.sample(0, offset_ns=2_000_000)), 2_000_000.0)

    def test_summary_uses_lowest_latency_samples(self):
        samples = [
            self.sample(0, offset_ns=9_000_000, round_trip_ns=20_000_000),
            self.sample(1, offset_ns=1_000_000, round_trip_ns=1_000_000),
            self.sample(2, offset_ns=2_000_000, round_trip_ns=2_000_000),
        ]
        summary = summarize_samples(samples, best_sample_count=2)
        self.assertEqual(summary["selected_sequences"], [1, 2])
        self.assertAlmostEqual(summary["median_offset_seconds"], 0.0015)

    @patch("squeakview_firmware.clock_validation.collect_burst")
    def test_gate_fails_without_permission_to_correct(self, collect_burst):
        collect_burst.return_value = [self.sample(index, offset_ns=2_000_000_000) for index in range(3)]
        record = validate_clock(
            object(),
            host_status={"ntp_synchronized": True},
            correct=False,
            sample_count=3,
            max_offset_seconds=1.5,
        )
        self.assertEqual(record["result"], "FAIL")
        self.assertEqual(record["reason"], "CLOCK_CORRECTION_REQUIRED")

    @patch("squeakview_firmware.clock_validation.set_rtc_from_host", return_value="ACK_SET_RTC")
    @patch("squeakview_firmware.clock_validation.collect_burst")
    def test_gate_corrects_then_verifies(self, collect_burst, set_rtc):
        collect_burst.side_effect = [
            [self.sample(index, offset_ns=2_000_000_000) for index in range(3)],
            [self.sample(index, offset_ns=100_000_000) for index in range(3, 6)],
        ]
        record = validate_clock(
            object(),
            host_status={"ntp_synchronized": True},
            correct=True,
            sample_count=3,
            max_offset_seconds=1.5,
        )
        self.assertEqual(record["result"], "PASS")
        self.assertTrue(record["correction_applied"])
        set_rtc.assert_called_once()


if __name__ == "__main__":
    unittest.main()
