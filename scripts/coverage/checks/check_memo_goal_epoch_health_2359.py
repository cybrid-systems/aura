#!/usr/bin/env python3
"""Issue #2359: occurrence_goals + predicate_memo epoch health query surface.

Manifest-backed thin wrapper (scripts/ coverage Phase 1).
Source of truth: scripts/coverage/manifests/2359.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2359"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
