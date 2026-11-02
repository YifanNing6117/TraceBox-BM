from __future__ import annotations

import sys
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from record_codec import (  # noqa: E402
    COMMIT_OFFSET,
    RecordFormatError,
    build_trace_frame,
    build_trace_page,
    decode_trace_frame,
    decode_trace_page,
)


class TraceFrameCodecTests(unittest.TestCase):
    def _frame(self) -> bytes:
        return build_trace_frame(
            frame_type=1,
            source_node=2,
            severity=3,
            event_id=0x1002,
            sequence=0x01020304,
            timestamp_ms=0x10203040,
            payload=bytes.fromhex("02 06 07 62 F1 90 46 31 30 33"),
        )

    def test_frame_round_trip_and_layout(self) -> None:
        raw = self._frame()
        self.assertEqual(len(raw), 32)
        self.assertEqual(raw[:3], b"TF\x01")
        decoded = decode_trace_frame(raw)
        self.assertEqual(decoded.source_node, 2)
        self.assertEqual(decoded.event_id, 0x1002)
        self.assertEqual(decoded.sequence, 0x01020304)
        self.assertEqual(decoded.payload[:3], bytes.fromhex("02 06 07"))

    def test_frame_crc_rejects_corruption(self) -> None:
        raw = bytearray(self._frame())
        raw[17] ^= 0x01
        with self.assertRaisesRegex(RecordFormatError, "trace CRC mismatch"):
            decode_trace_frame(bytes(raw))

    def test_payload_boundary(self) -> None:
        with self.assertRaisesRegex(ValueError, "exceeds 11"):
            build_trace_frame(
                frame_type=1,
                source_node=1,
                severity=0,
                event_id=1,
                sequence=0,
                timestamp_ms=0,
                payload=b"x" * 12,
            )

    def test_trace_page_round_trip(self) -> None:
        page = build_trace_page(
            self._frame(),
            flags=0x12,
            page_sequence=7,
            timestamp_us=123_000,
            capture_id=0x54523201,
            committed=True,
        )
        decoded = decode_trace_page(page)
        self.assertEqual(decoded.page_sequence, 7)
        self.assertEqual(decoded.frame.event_id, 0x1002)

    def test_trace_page_commit_and_crc(self) -> None:
        page = build_trace_page(
            self._frame(),
            flags=0,
            page_sequence=0,
            timestamp_us=0,
            capture_id=1,
            committed=False,
        )
        self.assertEqual(page[COMMIT_OFFSET], 0xFF)
        with self.assertRaisesRegex(RecordFormatError, "not committed"):
            decode_trace_page(page)

        corrupted = bytearray(page)
        corrupted[COMMIT_OFFSET] = 0
        corrupted[40] ^= 1
        with self.assertRaisesRegex(RecordFormatError, "CRC mismatch"):
            decode_trace_page(bytes(corrupted))


if __name__ == "__main__":
    unittest.main()
