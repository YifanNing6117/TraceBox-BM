#!/usr/bin/env python3
"""Automated USART1 acceptance check for the TraceBox Day 1 firmware."""

from __future__ import annotations

import argparse
import re
import sys
import time
from datetime import datetime
from pathlib import Path


EXPECTED_SYSCLK_HZ = 168_000_000
DWT_MIN_CYCLES_10MS = 1_600_000
DWT_MAX_CYCLES_10MS = 1_760_000


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM5")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=3.0)
    return parser.parse_args()


def read_until_quiet(port: object, deadline: float, quiet_seconds: float = 0.25) -> bytes:
    chunks: list[bytes] = []
    last_data = time.monotonic()

    while time.monotonic() < deadline:
        waiting = int(getattr(port, "in_waiting"))
        chunk = port.read(waiting or 1)
        if chunk:
            chunks.append(chunk)
            last_data = time.monotonic()
        elif chunks and (time.monotonic() - last_data) >= quiet_seconds:
            break

    return b"".join(chunks)


def extract_u32(text: str, key: str) -> int | None:
    match = re.search(rf"(?:^|\s){re.escape(key)}=(\d+)", text, re.MULTILINE)
    return int(match.group(1)) if match else None


def main() -> int:
    args = parse_args()
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        return 2

    print(f"Opening {args.port} at {args.baud} 8N1...")
    with serial.Serial(args.port, args.baud, timeout=0.05) as port:
        port.reset_input_buffer()
        port.write(b"\r\nping\r\ninfo\r\ndwt\r\nstats\r\n")
        port.flush()
        raw = read_until_quiet(port, time.monotonic() + args.timeout)

    text = raw.decode("ascii", errors="replace")
    print(text)

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"bringup_serial_{stamp}.log"
    log_path.write_bytes(raw)
    print(f"Raw evidence: {log_path}")

    failures: list[str] = []
    if "PONG" not in text:
        failures.append("ping did not return PONG")

    sysclk = extract_u32(text, "sysclk_hz")
    if sysclk != EXPECTED_SYSCLK_HZ:
        failures.append(f"sysclk_hz expected {EXPECTED_SYSCLK_HZ}, got {sysclk}")

    dwt_values = [int(value) for value in re.findall(r"dwt_10ms_cycles=(\d+)", text)]
    if not dwt_values:
        failures.append("no dwt_10ms_cycles value found")
    elif not any(DWT_MIN_CYCLES_10MS <= value <= DWT_MAX_CYCLES_10MS for value in dwt_values):
        failures.append(f"DWT values outside accepted range: {dwt_values}")

    if "rx_overflow=0" not in text:
        failures.append("UART RX overflow is non-zero or stats response is missing")

    if failures:
        print("SMOKE TEST: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("SMOKE TEST: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
