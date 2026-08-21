#!/usr/bin/env python3
"""Issue #3227: post-compact / remount re-bind TypeLinearCommitProof.

After GC compact or hot-update remount, live linear roots can move
without an outermost proof restamp. Production reuses #2984 compact
consistency + densify/steal invalidate_gen so Move/Drop cannot elide
until the next green bind. Soft observe; last==0 quiet. No new query key.

Contract:
  AC1 Production last!=0 → reject face + invalidate_gen; no green elision
  AC2 Soft observe only; quiet last==0 unchanged
  AC3 Densify/steal + post-steal revalidate retained
  AC4 Extend linear_gc / post_steal / hot-update suites; linter; no invent

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

    tma = _read("src/compiler/typed_mutation_audit.h")
    gc = _read("src/compiler/evaluator_gc.cpp")
    svc = _read("src/compiler/service.ixx")
    rt = _read("src/compiler/aura_jit_runtime.cpp")
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    steal = _read("tests/compiler/test_post_steal_linear_revalidate.cpp")
    gcwin = _read("tests/compiler/test_linear_gc_window.cpp")
    hot = _read("tests/compiler/test_compiler_hot_update_facade.cpp")
    build = _read("build.py")

    must("kLinearPostMigrationProofRebindIssue", "AC1 stamp", tma)
    must("rebind_linear_proof_after_root_migration", "AC1 helper", tma)
    must("g_rehydrate_miss_invalidate_gen", "AC1 steal/densify gen", tma)
    must("rebind_linear_proof_after_root_migration", "AC1 compact_sweep", gc)
    must("rebind_linear_proof_after_root_migration", "AC1 compact hook", svc)
    must("rebind_linear_proof_after_root_migration", "AC1 remount", rt)
    must("linear_move_drop_elision_ok", "AC1 elision", t)
    must("ac3227_1_prod_rebind_blocks_elision", "AC1 test", t)

    must("if (last == 0)", "AC2 quiet", tma)
    must("if (!hard)", "AC2 Soft skip gen", tma)
    must("ac3227_2_soft_and_quiet", "AC2 test", t)

    must("revalidate_linear_type_provenance_after_migration", "AC3 steal", gc)
    must("invalidate_fast_path_before_steal_densify_restamp", "AC3 #3063", tma)
    must("note_arena_compact_linear_root_consistency", "AC3 #2984", tma)
    must("revalidate_linear_type_provenance_after_migration", "AC3 steal suite", steal)

    must("rebind_linear_proof_after_root_migration", "AC4 gc window", gcwin)
    must("rebind_linear_proof_after_root_migration", "AC4 hot-update", hot)
    must("check_linear_post_migration_proof_rebind_3227", "AC4 build.py", build)
    if "g_3227_" in tma:
        fails.append("AC4: new g_3227_* counter")
    if "query:linear-post-migration" in tma:
        fails.append("AC4: new query:*")
    if (ROOT / "tests" / "issues" / "test_issue_3227.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3227.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3227.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3227.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3227-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3227 linear_post_migration_proof_rebind:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3227 linear_post_migration_proof_rebind: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
