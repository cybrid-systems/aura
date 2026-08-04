#!/usr/bin/env python3
"""Issue #2347: MultiFiberMailbox Guard-live blocking recv hard audit.

Manifest-backed thin wrapper (scripts/ coverage Phase 1).
Source of truth: scripts/coverage/manifests/2347.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2347"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
