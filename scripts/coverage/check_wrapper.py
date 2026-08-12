#!/usr/bin/env python3
"""Run one coverage check with optional cascade suppression.

When AURA_COVERAGE_NO_CASCADE is set (default under ./build.py gate), nested
subprocess invocations of other check_*.py scripts are short-circuited to
returncode=0. Gate already runs every check independently, so the historical
"Cross-check: re-run prior issue linter" pattern is pure O(N²) waste.

Usage:
  python3 scripts/coverage/check_wrapper.py path/to/check_foo.py [args...]
"""

from __future__ import annotations

import os
import runpy
import subprocess
import sys
from pathlib import Path


def _truthy(name: str, default: str = "0") -> bool:
    return os.environ.get(name, default).strip().lower() in ("1", "true", "yes", "on")


def _install_cascade_guard() -> None:
    if not _truthy("AURA_COVERAGE_NO_CASCADE", "0"):
        return

    _orig_run = subprocess.run
    _orig_call = subprocess.call
    _orig_check_call = subprocess.check_call

    def _is_nested_check(cmd: object) -> bool:
        if not isinstance(cmd, (list, tuple)) or len(cmd) < 2:
            return False
        # Typical: [sys.executable, "/abs/.../check_foo.py", ...]
        for arg in cmd[1:3]:
            name = Path(str(arg)).name
            if name.startswith("check_") and name.endswith(".py"):
                return True
        return False

    class _Skipped:
        returncode = 0
        stdout = "cascade-skipped\n"
        stderr = ""

        def __bool__(self) -> bool:
            return True

    def _run(cmd, *a, **kw):  # type: ignore[no-untyped-def]
        if _is_nested_check(cmd):
            return _Skipped()
        return _orig_run(cmd, *a, **kw)

    def _call(cmd, *a, **kw):  # type: ignore[no-untyped-def]
        if _is_nested_check(cmd):
            return 0
        return _orig_call(cmd, *a, **kw)

    def _check_call(cmd, *a, **kw):  # type: ignore[no-untyped-def]
        if _is_nested_check(cmd):
            return 0
        return _orig_check_call(cmd, *a, **kw)

    subprocess.run = _run  # type: ignore[assignment]
    subprocess.call = _call  # type: ignore[assignment]
    subprocess.check_call = _check_call  # type: ignore[assignment]


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    target = Path(args[0]).resolve()
    if not target.is_file():
        print(f"FAIL: check not found: {target}", file=sys.stderr)
        return 1
    _install_cascade_guard()
    # Make the target appear as __main__ with its own argv.
    sys.argv = [str(target), *args[1:]]
    try:
        runpy.run_path(str(target), run_name="__main__")
    except SystemExit as e:
        code = e.code
        if code is None:
            return 0
        if isinstance(code, int):
            return code
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
