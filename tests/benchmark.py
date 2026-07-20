#!/usr/bin/env python3
"""Thin entrypoint → tests/bench/benchmark.py (Issue #1932 layout).

Adds tests/bench/ and tests/python/ so benchmark_cases can import fixture_store
without requiring PYTHONPATH (./build.py bench / direct CLI).
"""

from __future__ import annotations

import runpy
import sys
from pathlib import Path

_TESTS = Path(__file__).resolve().parent
_BENCH = _TESTS / "bench"
_PYTHON = _TESTS / "python"
for p in (_BENCH, _PYTHON):
    s = str(p)
    if s not in sys.path:
        sys.path.insert(0, s)
runpy.run_path(str(_BENCH / "benchmark.py"), run_name="__main__")
