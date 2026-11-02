#!/usr/bin/env python3
"""Run a timed UART command/response soak test against TraceBox-BM."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
import time
from datetime import datetime
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM9")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=120.0, help="test time in seconds")
    parser.add_argument("--rate", type=float, default=20.0, help="ping commands per second")
    parser.add_argument("--response-timeout", type=float, default=0.25)
    return parser.parse_args()


def read_response(port: object, token: bytes, timeout_seconds: float) -> tuple[bytes, float | None]:
    start = time.monotonic()
    deadline = start + timeout_seconds
    raw = bytearray()
    token_latency_ms: float | None = None
    last_byte_at = start

    while time.monotonic() < deadline:
        waiting = int(getattr(port, "in_waiting"))
        chunk = port.read(waiting or 1)
        if chunk:
            raw.extend(chunk)
            last_byte_at = time.monotonic()
            if token_latency_ms is None and token in raw:
                token_latency_ms = (last_byte_at - start) * 1000.0
        elif token_latency_ms is not None and (time.monotonic() - last_byte_at) >= 0.015:
            break

    return bytes(raw), token_latency_ms


def read_until_quiet(port: object, timeout_seconds: float, quiet_seconds: float = 0.1) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    last_byte_at = time.monotonic()
    raw = bytearray()

    while time.monotonic() < deadline:
        waiting = int(getattr(port, "in_waiting"))
        chunk = port.read(waiting or 1)
        if chunk:
            raw.extend(chunk)
            last_byte_at = time.monotonic()
        elif raw and (time.monotonic() - last_byte_at) >= quiet_seconds:
            break

    return bytes(raw)


def extract_last_u32(text: str, key: str) -> int | None:
    values = re.findall(rf"(?:^|\s){re.escape(key)}=(\d+)", text, re.MULTILINE)
    return int(values[-1]) if values else None


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


def main() -> int:
    args = parse_args()
    if args.duration <= 0.0 or args.rate <= 0.0 or args.response_timeout <= 0.0:
        print("ERROR: duration, rate and response-timeout must be positive", file=sys.stderr)
        return 2

    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        return 2

    period = 1.0 / args.rate
    raw_log = bytearray()
    latencies_ms: list[float] = []
    sent = 0
    received = 0
    started_wall = datetime.now()
    started = time.monotonic()
    next_send = started
    next_progress = started + 10.0

    print(
        f"Opening {args.port} at {args.baud} 8N1; "
        f"duration={args.duration:.1f}s rate={args.rate:.1f}/s"
    )

    with serial.Serial(args.port, args.baud, timeout=0.01) as port:
        port.reset_input_buffer()

        while (time.monotonic() - started) < args.duration:
            now = time.monotonic()
            if now < next_send:
                time.sleep(min(next_send - now, 0.005))
                continue

            port.write(b"ping\r\n")
            port.flush()
            sent += 1
            response, latency_ms = read_response(port, b"PONG", args.response_timeout)
            raw_log.extend(response)
            if latency_ms is not None:
                received += 1
                latencies_ms.append(latency_ms)

            next_send += period
            now = time.monotonic()
            if next_send < now:
                next_send = now

            if now >= next_progress:
                print(
                    f"progress={now - started:6.1f}s sent={sent} "
                    f"received={received} lost={sent - received}"
                )
                next_progress += 10.0

        port.write(b"stats\r\n")
        port.flush()
        stats_raw = read_until_quiet(port, timeout_seconds=2.0)
        raw_log.extend(stats_raw)

    ended = time.monotonic()
    stats_text = stats_raw.decode("ascii", errors="replace")
    rx_high_water = extract_last_u32(stats_text, "rx_high_water")
    rx_overflow = extract_last_u32(stats_text, "rx_overflow")
    uart_errors = extract_last_u32(stats_text, "uart_errors")

    summary = {
        "started_at": started_wall.isoformat(timespec="seconds"),
        "port": args.port,
        "baud": args.baud,
        "requested_duration_s": args.duration,
        "actual_duration_s": round(ended - started, 3),
        "requested_rate_hz": args.rate,
        "sent": sent,
        "received": received,
        "lost": sent - received,
        "success_percent": round((received * 100.0 / sent) if sent else 0.0, 6),
        "latency_ms_min": round(min(latencies_ms), 3) if latencies_ms else None,
        "latency_ms_avg": round(sum(latencies_ms) / len(latencies_ms), 3) if latencies_ms else None,
        "latency_ms_p95": round(percentile(latencies_ms, 0.95), 3) if latencies_ms else None,
        "latency_ms_max": round(max(latencies_ms), 3) if latencies_ms else None,
        "rx_high_water": rx_high_water,
        "rx_overflow": rx_overflow,
        "uart_errors": uart_errors,
    }

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    raw_path = artifact_dir / f"uart_soak_{stamp}.log"
    json_path = artifact_dir / f"uart_soak_{stamp}.json"
    raw_path.write_bytes(raw_log)
    json_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(json.dumps(summary, indent=2, ensure_ascii=False))
    print(f"Raw evidence: {raw_path}")
    print(f"Summary evidence: {json_path}")

    failures: list[str] = []
    if sent == 0:
        failures.append("no ping command was sent")
    if received != sent:
        failures.append(f"lost {sent - received} of {sent} responses")
    if rx_overflow != 0:
        failures.append(f"rx_overflow expected 0, got {rx_overflow}")
    if uart_errors != 0:
        failures.append(f"uart_errors expected 0, got {uart_errors}")

    if failures:
        print("UART SOAK TEST: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("UART SOAK TEST: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
