#!/usr/bin/env python3
"""Run the 200-page ADC-to-W25Q64 end-to-end recorder acceptance test."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from datetime import datetime
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM9")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=30.0)
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


def main() -> int:
    args = parse_args()
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        return 2

    started = time.monotonic()
    with serial.Serial(args.port, args.baud, timeout=0.02) as port:
        port.reset_input_buffer()
        port.write(b"record selftest\r\n")
        port.flush()
        raw = read_until_token(port, b"RECORDER SELFTEST:", args.timeout)
    wall_seconds = time.monotonic() - started

    text = raw.decode("ascii", errors="replace")
    print(text)
    failures: list[str] = []
    metrics: dict[str, int | float | str] = {"wall_seconds": wall_seconds}

    header_match = re.search(
        r"RECORDER state=(\d+).*?capture_id=0x([0-9A-F]{8}) target_pages=(\d+) "
        r"sectors_required=(\d+) sectors_prepared=(\d+)",
        text,
    )
    counts_match = re.search(
        r"adc_blocks=(\d+) pages_packed=(\d+) pages_committed=(\d+) "
        r"pages_verified=(\d+) capture_ms=(\d+)",
        text,
    )
    error_match = re.search(
        r"sequence_errors=(\d+) page_crc_errors=(\d+) flash_errors=(\d+) "
        r"pack_max_cycles=(\d+)",
        text,
    )
    buffer_match = re.search(
        r"page_pool_hwm=(\d+) page_pool_overflow=(\d+) "
        r"adc_descriptor_hwm=(\d+) adc_descriptor_overflow=(\d+) "
        r"adc_dma_errors=(\d+) adc_overruns=(\d+)",
        text,
    )

    groups = (
        (
            header_match,
            ("state", "capture_id", "target_pages", "sectors_required", "sectors_prepared"),
        ),
        (
            counts_match,
            ("adc_blocks", "pages_packed", "pages_committed", "pages_verified", "capture_ms"),
        ),
        (
            error_match,
            ("sequence_errors", "page_crc_errors", "flash_errors", "pack_max_cycles"),
        ),
        (
            buffer_match,
            (
                "page_pool_high_water",
                "page_pool_overflow",
                "adc_descriptor_high_water",
                "adc_descriptor_overflow",
                "adc_dma_errors",
                "adc_overruns",
            ),
        ),
    )
    for match, names in groups:
        if match is None:
            failures.append(f"missing metric group beginning with {names[0]}")
            continue
        for name, value in zip(names, match.groups()):
            metrics[name] = value if name == "capture_id" else int(value)

    expected = {
        "state": 5,
        "capture_id": "20260913",
        "target_pages": 200,
        "sectors_required": 13,
        "sectors_prepared": 13,
        "adc_blocks": 200,
        "pages_packed": 200,
        "pages_committed": 200,
        "pages_verified": 200,
        "sequence_errors": 0,
        "page_crc_errors": 0,
        "flash_errors": 0,
        "page_pool_overflow": 0,
        "adc_descriptor_overflow": 0,
        "adc_dma_errors": 0,
        "adc_overruns": 0,
    }
    for name, expected_value in expected.items():
        if metrics.get(name) != expected_value:
            failures.append(
                f"{name}={metrics.get(name)!r}, expected {expected_value!r}"
            )

    if counts_match is not None and not (1900 <= int(metrics["capture_ms"]) <= 1970):
        failures.append("capture duration is outside 1.90..1.97 seconds")
    if error_match is not None and not (1 <= int(metrics["pack_max_cycles"]) < 1_612_800):
        failures.append("page packing maximum is zero or exceeds one ADC block period")
    if buffer_match is not None:
        pool_hwm = int(metrics["page_pool_high_water"])
        descriptor_hwm = int(metrics["adc_descriptor_high_water"])
        if not (1 <= pool_hwm < 8):
            failures.append("page-pool high water is outside 1..7")
        if not (1 <= descriptor_hwm <= 4):
            failures.append("ADC descriptor high water is outside 1..4")

    if "RECORDER completion_status=OK" not in text:
        failures.append("recorder completion status is not OK")
    if "RECORDER SELFTEST: PASS" not in text:
        failures.append("firmware did not report RECORDER SELFTEST: PASS")

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"recorder_selftest_{stamp}.log"
    json_path = artifact_dir / f"recorder_selftest_{stamp}.json"
    log_path.write_bytes(raw)
    json_path.write_text(
        json.dumps({"metrics": metrics, "failures": failures}, indent=2),
        encoding="utf-8",
    )
    print(f"Raw evidence: {log_path}")
    print(f"JSON evidence: {json_path}")

    if failures:
        print("RECORDER AUTOMATION: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("RECORDER AUTOMATION: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
