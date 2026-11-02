"""Scan publishable files without printing potentially sensitive values."""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATTERNS = {
    "github-token": r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})",
    "openai-token": r"\bsk-(?:proj-)?[A-Za-z0-9_-]{20,}",
    "aws-key": r"\bAKIA[0-9A-Z]{16}\b",
    "private-key": r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----",
    "personal-path": r"(?i)(?:[A-Z]:[\\/](?:Users|Anaconda|KeilProject|STM32CubeProject)[\\/]|/home/[\w.-]+/|/Users/[\w.-]+/)",
    "email": r"(?<![\w.])[A-Za-z0-9_.+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}",
    "china-phone": r"(?<![A-Za-z0-9_])1[3-9]\d{9}(?![A-Za-z0-9_])",
    "credential-assignment": r'''(?i)(?:api[_-]?key|access[_-]?token|password|client[_-]?secret)\s*[:=]\s*["'][^"'\r\n]{8,}["']''',
}
# Upstream contact addresses form part of vendor notices and must be retained.
VENDOR_EMAILS = {"support@keil.com", "support@arm.com", "info@freertos.org", "support@freertos.org", "info@segger.com", "support@segger.com"}
FORBIDDEN_DIRS = {".git", ".codex", ".claude", ".agents", ".ssh", ".aws", "__pycache__", "node_modules", "Debug", "Objects"}
FORBIDDEN_SUFFIXES = {".pem", ".key", ".p12", ".pfx", ".zip", ".7z", ".docx", ".xlsx", ".png", ".jpg", ".pdf", ".launch", ".elf", ".bin", ".hex", ".tbdump", ".log"}


def candidates() -> list[Path]:
    result = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--cached", "--others", "--exclude-standard", "-z"], capture_output=True)
    if result.returncode == 0:
        return sorted({ROOT / name for name in result.stdout.decode("utf-8").split("\0") if name})
    return sorted(p for p in ROOT.rglob("*") if p.is_file() and not any(v in FORBIDDEN_DIRS for v in p.relative_to(ROOT).parts))


def main() -> int:
    findings = []
    files = candidates()
    for item in files:
        rel = item.relative_to(ROOT)
        if item.is_symlink():
            findings.append((str(rel), 0, "symlink"))
            continue
        if not item.is_file():
            findings.append((str(rel), 0, "missing-tracked-file"))
            continue
        if any(v in FORBIDDEN_DIRS for v in rel.parts) or item.suffix.lower() in FORBIDDEN_SUFFIXES or item.name == ".env" or (item.name.startswith(".env.") and item.name != ".env.example"):
            findings.append((str(rel), 0, "forbidden-file"))
        if item.stat().st_size > 5 * 1024 * 1024:
            findings.append((str(rel), 0, "large-file"))
        try:
            text = item.read_text(encoding="utf-8-sig")
        except UnicodeError:
            findings.append((str(rel), 0, "non-text-file"))
            continue
        for label, pattern in PATTERNS.items():
            for match in re.finditer(pattern, text):
                if label == "email" and match.group(0).lower() in VENDOR_EMAILS:
                    continue
                # Upstream URLs can contain long decimal document identifiers.
                line_start = text.rfind("\n", 0, match.start()) + 1
                line = text[line_start:text.find("\n", match.end()) if "\n" in text[match.end():] else len(text)]
                if label == "china-phone" and ("http://" in line or "https://" in line):
                    continue
                findings.append((str(rel), text.count("\n", 0, match.start()) + 1, label))
    for path, line, label in findings:
        print(f"{path}:{line}: {label}")
    print(f"Publication scan: {len(files)} files, {len(findings)} findings")
    return int(bool(findings))


if __name__ == "__main__":
    raise SystemExit(main())
