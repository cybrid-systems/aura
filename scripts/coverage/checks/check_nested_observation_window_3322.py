#!/usr/bin/env python3
"""Issue #3322: nested / render-fast observation window close.

Nested Guards and render_fast_exit can leave defuse_index_ cached until
outermost TypeLinearCommitProof stamp (half-green under concurrent query).
Production/Full force-close (invalidate defuse, no persist/stamp). Soft/Off
zero extra. No new query key.

Contract:
  AC1 nested production exit closes window (success or fail)
  AC2 Soft/Off: helper no-ops
  AC3 outermost happy path unchanged; persist still outermost-only
  AC4 render-fast skip still closes under production
  AC5 extend hygiene_mutate_closed_loop + mutation_boundary_batch; no invent

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

    mb = _read("src/compiler/evaluator_mutation_boundary.cpp")
    met = _read("src/compiler/observability_metrics.h")
    hyg = _read("tests/compiler/test_hygiene_mutate_closed_loop.cpp")
    batch = _read("tests/compiler/test_mutation_boundary_batch.cpp")
    build = _read("build.py")

    must("Issue #3322", "AC1 boundary", mb)
    must("invalidate_defuse_index_for_nested", "AC1 helper", mb)
    must("nested_observation_window_closed_total", "AC1 counter", met)
    must("kNestedObservationWindowIssue = 3322", "AC1 stamp", met)
    must("ac3322_1_nested_closes_window", "AC1 test", hyg)

    must("Soft/Off: helper no-ops", "AC2 comment", mb)
    must("ac3322_2_soft_zero_extra", "AC2 test", hyg)

    must("aura_outermost_success_persist_occurrence", "AC3 persist outermost", mb)
    must("ac3322_3_outermost_happy_unchanged", "AC3 test", hyg)

    must("render_fast_exit_this_boundary_", "AC4 render-fast", mb)
    nest = mb.find("Issue #3322: production/Full still drop stale defuse")
    if nest < 0:
        fails.append("AC4: render-fast skip missing #3322 close")
    else:
        win = mb[nest : nest + 600]
        if "invalidate_defuse_index_for_nested" not in win:
            fails.append("AC4: render-fast skip does not call helper")

    must("check_nested_observation_window_3322", "AC5 build.py", build)
    must("ac3322_4_source_and_linter", "AC5 hygiene", hyg)
    must("run_3322_nested_observation_window", "AC5 batch", batch)
    prev = build.find("check_nested_return_not_triad_3312")
    ours = build.find("check_nested_observation_window_3322")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC5: linter must be wired in build.py AFTER #3312")
    if "schema-3322" in hyg or "schema-3322" in met:
        fails.append("AC5: new schema-3322 query key")
    if "g_3322_" in mb or "g_3322_" in met:
        fails.append("AC5: new g_3322_* counter")
    if (ROOT / "tests" / "issues" / "test_issue_3322.cpp").is_file():
        fails.append("AC5: tests/issues/test_issue_3322.cpp")
    if (ROOT / "tests" / "compiler" / "test_issue_3322.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3322.cpp")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3322-*")):
            fails.append(f"AC5: docs/design/{f.name}")

    if fails:
        print("FAIL #3322 nested_observation_window:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK #3322 nested_observation_window: all rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
