#!/usr/bin/env python3
"""Issue #2430: snapshot_capability_effect_stats double-check (#1840 pattern).

Manifest-backed thin wrapper (scripts/ coverage Phase 1).
Source of truth: scripts/coverage/manifests/2430.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2430"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
