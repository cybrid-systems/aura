#!/usr/bin/env python3
"""Issue #3441: aura_closure_call fast path + unnamed/sid==0 deopt_pending.

#3412 gated only the slow named arm. Warm g_closure_cache still called
pre-mutate fn; empty-name closures skipped `!slow_cname.empty()`.
Production Defer leaves g_jit_fns + cache warm. Same leave-native
decision on both arms, no new query key / counter.

Contract:
  AC1 fast path consults deopt_pending before cached fn()
  AC2 unnamed / sid==0 slow path also leave-native
  AC3 Soft/Off: #3412 named needle kept; pending count is 0
  AC4 no g_aot_table_epoch force-bump (peers stay #3300)
  AC5 no docs/design/*; no test_issue_3441.cpp; reuse fallbacks counter

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def main() -> int:
    fails: list[str] = []

    def must(n: str, label: str, hay: str) -> None:
        if n not in hay:
            fails.append(f"{label}: missing {n!r}")

    rt = (ROOT / "src" / "compiler" / "aura_jit_runtime.cpp").read_text()
    test = (ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp").read_text()
    build = (ROOT / "build.py").read_text()
    obs = (ROOT / "src" / "compiler" / "observability_metrics.h").read_text()

    must("Issue #3441", "AC1 marker", rt)
    must("closure_call_deopt_pending_leave_native_", "AC1 helper", rt)
    fast_header = rt.find("Inline cache check (Issue #1707")
    fast_gate = rt.find("closure_call_deopt_pending_leave_native_(fast_cid)")
    fast_fn = rt.find("int64_t fast_result = fn(locals, static_cast<uint32_t>(argc))")
    if fast_header < 0 or fast_gate < 0 or fast_fn < 0 or not (fast_header < fast_gate < fast_fn):
        fails.append("AC1: fast-path deopt gate must sit BETWEEN cache hit and fn()")
    must("deopt_pending_invoke_fallbacks", "AC1 reuse counter", rt)
    must("3441 AC1", "AC1 test", test)

    slow_header = rt.find("Slow path: full dispatch + cache update")
    unnamed = rt.find("closure_call_deopt_pending_leave_native_(slow_cid)")
    slow_fn = rt.find("entry.fn(locals, static_cast<uint32_t>(argc))")
    if slow_header < 0 or unnamed < 0 or slow_fn < 0 or not (slow_header < unnamed < slow_fn):
        fails.append("AC2: unnamed gate must sit BETWEEN slow resolve and entry.fn()")
    must("aura_jit_deopt_pending_count()", "AC2 unnamed count", rt)
    must("g_closure_names[slow_cid].empty()", "AC2 empty-name", rt)
    must("3441 AC2", "AC2 test", test)

    must("aura_jit_is_deopt_pending(slow_cname.c_str())", "AC3 #3412 needle", rt)
    must("Issue #3412", "AC3 #3412 cite", rt)
    must("3441 AC3", "AC3 test", test)

    must("Issue #3300", "AC4 #3300", rt)
    if "g_aot_table_epoch" in rt and "force-bump" in rt[rt.find("Issue #3441") : rt.find("Issue #3441") + 800]:
        fails.append("AC4: must not force-bump g_aot_table_epoch on owner-scoped path")
    must("3441 AC4", "AC4 test", test)

    must("check_deopt_pending_fast_path_3441", "AC5 build.py", build)
    must("3441 AC5", "AC5 test", test)
    if "3441_inv" in obs or "closure_call_fast_deopt" in obs:
        fails.append("AC5: new #3441 metric field forbidden")
    if (ROOT / "tests" / "compiler" / "test_issue_3441.cpp").is_file():
        fails.append("AC5: forbidden tests/compiler/test_issue_3441.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3441.cpp").is_file():
        fails.append("AC5: forbidden tests/issues/test_issue_3441.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3441-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    if fails:
        print("FAIL #3441 deopt_pending_fast_path:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3441 deopt_pending_fast_path: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
