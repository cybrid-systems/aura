#!/usr/bin/env python3
"""Issue #2841: multi-eval cascade must default to owner-scoped epoch.

Residual of #2713/#2744: under production multi-eval, soft cascade must
not advance process-global g_aot_table_epoch solely to heal one owner's
dirty set (peer force-stale). Hard invalidate keeps force-bump.

  AC1 dual-eval production cascade → owner-scoped (epoch stable, throttled++)
  AC2 hard force-bump still advances joint epoch
  AC3 single-eval / map size ≤1 unchanged
  AC4 Soft env opt-in preserved; production default arms throttle;
      soft cascade stamps owner TLS
  AC5 additive query schema-2841; preserve 2713/2744/2606
  AC6 test extension + linter; no docs/design/; no invent file

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import subprocess
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

    cpp = _read("src/compiler/aura_jit_bridge.cpp")
    hdr = _read("src/compiler/aura_jit_bridge.h")
    svc = _read("src/compiler/service.ixx")
    dirty = _read("src/compiler/service_dirty.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_named_closure_stable_id_at_create.cpp")
    build = _read("build.py")

    # AC1 — owner-scoped cascade path
    must("Issue #2841", "AC1", cpp)
    must("aura_aot_invalidate_all_stale_slots_for_eval(owner)", "AC1", cpp)
    must("g_cross_eval_epoch_action_throttled_total", "AC1", cpp)
    must("cross_eval_epoch_throttle_armed", "AC1", cpp)
    must("CascadeEvalOwnerGuard", "AC1", svc)
    must("aura_aot_set_reemit_owner_eval", "AC1", svc)

    # AC2 — hard force-bump
    must("aura_aot_note_cross_eval_epoch_force_bump()", "AC2", dirty)
    must("Issue #2841", "AC2", dirty)
    must("aura_aot_note_cross_eval_epoch_force_bump", "AC2", hdr)

    # AC3 — multi gate
    must("aura_aot_state_map_size() > 1", "AC3", cpp)
    must("if (multi && !force && cross_eval_epoch_throttle_armed())", "AC3", cpp)

    # AC4 — Soft opt-in + production default
    must("AURA_CROSS_EVAL_EPOCH_THROTTLE", "AC4", cpp)
    must("production_defaults_active()", "AC4", cpp)

    # AC5 — additive query
    must("schema-2841", "AC5", q)
    must("issue-2841", "AC5", q)
    must("cross-eval-epoch-cascade-owner-scoped-default-wired", "AC5", q)
    must("schema-2744", "AC5", q)
    must("schema-2713", "AC5", q)
    must("schema-2606", "AC5", q)

    # AC6 — tests + linter + no invent/design
    must("ac2841_1_dual_eval_cascade_owner_scoped", "AC6", t)
    must("ac2841_2_hard_force_advances_joint_epoch", "AC6", t)
    must("ac2841_3_single_eval_unchanged", "AC6", t)
    must("ac2841_4_soft_opt_in_production_default", "AC6", t)
    must("ac2841_5_query_additive", "AC6", t)
    must("ac2841_6_source_and_linter", "AC6", t)
    must("check_cross_eval_cascade_owner_scoped_2841", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2841.cpp").is_file():
        fails.append("AC6: test_issue_2841.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("*2841*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    # Regression: #2713 / #2744 linters still green
    for linter, path in (
        ("check_cross_eval_epoch_bump_2713.py", ROOT / "scripts" / "check_cross_eval_epoch_bump_2713.py"),
    ):
        if path.is_file():
            r = subprocess.run(
                [sys.executable, str(path)],
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            if r.returncode != 0:
                fails.append(f"{linter} regression:\n{r.stdout}\n{r.stderr}")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2841 multi-eval cascade owner-scoped epoch default")
    return 0


if __name__ == "__main__":
    sys.exit(main())
