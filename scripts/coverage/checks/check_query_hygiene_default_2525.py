#!/usr/bin/env python3
"""Issue #2525: unconstrained query hygiene residual — default MacroIntroduced skip.

Manifest-backed thin wrapper (scripts/ coverage Phase 1).
Source of truth: scripts/coverage/manifests/2525.json
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RUNNER = ROOT / "scripts" / "coverage" / "runner.py"


def main() -> int:
    return subprocess.call([sys.executable, str(RUNNER), "--issue", "2525"], cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
