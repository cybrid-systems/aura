#!/usr/bin/env python3
"""Issue #2472: aura_closure_call MustDeopt lock-downgrade TOCTOU closed.

Contract:
  AC1 free+realloc identity / stress test present
  AC2 exclusive re-verify freed + func_id + must_deopt
  AC3 baseline force-deopt retained
  AC4 source cites #2472
  AC5 gate wiring

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    rt = _read("src/compiler/aura_jit_runtime.cpp")
    test = _read("tests/compiler/test_closure_call_must_deopt_toctou.cpp")
    build = _read("build.py")
    cmake = _read("CMakeLists.txt")

    # Locate MustDeopt section inside aura_closure_call
    idx = rt.find("int64_t aura_closure_call(")
    body = rt[idx : idx + 4500] if idx >= 0 else ""
    md = body.find("MustDeoptBeforeNextCall")
    md_body = body[md : md + 2200] if md >= 0 else ""

    must("Issue #2472", "AC1", rt)
    must("2472 AC1", "AC1", test)
    must("free+realloc", "AC1", test.lower())

    must("orig_func_id", "AC2", md_body)
    must("g_closure_freed", "AC2", md_body)
    must("g_closure_must_deopt", "AC2", md_body)
    must("force_deopt_fail", "AC2", md_body)

    must("2472 AC3", "AC3", test)
    must("force-deopt", "AC3", test.lower())

    must("Issue #2472", "AC4", rt)
    must("2472 AC4", "AC4", test)
    must("lock-downgrade TOCTOU", "AC4", rt)

    must("check_closure_call_must_deopt_toctou_2472", "gate", build)
    must("cmd_closure_call_must_deopt_toctou_coverage", "gate", build)
    must("test_closure_call_must_deopt_toctou", "gate", cmake)
    must("2472 AC5", "gate", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: aura_closure_call MustDeopt TOCTOU #2472 coverage contract clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
