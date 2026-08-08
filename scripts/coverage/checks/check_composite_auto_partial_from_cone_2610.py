#!/usr/bin/env python3
"""Issue #2610: auto-detect expected_partial from dirty cone.

Manifest-backed thin wrapper (scripts/ coverage Phase 1).
Source of truth: scripts/coverage/manifests/2610.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _source_has_key(hay: str, key: str) -> bool:
    n = "".join(ch for ch in hay if not ch.isspace() and ch != '"')
    return key in n


RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2610"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
