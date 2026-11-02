#!/usr/bin/env python3
"""Send one bounded read-only TraceBox command and archive its UART response."""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path


COMMANDS = {
    "info": b"info\r\n",
    "metrics": b"record metrics\r\n",
    "stats": b"stats\r\n",
    "adc-info": b"adc info\r\n",
    "nor-info": b"nor info\r\n",
    "trace-ingress": b"trace ingress\r\n",
    "trace-metrics": b"trace metrics\r\n",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--command", choices=tuple(COMMANDS), required=True)
    parser.add_argument("--timeout", type=float, default=3.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        return 2

    raw = bytearray()
    with serial.Serial(args.port, args.baud, timeout=0.02) as port:
        port.reset_input_buffer()
        port.write(COMMANDS[args.command])
        port.flush()
        deadline = time.monotonic() + args.timeout
        last_data = time.monotonic()
        while time.monotonic() < deadline:
            waiting = port.in_waiting
            chunk = port.read(waiting or 1)
            if chunk:
                raw.extend(chunk)
                last_data = time.monotonic()
            elif raw and (time.monotonic() - last_data) >= 0.15:
                break

    text = raw.decode("ascii", errors="replace")
    print(text)
    if not raw:
        print("ERROR: device returned no data", file=sys.stderr)
        return 1

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"device_{args.command}_{stamp}.log"
    log_path.write_bytes(raw)
    print(f"Raw evidence: {log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
