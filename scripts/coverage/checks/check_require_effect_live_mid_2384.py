#!/usr/bin/env python3
"""Issue #2384: require_effect stamps live mutation_id (not hardcode 0).

Contract:
  AC1 Bound mid mismatch denies + capability_provenance_mismatch_total
  AC2 Bound mid match allows under live epoch
  AC3 Off sandbox allows; SecurityEvent mid non-zero
  AC4 SecurityEvent mutation_id != 0 on require_effect path
  AC5 Source-cite + CMake + build.py gate

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

    def must_absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden residual {n!r}")

    sec = _read("src/compiler/evaluator_security.cpp")
    ixx = _read("src/compiler/evaluator.ixx")
    test = _read("tests/compiler/test_require_effect_live_mid.cpp")
    cmake = _read("CMakeLists.txt")
    build = _read("build.py")

    # Locate require_effect body. Issue #3143 added a large doc-block to
    # require_effect, so the previous 1500-char window was too small
    # (the `return check_and_record_effect(...)` call was past it). Find
    # the function end properly (next "}\n" at column 0) — supports
    # arbitrary function body length.
    req_i = sec.find("bool Evaluator::require_effect")
    if req_i < 0:
        fails.append("AC1: require_effect definition missing")
        body = ""
    else:
        # Find the closing "}\n" at column 0 after the function start.
        body_end = sec.find("\n}\n", req_i)
        body = sec[req_i : req_i + 8192] if body_end < 0 else sec[req_i : body_end + 2]

    # AC1/AC2 live mid wiring
    must("Issue #2384", "AC1", sec)
    must("current_mutation_epoch()", "AC1", body)
    must("check_and_record_effect", "AC1", body)
    must_absent("/*provenance_mutation_id=*/0", "AC1", body)
    must("ac1_bound_mismatch_denies", "AC1", test)
    must("capability_provenance_mismatch_total", "AC1", test)
    must("ac2_bound_match_allows", "AC2", test)

    # AC3/AC4
    must("ac3_soft_off_allows_nonzero_mid", "AC3", test)
    must("ac4_security_event_mid", "AC4", test)
    must("SecurityEvent", "AC4", test)

    # AC5 registration
    must("Issue #2384", "AC5", ixx)
    must("test_require_effect_live_mid", "AC5", cmake)
    must("check_require_effect_live_mid_2384", "AC5", build)
    must("cmd_require_effect_live_mid_coverage", "AC5", build)
    must("ac5_source_and_gate", "AC5", test)
    must("Issue #2384", "AC5", test)

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2384 require_effect live mutation_id — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
