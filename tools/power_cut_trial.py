#!/usr/bin/env python3
"""Arm one record run, observe a manual power cut, and archive boot recovery."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


CONTROL_BAUD = 115_200
RECORD_START_PATTERN = re.compile(
    rb"RECORD start status=([A-Z_]+)[^\r\n]*(?:\r\n|\n)"
)
RECOVERY_PATTERN = re.compile(
    rb"RECOVERY RESULT status=([A-Z_]+) source=BOOT "
    rb"valid_pages=(\d+) scanned_pages=(\d+) rejected_torn_pages=(\d+) "
    rb"sequence_errors=(\d+) crc_errors=(\d+) capture_id=(0x[0-9A-Fa-f]+)"
)


@dataclass(frozen=True)
class PowerCutResult:
    result: str
    trial: int
    requested_seconds: int
    target_pages: int
    recovery_status: str | None
    valid_pages: int | None
    scanned_pages: int | None
    rejected_torn_pages: int | None
    sequence_errors: int | None
    crc_errors: int | None
    capture_id: int | None
    failure: str | None


def pages_for_seconds(seconds: int) -> int:
    return (seconds * 10_000) // 96


def evaluate_recovery(raw: bytes, trial: int, seconds: int) -> PowerCutResult:
    target_pages = pages_for_seconds(seconds)
    match = RECOVERY_PATTERN.search(raw)
    if not match:
        return PowerCutResult(
            "FAIL", trial, seconds, target_pages,
            None, None, None, None, None, None, None,
            "boot recovery result was not observed",
        )

    status = match.group(1).decode("ascii")
    valid_pages = int(match.group(2))
    scanned_pages = int(match.group(3))
    rejected_torn_pages = int(match.group(4))
    sequence_errors = int(match.group(5))
    crc_errors = int(match.group(6))
    capture_id = int(match.group(7), 16)

    failures: list[str] = []
    if status != "OK":
        failures.append(f"recovery status is {status}")
    if not 0 < valid_pages < target_pages:
        failures.append(
            f"valid_pages must prove a mid-record cut (1..{target_pages - 1}), "
            f"got {valid_pages}"
        )
    if scanned_pages != valid_pages + 1:
        failures.append(
            f"expected one boundary page after valid prefix, got "
            f"valid={valid_pages} scanned={scanned_pages}"
        )
    if rejected_torn_pages not in (0, 1):
        failures.append(f"unexpected rejected_torn_pages={rejected_torn_pages}")
    if sequence_errors != 0 or crc_errors != 0:
        failures.append(
            f"sequence/crc errors must be zero, got {sequence_errors}/{crc_errors}"
        )

    return PowerCutResult(
        "PASS" if not failures else "FAIL",
        trial,
        seconds,
        target_pages,
        status,
        valid_pages,
        scanned_pages,
        rejected_torn_pages,
        sequence_errors,
        crc_errors,
        capture_id,
        "; ".join(failures) if failures else None,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=int, default=15, choices=range(2, 61))
    parser.add_argument("--trial", type=int, required=True, choices=range(1, 100))
    parser.add_argument("--timeout", type=float, default=90.0)
    return parser.parse_args()


def archive(raw: bytes, result: PowerCutResult) -> tuple[Path, Path]:
    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    stem = artifact_dir / f"power_cut_trial_{result.trial}_{stamp}"
    log_path = stem.with_suffix(".log")
    json_path = stem.with_suffix(".json")
    log_path.write_bytes(raw)
    json_path.write_text(
        json.dumps(asdict(result), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return log_path, json_path


def main() -> int:
    args = parse_args()
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        return 2

    raw = bytearray()
    start_seen = False
    recovery_seen_at: float | None = None
    deadline = time.monotonic() + args.timeout

    print(
        f"Opening {args.port}; trial={args.trial}, record={args.seconds}s. "
        "Keep CH340 GND and RX<-PA9 connected.",
        flush=True,
    )
    try:
        with serial.Serial(args.port, CONTROL_BAUD, timeout=0.05) as port:
            port.reset_input_buffer()
            port.write(f"record {args.seconds}\r\n".encode("ascii"))
            port.flush()

            while time.monotonic() < deadline:
                waiting = port.in_waiting
                chunk = port.read(waiting or 1)
                if chunk:
                    raw.extend(chunk)
                    sys.stdout.write(chunk.decode("ascii", errors="replace"))
                    sys.stdout.flush()

                    if not start_seen:
                        start_match = RECORD_START_PATTERN.search(raw)
                        if start_match:
                            if start_match.group(1) != b"OK":
                                print("\nERROR: record start was rejected", file=sys.stderr)
                                break
                            start_seen = True
                            print(
                                "\nPOWER_CUT_ARMED: disconnect CH340 TX->PA10 now; "
                                "then remove the board Type-C for 1..3 seconds and "
                                "reconnect it. Do not unplug CH340 USB/GND/RX.\a",
                                flush=True,
                            )

                    if RECOVERY_PATTERN.search(raw):
                        recovery_seen_at = time.monotonic()
                elif recovery_seen_at is not None and (
                    time.monotonic() - recovery_seen_at
                ) >= 0.5:
                    break
    except (OSError, serial.SerialException) as error:
        print(f"\nSERIAL ERROR: {error}", file=sys.stderr)

    result = evaluate_recovery(bytes(raw), args.trial, args.seconds)
    log_path, json_path = archive(bytes(raw), result)
    print("\n" + json.dumps(asdict(result), indent=2, ensure_ascii=False), flush=True)
    print(f"artifact={log_path.resolve()}", flush=True)
    print(f"artifact={json_path.resolve()}", flush=True)
    return 0 if result.result == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
