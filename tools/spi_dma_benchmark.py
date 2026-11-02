#!/usr/bin/env python3
"""Run paired polling/DMA SPI1 loopback checks and save benchmark evidence."""

from __future__ import annotations

import argparse
import csv
import io
import re
import sys
import time
from datetime import datetime
from pathlib import Path


LENGTHS = (16, 256, 4096)


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


def parse_rows(text: str) -> tuple[list[dict[str, int | str]], list[str]]:
    rows: list[dict[str, int | str]] = []
    failures: list[str] = []

    polling_pattern = re.compile(
        r"SPI POLLING length=(\d+) status=(\w+) cycles=(\d+) bytes_per_s=(\d+)"
    )
    dma_pattern = re.compile(
        r"SPI DMA length=(\d+) status=(\w+) transfer_cycles=(\d+) "
        r"start_api_cycles=(\d+) bytes_per_s=(\d+) foreground_iterations=(\d+)"
    )

    for match in polling_pattern.finditer(text):
        rows.append(
            {
                "mode": "polling",
                "length_bytes": int(match.group(1)),
                "status": match.group(2),
                "transfer_cycles": int(match.group(3)),
                "start_api_cycles": int(match.group(3)),
                "throughput_bytes_per_s": int(match.group(4)),
                "foreground_iterations": 0,
            }
        )

    for match in dma_pattern.finditer(text):
        rows.append(
            {
                "mode": "dma",
                "length_bytes": int(match.group(1)),
                "status": match.group(2),
                "transfer_cycles": int(match.group(3)),
                "start_api_cycles": int(match.group(4)),
                "throughput_bytes_per_s": int(match.group(5)),
                "foreground_iterations": int(match.group(6)),
            }
        )

    for mode in ("polling", "dma"):
        for length in LENGTHS:
            matching = [
                row for row in rows
                if row["mode"] == mode and row["length_bytes"] == length
            ]
            if len(matching) != 1 or matching[0]["status"] != "OK":
                failures.append(f"{mode} {length}-byte result is missing or failed")

    return rows, failures


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

        port.write(b"spi loop all\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"SPI LOOPBACK SUITE: PASS", args.timeout))

        port.write(b"spi dma all\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"SPI DMA LOOPBACK SUITE: PASS", args.timeout))

        port.write(b"stats\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"spi_recoveries=", args.timeout))

    text = raw.decode("ascii", errors="replace")
    print(text)
    rows, failures = parse_rows(text)

    if "SPI LOOPBACK SUITE: PASS" not in text:
        failures.append("polling suite did not report PASS")
    if "SPI DMA LOOPBACK SUITE: PASS" not in text:
        failures.append("DMA suite did not report PASS")
    for expected in (
        "spi_dma_transactions=3",
        "spi_dma_bytes=4368",
        "spi_dma_errors=0",
        "spi_timeouts=0",
        "spi_hw_errors=0",
    ):
        if expected not in text:
            failures.append(f"missing clean statistic: {expected}")

    dma_4096 = next(
        (row for row in rows if row["mode"] == "dma" and row["length_bytes"] == 4096),
        None,
    )
    if dma_4096 is not None and int(dma_4096["foreground_iterations"]) == 0:
        failures.append("4096-byte DMA transfer allowed no foreground iterations")

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"spi_dma_benchmark_{stamp}.log"
    csv_path = artifact_dir / f"spi_dma_benchmark_{stamp}.csv"
    log_path.write_bytes(raw)

    csv_buffer = io.StringIO(newline="")
    writer = csv.DictWriter(
        csv_buffer,
        fieldnames=(
            "mode",
            "length_bytes",
            "status",
            "transfer_cycles",
            "start_api_cycles",
            "throughput_bytes_per_s",
            "foreground_iterations",
        ),
    )
    writer.writeheader()
    writer.writerows(rows)
    csv_path.write_text(csv_buffer.getvalue(), encoding="utf-8")
    print(f"Raw evidence: {log_path}")
    print(f"CSV evidence: {csv_path}")

    if failures:
        print("SPI POLLING/DMA BENCHMARK: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("SPI POLLING/DMA BENCHMARK: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
