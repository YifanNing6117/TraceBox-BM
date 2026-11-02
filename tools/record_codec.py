#!/usr/bin/env python3
"""TraceBox 256-byte ADC page codec shared by CLI and localhost tooling."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Sequence


PAGE_SIZE = 256
HEADER_SIZE = 32
SAMPLE_COUNT = 96
PAYLOAD_SIZE = 192
CRC_COVERAGE = 224
CRC_OFFSET = 24
COMMIT_OFFSET = 255
MAGIC = b"TBP1"
FORMAT_VERSION = 1
RECORD_TYPE_ADC = 1
RECORD_TYPE_TRACE = 2
TRACE_FRAME_SIZE = 32
TRACE_FRAME_CRC_OFFSET = 28
TRACE_FRAME_PAYLOAD_CAPACITY = 11
TRACE_FRAME_MAGIC = b"TF"
TRACE_FRAME_VERSION = 1


class RecordFormatError(ValueError):
    """A page violates the fixed TraceBox format or integrity contract."""


@dataclass(frozen=True)
class AdcPage:
    flags: int
    page_sequence: int
    t0_us: int
    sample_period_ns: int
    capture_id: int
    page_crc32: int
    samples: tuple[int, ...]


@dataclass(frozen=True)
class TraceFrame:
    frame_type: int
    source_node: int
    severity: int
    event_id: int
    sequence: int
    timestamp_ms: int
    payload: bytes
    crc32: int


@dataclass(frozen=True)
class TraceEventPage:
    flags: int
    page_sequence: int
    timestamp_us: int
    capture_id: int
    page_crc32: int
    frame: TraceFrame


def crc32_mpeg2(data: bytes) -> int:
    """CRC-32/MPEG-2: poly=0x04C11DB7, init=0xffffffff, no reflection/xor."""

    crc = 0xFFFFFFFF
    for value in data:
        crc ^= value << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def _page_crc(page: bytes | bytearray) -> int:
    covered = bytearray(page[:CRC_COVERAGE])
    covered[CRC_OFFSET : CRC_OFFSET + 4] = b"\x00\x00\x00\x00"
    return crc32_mpeg2(bytes(covered))


def build_trace_frame(
    *,
    frame_type: int,
    source_node: int,
    severity: int,
    event_id: int,
    sequence: int,
    timestamp_ms: int,
    payload: bytes = b"",
) -> bytes:
    for name, value, maximum in (
        ("frame_type", frame_type, 0xFF),
        ("source_node", source_node, 0xFF),
        ("severity", severity, 0xFF),
        ("event_id", event_id, 0xFFFF),
        ("sequence", sequence, 0xFFFFFFFF),
        ("timestamp_ms", timestamp_ms, 0xFFFFFFFF),
    ):
        if not 0 <= value <= maximum:
            raise ValueError(f"{name} must fit its serialized field")
    if len(payload) > TRACE_FRAME_PAYLOAD_CAPACITY:
        raise ValueError(
            f"trace payload exceeds {TRACE_FRAME_PAYLOAD_CAPACITY} bytes"
        )

    frame = bytearray(TRACE_FRAME_SIZE)
    frame[0:2] = TRACE_FRAME_MAGIC
    struct.pack_into(
        "<BBBBHII",
        frame,
        2,
        TRACE_FRAME_VERSION,
        frame_type,
        source_node,
        severity,
        event_id,
        sequence,
        timestamp_ms,
    )
    frame[16] = len(payload)
    frame[17 : 17 + len(payload)] = payload
    struct.pack_into("<I", frame, TRACE_FRAME_CRC_OFFSET, crc32_mpeg2(frame[:28]))
    return bytes(frame)


def decode_trace_frame(raw_frame: bytes) -> TraceFrame:
    if len(raw_frame) != TRACE_FRAME_SIZE:
        raise RecordFormatError(
            f"trace frame size must be {TRACE_FRAME_SIZE}, got {len(raw_frame)}"
        )
    if raw_frame[0:2] != TRACE_FRAME_MAGIC:
        raise RecordFormatError("invalid trace frame magic")
    version, frame_type, source_node, severity, event_id, sequence, timestamp_ms = (
        struct.unpack_from("<BBBBHII", raw_frame, 2)
    )
    if version != TRACE_FRAME_VERSION:
        raise RecordFormatError("unsupported trace frame version")
    payload_length = raw_frame[16]
    if payload_length > TRACE_FRAME_PAYLOAD_CAPACITY:
        raise RecordFormatError("invalid trace payload length")
    stored_crc = struct.unpack_from("<I", raw_frame, TRACE_FRAME_CRC_OFFSET)[0]
    calculated_crc = crc32_mpeg2(raw_frame[:TRACE_FRAME_CRC_OFFSET])
    if stored_crc != calculated_crc:
        raise RecordFormatError(
            f"trace CRC mismatch: stored=0x{stored_crc:08X} "
            f"calculated=0x{calculated_crc:08X}"
        )
    return TraceFrame(
        frame_type=frame_type,
        source_node=source_node,
        severity=severity,
        event_id=event_id,
        sequence=sequence,
        timestamp_ms=timestamp_ms,
        payload=bytes(raw_frame[17 : 17 + payload_length]),
        crc32=stored_crc,
    )


def build_adc_page(
    samples: Sequence[int],
    *,
    flags: int,
    page_sequence: int,
    t0_us: int,
    sample_period_ns: int,
    capture_id: int,
    committed: bool = False,
) -> bytes:
    if len(samples) != SAMPLE_COUNT:
        raise ValueError(f"expected {SAMPLE_COUNT} samples, got {len(samples)}")
    if sample_period_ns <= 0:
        raise ValueError("sample_period_ns must be positive")
    if not 0 <= flags <= 0xFF:
        raise ValueError("flags must fit uint8")
    for name, value in (
        ("page_sequence", page_sequence),
        ("t0_us", t0_us),
        ("sample_period_ns", sample_period_ns),
        ("capture_id", capture_id),
    ):
        if not 0 <= value <= 0xFFFFFFFF:
            raise ValueError(f"{name} must fit uint32")
    if any(not 0 <= sample <= 0xFFFF for sample in samples):
        raise ValueError("every sample must fit uint16")

    page = bytearray(b"\xFF" * PAGE_SIZE)
    page[0:4] = MAGIC
    struct.pack_into(
        "<BBBBIIIHHII",
        page,
        4,
        FORMAT_VERSION,
        RECORD_TYPE_ADC,
        HEADER_SIZE,
        flags,
        page_sequence,
        t0_us,
        sample_period_ns,
        SAMPLE_COUNT,
        PAYLOAD_SIZE,
        0,
        capture_id,
    )
    struct.pack_into(f"<{SAMPLE_COUNT}H", page, HEADER_SIZE, *samples)
    struct.pack_into("<I", page, CRC_OFFSET, _page_crc(page))
    page[COMMIT_OFFSET] = 0x00 if committed else 0xFF
    return bytes(page)


def build_trace_page(
    raw_frame: bytes,
    *,
    flags: int,
    page_sequence: int,
    timestamp_us: int,
    capture_id: int,
    committed: bool = False,
) -> bytes:
    decode_trace_frame(raw_frame)
    for name, value, maximum in (
        ("flags", flags, 0xFF),
        ("page_sequence", page_sequence, 0xFFFFFFFF),
        ("timestamp_us", timestamp_us, 0xFFFFFFFF),
        ("capture_id", capture_id, 0xFFFFFFFF),
    ):
        if not 0 <= value <= maximum:
            raise ValueError(f"{name} must fit its serialized field")

    page = bytearray(b"\xFF" * PAGE_SIZE)
    page[0:4] = MAGIC
    struct.pack_into(
        "<BBBBIIIHHII",
        page,
        4,
        FORMAT_VERSION,
        RECORD_TYPE_TRACE,
        HEADER_SIZE,
        flags,
        page_sequence,
        timestamp_us,
        0,
        1,
        TRACE_FRAME_SIZE,
        0,
        capture_id,
    )
    page[HEADER_SIZE : HEADER_SIZE + TRACE_FRAME_SIZE] = raw_frame
    struct.pack_into("<I", page, CRC_OFFSET, _page_crc(page))
    page[COMMIT_OFFSET] = 0x00 if committed else 0xFF
    return bytes(page)


def decode_adc_page(page: bytes, *, require_commit: bool = True) -> AdcPage:
    if len(page) != PAGE_SIZE:
        raise RecordFormatError(f"page size must be {PAGE_SIZE}, got {len(page)}")
    if page[0:4] != MAGIC:
        raise RecordFormatError("invalid magic")

    version, record_type, header_size, flags = struct.unpack_from("<BBBB", page, 4)
    if version != FORMAT_VERSION or record_type != RECORD_TYPE_ADC or header_size != HEADER_SIZE:
        raise RecordFormatError("unsupported page format")

    page_sequence, t0_us, sample_period_ns, sample_count, payload_length = struct.unpack_from(
        "<IIIHH", page, 8
    )
    if sample_count != SAMPLE_COUNT or payload_length != PAYLOAD_SIZE:
        raise RecordFormatError("invalid sample or payload length")
    if require_commit and page[COMMIT_OFFSET] != 0x00:
        raise RecordFormatError("page is not committed")

    stored_crc, capture_id = struct.unpack_from("<II", page, CRC_OFFSET)
    calculated_crc = _page_crc(page)
    if stored_crc != calculated_crc:
        raise RecordFormatError(
            f"CRC mismatch: stored=0x{stored_crc:08X} calculated=0x{calculated_crc:08X}"
        )

    samples = struct.unpack_from(f"<{SAMPLE_COUNT}H", page, HEADER_SIZE)
    return AdcPage(
        flags=flags,
        page_sequence=page_sequence,
        t0_us=t0_us,
        sample_period_ns=sample_period_ns,
        capture_id=capture_id,
        page_crc32=stored_crc,
        samples=samples,
    )


def decode_trace_page(page: bytes, *, require_commit: bool = True) -> TraceEventPage:
    if len(page) != PAGE_SIZE:
        raise RecordFormatError(f"page size must be {PAGE_SIZE}, got {len(page)}")
    if page[0:4] != MAGIC:
        raise RecordFormatError("invalid magic")

    version, record_type, header_size, flags = struct.unpack_from("<BBBB", page, 4)
    if (
        version != FORMAT_VERSION
        or record_type != RECORD_TYPE_TRACE
        or header_size != HEADER_SIZE
    ):
        raise RecordFormatError("unsupported trace page format")
    page_sequence, timestamp_us, reserved, event_count, payload_length = (
        struct.unpack_from("<IIIHH", page, 8)
    )
    if reserved != 0 or event_count != 1 or payload_length != TRACE_FRAME_SIZE:
        raise RecordFormatError("invalid trace page metadata")
    if require_commit and page[COMMIT_OFFSET] != 0x00:
        raise RecordFormatError("page is not committed")

    stored_crc, capture_id = struct.unpack_from("<II", page, CRC_OFFSET)
    calculated_crc = _page_crc(page)
    if stored_crc != calculated_crc:
        raise RecordFormatError(
            f"CRC mismatch: stored=0x{stored_crc:08X} calculated=0x{calculated_crc:08X}"
        )
    frame = decode_trace_frame(page[HEADER_SIZE : HEADER_SIZE + TRACE_FRAME_SIZE])
    return TraceEventPage(
        flags=flags,
        page_sequence=page_sequence,
        timestamp_us=timestamp_us,
        capture_id=capture_id,
        page_crc32=stored_crc,
        frame=frame,
    )
