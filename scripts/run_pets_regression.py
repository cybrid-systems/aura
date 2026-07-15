#!/usr/bin/env python3
"""Issue #1454: aura-pets headless TUI regression runner.

Runs:
  1. Headless demos via ``aura --load examples/{snake,tetris,cyber_cat}.aura``
  2. C++ smoke binaries when present (test_aura_pets_smoke, test_cyber_cat_smoke,
     selected terminal/render tests)
  3. Optional suite file tests/suite/aura_pets.aura

Usage:
  python3 scripts/run_pets_regression.py
  python3 scripts/run_pets_regression.py --build-dir build
  python3 scripts/run_pets_regression.py --demos-only

Exit 0 = all green, 1 = failure.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

DEMOS = (
    "examples/snake.aura",
    "examples/tetris.aura",
    "examples/cyber_cat.aura",
)

# Hard gate binaries (skip if not built — demos still run).
# Reliable terminal / pets smokes only (omit flaky render_telemetry #1357).
PETS_BINARIES = (
    "test_aura_pets_smoke",
    "test_cyber_cat_smoke",
    "test_terminal_lifecycle",
    "test_terminal_ansi_emit",
    "test_terminal_rgb",
    "test_terminal_input",
    "test_terminal_deprecation",
)

# Fatal markers in demo stdout/stderr (process may still exit 0 on soft errors).
FATAL_RE = re.compile(
    r"(?i)(internal error:|fatal:|segmentation fault|Aborted|asan:|"
    r"AddressSanitizer|undefined.?behavior)",
)


def find_aura(build_dir: Path) -> Path | None:
    for cand in (
        build_dir / "aura",
        ROOT / "build" / "aura",
        ROOT / "build_asan" / "aura",
    ):
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    return None


def run_cmd(
    cmd: list[str],
    *,
    cwd: Path,
    timeout: float,
    env: dict[str, str] | None = None,
) -> tuple[int, str, str]:
    r = subprocess.run(
        cmd,
        cwd=str(cwd),
        capture_output=True,
        text=True,
        timeout=timeout,
        env=env,
    )
    return r.returncode, r.stdout or "", r.stderr or ""


def check_demo(aura: Path, rel: str, timeout: float) -> tuple[bool, str]:
    path = ROOT / rel
    if not path.is_file():
        return False, f"missing {rel}"
    env = os.environ.copy()
    env.setdefault("AURA_RUNTIME_DIR", str(ROOT))
    # Force non-interactive / no raw TTY assumptions
    env.setdefault("TERM", "dumb")
    try:
        rc, out, err = run_cmd(
            [str(aura), "--load", str(path)],
            cwd=ROOT,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return False, f"timeout after {timeout}s"
    blob = out + "\n" + err
    if rc != 0:
        return False, f"exit {rc}: {err[:200] or out[:200]}"
    if FATAL_RE.search(blob):
        m = FATAL_RE.search(blob)
        return False, f"fatal marker: {m.group(0) if m else '?'}"
    return True, "ok"


def check_binary(build_dir: Path, name: str, timeout: float) -> tuple[str, bool, str]:
    """Returns (status, ok, detail) where status is pass|fail|skip."""
    bin_path = build_dir / name
    if not bin_path.is_file():
        return "skip", True, "not built"
    env = os.environ.copy()
    env.setdefault("AURA_RUNTIME_DIR", str(ROOT))
    env.setdefault("TERM", "dumb")
    try:
        rc, out, err = run_cmd(
            [str(bin_path)],
            cwd=ROOT,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return "fail", False, f"timeout after {timeout}s"
    if rc != 0:
        return "fail", False, f"exit {rc}: {(err or out)[:240]}"
    return "pass", True, "ok"


def check_suite_aura(aura: Path, timeout: float) -> tuple[str, bool, str]:
    suite = ROOT / "tests" / "suite" / "aura_pets.aura"
    if not suite.is_file():
        return "skip", True, "no suite file"
    env = os.environ.copy()
    env.setdefault("AURA_RUNTIME_DIR", str(ROOT))
    env.setdefault("TERM", "dumb")
    try:
        rc, out, err = run_cmd(
            [str(aura), "--load", str(suite)],
            cwd=ROOT,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return "fail", False, f"timeout after {timeout}s"
    blob = out + "\n" + err
    if rc != 0:
        return "fail", False, f"exit {rc}: {(err or out)[:200]}"
    if FATAL_RE.search(blob):
        return "fail", False, "fatal marker in suite output"
    # edsl-test-harness / std/test summaries (aura may still exit 0).
    if re.search(r"(?<![0-9])[1-9][0-9]* failed", blob, re.I):
        return "fail", False, "suite reported failures"
    if "❌" in blob or "aura-pets suite failed" in blob:
        return "fail", False, "suite assertion failed"
    # Require an explicit green summary from harness when totals are present.
    if (
        "All tests passed" not in blob
        and "0 failed" not in blob
        and "passed" in blob.lower()
        and "failed" in blob.lower()
    ):
        return "fail", False, "missing green summary"
    return "pass", True, "ok"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--build-dir",
        default=os.environ.get("AURA_BUILD_DIR", str(ROOT / "build")),
        help="directory containing aura + test_* binaries",
    )
    ap.add_argument("--demos-only", action="store_true", help="skip C++ binaries")
    ap.add_argument("--timeout-demo", type=float, default=90.0)
    ap.add_argument("--timeout-bin", type=float, default=120.0)
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir

    aura = find_aura(build_dir)
    if aura is None:
        print("FAIL: aura binary not found (build first)", file=sys.stderr)
        return 1

    failed = 0
    passed = 0
    skipped = 0

    print("═══ aura-pets headless regression (#1454) ═══")
    print(f"  aura: {aura}")
    print(f"  build: {build_dir}")

    print("\n── demos ──")
    for rel in DEMOS:
        ok, detail = check_demo(aura, rel, args.timeout_demo)
        if ok:
            print(f"  PASS  {rel}")
            passed += 1
        else:
            print(f"  FAIL  {rel}: {detail}")
            failed += 1

    if not args.demos_only:
        print("\n── C++ TUI / terminal smokes ──")
        for name in PETS_BINARIES:
            status, ok, detail = check_binary(build_dir, name, args.timeout_bin)
            if status == "skip":
                print(f"  SKIP  {name} ({detail})")
                skipped += 1
            elif ok:
                print(f"  PASS  {name}")
                passed += 1
            else:
                print(f"  FAIL  {name}: {detail}")
                failed += 1

        print("\n── suite/aura_pets.aura ──")
        status, ok, detail = check_suite_aura(aura, args.timeout_demo)
        if status == "skip":
            print(f"  SKIP  suite ({detail})")
            skipped += 1
        elif ok:
            print("  PASS  suite/aura_pets.aura")
            passed += 1
        else:
            print(f"  FAIL  suite/aura_pets.aura: {detail}")
            failed += 1

    print(f"\n── summary: {passed} passed, {failed} failed, {skipped} skipped ──")
    if failed:
        print("FAIL: aura-pets regression")
        return 1
    if passed == 0:
        print("FAIL: nothing ran")
        return 1
    print("OK: aura-pets regression")
    return 0


if __name__ == "__main__":
    sys.exit(main())
