#!/usr/bin/env python3
"""Validate a TraceBox binary page dump and export samples plus a report."""

from __future__ import annotations

import argparse
import csv
import json
from dataclasses import asdict, dataclass
from pathlib import Path

from record_codec import (
    PAGE_SIZE,
    RECORD_TYPE_ADC,
    RECORD_TYPE_TRACE,
    RecordFormatError,
    decode_adc_page,
    decode_trace_page,
)


SOURCE_NAMES = {1: "TESTER", 2: "ECU"}
EVENT_NAMES = {
    0x0001: "BOOT",
    0x1001: "CAN_TX",
    0x1002: "CAN_RX",
    0x3001: "CAN_ERROR",
    0x4001: "TX_QUEUE_OVERFLOW",
    0x4002: "RX_QUEUE_OVERFLOW",
}


@dataclass(frozen=True)
class DumpSummary:
    result: str
    record_type: str
    page_count: int
    sample_count: int
    event_count: int
    capture_id: int | None
    sequence_errors: int
    trace_sequence_errors: int
    crc_or_format_errors: int
    first_bad_page: int | None
    sample_minimum: int | None
    sample_maximum: int | None
    sample_average: float | None
    duration_seconds: float
    source_counts: dict[str, int]
    event_counts: dict[str, int]


