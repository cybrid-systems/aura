#!/usr/bin/env python3
"""Thin entrypoint → tests/python/fixture_check.py (Issue #1932 layout)."""

from __future__ import annotations

import runpy
import sys
from pathlib import Path

_DIR = Path(__file__).resolve().parent / "python"
if str(_DIR) not in sys.path:
    sys.path.insert(0, str(_DIR))
runpy.run_path(str(_DIR / "fixture_check.py"), run_name="__main__")
