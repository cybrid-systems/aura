#!/usr/bin/env python3
"""Issue #2984: arena compact keeps TypeLinearCommitProof.linear_root_count consistent.

Contract:
  AC1 production + prior non-zero count + mismatch → collect + reject proof
  AC2 Soft observe only
  AC3 last==0 / no compact → no extra collect
  AC4 same linear-root family as #2673 densify scan
  AC5 additive schema-2984 on health / linear stats; preserve #2908/#2899/#2673
  AC6 source-cite evaluator_gc + arena hook + proof stamp; extend suite; no docs/design
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from query_prims_sources import read_query_prims  # Issue #2914

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
    q = read_query_prims()
    t = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    must("Issue #2984", "AC1", tma)
    must("note_arena_compact_linear_root_consistency", "AC1", tma)
    must("linear_or_dirty_roots_count_for_rebind", "AC1", tma)
    must("kTypeLinearProofOutcomeReject", "AC1", tma)
    must("ac2984_1_prod_mismatch_rejects", "AC1", t)

    must("linear_compact_root_mismatch_observe_total", "AC2", tma)
    must("ac2984_2_soft_observe", "AC2", t)

    must("last == 0", "AC3", tma)
    must("ac2984_3_quiet_no_collect", "AC3", t)

    must("2673", "AC4", tma)
    must("scan_linear_roots_after_densify", "AC4", _read("src/compiler/evaluator_typecheck.cpp"))
    must("ac2984_4_family_2673", "AC4", t)

    must("schema-2984", "AC5", q)
    must("schema-2673", "AC5", q)
    must("schema-2899", "AC5", q)
    must("schema-2908", "AC5", q)
    must("linear-compact-root-mismatch-observe-total", "AC5", q)
    must("ac2984_5_schema", "AC5", t)

    must("Issue #2984", "AC6", gc)
    must("note_arena_compact_linear_root_consistency", "AC6", gc)
    must("note_arena_compact_linear_root_consistency", "AC6", svc)
    must("ac2984_6_source_and_linter", "AC6", t)
    must("check_linear_compact_root_consistency_2984", "AC6", build)

    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in docs.glob("2984-*"):
            fails.append(f"docs/design/{f.name} present (forbidden #1655)")
    if (ROOT / "tests" / "compiler" / "test_issue_2984.cpp").is_file():
        fails.append("tests/compiler/test_issue_2984.cpp present (forbidden #81967)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2984 compact linear_root_count consistency — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
