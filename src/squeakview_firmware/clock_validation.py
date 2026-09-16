"""Pre-run MouseHouse clock validation against an NTP-synchronized host."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Sequence

import serial

from .clock_sync import collect, positive_float, wait_for


DEFAULT_SAMPLE_COUNT = 7
DEFAULT_BEST_SAMPLE_COUNT = 3
DEFAULT_MAX_OFFSET_SECONDS = 1.5


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def _timedatectl_property(name: str) -> str:
    try:
        result = subprocess.run(
            ["timedatectl", "show", f"--property={name}", "--value"],
            check=False,
            capture_output=True,
            text=True,
            timeout=3.0,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        raise RuntimeError(f"Could not query Jetson time status: {exc}") from exc
    value = result.stdout.strip()
    if result.returncode != 0 or not value:
        detail = result.stderr.strip() or "no value returned"
        raise RuntimeError(f"Could not query Jetson time status: {detail}")
    return value


def host_time_status(
    property_reader: Callable[[str], str] = _timedatectl_property,
) -> dict[str, Any]:
    synchronized = property_reader("NTPSynchronized").lower() == "yes"
    return {
        "ntp_synchronized": synchronized,
        "timezone": property_reader("Timezone"),
        "checked_utc": datetime.now(timezone.utc).isoformat(),
    }


def sample_offset_ns(sample: dict[str, Any]) -> float:
    """Estimate controller UTC minus Jetson UTC at the exchange midpoint."""
    controller_send_ns = float(sample["controller_unix_us"]) * 1000.0
    controller_midpoint_to_send_ns = (
        float(sample["rp2040_send_us"]) - float(sample["rp2040_midpoint_us"])
    ) * 1000.0
    controller_midpoint_ns = controller_send_ns - controller_midpoint_to_send_ns
    return controller_midpoint_ns - float(sample["jetson_midpoint_ns"])


def summarize_samples(
    samples: Sequence[dict[str, Any]],
    best_sample_count: int = DEFAULT_BEST_SAMPLE_COUNT,
) -> dict[str, Any]:
    if not samples:
        raise ValueError("At least one clock sample is required")
    selected = sorted(samples, key=lambda item: int(item["round_trip_ns"]))[
        : min(best_sample_count, len(samples))
    ]
    offsets = [sample_offset_ns(sample) for sample in selected]
    return {
        "sample_count": len(samples),
        "selected_sample_count": len(selected),
        "all_rtc_valid": all(sample["rtc_status"] == "RTC_VALID" for sample in samples),
        "median_offset_ns": statistics.median(offsets),
        "median_offset_seconds": statistics.median(offsets) / 1_000_000_000.0,
        "median_round_trip_ms": statistics.median(
            float(sample["round_trip_ns"]) for sample in selected
        )
        / 1_000_000.0,
        "selected_sequences": [int(sample["sequence"]) for sample in selected],
    }


def collect_burst(
    port: Any,
    sample_count: int = DEFAULT_SAMPLE_COUNT,
    interval: float = 0.1,
    starting_sequence: int = 0,
) -> list[dict[str, Any]]:
    samples = []
    for index in range(sample_count):
        samples.append(collect(port, starting_sequence + index))
        if index + 1 < sample_count:
            time.sleep(interval)
    return samples


def set_rtc_from_host(port: Any) -> str:
    # The PCF8523 stores whole seconds. Rounding limits the initial quantization
    # error to roughly half a second before serial and I2C latency.
    epoch_seconds = int(time.time() + 0.5)
    port.write(f"SET_RTC,{epoch_seconds}\n".encode())
    return wait_for(port, "ACK_SET_RTC,")[0]


def default_output_path() -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return Path.home() / "MouseHouseClockValidationLogs" / f"clock_validation_{stamp}.json"


def validate_clock(
    port: Any,
    *,
    host_status: dict[str, Any],
    correct: bool,
    sample_count: int = DEFAULT_SAMPLE_COUNT,
    best_sample_count: int = DEFAULT_BEST_SAMPLE_COUNT,
    max_offset_seconds: float = DEFAULT_MAX_OFFSET_SECONDS,
    interval: float = 0.1,
) -> dict[str, Any]:
    record: dict[str, Any] = {
        "gate": "MouseHouse pre-run clock validation",
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "host": host_status,
        "max_offset_seconds": max_offset_seconds,
        "correction_requested": correct,
        "correction_applied": False,
    }
    if not host_status.get("ntp_synchronized", False):
        record.update(result="FAIL", reason="JETSON_NTP_NOT_SYNCHRONIZED")
        return record

    before_samples = collect_burst(port, sample_count, interval, 0)
    before = summarize_samples(before_samples, best_sample_count)
    record["before"] = before
    record["before_samples"] = before_samples

    within_tolerance = (
        before["all_rtc_valid"]
        and abs(float(before["median_offset_seconds"])) <= max_offset_seconds
    )
    if within_tolerance:
        record.update(result="PASS", reason="CLOCK_WITHIN_TOLERANCE")
        record["completed_utc"] = datetime.now(timezone.utc).isoformat()
        return record

    if not correct:
        record.update(result="FAIL", reason="CLOCK_CORRECTION_REQUIRED")
        record["completed_utc"] = datetime.now(timezone.utc).isoformat()
        return record

    record["set_rtc_ack"] = set_rtc_from_host(port)
    record["correction_applied"] = True
    after_samples = collect_burst(port, sample_count, interval, sample_count)
    after = summarize_samples(after_samples, best_sample_count)
    record["after"] = after
    record["after_samples"] = after_samples
    passed = (
        after["all_rtc_valid"]
        and abs(float(after["median_offset_seconds"])) <= max_offset_seconds
    )
    record.update(
        result="PASS" if passed else "FAIL",
        reason="CLOCK_CORRECTED_AND_VERIFIED" if passed else "CLOCK_CORRECTION_FAILED",
        completed_utc=datetime.now(timezone.utc).isoformat(),
    )
    return record


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Validate the idle MouseHouse clock before an experiment."
    )
    parser.add_argument("port", help="For example, /dev/ttyACM0")
    parser.add_argument(
        "--correct",
        action="store_true",
        help="Correct and recheck an out-of-tolerance RTC; never used during a run.",
    )
    parser.add_argument("--samples", type=positive_int, default=DEFAULT_SAMPLE_COUNT)
    parser.add_argument("--interval", type=positive_float, default=0.1)
    parser.add_argument(
        "--max-offset-seconds",
        type=positive_float,
        default=DEFAULT_MAX_OFFSET_SECONDS,
    )
    parser.add_argument("--output", type=Path, default=None)
    return parser


def main(argv: Sequence[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    output_path = args.output or default_output_path()
    try:
        status = host_time_status()
        if status["ntp_synchronized"]:
            with serial.Serial(args.port, 115200, timeout=0.25) as port:
                record = validate_clock(
                    port,
                    host_status=status,
                    correct=args.correct,
                    sample_count=args.samples,
                    max_offset_seconds=args.max_offset_seconds,
                    interval=args.interval,
                )
        else:
            record = validate_clock(
                None,
                host_status=status,
                correct=args.correct,
                sample_count=args.samples,
                max_offset_seconds=args.max_offset_seconds,
                interval=args.interval,
            )
    except (OSError, RuntimeError, TimeoutError, ValueError, serial.SerialException) as exc:
        record = {
            "gate": "MouseHouse pre-run clock validation",
            "started_utc": datetime.now(timezone.utc).isoformat(),
            "result": "FAIL",
            "reason": "VALIDATION_ERROR",
            "error": str(exc),
        }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    summary = record.get("after") or record.get("before") or {}
    offset = summary.get("median_offset_seconds")
    offset_text = "unavailable" if offset is None else f"{float(offset):+.6f} s"
    print(f"CLOCK_VALIDATION_{record['result']}")
    print(f"reason={record['reason']}")
    print(f"median_offset={offset_text}")
    print(f"record={output_path}")
    if record["result"] != "PASS":
        raise SystemExit(2)
