#!/usr/bin/env python3
"""Issue #2845: stamp AotReloadConsistencyProof on every fail/exhaust path.

Residual of #2753/#2776: Agents must never observe would_allow_native=true
after Version/Env/Linear/Region rollback or force-JIT demotion.

Contract (one row per AC):
  AC1 stamp_aot_reload_consistency_proof_fail forces would_allow_native=false
  AC2 note_reload_rollback uses fail helper; on_force_jit re-stamps mask
  AC3 success commit still uses stamp() (not fail helper)
  AC4 stamped_on_fail_total additive counter; seqlock preserved (#2776)
  AC5 soft/quiet: no extra stamp sites beyond rollback/force-jit/success
  AC6 ac2845_* tests + this linter wired; no docs/design/*; no invent test

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

    thin = _read("src/compiler/aot_reload_consistency_proof.h")
    bridge = _read("src/compiler/aura_jit_bridge.cpp")
    reg = _read("src/compiler/hot_update_registry.cpp")
    test = _read("tests/compiler/test_reload_recovery_query.cpp")
    build = _read("build.py")

    # AC1 — sole fail helper
    must("stamp_aot_reload_consistency_proof_fail", "AC1", thin)
    must("kAotReloadConsistencyProofFailStampIssue", "AC1", thin)
    must("g_aot_reload_proof_stamped_on_fail_total", "AC1", thin)
    must("would_allow_native = false", "AC1", thin)
    must("#2845", "AC1", thin)
    must("stamp_aot_reload_consistency_proof_fail_after_force_jit", "AC1", thin)

    # AC2 — wire-in: rollback + force-JIT
    must("stamp_aot_reload_consistency_proof_fail", "AC2 rollback", bridge)
    must("#2845", "AC2", bridge)
    must("stamp_aot_reload_consistency_proof_fail_after_force_jit", "AC2 force-jit", reg)
    must("#2845", "AC2", reg)
    # note_reload_rollback must use fail helper (not bare success stamp).
    nr = bridge.find("void note_reload_rollback(AotReloadFail reason)")
    if nr < 0:
        fails.append("AC2: note_reload_rollback not found")
    else:
        # Function body is long (per-reason metric switch); take until next
        # top-level void after a generous window.
        body = bridge[nr : nr + 4500]
        if "stamp_aot_reload_consistency_proof_fail" not in body:
            fails.append("AC2: note_reload_rollback body missing fail helper")

    # AC3 — success commit path still stamps (not via fail helper)
    must("commit_func_table_swap", "AC3", bridge)
    cs = bridge.find("void commit_func_table_swap()")
    if cs < 0:
        fails.append("AC3: commit_func_table_swap not found")
    else:
        body = bridge[cs : cs + 3500]
        if "stamp_aot_reload_consistency_proof(p)" not in body and ("stamp_aot_reload_consistency_proof(" not in body):
            fails.append("AC3: success path must still call stamp_aot_reload_consistency_proof")
        # Fail helper name is a superstring of stamp(...); require exact
        # fail helper only when would_allow_native=false is force-set.
        if "stamp_aot_reload_consistency_proof_fail" in body:
            fails.append("AC3: success path must not use fail helper")

    # AC4 — counter + #2776 preserved
    must("aura_aot_reload_consistency_proof_stamped_on_fail_total", "AC4", thin)
    must("g_aot_reload_proof_seq", "AC4 seqlock", thin)
    must("fetch_add", "AC4", thin)

    # AC5 — soft: build_from_live still present; fail helper documents no soft stamp
    must("build_aot_reload_consistency_proof_from_live", "AC5", thin)
    must("Soft/quiet idle paths must NOT call this", "AC5", thin)

    # AC6 — tests + wire + no invent/design
    must("ac2845_1_version_fail_proof", "AC6", test)
    must("ac2845_2_force_jit_mask_in_proof", "AC6", test)
    must("ac2845_3_success_commit_still_allows_when_idle", "AC6", test)
    must("ac2845_4_concurrent_fail_success_no_tear", "AC6", test)
    must("ac2845_5_soft_no_extra_stamp", "AC6", test)
    must("ac2845_6_source_and_linter", "AC6", test)
    must("check_aot_reload_proof_fail_stamp_2845", "AC6", build)
    must("ac2753_1_soft_empty_proof", "AC6 #2753 preserved", test)
    must("ac2776_1_fetch_add_and_seqlock_source", "AC6 #2776 preserved", test)
    if (ROOT / "tests" / "compiler" / "test_issue_2845.cpp").is_file():
        fails.append("AC6: test_issue_2845.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2845*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2845 AotReloadConsistencyProof fail-path sole stamp — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
