#!/usr/bin/env python3
"""Run the TraceBox register-level SPI1 polling loopback acceptance test."""

from __future__ import annotations

import argparse
import re
import sys
import time
from datetime import datetime
from pathlib import Path


EXPECTED_LENGTHS = (16, 256, 4096)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM9")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
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
        port.write(b"\r\nspi info\r\nspi loop all\r\nstats\r\n")
        port.flush()
        raw = read_until_quiet(port, time.monotonic() + args.timeout)

    text = raw.decode("ascii", errors="replace")
    print(text)

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"spi_polling_loopback_{stamp}.log"
    log_path.write_bytes(raw)
    print(f"Raw evidence: {log_path}")

    failures: list[str] = []
    if "spi1_mode=master,mode0,8bit,msb-first" not in text:
        failures.append("SPI mode/configuration response is missing")
    if "sck_hz=1312500" not in text:
        failures.append("SPI SCK is not the expected 1.3125 MHz baseline")

    for length in EXPECTED_LENGTHS:
        pattern = rf"SPI POLLING length={length} status=OK cycles=\d+ bytes_per_s=\d+"
        if re.search(pattern, text) is None:
            failures.append(f"{length}-byte polling loopback did not pass")

    if "SPI LOOPBACK SUITE: PASS" not in text:
        failures.append("loopback suite did not report PASS")
    if "spi_timeouts=0" not in text:
        failures.append("SPI timeout count is non-zero or stats are missing")
    if "spi_hw_errors=0" not in text:
        failures.append("SPI hardware error count is non-zero or stats are missing")

    if failures:
        print("SPI POLLING LOOPBACK: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("Check that PA7 is connected directly to PA6 and retry.", file=sys.stderr)
        return 1

    print("SPI POLLING LOOPBACK: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
