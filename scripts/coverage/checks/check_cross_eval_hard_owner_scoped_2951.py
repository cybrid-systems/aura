#!/usr/bin/env python3
"""Issue #2951: multi-eval hard invalidate owner-scoped option.

Refine #2841/#2744/#2713 — production multi-eval hard invalidate prefers
owner-scoped stale-slot invalidate (no peer force-stale); Soft/force path
keeps joint epoch; counters distinguish hard-owner-scoped vs hard-global.

Contract (one row per AC):
  AC1  dual-eval hard owner-scoped → joint epoch stable
  AC2  Soft / env=0 / force → joint advance
  AC3  force-bump still available (reload fall-back)
  AC4  same-eval joint / throttle gate preserved
  AC5  schema-2951 + hard-owner-scoped / hard-global-bump; lineage
  AC6  tests + build.py; no invent/design

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

    cpp = _read("src/compiler/aura_jit_bridge.cpp")
    hdr = _read("src/compiler/aura_jit_bridge.h")
    dirty = _read("src/compiler/service_dirty.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_eval.cpp")
    t = _read("tests/compiler/test_named_closure_stable_id_at_create.cpp")
    build = _read("build.py")
    stub = _read("src/compiler/aura_jit_bridge_stub.cpp")

    # AC1
    must("Issue #2951", "AC1", cpp)
    must("aura_aot_note_cross_eval_hard_owner_scoped", "AC1", cpp)
    must("cross_eval_hard_owner_scoped_armed", "AC1", cpp)
    must("g_cross_eval_hard_owner_scoped_total", "AC1", cpp)
    must("aura_aot_note_cross_eval_hard_owner_scoped", "AC1", dirty)
    must("2951 AC1", "AC1", t)

    # AC2
    must("AURA_CROSS_EVAL_HARD_OWNER_SCOPED", "AC2", cpp)
    must("aura_aot_note_cross_eval_epoch_force_bump", "AC2", dirty)
    must("2951 AC2", "AC2", t)

    # AC3
    must("aura_aot_note_cross_eval_epoch_force_bump()", "AC3", cpp)
    must("g_cross_eval_hard_global_bump_total", "AC3", cpp)
    must("2951 AC3", "AC3", t)

    # AC4
    must("if (multi && !force && cross_eval_epoch_throttle_armed())", "AC4", cpp)
    must("notify_epoch_bump", "AC4", cpp)
    must("2951 AC4", "AC4", t)

    # AC5
    must("schema-2951", "AC5", q)
    must("issue-2951", "AC5", q)
    must("cross-eval-hard-owner-scoped-total", "AC5", q)
    must("cross-eval-hard-global-bump-total", "AC5", q)
    must("cross-eval-hard-owner-scoped-armed", "AC5", q)
    must("schema-2841", "AC5", q)
    must("schema-2744", "AC5", q)
    must("schema-2713", "AC5", q)
    must("aura_aot_note_cross_eval_hard_owner_scoped", "AC5", hdr)
    must("cross_eval_hard_owner_scoped_total_v_read", "AC5", hdr)
    must("aura_aot_note_cross_eval_hard_owner_scoped", "AC5", stub)

    # AC6
    must("ac2951", "AC6", t)
    must("check_cross_eval_hard_owner_scoped_2951", "AC6", build)
    if (ROOT / "tests" / "compiler" / "test_issue_2951.cpp").is_file():
        fails.append("AC6: test_issue_2951.cpp present (forbidden per #81967)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("2951-*")):
            fails.append(f"AC6: docs/design/{f.name} present (forbidden per #1655)")

    if fails:
        for f in fails:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\n{len(fails)} contract row(s) failed", file=sys.stderr)
        return 1
    print("OK: Issue #2951 multi-eval hard invalidate owner-scoped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
