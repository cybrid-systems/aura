#!/usr/bin/env python3
"""Issue #2494: hard-fail check_side_effect_security.py for new prims without

Manifest-backed thin wrapper (scripts/ coverage Phase 1).
Source of truth: scripts/coverage/manifests/2494.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2494"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
