from __future__ import annotations

import sys
import unittest
from pathlib import Path


TOOLS_DIR = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS_DIR))

from power_cut_trial import evaluate_recovery, pages_for_seconds  # noqa: E402


class PowerCutTrialTests(unittest.TestCase):
    def test_clean_boundary_recovery_passes(self) -> None:
        raw = (
            b"RECOVERY RESULT status=OK source=BOOT valid_pages=327 "
            b"scanned_pages=328 rejected_torn_pages=0 sequence_errors=0 "
            b"crc_errors=0 capture_id=0x20260914\r\n"
        )
        result = evaluate_recovery(raw, trial=1, seconds=15)
        self.assertEqual("PASS", result.result)
        self.assertEqual(327, result.valid_pages)

    def test_one_torn_tail_recovery_passes(self) -> None:
        raw = (
            b"RECOVERY RESULT status=OK source=BOOT valid_pages=511 "
            b"scanned_pages=512 rejected_torn_pages=1 sequence_errors=0 "
            b"crc_errors=0 capture_id=0x20260914\r\n"
        )
        result = evaluate_recovery(raw, trial=2, seconds=15)
        self.assertEqual("PASS", result.result)
        self.assertEqual(1, result.rejected_torn_pages)

    def test_completed_record_is_not_a_mid_record_cut(self) -> None:
        target = pages_for_seconds(15)
        raw = (
            f"RECOVERY RESULT status=OK source=BOOT valid_pages={target} "
            f"scanned_pages={target + 1} rejected_torn_pages=0 "
            "sequence_errors=0 crc_errors=0 capture_id=0x20260914\r\n"
        ).encode("ascii")
        result = evaluate_recovery(raw, trial=3, seconds=15)
        self.assertEqual("FAIL", result.result)
        self.assertIn("mid-record cut", result.failure or "")

    def test_missing_recovery_fails(self) -> None:
        result = evaluate_recovery(b"record started\r\n", trial=4, seconds=15)
        self.assertEqual("FAIL", result.result)
        self.assertIn("not observed", result.failure or "")


if __name__ == "__main__":
    unittest.main()
