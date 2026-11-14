#!/usr/bin/env python3
"""Issue #5201: CMake 4.5 / aura-ci:v0.1.1 baseline pin.

Runs the unit test that asserts the toolchain image tag, the 4.5 minimum,
the removal of the experimental "import std" gate, and Presets schema 13.
Exit 0 = pins satisfied.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TEST = ROOT / "tests" / "python" / "process" / "test_ci_toolchain_image.py"


def main() -> int:
    if not TEST.is_file():
        print(f"missing {TEST}", file=sys.stderr)
        return 1
    return subprocess.run([sys.executable, str(TEST)], cwd=ROOT).returncode


if __name__ == "__main__":
    raise SystemExit(main())
