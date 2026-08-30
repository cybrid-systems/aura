#!/usr/bin/env python3
"""Issue #3448: remount last_proof_linear_root_count==0 must drop green face.

#3227 / #2984 last==0 quiet-returned before face stores. A green stamp
with linear_root_count==0 survived compact / remount, so Move/Drop
elision rode the pre-remount face.

Contract:
  AC1 Production last==0 + green face → drop face + invalidate_gen
  AC2 last>0 path unchanged (#3227 count-match still drops face + gen)
  AC3 no green face + last==0 → still quiet (no extra collect)
  AC4 Soft/Off: no new query key; Soft does not hard-drop last==0 green

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

    def must_not(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    tma = _read("src/compiler/typed_mutation_audit.h")
    gc = _read("src/compiler/evaluator_gc.cpp")
    svc = _read("src/compiler/service.ixx")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    elide = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    steal = _read("tests/compiler/test_post_steal_linear_revalidate.cpp")
    gcwin = _read("tests/compiler/test_linear_gc_window.cpp")
    hot = _read("tests/compiler/test_compiler_hot_update_facade.cpp")
    mutate = _read("src/compiler/evaluator_primitives_mutate.cpp")
    q = _read("src/compiler/evaluator_primitives_query_reflect.cpp")
    build = _read("build.py")

    fn = tma.find("rebind_linear_proof_after_root_migration() noexcept")
    if fn < 0:
        fails.append("AC1: helper missing")
        body = ""
    else:
        body = tma[fn : fn + 2200]
    must("Issue #3448", "AC1 helper cite", tma)
    must("kLinearZeroRootGreenFaceDropIssue", "AC1 stamp", tma)
    must("green_face", "AC1 green-face predicate", body)
    must("g_last_proof_would_allow_commit", "AC1 would_allow load", body)
    must("g_last_proof_linear_ok", "AC1 linear_ok load", body)
    must("linear_or_dirty_roots_count_for_rebind", "AC1 extra collect", body)
    must("g_rehydrate_miss_invalidate_gen", "AC1 steal/densify gen", body)
    must("kTypeLinearProofOutcomeReject", "AC1 reject outcome", body)
    must("rebind_linear_proof_after_root_migration", "AC1 compact_sweep", gc)
    must("Issue #3448", "AC1 compact_sweep cite", gc)
    must("rebind_linear_proof_after_root_migration", "AC1 compact hook", svc)
    must("Issue #3448", "AC1 remount cite", rt)
    must("ac3448_1_prod_last0_green_drops_face", "AC1 health test", t)
    must("3448 AC1", "AC1 elision suite", elide)
    must("linear_move_drop_elision_ok", "AC1 elision assert", t)

    must("note_arena_compact_linear_root_consistency", "AC2 #2984 last>0", body)
    must("ac3448_2_last_gt0_unchanged", "AC2 test", t)
    must("ac3227_1_prod_rebind_blocks_elision", "AC2 #3227 retained", t)

    must("if (last == 0 && !green_face)", "AC3 quiet", body)
    must("ac3448_3_no_face_last0_quiet", "AC3 test", t)

    must("if (!hard)", "AC4 Soft skip gen", body)
    must("ac3448_4_soft_no_new_key", "AC4 test", t)
    must_not("schema-3448", "AC4 no schema in tma", tma)
    must_not("schema-3448", "AC4 no new query key", mutate)
    must_not("schema-3448", "AC4 no schema in query", q)
    must_not("g_3448_", "AC4 no new metric", tma)
    must("check_linear_zero_root_green_face_drop_3448", "AC4 build.py", build)
    must("kLinearZeroRootGreenFaceDropIssue", "AC4 steal lineage", steal)
    must("Issue #3448", "AC4 gc window", gcwin)
    must("kLinearZeroRootGreenFaceDropIssue", "AC4 hot-update", hot)
    prev = build.find("check_linear_post_migration_proof_rebind_3227")
    ours = build.find("check_linear_zero_root_green_face_drop_3448")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC4: #3448 linter must run after #3227")
    if (ROOT / "tests" / "compiler" / "test_issue_3448.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3448.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3448.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3448.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3448-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3448 linear_zero_root_green_face_drop:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3448 linear_zero_root_green_face_drop: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
