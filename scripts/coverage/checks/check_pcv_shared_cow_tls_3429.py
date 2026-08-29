#!/usr/bin/env python3
"""Issue #3429: shared-but-not-stale COW consumes TLS freelist before heap.

#2521 / #2406 shipped a TLS freelist for exclusive short-lived PCV
allocs. #2906 move-out exclusive and #3233 stale-span force-exclusive
are Hard. Residual: live SafePCVSpan / checkpoint snapshot still
heap-COWs on set_child_locked. The shared with_set path now pops the
TLS freelist first; heap only on miss. Still COWs (no write-through).

Contract:
  AC1 unique move-out exclusive; with_set_cow_total does not increment
  AC2 live snapshot / same-gen span still COWs; TLS acquire before heap
  AC3 Soft + stale_span_force_exclusive_enabled==0 unchanged vs
      #3233 AC2 / #3393 AC2
  AC4 extend test_pcv_exclusive_with_set; linter after #3233;
      no docs/design/3429-*; no test_issue_3429.cpp
  AC5 no new query key / FFI / packed-children rewrite; reuse
      with_set_cow_total + flatast_locked_move_out_cow_total

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _read(rel: str) -> str:
    p = ROOT / rel
    return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    hh = _read("src/core/persistent_child_vector.hh")
    ast = _read("src/core/ast.ixx")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/core/test_pcv_exclusive_with_set.cpp")
    build = _read("build.py")
    lint3233 = _read("scripts/coverage/checks/check_pcv_stale_span_exclusive_3233.py")

    must("kPcvSharedCowTlsIssue = 3429", "AC1 stamp", hh)
    must("flatast_locked_move_out_exclusive_total", "AC1 exclusive metric", ast)
    must("ac3429_1_unique_move_out_exclusive", "AC1 test", t)

    start = hh.find("PersistentChildVector with_set(")
    nxt = hh.find("void cow_set(", start) if start >= 0 else -1
    win = hh[start:nxt] if start >= 0 and nxt > start else ""
    cow = hh[hh.find("void cow_set(") : hh.find("void cow_push_back(")]
    must("tls_pcv_acquire", "AC2 with_set TLS", win)
    must("heap_pcv_allocate", "AC2 with_set heap", win)
    tls_pos = win.find("tls_pcv_acquire")
    heap_pos = win.find("heap_pcv_allocate")
    if not (0 <= tls_pos < heap_pos):
        fails.append("AC2: with_set must call tls_pcv_acquire before heap_pcv_allocate")
    must("*this = with_set", "AC2 cow_set delegates", cow)
    must("Issue #3429", "AC2 with_set cite", win)
    must("ac3429_2_live_span_tls_before_heap", "AC2 test", t)
    must("make_from_tls_or_new", "AC2 2406 helper kept", hh)
    must("note_pcv_alloc", "AC2 TLS hit skips cow_alloc", hh)

    must("stale_span_force_exclusive_enabled{0}", "AC3 Soft default", hh)
    must("ac3233_2_live_span_still_cows", "AC3 3233 AC2", t)
    must("ac3393_2_soft_off_does_not_arm", "AC3 3393 AC2", t)
    must("ac3429_3_soft_flag_off_unchanged", "AC3 test", t)
    must("3233", "AC3 3233 linter kept", lint3233)

    must("check_pcv_shared_cow_tls_3429", "AC4 build.py", build)
    must("ac3429_4_extend_suite_no_invent", "AC4 test", t)
    prev = build.find("check_pcv_stale_span_exclusive_3233")
    ours = build.find("check_pcv_shared_cow_tls_3429")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: #3429 linter must run after #3233")
    if (ROOT / "tests" / "core" / "test_issue_3429.cpp").is_file():
        fails.append("AC4: forbidden tests/core/test_issue_3429.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3429.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3429.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3429-*")):
            fails.append(f"AC4: docs/design/{f.name} present (forbidden #1655)")

    must("with_set_cow_total", "AC5 reuse with_set_cow", hh)
    must("flatast_locked_move_out_cow_total", "AC5 reuse locked COW", hh)
    must("with-set-cow-total", "AC5 query reuse", q)
    must("ac3429_5_no_new_query_or_ffi", "AC5 test", t)
    if "schema-3429" in q:
        fails.append("AC5: new schema-3429 query key")
    if "g_3429_" in hh or "g_3429_" in ast:
        fails.append("AC5: new g_3429_* counter")
    if "aura_pcv_shared_cow" in hh:
        fails.append("AC5: new FFI")
    must("std::vector<PersistentChildVector", "AC5 no packed rewrite", ast)
    must("sizeof(PcvHotpathMetrics) == 136", "AC5 metrics size", hh)

    if fails:
        print("FAIL #3429 pcv_shared_cow_tls:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3429 pcv_shared_cow_tls: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
