#!/usr/bin/env python3
"""Hard-timeout Soft oneshot stdlib smokes for #4224–#4226.

Verifies Soft oneshot (`aura -e`) binds Soft std/list remove + delete and
exposes list (range start end) after math statistical range was renamed to
extent (#4225). Prefer Soft std primitives, not aura-build fills.
"""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AURA = Path(os.environ.get("AURA_BIN", str(ROOT / "build_soft4132" / "aura")))
TIMEOUT_S = float(os.environ.get("AURA_ONESHOT_TIMEOUT", "8"))

# (issue, expr, expect_substr_in_stdout)
CASES: list[tuple[int, str, str]] = [
    # #4224 remove (SRFI-1; complement of filter)
    (4224, "(remove even? (quote (1 2 3 4)))", "(1 3)"),
    (4224, "(null? (remove (lambda (x) #t) (quote ())))", "#t"),
    (4224, "(remove (lambda (x) #f) (quote (1 2)))", "(1 2)"),
    # #4226 delete (SRFI-1; equal?)
    (4226, "(delete 2 (quote (1 2 3 2)))", "(1 3)"),
    (4226, "(null? (delete 0 (quote ())))", "#t"),
    (4226, "(null? (delete 7 (quote (7))))", "#t"),
    (4226, "(delete 9 (quote (1 2 3)))", "(1 2 3)"),
    # #4225 range — Soft list [start,end); math statistical extent renamed
    (4225, "(range 0 3)", "(0 1 2)"),
    (4225, "(range 1 5)", "(1 2 3 4)"),
    (4225, "(null? (range 3 3))", "#t"),
    (4225, "(extent (quote (1 5 3)))", "4"),
]


def run_case(issue: int, expr: str, expect: str) -> tuple[bool, str]:
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
                # Issue #4150: stdlib oneshots verify values, not the pipeline
                # face — default the soft oneshot spawn to the diagnostics
                # face (build.py's own default) so gate-composed runs stay
                # deterministic; an explicit caller value still wins.
                "AURA_PIPELINE_STRICT": os.environ.get("AURA_PIPELINE_STRICT", "0"),
            },
        )

    try:
        proc = _spawn()
    except subprocess.TimeoutExpired:
        return False, f"TIMEOUT>{TIMEOUT_S}s"
    if proc.returncode is not None and proc.returncode < 0:
        # Externally SIGKILLed child (negative rc, empty streams): the shared
        # gate host's memory-pressure waves kill freshly forked interpreter
        # children — the chronic stdlib-oneshot exit=-9 flakes (#4152/#4154/
        # #4155; reproduced even at lane jobs=2, so not burst-only). A signal
        # death is not a case outcome: retry once so the assertions below run
        # on a live child. A real interpreter crash reproduces and still
        # fails; no value/exit/unbound assertion is relaxed.
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
        # The Soft oneshot stdlib auto-load face needs a Soft build; the CI
        # gate job is a static-checks environment and does not build C++
        # binaries. Skip there; run locally via: ninja -C build_soft4132 aura
        # (or point AURA_BIN at any Soft oneshot build). (#4178-#4226)
        print(f"SKIP: no Soft oneshot binary at {AURA}")
        print("SKIP reason: this check exercises the Soft oneshot stdlib")
        print("SKIP auto-load face; the CI gate does not build C++ binaries.")
        print("SKIP locally: ninja -C build_soft4132 aura (or set AURA_BIN)")
        return 0
    print(f"AURA_BIN={AURA}")
    failed = 0
    for issue, expr, expect in CASES:
        ok, detail = run_case(issue, expr, expect)
        mark = "PASS" if ok else "FAIL"
        print(f"#{issue} {mark}: {expr} -> {detail}")
        if not ok:
            failed += 1
    print(f"summary: {len(CASES) - failed}/{len(CASES)} pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
