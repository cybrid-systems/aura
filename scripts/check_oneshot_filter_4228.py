#!/usr/bin/env python3
"""Hard-timeout Soft oneshot filter smokes for #4228.

Native `filter` with a Soft std/math predicate passed directly
(`(filter even? lst)`) must match let/apply/lambda and Soft partition /
native `number?` (not empty `()`). Soft binary default: build_soft4132/aura.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AURA = Path(os.environ.get("AURA_BIN", str(ROOT / "build" / "aura")))
TIMEOUT_S = float(os.environ.get("AURA_ONESHOT_TIMEOUT", "8"))

# (label, expr, expect_substr_in_stdout)
CASES: list[tuple[str, str, str]] = [
    # Direct math preds (the #4228 failure mode) — even?/odd?/positive?/
    # zero? come from the std/math auto-load face; the standard-face
    # completion is fecdf639e's face-agnostic runtime cell handling
    # (apply_unary/apply_pred/apply_binary define-cell deref + filter car
    # snapshot + oneshot prelude sync), verified against a current
    # build/aura (restored without any soft_only gating per the
    # single-binary direction; all 9 cases run against build/aura).
    ("direct-even?", "(filter even? (list 1 2 3 4))", "(2 4)"),
    ("direct-odd?", "(filter odd? (list 1 2 3 4))", "(1 3)"),
    ("direct-positive?", "(filter positive? (list -1 0 2 -3 4))", "(2 4)"),
    ("direct-zero?", "(filter zero? (list 0 1 0 2))", "(0 0)"),
    # Paths that already worked — must stay green.
    ("let-bind", "(let ((f filter) (e even?)) (f e (list 1 2 3 4)))", "(2 4)"),
    ("apply", "(apply filter (list even? (list 1 2 3 4)))", "(2 4)"),
    ("lambda", "(filter (lambda (x) (even? x)) (list 1 2 3 4))", "(2 4)"),
    ("number?", "(filter number? (list 1 2 #f 3))", "(1 2 3)"),
    ("partition", "(partition even? (list 1 2 3 4))", "((2 4) (1 3))"),
]


def run_case(label: str, expr: str, expect: str) -> tuple[bool, str]:
    if not AURA.is_file():
        return False, f"missing aura binary: {AURA}"

    def _spawn() -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(AURA), "-e", expr],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_S,
            cwd=str(ROOT),
            env={
                **os.environ,
                "AURA_SANDBOX": os.environ.get("AURA_SANDBOX", "off"),
                "AURA_PIPELINE_STRICT": os.environ.get("AURA_PIPELINE_STRICT", "0"),
            },
        )

    try:
        proc = _spawn()
    except subprocess.TimeoutExpired:
        return False, f"TIMEOUT>{TIMEOUT_S}s"
    if proc.returncode is not None and proc.returncode < 0:
        time.sleep(2.0)
        try:
            proc = _spawn()
        except subprocess.TimeoutExpired:
            return False, f"TIMEOUT>{TIMEOUT_S}s (after sigkill retry)"
    out = (proc.stdout or "").strip()
    err = (proc.stderr or "").strip()
    if proc.returncode != 0:
        return False, f"exit={proc.returncode} stderr={err!r} stdout={out!r}"
    if "unbound variable" in err.lower() or "unbound variable" in out.lower():
        return False, f"unbound: stderr={err!r} stdout={out!r}"
    if expect not in out:
        return False, f"expected {expect!r} in stdout={out!r} stderr={err!r}"
    return True, out


def main() -> int:
    if not AURA.exists():
        # The stdlib oneshot filter face needs an aura binary; the CI gate job
        # is a static-checks environment and does not build C++ binaries.
        # Skip there; run locally via: ninja -C build aura (or point AURA_BIN
        # at any aura build). (#4228)
        print(f"SKIP: no aura binary at {AURA}")
        print("SKIP reason: this check exercises the oneshot stdlib filter")
        print("SKIP face; the CI gate does not build C++ binaries.")
        print("SKIP locally: ninja -C build aura (or set AURA_BIN)")
        return 0
    print(f"AURA_BIN={AURA}")
    failed = 0
    for label, expr, expect in CASES:
        ok, detail = run_case(label, expr, expect)
        mark = "PASS" if ok else "FAIL"
        print(f"#4228 {mark}: {label}: {expr} -> {detail}")
        if not ok:
            failed += 1
    print(f"summary: {len(CASES) - failed}/{len(CASES)} pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
