#!/usr/bin/env python3
"""Run MCU RAM ring/page/CRC self-test and cross-check its CRC on the PC."""

from __future__ import annotations

import argparse
import re
import sys
import time
from datetime import datetime
from pathlib import Path

from record_codec import build_adc_page, decode_adc_page


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
            found = found or token in raw
        elif found and (time.monotonic() - last_data) >= 0.05:
            break
    return bytes(raw)


def expected_page_crc() -> int:
    samples = tuple(((index * 37) + 11) & 0x0FFF for index in range(96))
    page = build_adc_page(
        samples,
        flags=0xA5,
        page_sequence=0x01020304,
        t0_us=0x10203040,
        sample_period_ns=100_000,
        capture_id=0x55667788,
        committed=True,
    )
    return decode_adc_page(page).page_crc32


def main() -> int:
    args = parse_args()
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        return 2

    with serial.Serial(args.port, args.baud, timeout=0.02) as port:
        port.reset_input_buffer()
        port.write(b"core selftest\r\n")
        port.flush()
        raw = read_until_token(port, b"CORE SELFTEST:", args.timeout)

    text = raw.decode("ascii", errors="replace")
    print(text)
    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"core_selftest_{stamp}.log"
    log_path.write_bytes(raw)
    print(f"Raw evidence: {log_path}")

    failures: list[str] = []
    for expected in (
        "core_status=OK failure_step=0",
        "event_pushed=32 event_popped=32 event_high_water=32 event_dropped_newest=1",
        "pool_in_use_high_water=8 pool_ready_high_water=8 pool_overflow=1 pool_ownership_errors=0",
        "crc_corruption_rejected=1 restored_page_valid=1",
        "CORE SELFTEST: PASS",
    ):
        if expected not in text:
            failures.append(f"missing expected result: {expected}")

    crc_match = re.search(r"page_crc=0x([0-9A-Fa-f]{8})", text)
    if crc_match is None:
        failures.append("MCU page CRC is missing")
    else:
        mcu_crc = int(crc_match.group(1), 16)
        pc_crc = expected_page_crc()
        print(f"CRC cross-check: MCU=0x{mcu_crc:08X} PC=0x{pc_crc:08X}")
        if mcu_crc != pc_crc:
            failures.append(
                f"MCU/PC CRC mismatch: MCU=0x{mcu_crc:08X} PC=0x{pc_crc:08X}"
            )

    if failures:
        print("CORE SELFTEST AUTOMATION: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("CORE SELFTEST AUTOMATION: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
