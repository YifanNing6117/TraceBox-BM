#!/usr/bin/env python3
"""Exercise the complete 60-second/600k-sample PC decode/export path offline."""

from __future__ import annotations

import tempfile
import time
from pathlib import Path

from dump_validate import validate_and_export
from record_codec import SAMPLE_COUNT, build_adc_page


PAGE_COUNT = 6250
CAPTURE_ID = 0x20260914


def main() -> int:
    generated = bytearray()
    generation_started = time.monotonic()
    for page_index in range(PAGE_COUNT):
        samples = tuple((page_index + offset) & 0x0FFF for offset in range(SAMPLE_COUNT))
        generated.extend(
            build_adc_page(
                samples,
                flags=0,
                page_sequence=page_index,
                t0_us=page_index * 9600,
                sample_period_ns=100_000,
                capture_id=CAPTURE_ID,
                committed=True,
            )
        )
    generation_seconds = time.monotonic() - generation_started

    with tempfile.TemporaryDirectory() as temporary_directory:
        validation_started = time.monotonic()
        summary, outputs = validate_and_export(
            bytes(generated), Path(temporary_directory) / "full_scale"
        )
        validation_seconds = time.monotonic() - validation_started
        if (
            summary.result != "PASS"
            or summary.page_count != PAGE_COUNT
            or summary.sample_count != 600_000
            or summary.capture_id != CAPTURE_ID
            or summary.sequence_errors != 0
            or summary.crc_or_format_errors != 0
            or abs(summary.duration_seconds - 59.9999) > 0.000001
        ):
            print(f"FULL SCALE OFFLINE: FAIL {summary}")
            return 1
        csv_size = outputs[0].stat().st_size

    print(
        "FULL SCALE OFFLINE: PASS "
        f"pages={PAGE_COUNT} samples=600000 bytes={len(generated)} "
        f"duration_s={summary.duration_seconds:.4f} csv_bytes={csv_size} "
        f"generation_s={generation_seconds:.3f} validation_s={validation_seconds:.3f}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
