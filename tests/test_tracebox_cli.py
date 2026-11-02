from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from tracebox_cli import (  # noqa: E402
    START_STATUS_PATTERN,
    read_exact,
    read_line_until,
)


class FakePort:
    def __init__(self, data: bytes) -> None:
        self.data = bytearray(data)

    @property
    def in_waiting(self) -> int:
        return len(self.data)

    def read(self, length: int) -> bytes:
        chunk = bytes(self.data[:length])
        del self.data[:length]
        return chunk


class TraceBoxCliTests(unittest.TestCase):
    def test_start_status_waits_for_complete_serial_line(self) -> None:
        self.assertIsNone(
            START_STATUS_PATTERN.search(b"PREPARE start status=O")
        )
        match = START_STATUS_PATTERN.search(
            b"PREPARE start status=OK seconds=3 target_pages=312\r\n"
        )
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), b"OK")

    def test_dump_preamble_after_command_text(self) -> None:
        port = FakePort(
            b"dump 460800\r\nDUMP start status=OK\r\n> "
            b"TB_DUMP_BEGIN 6250 256 460800\r\n"
        )
        pattern = re.compile(rb"TB_DUMP_BEGIN\s+(\d+)\s+(256)\s+(\d+)\r?\n")
        raw, match = read_line_until(port, pattern, 0.1)
        self.assertIn(b"DUMP start status=OK", raw)
        self.assertEqual(match.group(1), b"6250")
        self.assertEqual(match.group(3), b"460800")

    def test_exact_binary_read_preserves_control_bytes(self) -> None:
        payload = bytes(range(256)) * 2
        port = FakePort(payload + b"trailer")
        self.assertEqual(read_exact(port, len(payload), 0.1), payload)
        self.assertEqual(port.read(7), b"trailer")


if __name__ == "__main__":
    unittest.main()
