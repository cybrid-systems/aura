#!/usr/bin/env python3
"""Issue #3654: Production/Full unset linear_post_mutate_enforce is unsafe.

Strong aura_jit_bridge.cpp used to return 0 (safe) when the host callback
or env context was missing. Production native then skipped Evaluator
linear revalidate. Soft / light-link keep pass-through 0.

Contract (one row per AC):
  AC1  production_hard_face_active + !fn / null env → return 1 (deopt)
  AC2  Soft / Off / stub without defaults → return 0
  AC3  registered callback still 0=safe / 1=unsafe (g_linear_enforce_fn)
  AC4  typed-entry and Move/Drop elision stay AND (not OR'd into elision)
  AC5  extends test_occurrence_goal_persist_rehydrate +
       test_steal_complete_strong_entry; linter AFTER #3343; no
       test_issue_3654.cpp; no docs/design/; no schema-3654 / g_3654_*

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

    brc = _read("src/compiler/aura_jit_bridge.cpp")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")
    hdr = _read("src/compiler/aura_jit_bridge.h")
    tma = _read("src/compiler/typed_mutation_audit.h")
    jit = _read("src/compiler/aura_jit.cpp")
    occ = _read("tests/compiler/test_occurrence_goal_persist_rehydrate.cpp")
    steal = _read("tests/serve/test_steal_complete_strong_entry.cpp")
    build = _read("build.py")

    fn = brc.find('extern "C" int aura_jit_linear_post_mutate_enforce')
    win = brc[fn : fn + 1800] if fn >= 0 else ""

    # AC1
    must("Issue #3654", "AC1 strong cite", win)
    must("production_hard_face_active()", "AC1 hard-face", win)
    must("return 1", "AC1 production unsafe", win)

    # AC2
    must("return 0", "AC2 Soft pass-through", win)
    must("Issue #3343 / #3654", "AC2 stub cite", stub)
    pm = stub.find('extern "C" __attribute__((weak)) int aura_jit_linear_post_mutate_enforce')
    pm_win = stub[pm : pm + 450] if pm >= 0 else ""
    must("return 1", "AC2 stub production unsafe", pm_win)
    must("return 0", "AC2 stub Soft pass", pm_win)

    # AC3
    must("g_linear_enforce_fn(g_linear_enforce_user, id)", "AC3 callback still invoked", win)
    must("Production/Full + no callback", "AC3 header contract", hdr)

    # AC4 — do not OR typed-entry into elision_ok
    el = tma.find("inline bool linear_move_drop_elision_ok()")
    el_win = tma[el : el + 900] if el >= 0 else ""
    must("linear_ir_fastpath_try_skip()", "AC4 elision AND fast-path", el_win)
    must("commit_readiness(", "AC4 elision AND live commit_readiness", el_win)
    must("ir_typed_entry_commit_readiness_ok", "AC4 typed-entry stays separate", tma)
    must("can_linear = hard_typed_entry && !can_epoch", "AC4 #3616 anon emit", jit)

    # AC5
    must("check_jit_linear_post_mutate_unset_3654", "AC5 build.py", build)
    must("ac3654_linear_post_mutate_unset_fail_closed", "AC5 steal test", steal)
    must("ac3654_linear_post_mutate_unset_fail_closed", "AC5 occ test", occ)
    prev = build.find("check_production_weak_abi_commit_readiness_3343")
    ours = build.find("check_jit_linear_post_mutate_unset_3654")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3343")
    if "schema-3654" in brc or "schema-3654" in stub:
        fails.append("AC5: new schema-3654 query key")
    if "g_3654_" in brc or "g_3654_" in stub:
        fails.append("AC5: new g_3654_* counter")
    if _read("tests/compiler/test_issue_3654.cpp"):
        fails.append("AC5: test_issue_3654.cpp present (forbidden)")
    if _read("tests/serve/test_issue_3654.cpp"):
        fails.append("AC5: tests/serve/test_issue_3654.cpp present (forbidden)")
    if _read("docs/design/3654-jit-linear-post-mutate-unset.md"):
        fails.append("AC5: docs/design/ exists — forbidden")

    if fails:
        print("FAIL #3654 jit_linear_post_mutate_unset:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3654 jit_linear_post_mutate_unset")
    return 0


if __name__ == "__main__":
    sys.exit(main())