def validate_and_export(raw: bytes, output_stem: Path) -> tuple[DumpSummary, list[Path]]:
    if len(raw) % PAGE_SIZE:
        raise RecordFormatError(
            f"dump length {len(raw)} is not a multiple of {PAGE_SIZE}"
        )

    output_stem.parent.mkdir(parents=True, exist_ok=True)
    csv_path = output_stem.with_suffix(".csv")
    json_path = output_stem.with_suffix(".json")
    markdown_path = output_stem.with_suffix(".md")
    page_count = len(raw) // PAGE_SIZE
    capture_id: int | None = None
    sequence_errors = 0
    format_errors = 0
    first_bad_page: int | None = None
    sample_count = 0
    sample_sum = 0
    sample_minimum: int | None = None
    sample_maximum: int | None = None
    last_time_us = 0.0
    first_time_ms: int | None = None
    last_time_ms: int | None = None
    event_count = 0
    trace_sequence_errors = 0
    source_next_sequence: dict[int, int] = {}
    source_counts: dict[str, int] = {}
    event_counts: dict[str, int] = {}
    record_type_code = raw[5] if page_count else 0
    record_type = {
        RECORD_TYPE_ADC: "ADC",
        RECORD_TYPE_TRACE: "TRACE",
    }.get(record_type_code, "UNKNOWN")

    with csv_path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        if record_type_code == RECORD_TYPE_TRACE:
            writer.writerow(
                (
                    "page_sequence",
                    "capture_id",
                    "timestamp_ms",
                    "source",
                    "severity",
                    "event_id",
                    "event",
                    "trace_sequence",
                    "payload_hex",
                    "can_id",
                    "can_dlc",
                    "can_data_hex",
                    "page_crc32",
                    "frame_crc32",
                )
            )
        else:
            writer.writerow(
                ("sample_index", "page_sequence", "sample_in_page", "time_us", "adc_raw")
            )
        for page_index in range(page_count):
            page_raw = raw[page_index * PAGE_SIZE : (page_index + 1) * PAGE_SIZE]
            if page_raw[5] != record_type_code:
                format_errors += 1
                if first_bad_page is None:
                    first_bad_page = page_index
                continue
            try:
                page = (
                    decode_trace_page(page_raw)
                    if record_type_code == RECORD_TYPE_TRACE
                    else decode_adc_page(page_raw)
                )
            except RecordFormatError:
                format_errors += 1
                if first_bad_page is None:
                    first_bad_page = page_index
                continue

            if page.page_sequence != page_index:
                sequence_errors += 1
                if first_bad_page is None:
                    first_bad_page = page_index
            if capture_id is None:
                capture_id = page.capture_id
            elif page.capture_id != capture_id:
                sequence_errors += 1
                if first_bad_page is None:
                    first_bad_page = page_index

            if record_type_code == RECORD_TYPE_TRACE:
                frame = page.frame
                source_name = SOURCE_NAMES.get(frame.source_node, f"NODE_{frame.source_node}")
                event_name = EVENT_NAMES.get(frame.event_id, f"EVENT_0x{frame.event_id:04X}")
                source_counts[source_name] = source_counts.get(source_name, 0) + 1
                event_counts[event_name] = event_counts.get(event_name, 0) + 1
                if frame.source_node in source_next_sequence:
                    if frame.sequence != source_next_sequence[frame.source_node]:
                        trace_sequence_errors += 1
                        if first_bad_page is None:
                            first_bad_page = page_index
                source_next_sequence[frame.source_node] = frame.sequence + 1
                first_time_ms = (
                    frame.timestamp_ms if first_time_ms is None else min(first_time_ms, frame.timestamp_ms)
                )
                last_time_ms = (
                    frame.timestamp_ms if last_time_ms is None else max(last_time_ms, frame.timestamp_ms)
                )
                can_id = ""
                can_dlc = ""
                can_data = ""
                if frame.event_id in (0x1001, 0x1002) and len(frame.payload) >= 3:
                    parsed_can_id = frame.payload[0] | (frame.payload[1] << 8)
                    parsed_dlc = min(frame.payload[2], 8)
                    can_id = f"0x{parsed_can_id:03X}"
                    can_dlc = str(parsed_dlc)
                    can_data = frame.payload[3 : 3 + parsed_dlc].hex(" ").upper()
                writer.writerow(
                    (
                        page.page_sequence,
                        f"0x{page.capture_id:08X}",
                        frame.timestamp_ms,
                        source_name,
                        frame.severity,
                        f"0x{frame.event_id:04X}",
                        event_name,
                        frame.sequence,
                        frame.payload.hex(" ").upper(),
                        can_id,
                        can_dlc,
                        can_data,
                        f"0x{page.page_crc32:08X}",
                        f"0x{frame.crc32:08X}",
                    )
                )
                event_count += 1
            else:
                period_us = page.sample_period_ns / 1000.0
                for sample_in_page, sample in enumerate(page.samples):
                    absolute_index = (page_index * len(page.samples)) + sample_in_page
                    time_us = page.t0_us + (sample_in_page * period_us)
                    writer.writerow(
                        (absolute_index, page.page_sequence, sample_in_page, f"{time_us:.3f}", sample)
                    )
                    sample_count += 1
                    sample_sum += sample
                    sample_minimum = sample if sample_minimum is None else min(sample_minimum, sample)
                    sample_maximum = sample if sample_maximum is None else max(sample_maximum, sample)
                    last_time_us = time_us

    result = "PASS" if (
        sequence_errors == 0
        and trace_sequence_errors == 0
        and format_errors == 0
        and record_type_code in (RECORD_TYPE_ADC, RECORD_TYPE_TRACE)
    ) else "FAIL"
    summary = DumpSummary(
        result=result,
        record_type=record_type,
        page_count=page_count,
        sample_count=sample_count,
        event_count=event_count,
        capture_id=capture_id,
        sequence_errors=sequence_errors,
        trace_sequence_errors=trace_sequence_errors,
        crc_or_format_errors=format_errors,
        first_bad_page=first_bad_page,
        sample_minimum=sample_minimum,
        sample_maximum=sample_maximum,
        sample_average=(sample_sum / sample_count) if sample_count else None,
        duration_seconds=(
            ((last_time_ms - first_time_ms) / 1000.0)
            if event_count and first_time_ms is not None and last_time_ms is not None
            else ((last_time_us / 1_000_000.0) if sample_count else 0.0)
        ),
        source_counts=source_counts,
        event_counts=event_counts,
    )
    json_path.write_text(
        json.dumps(asdict(summary), indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    markdown_path.write_text(
        "# TraceBox dump validation\n\n"
        f"- Result: **{summary.result}**\n"
        f"- Record type: {summary.record_type}\n"
        f"- Pages: {summary.page_count}\n"
        f"- Samples: {summary.sample_count}\n"
        f"- Events: {summary.event_count}\n"
        f"- Capture ID: "
        f"{('0x%08X' % summary.capture_id) if summary.capture_id is not None else 'N/A'}\n"
        f"- Sequence/capture errors: {summary.sequence_errors}\n"
        f"- Trace source sequence errors: {summary.trace_sequence_errors}\n"
        f"- CRC/format errors: {summary.crc_or_format_errors}\n"
        f"- First bad page: "
        f"{summary.first_bad_page if summary.first_bad_page is not None else 'none'}\n"
        f"- ADC min/max/average: {summary.sample_minimum} / {summary.sample_maximum} / "
        f"{summary.sample_average if summary.sample_average is not None else 'N/A'}\n"
        f"- Reconstructed duration: {summary.duration_seconds:.6f} s\n"
        f"- Source counts: {summary.source_counts}\n"
        f"- Event counts: {summary.event_counts}\n",
        encoding="utf-8",
    )
    return summary, [csv_path, json_path, markdown_path]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="raw .tbdump file")
    parser.add_argument("--output-stem", type=Path)
    parser.add_argument(
        "--inject-bit-flip",
        action="store_true",
        help="flip one payload bit to prove CRC rejection",
    )
    args = parser.parse_args()

    raw = bytearray(args.dump.read_bytes())
    if args.inject_bit_flip:
        if len(raw) < PAGE_SIZE:
            raise SystemExit("cannot inject a bit flip into an empty dump")
        raw[32] ^= 0x01
    output_stem = args.output_stem or args.dump.with_suffix("")
    summary, outputs = validate_and_export(bytes(raw), output_stem)
    print(json.dumps(asdict(summary), indent=2, ensure_ascii=False))
    for output in outputs:
        print(f"artifact={output.resolve()}")
    return 0 if summary.result == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
