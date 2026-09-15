#!/usr/bin/env python3
"""Issue #3818: #3472 post-persist linear deny undoes #3545 CoercionMap journal.

In-helper persist-reject runs note_3440_restore → aura_persist_reject_undo
(AST #3687 + CoercionMap #3545). #3472 post-persist belt previously flipped
success without journal undo; abort_restore restores AST but not elim counts.

Contract (one row per AC):
  AC1  #3472 window calls aura_persist_reject_undo / undo_apply_coercion_map
  AC2  Shared helper keeps AST+#3545 order; abort_restore / #3687 still SSOT
  AC3  Soft/Off: hard gate unchanged (production_defaults / Full only)
  AC4  Tests extend named suites; no invent / docs/design / new query key

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

    emb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    t_persist = _read("tests/compiler/test_outermost_persist_fail_closed.cpp")
    t_lin = _read("tests/compiler/test_linear_enforce_production_defaults.cpp")
    t_health = _read("tests/compiler/test_type_linear_commit_health.cpp")
    build = _read("build.py")

    must("Issue #3818", "AC1 cite", emb)
    must("aura_persist_reject_undo", "AC1 shared helper", emb)

    issue = emb.find("Issue #3472")
    exit_pos = emb.find("ev_->exit_mutation_boundary(success)")
    win = emb[issue:exit_pos] if issue >= 0 and exit_pos > issue else ""
    if "aura_persist_reject_undo" not in win and "undo_apply_coercion_map_recent" not in win:
        fails.append("AC1: #3472 window missing CoercionMap journal undo")

    shared_pos = emb.find("static void aura_persist_reject_undo")
    shared = emb[shared_pos : emb.find("extern \"C\" void aura_outermost_success_persist_occurrence", shared_pos)] if shared_pos >= 0 else ""
    topo = shared.find("restore_checkpoint_topology_for_persist_reject")
    undo = shared.find("undo_apply_coercion_map_recent")
    if not (topo >= 0 and undo >= 0 and topo < undo):
        fails.append("AC2: shared helper must restore AST before CoercionMap undo")
    must("if (!cp.topology_restored)", "AC2 abort_restore no-op", emb)
    must("note_3440_restore", "AC2 in-helper still uses shared undo", emb)

    must("production_defaults_active()", "AC3 hard gate", win)
    must("AuditStrategy::Full", "AC3 Full", win)

    must("3818", "AC4 persist suite", t_persist)
    must("3818 soak", "AC4 soak", t_persist)
    must("3818", "AC4 linear suite", t_lin)
    must("3818", "AC4 health suite", t_health)
    must("check_post_persist_linear_deny_coercion_undo_3818", "AC4 build", build)
    if (ROOT / "tests" / "compiler" / "test_issue_3818.cpp").is_file():
        fails.append("AC4: forbidden tests/compiler/test_issue_3818.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3818.cpp").is_file():
        fails.append("AC4: forbidden tests/issues/test_issue_3818.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3818-*")):
            fails.append(f"AC4: docs/design/{f.name} present")
    if "schema-3818" in emb:
        fails.append("AC4: schema-3818 query key present")

    if fails:
        print("FAIL #3818 post-persist linear deny CoercionMap undo:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3818 post-persist linear deny undoes CoercionMap journal like in-helper")
    return 0


if __name__ == "__main__":
    sys.exit(main())
