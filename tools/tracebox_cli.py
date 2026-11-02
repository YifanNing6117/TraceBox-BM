#!/usr/bin/env python3
"""Single serial/codec CLI used by TraceBox hardware tests and localhost."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import subprocess
import sys
import time
from dataclasses import asdict
from datetime import datetime
from pathlib import Path

from dump_validate import validate_and_export
from record_codec import PAGE_SIZE


# Windows PowerShell may expose a legacy GBK stream while Mini-DCM JSON can
# contain replacement-decoded binary bytes.  Keep orchestration logs valid.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
except AttributeError:
    pass


REPO_ROOT = Path(__file__).resolve().parents[1]
RAW_DIR = REPO_ROOT / "artifacts" / "raw"
EXPORT_DIR = REPO_ROOT / "artifacts" / "exports"
CONTROL_BAUD = 115_200
ALLOWED_DUMP_BAUDS = (115_200, 230_400, 460_800)
MINIDCM_BRIDGE = Path(os.environ.get("MINIDCM_BRIDGE", str(REPO_ROOT.parent / "Mini-DCM" / "tools" / "pc_diag" / "serial_bridge.py")))
START_STATUS_PATTERN = re.compile(
    rb"(?:PREPARE|RECORD|RECOVERY|FAULT TORN) "
    rb"start status=([A-Z_]+)[^\r\n]*(?:\r\n|\n)"
)


def serial_module():
    try:
        import serial  # type: ignore
    except ImportError:
        print("ERROR: pyserial is missing; install tools/requirements.txt", file=sys.stderr)
        raise SystemExit(2)
    return serial


def stamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S")


def archive_log(prefix: str, raw: bytes) -> Path:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    path = RAW_DIR / f"{prefix}_{stamp()}.log"
    path.write_bytes(raw)
    print(f"artifact={path.resolve()}", flush=True)
    return path


def emit(data: bytes) -> None:
    sys.stdout.write(data.decode("ascii", errors="replace"))
    sys.stdout.flush()


def read_quiet(port: object, timeout_seconds: float, quiet_seconds: float = 0.15) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    last_data = time.monotonic()
    raw = bytearray()
    while time.monotonic() < deadline:
        waiting = int(getattr(port, "in_waiting"))
        chunk = port.read(waiting or 1)
        if chunk:
            raw.extend(chunk)
            last_data = time.monotonic()
        elif raw and (time.monotonic() - last_data) >= quiet_seconds:
            break
    return bytes(raw)


def run_short(port_name: str, command: str, timeout: float) -> int:
    serial = serial_module()
    with serial.Serial(port_name, CONTROL_BAUD, timeout=0.02) as port:
        port.reset_input_buffer()
        port.write(command.encode("ascii") + b"\r\n")
        port.flush()
        raw = read_quiet(port, timeout)
    emit(raw)
    archive_log(command.replace(" ", "_"), raw)
    return 0 if raw else 1


def run_long(
    port_name: str,
    command: str,
    final_token: bytes,
    timeout: float,
    progress: bool,
) -> int:
    serial = serial_module()
    raw = bytearray()
    deadline = time.monotonic() + timeout
    next_metrics = time.monotonic() + 1.0
    failed_start = False
    final_seen = False
    last_data = time.monotonic()
    with serial.Serial(port_name, CONTROL_BAUD, timeout=0.05) as port:
        port.reset_input_buffer()
        port.write(command.encode("ascii") + b"\r\n")
        port.flush()
        while time.monotonic() < deadline:
            waiting = port.in_waiting
            chunk = port.read(waiting or 1)
            if chunk:
                raw.extend(chunk)
                emit(chunk)
                last_data = time.monotonic()
                if final_token in raw:
                    final_seen = True
                # Serial reads may split anywhere, including between the O and
                # K in "OK".  Never classify a status until its line terminator
                # has arrived.
                start_match = START_STATUS_PATTERN.search(bytes(raw))
                if start_match and start_match.group(1) != b"OK":
                    failed_start = True
                    break
            elif final_seen and (time.monotonic() - last_data) >= 0.20:
                break
            if progress and time.monotonic() >= next_metrics and not final_seen:
                port.write(b"record progress\r\n")
                port.flush()
                next_metrics = time.monotonic() + 1.0

        # The target can complete just after the final periodic progress query.
        # Ask once more and allow a short bounded grace period so a successful
        # flash commit is not misreported as a host-side timeout.
        if progress and not final_seen and not failed_start:
            port.write(b"record progress\r\n")
            port.flush()
            grace_deadline = time.monotonic() + 1.0
            while time.monotonic() < grace_deadline:
                waiting = port.in_waiting
                chunk = port.read(waiting or 1)
                if chunk:
                    raw.extend(chunk)
                    emit(chunk)
                    if final_token in raw:
                        final_seen = True
                        break

    archive_log(command.replace(" ", "_"), bytes(raw))
    if failed_start:
        print("ERROR: device rejected the operation start", file=sys.stderr)
        return 1
    if final_token not in raw:
        print(f"ERROR: timed out waiting for {final_token.decode()}", file=sys.stderr)
        return 1
    text = raw.decode("ascii", errors="replace")
    success_pattern = re.escape(final_token.decode("ascii")) + r"[^\r\n]*status=OK"
    if not re.search(success_pattern, text):
        print("ERROR: final result did not report status=OK", file=sys.stderr)
        return 1
    if re.search(r"RESULT status=(?!OK)", text) or "result=FAIL" in text:
        return 1
    return 0


def read_line_until(port: object, pattern: re.Pattern[bytes], timeout: float) -> tuple[bytes, re.Match[bytes]]:
    deadline = time.monotonic() + timeout
    raw = bytearray()
    while time.monotonic() < deadline:
        byte = port.read(1)
        if byte:
            raw.extend(byte)
            if byte == b"\n":
                match = pattern.search(bytes(raw))
                if match:
                    return bytes(raw), match
    raise TimeoutError("timed out waiting for dump preamble")


def read_exact(port: object, length: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    raw = bytearray()
    while len(raw) < length and time.monotonic() < deadline:
        chunk = port.read(min(4096, length - len(raw)))
        if chunk:
            raw.extend(chunk)
            if length:
                percent = (len(raw) * 100) // length
                if len(raw) == length or len(raw) % (PAGE_SIZE * 250) < len(chunk):
                    print(f"dump_progress={percent}% bytes={len(raw)}/{length}", flush=True)
    if len(raw) != length:
        raise TimeoutError(f"incomplete dump: received {len(raw)} of {length} bytes")
    return bytes(raw)


def run_dump(port_name: str, dump_baud: int, timeout: float) -> int:
    serial = serial_module()
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    EXPORT_DIR.mkdir(parents=True, exist_ok=True)
    run_stamp = stamp()
    dump_path = RAW_DIR / f"tracebox_{run_stamp}.tbdump"
    pattern = re.compile(rb"TB_DUMP_BEGIN\s+(\d+)\s+(256)\s+(\d+)\r?\n")

    try:
        with serial.Serial(port_name, CONTROL_BAUD, timeout=0.05) as port:
            port.reset_input_buffer()
            port.write(f"dump {dump_baud}\r\n".encode("ascii"))
            port.flush()
            preamble, match = read_line_until(port, pattern, 5.0)
            emit(preamble)
            page_count = int(match.group(1))
            announced_baud = int(match.group(3))
            if announced_baud != dump_baud:
                raise RuntimeError(
                    f"device announced baud {announced_baud}, expected {dump_baud}"
                )
            expected_bytes = page_count * PAGE_SIZE
            port.baudrate = dump_baud
            raw = read_exact(port, expected_bytes, timeout)
            trailer = read_exact(port, len(b"\r\nTB_DUMP_END\r\n"), 3.0)
            if trailer != b"\r\nTB_DUMP_END\r\n":
                raise RuntimeError(f"invalid dump trailer: {trailer!r}")
            port.baudrate = CONTROL_BAUD
            status = read_quiet(port, 2.0, quiet_seconds=0.25)
            emit(status)
    except (OSError, RuntimeError, TimeoutError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    dump_path.write_bytes(raw)
    print(f"artifact={dump_path.resolve()}", flush=True)
    output_stem = EXPORT_DIR / f"tracebox_{run_stamp}"
    summary, outputs = validate_and_export(raw, output_stem)
    print(json.dumps(asdict(summary), indent=2, ensure_ascii=False), flush=True)
    for output in outputs:
        print(f"artifact={output.resolve()}", flush=True)
    return 0 if summary.result == "PASS" else 1


def run_benchmark_report() -> int:
    source = REPO_ROOT / "docs" / "evidence" / "2026-09-13_spi_polling_dma_benchmark.csv"
    rows = list(csv.DictReader(source.open(encoding="utf-8")))
    longest = [row for row in rows if row["length_bytes"] == "4096"]
    print(json.dumps({"source": str(source), "rows": longest}, indent=2))
    print(f"artifact={source.resolve()}")
    return 0


def run_child(arguments: list[str], timeout: float) -> subprocess.CompletedProcess[str]:
    """Run one trusted Python helper without a shell and mirror its evidence."""
    result = subprocess.run(
        [sys.executable, *arguments],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr, flush=True)
    return result


def parse_last_json(text: str) -> dict[str, object]:
    for line in reversed(text.splitlines()):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value
    raise ValueError("helper did not return a JSON object")


def run_trace_demo(
    tracebox_port: str,
    tester_port: str,
    frames: int,
    dump_baud: int,
) -> int:
    """Orchestrate the V2 three-board capture using the existing two CLIs."""
    if tracebox_port.upper() == tester_port.upper():
        print("ERROR: TraceBox and Tester must use different serial ports", file=sys.stderr)
        return 2
    if not MINIDCM_BRIDGE.is_file():
        print(f"ERROR: Mini-DCM bridge not found: {MINIDCM_BRIDGE}", file=sys.stderr)
        return 2

    print(
        f"V2 DEMO start tracebox={tracebox_port} tester={tester_port} "
        f"target_frames={frames}",
        flush=True,
    )
    prepare = run_child(
        [
            str(Path(__file__).resolve()),
            "trace-prepare",
            "--port",
            tracebox_port,
            "--frames",
            str(frames),
            "--timeout",
            "60",
        ],
        timeout=70.0,
    )
    if prepare.returncode != 0:
        return prepare.returncode

    stats = run_child(
        [
            str(MINIDCM_BRIDGE),
            "trace-stats",
            "--port",
            tester_port,
            "--baudrate",
            "9600",
            "--timeout-ms",
            "1200",
            "--dtr",
            "off",
            "--rts",
            "off",
        ],
        timeout=5.0,
    )
    if stats.returncode != 0:
        return stats.returncode
    try:
        queue_pending = int(parse_last_json(stats.stdout).get("queuePending", 0))
    except (TypeError, ValueError) as error:
        print(f"ERROR: cannot parse Tester trace counters: {error}", file=sys.stderr)
        return 1

    record_arguments = [
        sys.executable,
        str(Path(__file__).resolve()),
        "trace-record",
        "--port",
        tracebox_port,
        "--frames",
        str(frames),
        "--timeout",
        "45",
    ]
    record = subprocess.Popen(
        record_arguments,
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    time.sleep(0.5)

    # One DID round trip normally emits CAN_TX + CAN_RX.  Generate one spare
    # event so a deliberately rejected/stale boundary frame cannot starve the
    # requested number of accepted records.
    missing = max(0, frames - queue_pending)
    uds_count = math.ceil((missing + 1) / 2) if missing else 0
    if uds_count:
        uds = run_child(
            [
                str(MINIDCM_BRIDGE),
                "uds-stress",
                "--port",
                tester_port,
                "--baudrate",
                "9600",
                "--hex",
                "22 F1 90",
                "--expected",
                "62 F1 90 46 31 30 33",
                "--count",
                str(uds_count),
                "--timeout-ms",
                "1500",
                "--dtr",
                "off",
                "--rts",
                "off",
            ],
            timeout=max(10.0, uds_count * 2.0),
        )
        if uds.returncode != 0:
            record.terminate()
            record.wait(timeout=3.0)
            return uds.returncode

    try:
        record_output, _ = record.communicate(timeout=50.0)
    except subprocess.TimeoutExpired:
        record.terminate()
        record_output, _ = record.communicate(timeout=3.0)
        print(record_output, end="", flush=True)
        print("ERROR: V2 record step timed out", file=sys.stderr)
        return 1
    print(record_output, end="", flush=True)
    if record.returncode != 0:
        return int(record.returncode or 1)

    dump = run_child(
        [
            str(Path(__file__).resolve()),
            "dump",
            "--port",
            tracebox_port,
            "--dump-baud",
            str(dump_baud),
            "--timeout",
            "60",
        ],
        timeout=70.0,
    )
    if dump.returncode != 0:
        return dump.returncode
    print("V2 DEMO RESULT status=OK", flush=True)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)

    for name in ("info", "metrics"):
        child = subparsers.add_parser(name)
        child.add_argument("--port", required=True)
        child.add_argument("--timeout", type=float, default=3.0)

    for name in ("prepare", "record"):
        child = subparsers.add_parser(name)
        child.add_argument("--port", required=True)
        child.add_argument("--seconds", type=int, default=60, choices=range(1, 61))
        child.add_argument("--timeout", type=float, default=240.0)

    for name in ("trace-prepare", "trace-record"):
        child = subparsers.add_parser(name)
        child.add_argument("--port", required=True)
        child.add_argument("--frames", type=int, default=64, choices=range(1, 10_001))
        child.add_argument("--timeout", type=float, default=240.0)

    recover = subparsers.add_parser("recover")
    recover.add_argument("--port", required=True)
    recover.add_argument("--timeout", type=float, default=30.0)

    dump = subparsers.add_parser("dump")
    dump.add_argument("--port", required=True)
    dump.add_argument("--dump-baud", type=int, default=460_800, choices=ALLOWED_DUMP_BAUDS)
    dump.add_argument("--timeout", type=float, default=200.0)

    torn = subparsers.add_parser("fault-torn")
    torn.add_argument("--port", required=True)
    torn.add_argument("--timeout", type=float, default=10.0)

    subparsers.add_parser("bench")

    full = subparsers.add_parser("full")
    full.add_argument("--port", required=True)
    full.add_argument("--seconds", type=int, default=60, choices=range(1, 61))
    full.add_argument("--dump-baud", type=int, default=460_800, choices=ALLOWED_DUMP_BAUDS)

    trace_demo = subparsers.add_parser("trace-demo")
    trace_demo.add_argument("--port", required=True, help="TraceBox UART port")
    trace_demo.add_argument("--tester-port", required=True, help="Mini-DCM Tester UART port")
    trace_demo.add_argument("--frames", type=int, default=8, choices=range(2, 257))
    trace_demo.add_argument(
        "--dump-baud", type=int, default=460_800, choices=ALLOWED_DUMP_BAUDS
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.operation == "info":
        return run_short(args.port, "info", args.timeout)
    if args.operation == "metrics":
        return run_short(args.port, "record metrics", args.timeout)
    if args.operation == "prepare":
        return run_long(
            args.port,
            f"prepare {args.seconds}",
            b"PREPARE RESULT",
            args.timeout,
            progress=True,
        )
    if args.operation == "record":
        return run_long(
            args.port,
            f"record {args.seconds}",
            b"RECORD RESULT",
            args.timeout,
            progress=True,
        )
    if args.operation == "trace-prepare":
        return run_long(
            args.port,
            f"trace prepare {args.frames}",
            b"PREPARE RESULT",
            args.timeout,
            progress=True,
        )
    if args.operation == "trace-record":
        return run_long(
            args.port,
            f"trace record {args.frames}",
            b"TRACE RECORD RESULT",
            args.timeout,
            progress=True,
        )
    if args.operation == "recover":
        return run_long(
            args.port, "recover", b"RECOVERY RESULT", args.timeout, progress=False
        )
    if args.operation == "dump":
        return run_dump(args.port, args.dump_baud, args.timeout)
    if args.operation == "fault-torn":
        return run_long(
            args.port,
            "fault torn",
            b"FAULT TORN RESULT",
            args.timeout,
            progress=False,
        )
    if args.operation == "bench":
        return run_benchmark_report()
    if args.operation == "full":
        if run_long(
            args.port,
            f"prepare {args.seconds}",
            b"PREPARE RESULT",
            240.0,
            progress=True,
        ):
            return 1
        if run_long(
            args.port,
            f"record {args.seconds}",
            b"RECORD RESULT",
            max(180.0, args.seconds + 90.0),
            progress=True,
        ):
            return 1
        return run_dump(args.port, args.dump_baud, 200.0)
    if args.operation == "trace-demo":
        return run_trace_demo(args.port, args.tester_port, args.frames, args.dump_baud)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
