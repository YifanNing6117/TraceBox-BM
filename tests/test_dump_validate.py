from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from dump_validate import validate_and_export  # noqa: E402
from record_codec import build_adc_page, build_trace_frame, build_trace_page  # noqa: E402


def page(sequence: int, capture_id: int = 0x20260914) -> bytes:
    return build_adc_page(
        [sequence + index for index in range(96)],
        flags=0,
        page_sequence=sequence,
        t0_us=sequence * 9600,
        sample_period_ns=100_000,
        capture_id=capture_id,
        committed=True,
    )


class DumpValidateTests(unittest.TestCase):
    def test_two_page_export(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            stem = Path(directory) / "capture"
            summary, outputs = validate_and_export(page(0) + page(1), stem)
            self.assertEqual(summary.result, "PASS")
            self.assertEqual(summary.page_count, 2)
            self.assertEqual(summary.sample_count, 192)
            self.assertEqual(summary.sequence_errors, 0)
            self.assertEqual(summary.crc_or_format_errors, 0)
            self.assertAlmostEqual(summary.duration_seconds, 0.0191)
            self.assertTrue(all(output.is_file() for output in outputs))

    def test_sequence_gap_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            summary, _ = validate_and_export(
                page(0) + page(2), Path(directory) / "gap"
            )
            self.assertEqual(summary.result, "FAIL")
            self.assertEqual(summary.sequence_errors, 1)
            self.assertEqual(summary.first_bad_page, 1)

    def test_crc_corruption_is_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            raw = bytearray(page(0))
            raw[32] ^= 0x01
            summary, _ = validate_and_export(bytes(raw), Path(directory) / "bad")
            self.assertEqual(summary.result, "FAIL")
            self.assertEqual(summary.crc_or_format_errors, 1)
            self.assertEqual(summary.first_bad_page, 0)

    def test_trace_timeline_export(self) -> None:
        frames = []
        for page_sequence, (source, source_sequence, event_id) in enumerate(
            ((1, 7, 0x1001), (2, 20, 0x1002), (1, 8, 0x4001))
        ):
            frame = build_trace_frame(
                frame_type=1,
                source_node=source,
                severity=1,
                event_id=event_id,
                sequence=source_sequence,
                timestamp_ms=100 + page_sequence,
                payload=bytes.fromhex("01 06 08 07 62 F1 90 46 31 30 33")
                if event_id in (0x1001, 0x1002)
                else b"",
            )
            frames.append(
                build_trace_page(
                    frame,
                    flags=0,
                    page_sequence=page_sequence,
                    timestamp_us=(100 + page_sequence) * 1000,
                    capture_id=0x54523201,
                    committed=True,
                )
            )

        with tempfile.TemporaryDirectory() as directory:
            summary, outputs = validate_and_export(
                b"".join(frames), Path(directory) / "timeline"
            )
            self.assertEqual(summary.result, "PASS")
            self.assertEqual(summary.record_type, "TRACE")
            self.assertEqual(summary.event_count, 3)
            self.assertEqual(summary.trace_sequence_errors, 0)
            self.assertEqual(summary.source_counts, {"TESTER": 2, "ECU": 1})
            self.assertTrue(all(output.is_file() for output in outputs))


if __name__ == "__main__":
    unittest.main()
