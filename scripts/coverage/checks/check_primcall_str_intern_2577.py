#!/usr/bin/env python3
"""Issue #2577: PrimCall string re-intern content intern (no O(N) growth).

Manifest-backed thin wrapper (scripts/ coverage Phase 1).
Source of truth: scripts/coverage/manifests/2577.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2577"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
