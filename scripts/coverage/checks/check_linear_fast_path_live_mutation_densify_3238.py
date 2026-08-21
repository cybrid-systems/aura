#!/usr/bin/env python3
"""Issue #3238: densify/escape under live mutation forces !linear_fast_path_ok.

#2964/#3006 force dirty-root revalidate on Guard *exit*. Residual: densify
entry or escape while depth!=0 / process-held can still elide against a
stale fast-path face. Production: invalidate_gen + dirty-root immediately.
Soft observe. Quiet (!live) two loads. Reuses #3006 dirty counter and
#3171 gen face. No new query key.

Contract:
  AC1 Production densify under live mutation → !ok + dirty-root before elision
  AC2 Soft observe; quiet !live is no extra
  AC3 lineage #2964/#3006/#3224/#3227
  AC4 extend test_escape_move_elision_gate; this linter; no invent / docs

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

    aud = _read("src/compiler/typed_mutation_audit.h")
    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    efm = _read("src/compiler/evaluator_fiber_mutation.cpp")
    t = _read("tests/compiler/test_escape_move_elision_gate.cpp")
    q = _read("src/compiler/evaluator_primitives_obs_jit.cpp")
    build = _read("build.py")

    must("kLinearFastPathLiveMutationDensifyIssue = 3238", "AC1 stamp", aud)
    must("note_densify_entry_under_live_mutation", "AC1 helper", aud)
    must("linear_fast_path_live_mutation_active", "AC1 live", aud)
    must("note_densify_entry_under_live_mutation", "AC1 densify entry", mb)
    must("enforce_linear_boundary_consistency", "AC1 dirty-root", mb)
    must("ac3238_1_live_densify_production", "AC1 test", t)

    must("ac3238_2_soft_quiet", "AC2 test", t)
    must("g_linear_fast_path_force_revalidate_observe_total", "AC2 soft", aud)
    h = aud.find("note_densify_entry_under_live_mutation")
    if h < 0:
        fails.append("AC2: helper missing")
    else:
        end = aud.find("}", h)
        body = aud[h:end] if end > h else ""
        if "if (!linear_fast_path_live_mutation_active())" not in body:
            fails.append("AC2: quiet live-mutation check first")

    must("linear_fast_path_ok", "AC3 #2964", aud)
    must("kLinearFastPathDirtyRevalidateIssue", "AC3 #3006", aud)
    must("kIrTypedEntryCommitReadinessIssue = 3224", "AC3 #3224", aud)
    must("invalidate_fast_path_before_steal_densify_restamp", "AC3 #3227", aud)
    must("note_densify_entry_under_live_mutation", "AC3 steal/densify", efm)
    must("ac3238_3_lineage", "AC3 test", t)
    if "schema-3238" in q:
        fails.append("AC3: new schema-3238 query key")
    if "g_3238_" in aud:
        fails.append("AC3: new g_3238_* counter")

    must("check_linear_fast_path_live_mutation_densify_3238", "AC4 build.py", build)
    must("ac3238_4_source_linter", "AC4 test", t)
    must("Issue #3238", "AC4 try_skip resample", aud)
    if (ROOT / "tests" / "compiler" / "test_issue_3238.cpp").is_file():
        fails.append("AC4: tests/compiler/test_issue_3238.cpp")
    if (ROOT / "tests" / "issues" / "test_issue_3238.cpp").is_file():
        fails.append("AC4: tests/issues/test_issue_3238.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3238-*")):
            fails.append(f"AC4: docs/design/{f.name}")

    if fails:
        print("FAIL #3238 linear_fast_path_live_mutation_densify:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3238 linear_fast_path_live_mutation_densify: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
