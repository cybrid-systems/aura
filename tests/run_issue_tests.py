#!/usr/bin/env python3
"""Thin entrypoint → tests/python/run_issue_tests.py (Issue #1932 layout)."""

from __future__ import annotations

import runpy
import sys
from pathlib import Path

_DIR = Path(__file__).resolve().parent / "python"
if str(_DIR) not in sys.path:
    sys.path.insert(0, str(_DIR))
runpy.run_path(str(_DIR / "run_issue_tests.py"), run_name="__main__")
