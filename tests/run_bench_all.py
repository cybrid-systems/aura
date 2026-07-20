#!/usr/bin/env python3
"""Thin entrypoint → tests/bench/run_bench_all.py (Issue #1932 layout)."""

from __future__ import annotations

import runpy
import sys
from pathlib import Path

_DIR = Path(__file__).resolve().parent / "bench"
if str(_DIR) not in sys.path:
    sys.path.insert(0, str(_DIR))
runpy.run_path(str(_DIR / "run_bench_all.py"), run_name="__main__")
