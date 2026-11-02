#!/usr/bin/env python3
"""Run the real-hardware TIM2 -> ADC1 -> DMA2 double-buffer acceptance test."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from datetime import datetime
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="serial port, for example COM9")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=6.0)
    return parser.parse_args()


def read_until_token(port: object, token: bytes, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    last_data = time.monotonic()
    raw = bytearray()
    found = False

    while time.monotonic() < deadline:
        waiting = int(getattr(port, "in_waiting"))
        chunk = port.read(waiting or 1)
        if chunk:
            raw.extend(chunk)
            last_data = time.monotonic()
            found = found or token in raw
        elif found and (time.monotonic() - last_data) >= 0.05:
            break
    return bytes(raw)


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
        port.write(b"adc info\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"block_period_us=9600", 2.0))

        port.write(b"adc test\r\n")
        port.flush()
        raw.extend(read_until_token(port, b"ADC DMA TEST:", args.timeout))

    text = raw.decode("ascii", errors="replace")
    print(text)
    failures: list[str] = []

    for expected in (
        "adc1_channel=0 pin=PA0 resolution_bits=12",
        "adc_trigger=TIM2_TRGO sample_rate_hz=10000 adc_clock_hz=21000000",
        "adc_dma=DMA2_Stream4_Channel0 mode=circular_double_buffer",
        "adc_buffers=2 samples_per_buffer=96 descriptor_capacity=4 block_period_us=9600",
        "ADC DMA TEST: PASS",
    ):
        if expected not in text:
            failures.append(f"missing expected result: {expected}")

    result_match = re.search(
        r"ADC DMA result status=(\w+) blocks=(\d+) samples=(\d+) "
        r"sample_rate_hz=(\d+) samples_per_buffer=(\d+) min=(\d+) "
        r"max=(\d+) avg=(\d+)",
        text,
    )
    stats_match = re.search(
        r"seq_errors=(\d+) buffer_errors=(\d+) produced=(\d+) consumed=(\d+) "
        r"descriptor_hwm=(\d+) descriptor_overflow=(\d+)\r?\n"
        r"period_count=(\d+) period_min_cycles=(\d+) period_max_cycles=(\d+) "
        r"period_avg_cycles=(\d+) period_errors=(\d+)\r?\n"
        r"dma_irqs=(\d+) dma_errors=(\d+) adc_overruns=(\d+) "
        r"isr_max_cycles=(\d+)",
        text,
    )

    metrics: dict[str, int | str] = {}
    if result_match is None:
        failures.append("ADC result metrics are missing")
    else:
        fields = (
            "status",
            "blocks",
            "samples",
            "sample_rate_hz",
            "samples_per_buffer",
            "minimum",
            "maximum",
            "average",
        )
        metrics.update(
            {
                name: value if name == "status" else int(value)
                for name, value in zip(fields, result_match.groups())
            }
        )

    if stats_match is None:
        failures.append("ADC continuity/IRQ metrics are missing")
    else:
        fields = (
            "sequence_errors",
            "buffer_errors",
            "produced",
            "consumed",
            "descriptor_high_water",
            "descriptor_overflow",
            "period_count",
            "period_min_cycles",
            "period_max_cycles",
            "period_average_cycles",
            "period_errors",
            "dma_irqs",
            "dma_errors",
            "adc_overruns",
            "isr_max_cycles",
        )
        metrics.update(
            {name: int(value) for name, value in zip(fields, stats_match.groups())}
        )

    expected_values = {
        "status": "OK",
        "blocks": 200,
        "samples": 19_200,
        "sample_rate_hz": 10_000,
        "samples_per_buffer": 96,
        "sequence_errors": 0,
        "buffer_errors": 0,
        "consumed": 200,
        "descriptor_overflow": 0,
        "period_count": 199,
        "period_errors": 0,
        "dma_errors": 0,
        "adc_overruns": 0,
    }
    for name, expected in expected_values.items():
        if metrics.get(name) != expected:
            failures.append(f"{name}={metrics.get(name)!r}, expected {expected!r}")

    if stats_match is not None:
        produced = int(metrics["produced"])
        consumed = int(metrics["consumed"])
        dma_irqs = int(metrics["dma_irqs"])
        if not (consumed <= produced <= (consumed + 1)):
            failures.append(
                "DMA producer may lead the finite foreground target by at most one tail block"
            )
        if dma_irqs != produced:
            failures.append("DMA IRQ count does not equal completed-buffer count")

    if result_match is not None:
        minimum = int(metrics["minimum"])
        maximum = int(metrics["maximum"])
        average = int(metrics["average"])
        if not (0 <= minimum <= average <= maximum <= 4095):
            failures.append("ADC sample min/avg/max are outside the 12-bit range")
    if stats_match is not None:
        high_water = int(metrics["descriptor_high_water"])
        isr_cycles = int(metrics["isr_max_cycles"])
        period_min = int(metrics["period_min_cycles"])
        period_max = int(metrics["period_max_cycles"])
        period_average = int(metrics["period_average_cycles"])
        if not (1 <= high_water <= 4):
            failures.append("descriptor high water is outside 1..4")
        if not (1 <= isr_cycles < 168_000):
            failures.append("ISR maximum is zero or exceeds 1 ms at 168 MHz")
        if not (1_580_544 <= period_min <= period_average <= period_max <= 1_645_056):
            failures.append("96-sample block cadence is outside the +/-2% limit")

    artifact_dir = Path(__file__).resolve().parents[1] / "artifacts" / "raw"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = artifact_dir / f"adc_dma_test_{stamp}.log"
    json_path = artifact_dir / f"adc_dma_test_{stamp}.json"
    log_path.write_bytes(raw)
    json_path.write_text(
        json.dumps({"metrics": metrics, "failures": failures}, indent=2),
        encoding="utf-8",
    )
    print(f"Raw evidence: {log_path}")
    print(f"JSON evidence: {json_path}")

    if failures:
        print("ADC DMA AUTOMATION: FAIL", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print("ADC DMA AUTOMATION: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
