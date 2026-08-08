#!/usr/bin/env python3
"""Issue #2772: denseness multi-process AURA_BIN export / self-path DX.

Child (shell …) only inherits *exported* env. Span runners that set
AURA_BIN as a non-exported shell var saw empty (getenv \"AURA_BIN\") and
exit 127 (\"aura: not found\"). Fix: seed process environ from self path,
expose (aura-executable-path), document export AURA_BIN in denseness usage.

Contract (one row per AC):
  AC1 ensure_aura_bin_environ / resolve_self_executable in runtime_paths + main
  AC2 aura-executable-path prim + #2772 cite
  AC3 print_denseness_usage lists export AURA_BIN + multi-process child note
  AC4 live smoke (when build/aura): unset AURA_BIN → getenv non-empty + shell child
  AC5 this linter wired; no docs/design/2772-*

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import contextlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def live_smoke() -> list[str]:
    aura = ROOT / "build" / "aura"
    if not aura.is_file() or not os.access(aura, os.X_OK):
        return []
    fails: list[str] = []
    env = os.environ.copy()
    # Simulate runner that never exported AURA_BIN.
    env.pop("AURA_BIN", None)
    env["AURA_SANDBOX"] = "off"
    env["AURA_PIPELINE_STRICT"] = "0"
    env.setdefault("AURA_PATH", str(ROOT / "lib"))

    # AC4a: getenv AURA_BIN seeded from self
    r = subprocess.run(
        [str(aura), "-e", '(display (getenv "AURA_BIN")) (newline)'],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
    )
    out = (r.stdout or "").strip()
    err = r.stderr or ""
    if r.returncode != 0:
        fails.append(f"smoke getenv: exit={r.returncode} err={err[:200]!r}")
    elif not out or out == "()":
        fails.append(f"smoke getenv: expected non-empty AURA_BIN, got {out!r}")
    elif "aura" not in out.lower() and not Path(out).is_file():
        # path should look like an executable path
        fails.append(f"smoke getenv: path looks wrong: {out!r}")

    # AC4b: aura-executable-path
    r2 = subprocess.run(
        [
            str(aura),
            "-e",
            "(display (procedure? aura-executable-path)) (newline)(display (aura-executable-path)) (newline)",
        ],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
    )
    lines = [ln.strip() for ln in (r2.stdout or "").splitlines() if ln.strip()]
    if r2.returncode != 0 or not lines or lines[0] != "#t":
        fails.append(f"smoke aura-executable-path procedure?: {r2.stdout!r} {r2.stderr[:200]!r}")
    elif len(lines) < 2 or not lines[1] or lines[1] == "()":
        fails.append(f"smoke aura-executable-path empty: {lines!r}")

    # AC4c: child shell sees AURA_BIN (process seed is inherited)
    with tempfile.NamedTemporaryFile(mode="w", suffix=".txt", delete=False) as tf:
        child_out = tf.name
    try:
        code = (
            f'(display (shell "echo -n \\"$AURA_BIN\\" > {child_out}")) (newline)'
            f'(display (getenv "AURA_BIN")) (newline)'
        )
        r3 = subprocess.run(
            [str(aura), "-e", code],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
            timeout=30,
        )
        child_val = Path(child_out).read_text(encoding="utf-8", errors="replace").strip()
        if r3.returncode != 0:
            fails.append(f"smoke shell child: exit={r3.returncode} {(r3.stderr or '')[:200]!r}")
        elif not child_val:
            fails.append(f"smoke shell child: AURA_BIN empty in child (parent getenv={(r3.stdout or '').strip()!r})")
    finally:
        with contextlib.suppress(OSError):
            Path(child_out).unlink(missing_ok=True)

    # AC4d: --help mentions export AURA_BIN + multi-process
    r4 = subprocess.run(
        [str(aura), "--help"],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        timeout=15,
    )
    help_out = (r4.stdout or "") + (r4.stderr or "")
    if "AURA_BIN" not in help_out:
        fails.append("smoke --help missing AURA_BIN")
    # usage line is "export AURA_BIN=…/build/aura"
    if "export AURA_BIN" not in help_out and "AURA_BIN=" not in help_out:
        fails.append("smoke --help missing export AURA_BIN contract")
    if "multi-process" not in help_out.lower() and "Multi-process" not in help_out:
        fails.append("smoke --help missing multi-process note")
    if "aura-executable-path" not in help_out:
        fails.append("smoke --help missing aura-executable-path")

    return fails


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    paths = _read("src/compiler/runtime_paths.h")
    main_cpp = _read("src/main.cpp")
    io = _read("src/compiler/evaluator_primitives_io.cpp")
    build = _read("build.py")

    # AC1
    must("#2772", "AC1", paths)
    must("resolve_self_executable", "AC1", paths)
    must("ensure_aura_bin_environ", "AC1", paths)
    must("ensure_aura_bin_environ", "AC1", main_cpp)
    must("#2772", "AC1", main_cpp)
    must("/proc/self/exe", "AC1", paths)

    # AC2
    must('add("aura-executable-path"', "AC2", io)
    must("#2772", "AC2", io)
    must("resolve_self_executable", "AC2", io)

    # AC3
    must("export AURA_BIN", "AC3", main_cpp)
    must("Multi-process", "AC3", main_cpp)
    must("aura-executable-path", "AC3", main_cpp)
    must("print_denseness_usage", "AC3", main_cpp)

    # AC4 live
    fails.extend(f"AC4: {m}" for m in live_smoke())

    # AC5
    must("check_denseness_multiprocess_env_2772", "AC5", build)
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2772-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden per #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2772.cpp").is_file():
        fails.append("AC5: test_issue_2772.cpp present (forbidden per #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2772 denseness multi-process AURA_BIN — seed + aura-executable-path + usage + live smoke green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
