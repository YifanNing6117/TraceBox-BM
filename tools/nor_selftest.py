#!/usr/bin/env python3
"""Exercise W25Q64 erase/program/read/CRC and timeout recovery on hardware."""

from __future__ import annotations

import argparse
import json
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
    parser.add_argument("--timeout", type=float, default=8.0)
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


def expected_crc() -> int:
    samples = tuple(((index * 53) + 7) & 0x0FFF for index in range(96))
    page = build_adc_page(
        samples,
        flags=0x5A,
        page_sequence=0x11223344,
        t0_us=0x55667788,
        sample_period_ns=100_000,
        capture_id=0xA1B2C3D4,
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
        port.write(b"nor selftest\r\n")
        port.flush()
        raw = read_until_token(port, b"NOR SELFTEST:", args.timeout)

    text = raw.decode("ascii", errors="replace")
    print(text)
    failures: list[str] = []
    metrics: dict[str, int | str] = {}

    id_match = re.search(
        r"nor_jedec manufacturer=0x([0-9A-F]{2}) memory_type=0x([0-9A-F]{2}) "
        r"capacity_code=0x([0-9A-F]{2}) capacity_bytes=(\d+) status1=0x([0-9A-F]{2})",
        text,
    )
    result_match = re.search(
        r"NOR SELFTEST scratch_address=0x([0-9A-F]{8}) erased_not_ff=(\d+) "
        r"mismatches=(\d+) page_crc=0x([0-9A-F]{8}) "
        r"calculated_crc=0x([0-9A-F]{8})",
        text,
    )
    operation_match = re.search(
        r"program_operations=(\d+) programmed_bytes=(\d+) reads=(\d+) "
        r"read_bytes=(\d+) erases=(\d+) status_polls=(\d+)",
        text,
    )
    recovery_match = re.search(
        r"timeout_observed=(\d+) driver_timeouts=(\d+) recoveries=(\d+) "
        r"spi_errors=(\d+) wel_errors=(\d+)",
        text,
    )

    if id_match is None:
        failures.append("JEDEC metrics are missing")
    else:
        manufacturer, memory_type, capacity_code, capacity, status1 = id_match.groups()
        metrics.update(
            manufacturer_id=manufacturer,
            memory_type=memory_type,
            capacity_code=capacity_code,
            capacity_bytes=int(capacity),
            final_status1=status1,
        )
        if (manufacturer, memory_type, capacity_code, int(capacity)) != (
            "EF",
            "40",
            "17",
            8_388_608,
        ):
            failures.append("JEDEC ID is not W25Q64 EF 40 17")

    if result_match is None:
        failures.append("erase/readback/CRC metrics are missing")
    else:
        scratch, erased_not_ff, mismatches, page_crc, calculated_crc = (
            result_match.groups()
        )
        metrics.update(
            scratch_address=f"0x{scratch}",
            erased_not_ff=int(erased_not_ff),
            mismatches=int(mismatches),
            page_crc=f"0x{page_crc}",
            calculated_crc=f"0x{calculated_crc}",
        )
        pc_crc = expected_crc()
        mcu_crc = int(page_crc, 16)
        if int(scratch, 16) != 0x007FF000:
            failures.append("scratch sector is not the final W25Q64 sector")
        if int(erased_not_ff) != 0 or int(mismatches) != 0:
            failures.append("erase or byte-exact readback failed")
        if mcu_crc != int(calculated_crc, 16) or mcu_crc != pc_crc:
            failures.append(
                f"CRC mismatch: page=0x{mcu_crc:08X}, "
                f"read=0x{int(calculated_crc, 16):08X}, PC=0x{pc_crc:08X}"
            )
        print(f"CRC cross-check: MCU=0x{mcu_crc:08X} PC=0x{pc_crc:08X}")

    if operation_match is None:
        failures.append("NOR operation counters are missing")
    else:
        names = (
            "program_operations",
            "programmed_bytes",
            "reads",
            "read_bytes",
            "erases",
            "status_polls",
        )
        metrics.update(
            {name: int(value) for name, value in zip(names, operation_match.groups())}
        )
        for name, expected in {
            "program_operations": 2,
            "programmed_bytes": 256,
            "reads": 2,
            "read_bytes": 512,
            "erases": 1,
        }.items():
            if metrics.get(name) != expected:
                failures.append(f"{name}={metrics.get(name)!r}, expected {expected}")
        if int(metrics["status_polls"]) < 4:
            failures.append("too few status polls to prove WEL/WIP state handling")

    if recovery_match is None:
        failures.append("timeout/recovery metrics are missing")
    else:
        names = (
            "timeout_observed",
            "driver_timeouts",
            "recoveries",
            "spi_errors",
            "wel_errors",
        )
        metrics.update(
            {name: int(value) for name, value in zip(names, recovery_match.groups())}
        )
        for name, expected in {
            "timeout_observed": 1,
            "driver_timeouts": 1,
            "recoveries": 1,
            "spi_errors": 0,
            "wel_errors": 0,
        }.items():
            if metrics.get(name) != expected:
                failures.append(f"{name}={metrics.get(name)!r}, expected {expected}")

    if "NOR SELFTEST: PASS" not in text:
        failures.append("firmware did not report NOR SELFTEST: PASS")

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"nor_selftest_{stamp}.log"
    json_path = artifact_dir / f"nor_selftest_{stamp}.json"
    log_path.write_bytes(raw)
    json_path.write_text(
        json.dumps({"metrics": metrics, "failures": failures}, indent=2),
        encoding="utf-8",
    )
    print(f"Raw evidence: {log_path}")
    print(f"JSON evidence: {json_path}")

    if failures:
        print("NOR SELFTEST AUTOMATION: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("NOR SELFTEST AUTOMATION: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
