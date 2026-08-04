#!/usr/bin/env python3
"""Issue #2623: configurable cross-closure depth + production fail-closed trunc.

Manifest-backed thin wrapper (architecture Phase 1).
Source of truth: scripts/coverage/manifests/2623.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2623"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
