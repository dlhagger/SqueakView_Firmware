#!/usr/bin/env python3
"""Log Jetson/RP2040 clock samples and estimate offset and drift."""

import argparse
import csv
import statistics
import time
from pathlib import Path

import serial


def wait_for(port, prefix, timeout=3.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline().decode(errors="replace").strip()
        received_ns = time.time_ns()
        if line.startswith(prefix):
            return line, received_ns
        if line.startswith("NACK,"):
            raise RuntimeError(line)
    raise TimeoutError(f"No {prefix} response")


def collect(port, sequence):
    sent_ns = time.time_ns()
    port.write(f"TIME_SYNC,{sequence},{sent_ns}\n".encode())
    line, received_ns = wait_for(port, "CLOCK_SYNC,")
    fields = line.split(",")
    if len(fields) != 7 or int(fields[1]) != sequence or int(fields[2]) != sent_ns:
        raise ValueError(f"Malformed or mismatched response: {line}")
    receive_us, send_us = int(fields[3]), int(fields[4])
    return {
        "sequence": sequence,
        "jetson_send_ns": sent_ns,
        "jetson_receive_ns": received_ns,
        "round_trip_ns": received_ns - sent_ns,
        "rp2040_receive_us": receive_us,
        "rp2040_send_us": send_us,
        "rp2040_midpoint_us": (receive_us + send_us) / 2.0,
        "jetson_midpoint_ns": (sent_ns + received_ns) / 2.0,
        "controller_unix_us": int(fields[5]),
        "rtc_status": fields[6],
    }


def fit(samples):
    if len(samples) < 2:
        return None
    xs = [s["rp2040_midpoint_us"] for s in samples]
    ys = [s["jetson_midpoint_ns"] for s in samples]
    mean_x, mean_y = statistics.fmean(xs), statistics.fmean(ys)
    denominator = sum((x - mean_x) ** 2 for x in xs)
    if denominator == 0:
        return None
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denominator
    intercept = mean_y - slope * mean_x
    drift_ppm = (slope / 1000.0 - 1.0) * 1_000_000.0
    return intercept, slope, drift_ppm


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("port", help="For example, /dev/ttyACM0")
    parser.add_argument("--interval", type=float, default=30.0)
    parser.add_argument("--samples", type=int, default=0, help="Zero runs forever")
    parser.add_argument("--set-rtc", action="store_true")
    parser.add_argument("--csv", type=Path, default=Path("mousehouse_clock_sync.csv"))
    args = parser.parse_args()

    fields = ["sequence", "jetson_send_ns", "jetson_receive_ns", "round_trip_ns",
              "rp2040_receive_us", "rp2040_send_us", "rp2040_midpoint_us",
              "jetson_midpoint_ns", "controller_unix_us", "rtc_status"]
    history = []
    new_file = not args.csv.exists()
    with serial.Serial(args.port, 115200, timeout=0.25) as port, \
            args.csv.open("a", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        if new_file:
            writer.writeheader()
        if args.set_rtc:
            epoch = int(time.time())
            port.write(f"SET_RTC,{epoch}\n".encode())
            print(wait_for(port, "ACK_SET_RTC,")[0])

        sequence = 0
        while args.samples == 0 or sequence < args.samples:
            sample = collect(port, sequence)
            writer.writerow(sample)
            output.flush()
            history.append(sample)
            cutoff = statistics.median(s["round_trip_ns"] for s in history)
            estimate = fit([s for s in history if s["round_trip_ns"] <= cutoff])
            message = f"seq={sequence} rtt_ms={sample['round_trip_ns'] / 1e6:.3f}"
            if estimate:
                intercept, slope, drift = estimate
                message += f" drift_ppm={drift:+.3f} utc_ns={intercept:.0f}+{slope:.9f}*rp2040_us"
            print(message)
            sequence += 1
            if args.samples == 0 or sequence < args.samples:
                time.sleep(args.interval)


if __name__ == "__main__":
    main()
