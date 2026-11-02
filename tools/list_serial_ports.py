#!/usr/bin/env python3
"""Print available serial ports as JSON for the localhost console."""

from __future__ import annotations

import json
import sys


def main() -> int:
    try:
        from serial.tools import list_ports  # type: ignore
    except ImportError:
        print(json.dumps({"error": "pyserial is missing", "ports": []}))
        return 2

    ports = [
        {
            "device": port.device,
            "description": port.description,
            "hwid": port.hwid,
        }
        for port in sorted(list_ports.comports(), key=lambda item: item.device)
    ]
    print(json.dumps({"ports": ports}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
