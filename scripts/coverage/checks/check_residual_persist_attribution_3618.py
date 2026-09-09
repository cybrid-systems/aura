#!/usr/bin/env python3
"""Issue #3618: unmatched residual CastOp persist forces full relower.

#3349's tail gate fail-closed full only when the source_to_ir_map was
EMPTY: a map non-empty but missing the persisted CastOp site (partial
rebuild of another fn) kept want_partial — impact_ub from the incomplete
map stayed ≤ dirty_n and DeadCoercionEliminationPass cone-skipped the
type-changed site. Attribution success (site mapped + block marked)
still keeps partial; Soft / Off / empty persist stay zero-extra.

Contract:
  AC1 tail gate keys on production + persist-nonempty + !persist_attributed
      WITHOUT the source_to_ir_map.empty() conjunct (#3618 comment present);
      windowed structural check — the gate terms sit inside one bounded
      region after the attribution read
  AC2 attribution path retained: mark_entry_from_dead_coercion_persist_
      still runs and attributed persist keeps partial
  AC3 Soft / Off / empty persist zero extra (force_residual_castop_
      undermark_into_cone stays the only pre-gate touch)
  AC4 reuse partial_forced_full_by_impact_total (no new metric / query
      key); no g_3618_* counter
  AC5 test extends test_dead_coercion_dirty_cone.cpp (test_
      occurrence_coercion_batch member); no new test file; no docs/design
  AC6 build.py wires this linter after the #3617 linter

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

    def must_absent(n: str, label: str, hay: str) -> None:
        if n in hay:
            fails.append(f"{label}: forbidden {n!r}")

    svc = _read("src/compiler/service.ixx")
    test = _read("tests/compiler/test_dead_coercion_dirty_cone.cpp")
    build = _read("build.py")

    # AC1 — windowed structural check: the retargeted tail gate.
    g = svc.find("Issue #3618: persist nonempty + production + attribution")
    win = svc[g - 400 : g + 700] if g >= 0 else ""
    must("Issue #3618", "AC1 service cites #3618", win)
    must("!persist_attributed) {", "AC1 keyed tail gate", win)
    must("residual_castop_persist_size() > 0", "AC1 persist-nonempty term", win)
    must_absent("source_to_ir_map.empty()", "AC1 empty-map conjunct dropped", win)
    must("metrics_.partial_forced_full_by_impact_total.fetch_add", "AC1 reuse counter", win)

    # AC2 — attribution path retained (whole-file rows; the flag declaration
    # sits ~1.2k chars above the tail gate, outside the AC1 window).
    must("bool persist_attributed = false;", "AC2 attribution flag", svc)
    must("mark_entry_from_dead_coercion_persist_(it->second)", "AC2 mark helper call", svc)

    # AC3 — Soft zero-extra retained (force-undermark remains the only
    # pre-gate touch; Soft/Off persist writes stay no-ops).
    must("force_residual_castop_undermark_into_cone", "AC3 soft undermark", svc)

    # AC4 — no new counters / query keys.
    for f in (svc, test):
        must_absent("g_3618_", "AC4 no new counter", f)
    must_absent("schema-3618", "AC4 no new query key", svc)

    # AC5 — test extension, no invent, no docs.
    must("ac3618_persist_attribution_forces_full", "AC5 gc-defer/dirty-cone test", test)
    must("3618 AC1: empty-map conjunct dropped", "AC5 test asserts retarget", test)
    if (ROOT / "tests" / "compiler" / "test_issue_3618.cpp").is_file():
        fails.append("AC5: tests/compiler/test_issue_3618.cpp present (forbidden)")
    docs = ROOT / "docs" / "design"
    if docs.is_dir():
        for f in sorted(docs.glob("3618-*")):
            fails.append(f"AC5: docs/design/{f.name} present (forbidden #1655)")

    # AC6 — build.py wiring order (after #3617).
    must("check_residual_persist_attribution_3618", "AC6 build.py", build)
    prev = build.find("check_steal_eval_keyed_residual_3617")
    ours = build.find("check_residual_persist_attribution_3618")
    if prev < 0 or ours < 0 or ours < prev:
        fails.append("AC6: #3618 linter must run after #3617")

    if fails:
        print("check_residual_persist_attribution_3618: FAIL")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("OK: Issue #3618 residual persist attribution — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
