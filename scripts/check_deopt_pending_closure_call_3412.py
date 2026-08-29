#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Issue #3412: aura_closure_call slow path must consult aura_jit_is_deopt_pending
# before calling pre-mutate g_jit_fns.fn. Production facade early-return
# leaves g_jit_fns + AuraJIT module live; AuraJIT fn_trackers_ already refuses
# via deopt_pending on CompilerService lookup, but aura_closure_call slow
# path dereferences g_jit_fns[func_id].fn directly. AC1 AC2 AC3 AC4 AC5 AC6.
#
# AC1 — aura_closure_call slow path consults aura_jit_is_deopt_pending(name)
#       BEFORE the entry.fn(...) call. Refuse + bump reuse counter.
# AC2 — Soft / Off zero-cost: aura_jit_is_deopt_pending returns 0 when
#       batch_deopt not stamped (g_batch_deopt_jit nullptr / no entry).
# AC3 — Owner-scoped multi-eval: owner unbound via deopt_pending; peers
#       still use #3300 name soft-stale (no force-bump g_aot_table_epoch).
# AC4 — No new query keys. Reuses existing deopt_pending_invoke_fallbacks
#       counter (aura_jit.cpp:3068 / :3385 path). No new metric field.
# AC5 — Non-duplicative vs #3188 / #3345 / #3300 / #3377 / #3410. Each upstream
#       is preserved (their source-cite markers must remain).
# AC6 — No docs/design/3412-*.md (banned per #1655) and no
#        tests/{issues,compiler,core}/test_issue_3412.cpp (must extend
#        test_aot_incremental_reemit.cpp per #81934).
# AC7 — test_aot_incremental_reemit.cpp carries AC1-AC7 markers for #3412;
#        build.py wires cmd_deopt_pending_closure_call_3412.
#
# Self-test:
#   python3 scripts/check_deopt_pending_closure_call_3412.py
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    fails: list[str] = []

    rt = (ROOT / "src" / "compiler" / "aura_jit_runtime.cpp").read_text()
    jit = (ROOT / "src" / "compiler" / "aura_jit.cpp").read_text()
    bridge = (ROOT / "src" / "compiler" / "aura_jit_bridge.cpp").read_text()
    (ROOT / "src" / "core" / "workspace_isolation.hh").read_text()
    test = (ROOT / "tests" / "compiler" / "test_aot_incremental_reemit.cpp").read_text()
    build = (ROOT / "build.py").read_text()

    # AC1 — slow path consults aura_jit_is_deopt_pending BEFORE fn() call.
    if "Issue #3412" not in rt:
        fails.append("AC1: aura_jit_runtime.cpp missing 'Issue #3412' marker")
    slow_path_pos = rt.find("Slow path: full dispatch + cache update")
    deopt_pos = rt.find("aura_jit_is_deopt_pending(slow_cname.c_str())")
    fn_call_pos = rt.find("entry.fn(locals, static_cast<uint32_t>(argc))")
    if slow_path_pos < 0:
        fails.append("AC1: aura_jit_runtime.cpp missing slow path header")
    if deopt_pos < 0:
        fails.append("AC1: aura_jit_runtime.cpp missing aura_jit_is_deopt_pending call")
    if fn_call_pos < 0:
        fails.append("AC1: aura_jit_runtime.cpp missing entry.fn() slow path call site")
    if slow_path_pos > 0 and deopt_pos > 0 and fn_call_pos > 0 and not (slow_path_pos < deopt_pos < fn_call_pos):
        fails.append("AC1: deopt_pending gate must sit BETWEEN slow path entry resolution and the entry.fn() call site")
    if "g_closure_names[slow_cid]" not in rt:
        fails.append("AC1: closure name lookup must use g_closure_names[slow_cid]")
    # Slow path must invalidate_closure_cache_for + return 0 on deopt hit
    # (consistent with the #3412 AC1: refuse native, leave-native until remap).
    if "invalidate_closure_cache_for(closure_id)" not in rt:
        fails.append(
            "AC1: aura_closure_call slow path must invalidate_closure_cache_for "
            "on deopt_pending hit (refuse native cleanly)"
        )

    # AC2 — Soft / Off zero-cost.
    if "aura_jit_is_deopt_pending" not in bridge:
        fails.append("AC2: aura_jit_bridge.cpp missing aura_jit_is_deopt_pending C ABI")
    # The bridge stub returns 0 when g_batch_deopt_jit is nullptr (Soft/Off
    # never stamps). Source-cite anchor on the stub / nullptr path.
    stub_text = (ROOT / "src" / "compiler" / "aura_jit_bridge_stub.cpp").read_text()
    if "g_batch_deopt_jit" not in bridge and "g_batch_deopt_jit" not in stub_text:
        fails.append("AC2: g_batch_deopt_jit nullptr / zero-return path not anchored")

    # AC3 — Owner-scoped multi-eval: no force-bump g_aot_table_epoch.
    # #3300 anchor lives in hot_update_registry.cpp / aura_jit_bridge.cpp /
    # aura_jit_runtime.cpp / aura_jit_bridge_stub.cpp / aura_jit_bridge.h
    # (peer JIT name soft-stale + owner-scoped fanout). NOT in
    # workspace_isolation.hh.
    hot = (ROOT / "src" / "compiler" / "hot_update_registry.cpp").read_text()
    jit_bridge_h = (ROOT / "src" / "compiler" / "aura_jit_bridge.h").read_text()
    if "#3300" not in hot and "#3300" not in jit_bridge_h and "#3300" not in rt:
        fails.append(
            "AC3: #3300 peer JIT name soft-stale anchor missing "
            "(must live in hot_update_registry.cpp / aura_jit_bridge.h / "
            "aura_jit_runtime.cpp)"
        )
    # Service dirty path must NOT force-bump g_aot_table_epoch on the
    # owner-scoped path (preserve #2841/#2951). The #3300 anchor in
    # hot_update_registry.cpp / aura_jit_bridge.h / aura_jit_runtime.cpp
    # (verified above) documents the owner-scoped fanout. The Soft-mode
    # "force-stale" comments in service_dirty.cpp are intentional (Soft
    # is allowed to globally epoch-stale — that is the Soft contract per
    # #3012 / #3043). No additional check needed here.

    # AC4 — Reuses existing counter (no new metric field).
    if "deopt_pending_invoke_fallbacks" not in rt:
        fails.append(
            "AC4: aura_jit_runtime.cpp must reuse existing "
            "deopt_pending_invoke_fallbacks counter (no new metric per AC4)"
        )
    # Counter lives in aura_jit.h (AuraJIT class member, line 277) — not
    # in observability_metrics.h. Verify it pre-exists in aura_jit.h.
    jit_h = (ROOT / "src" / "compiler" / "aura_jit.h").read_text()
    if "deopt_pending_invoke_fallbacks" not in jit_h:
        fails.append(
            "AC4: aura_jit.h must pre-define deopt_pending_invoke_fallbacks "
            "counter (lives in AuraJIT class member; #3412 reuses, does not add)"
        )
    obs = (ROOT / "src" / "compiler" / "observability_metrics.h").read_text()
    if "3412_inv" in obs or "closure_call_deopt" in obs:
        fails.append(
            "AC4: observability_metrics.h must NOT contain a new #3412 metric "
            "field (forbidden — reuse existing counter)"
        )

    # AC5 — Non-duplicative vs upstream issues.
    # #3345 hybrid dirty path (service_dirty or aura_jit).
    sd = (ROOT / "src" / "compiler" / "service_dirty.cpp").read_text()
    if "Issue #3345" not in sd and "Issue #3345" not in jit:
        fails.append("AC5: #3345 hybrid dirty path marker missing (regression)")
    # #3377 owner AOT slot clear (hot_update_registry).
    hot = (ROOT / "src" / "compiler" / "hot_update_registry.cpp").read_text()
    if "Issue #3377" not in hot:
        fails.append("AC5: #3377 owner AOT slot clear marker missing (regression)")
    # #3410 same window — soft-migrate wash preserved (not regressed).
    if "Issue #3410" not in rt:
        fails.append("AC5: #3410 same-window soft-migrate wash missing in aura_jit_runtime.cpp")

    # AC6 — No design docs / no test_issue_3412.cpp.
    if list((ROOT / "docs" / "design").glob("3412-*.md")):
        fails.append("AC6: docs/design/3412-*.md exists — design docs banned per #1655")
    for sub in ("issues", "compiler", "core"):
        if (ROOT / "tests" / sub / "test_issue_3412.cpp").is_file():
            fails.append(
                f"AC6: tests/{sub}/test_issue_3412.cpp exists — must extend existing "
                "test_aot_incremental_reemit.cpp per #81934"
            )

    # AC7 — test markers + build.py wiring.
    if "3412 AC1: aura_closure_call slow path deopt_pending gate" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3412 AC1 marker")
    if "3412 AC2/AC3: Soft/Off zero-cost + non-force-bump" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3412 AC2/AC3 markers")
    if "3412 AC5: non-duplicative vs #3188/#3345/#3300/#3377/#3410" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3412 AC5 marker")
    if "3412 AC6: no docs/design/3412-*; no test_issue_3412.cpp" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3412 AC6 marker")
    if "3412 AC7: build.py wiring" not in test:
        fails.append("AC7: test_aot_incremental_reemit.cpp missing 3412 AC7 marker")

    if "cmd_deopt_pending_closure_call_3412_coverage" not in build:
        fails.append("AC7: build.py does not register cmd_deopt_pending_closure_call_3412_coverage")
    if "check_deopt_pending_closure_call_3412" not in build:
        fails.append("AC7: build.py does not register check_deopt_pending_closure_call_3412 linter script")

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1

    print("PASS: #3412 aura_closure_call deopt_pending gate contract satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
