from __future__ import annotations

import sys
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from record_codec import (  # noqa: E402
    COMMIT_OFFSET,
    RecordFormatError,
    build_adc_page,
    crc32_mpeg2,
    decode_adc_page,
)


def build_vector(*, committed: bool) -> bytes:
    samples = tuple(((index * 37) + 11) & 0x0FFF for index in range(96))
    return build_adc_page(
        samples,
        flags=0xA5,
        page_sequence=0x01020304,
        t0_us=0x10203040,
        sample_period_ns=100_000,
        capture_id=0x55667788,
        committed=committed,
    )


class RecordCodecTests(unittest.TestCase):
    def test_standard_crc_vector(self) -> None:
        self.assertEqual(crc32_mpeg2(b"123456789"), 0x0376E6E7)

    def test_round_trip(self) -> None:
        decoded = decode_adc_page(build_vector(committed=True))
        self.assertEqual(decoded.page_sequence, 0x01020304)
        self.assertEqual(decoded.samples[0], 11)
        self.assertEqual(decoded.samples[-1], ((95 * 37) + 11) & 0x0FFF)

    def test_uncommitted_rejected(self) -> None:
        page = build_vector(committed=False)
        self.assertEqual(page[COMMIT_OFFSET], 0xFF)
        with self.assertRaisesRegex(RecordFormatError, "not committed"):
            decode_adc_page(page)

    def test_corruption_rejected(self) -> None:
        page = bytearray(build_vector(committed=True))
        page[32] ^= 0x01
        with self.assertRaisesRegex(RecordFormatError, "CRC mismatch"):
            decode_adc_page(bytes(page))

    def test_sample_boundary_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "expected 96"):
            build_adc_page(
                [0] * 95,
                flags=0,
                page_sequence=0,
                t0_us=0,
                sample_period_ns=100_000,
                capture_id=0,
            )


if __name__ == "__main__":
    unittest.main()
