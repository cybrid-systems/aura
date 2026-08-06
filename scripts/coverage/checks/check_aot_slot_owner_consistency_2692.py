#!/usr/bin/env python3
"""Issue #2692: Post-#2670 stable_func_id ↔ AOT slot owner consistency assert (multi-eval safety).

Contract:
  AC1 Dual Evaluator, same Define name, concurrent reemit → distinct sids;
      no map collision (regression of #2670). Distinct cross-eval counter
      bumps only when slot.owner != current owner; soft single-eval /
      process-default (filter eval = nullptr) keeps it at 0.
  AC2 Force-inject slot.owner ≠ map owner → mismatch counter bumps;
      production hard path rejects probe / clears slot; Soft observes only.
  AC3 Single-eval / owner TLS nullptr → no extra work, legacy behavior.
  AC4 #2606 cross-eval candidate skip still works; additive only.
  AC5 Query surface + schema; #2550/#2670 axes preserved.
  AC6 Source-cite + coverage linter; extend test_named_closure_stable_id_at_create
      per #81967 (no docs/design per #1655).

This linter (AC5/AC6) verifies:
  - counter cross_eval_sid_owner_mismatch_total declared in CompilerMetrics
  - counter bumper aura_bump_cross_eval_sid_owner_mismatch_total exists
  - the post-aura_register_fn_tracked assert added (cross-eval owner
    comparison + counter bump + production-hard path clears slot)
  - aura_aot_get_register_owner_eval + aura_aot_get_reemit_owner_eval
    used to compute current owner
  - query surface: counter + cross-eval-sid-owner-mismatch-wired + schema-2692
    + issue-2692 sentinels
  - no docs/design/* regression
  - #2606 / #2550 / #2670 lineage preserved

Exit 0 = OK, 1 = violation found.
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

    br = _read("src/compiler/aura_jit_bridge.cpp")
    obs = _read("src/compiler/observability_metrics.h")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    _read("build.py")

    # AC1 — counter + bumper.
    must("cross_eval_sid_owner_mismatch_total", "AC1-counter", obs)
    must("aura_bump_cross_eval_sid_owner_mismatch_total", "AC1-bumper", br)
    # The post-aura_register_fn_tracked assert must read both owner TLS keys.
    must("aura_aot_get_register_owner_eval", "AC1-owner-register", br)
    must("aura_aot_get_reemit_owner_eval", "AC1-owner-reemit", br)
    must("g_aot_register_owner_eval", "AC1-source-register", br)
    must("g_aot_reemit_owner_eval", "AC1-source-reemit", br)
    must("slot.owner_eval", "AC1-slot-owner", br)
    must("cross_eval_sid_owner_mismatch_total", "AC1-bump-usage", br)

    # AC2 — production hard path clears slot on mismatch.
    must("production_defaults_active", "AC2-prod", br)
    must("slot.fn_ptr.store(0", "AC2-clear", br)

    # AC3 — single-eval / owner TLS nullptr short-circuit.
    if "current_owner != 0" not in br:
        fails.append("AC3: single-eval short-circuit missing (current_owner != 0 gate)")

    # AC4 — #2606 lineage preserved.
    must("reemit_cross_eval_candidate_skipped_total", "AC4-2606", obs)

    # AC5 — query surface + schema-2692 sentinels.
    must("cross-eval-sid-owner-mismatch-total", "AC5-q-counter", q)
    must("cross-eval-sid-owner-mismatch-wired", "AC5-q-wired", q)
    must("schema-2692", "AC5-q-schema", q)
    must("issue-2692", "AC5-q-issue", q)

    # AC5 — #2550 / #2670 lineage preserved in C++ code (stamp / nest by
    # eval owner); query surface only carries the #2692 schema.
    must("preserve_stable_func_id_for_eval_locked", "AC5-lin-2670-code", br)
    es = _read("src/compiler/evaluator_security.cpp")
    must("stamp_stable_ref", "AC5-lin-2550-code", es)

    # AC6 — source-cite.
    must("#2692", "AC6-rt", br)
    must("#2692", "AC6-obs", obs)
    must("#2692", "AC6-q", q)

    # AC6 — no docs/design/* per #1655.
    for rel in (
        "docs/design/aot_slot_owner_consistency_2692.md",
        "docs/aot_slot_owner_consistency_2692.md",
        "design/2692.md",
    ):
        if (ROOT / rel).is_file():
            fails.append(f"AC6: unexpected design doc {rel}")

    # Linter file on disk.
    linter_path = ROOT / "scripts/coverage/checks/check_aot_slot_owner_consistency_2692.py"
    if not linter_path.is_file():
        fails.append("AC6: linter file missing on disk")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2692 AOT slot owner consistency — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
