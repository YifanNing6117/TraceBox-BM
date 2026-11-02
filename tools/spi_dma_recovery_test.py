#!/usr/bin/env python3
"""Force a DMA deadline, then prove TraceBox can immediately transfer again."""

from __future__ import annotations

import argparse
import re
import sys
import time
from datetime import datetime
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM9")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=5.0)
    return parser.parse_args()


def read_until_token(port: object, token: bytes, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    last_data = time.monotonic()
    raw = bytearray()
    found = False

    while time.monotonic() < deadline:
        waiting = int(getattr(port, "in_waiting"))
        chunk = port.read(waiting or 1)
        if chunk:
            raw.extend(chunk)
            last_data = time.monotonic()
            found = found or (token in raw)
        elif found and (time.monotonic() - last_data) >= 0.05:
            break

    return bytes(raw)


def main() -> int:
    args = parse_args()
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        return 2

    raw = bytearray()
    print(f"Opening {args.port} at {args.baud} 8N1...")
    with serial.Serial(args.port, args.baud, timeout=0.02) as port:
        port.reset_input_buffer()

        port.write(b"spi dma timeout\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"status=TIMEOUT", args.timeout))

        port.write(b"spi dma 4096\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"status=OK", args.timeout))

        port.write(b"stats\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"spi_recoveries=", args.timeout))

    text = raw.decode("ascii", errors="replace")
    print(text)

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"spi_dma_recovery_{stamp}.log"
    log_path.write_bytes(raw)
    print(f"Raw evidence: {log_path}")

    failures: list[str] = []
    if re.search(r"SPI DMA length=4096 status=TIMEOUT transfer_cycles=\d+", text) is None:
        failures.append("forced DMA transfer did not publish TIMEOUT")
    if re.search(r"SPI DMA length=4096 status=OK transfer_cycles=\d+", text) is None:
        failures.append("post-recovery 4096-byte DMA transfer did not pass")
    for expected in (
        "spi_dma_errors=1",
        "spi_timeouts=1",
        "spi_hw_errors=0",
        "spi_recoveries=1",
    ):
        if expected not in text:
            failures.append(f"missing expected recovery statistic: {expected}")

    if failures:
        print("SPI DMA RECOVERY TEST: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("SPI DMA RECOVERY TEST: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
