#!/usr/bin/env python3
"""Issue #3311: Soft → Production arm invalidates Soft-only schema-2 QueryResult.

Contract:
  AC1 production stamp sets the Prod marker (kQueryResultMatchSchema2Prod);
     freshness under hard requires the Prod marker → durable exports never
     accept layout-only / Soft-stamped matches (#3231 reject unchanged).
  AC2 Soft / Off: stamp keeps the Soft marker; freshness Prod gate only
     runs under hard (zero-cost observe path unchanged).
  AC3 cached Soft-stamped match (reserved == Soft marker) fails the Prod
     discriminator under hard → SoftOnlyNoProvenance + stale counter
     (no silent promotion across the canary window).
  AC4 transition fixture: structural discriminator test + live Soft canary
     → arm production → re-query fixture, registered in main.
  AC5 no new public query key (reuses restamp-lag / provenance counters).

Exit 0 = all rows satisfied.
"""

from __future__ import annotations

import re
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

    def must(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    epoch = _read("src/core/workspace_epoch.hh")
    qw = _read("src/compiler/evaluator_primitives_query_workspace.cpp")
    test = _read("tests/compiler/test_query_result_full_provenance.cpp")
    build = _read("build.py")

    # AC1/AC3: distinct markers (clang-format tolerant on the `= N` split).
    must(
        re.search(r"kQueryResultMatchSchema2\s*=\s*1", epoch) is not None,
        "AC1: Soft marker kQueryResultMatchSchema2 = 1",
    )
    must(
        re.search(r"kQueryResultMatchSchema2Prod\s*=\s*2", epoch) is not None,
        "AC1: Prod marker kQueryResultMatchSchema2Prod = 2",
    )
    must("#3311" in epoch, "AC1: workspace_epoch.hh cites #3311")

    # AC1: stamp branches on production_defaults_active().
    stamp_zone = qw[qw.find("stamp_query_result_full_provenance") :]
    must("#3311" in qw, "AC1: query_workspace cites #3311")
    must(
        "production_defaults_active()" in stamp_zone and "kQueryResultMatchSchema2Prod" in stamp_zone,
        "AC1: stamp sets Prod marker under production_defaults",
    )
    must(
        "kQueryResultMatchSchema2;" in stamp_zone or "kQueryResultMatchSchema2 :" in stamp_zone,
        "AC2: stamp keeps Soft marker otherwise",
    )

    # AC3: freshness Prod gate behind hard.
    fresh_zone = qw[qw.find("query_result_is_fresh_with_refs") :]
    gate_m = re.search(
        r"hard\s*&&\s*qr\.matches\[0\]\.reserved\s*!=\s*"
        r"aura::core::kQueryResultMatchSchema2Prod",
        fresh_zone,
    )
    must(gate_m is not None, "AC3: hard gate requires Prod marker")
    if gate_m:
        gate_window = fresh_zone[gate_m.start() : gate_m.start() + 400]
        must(
            "SoftOnlyNoProvenance;" in gate_window,
            "AC3: gate returns SoftOnlyNoProvenance",
        )
    must(
        "note_query_result_full_provenance_stale()" in fresh_zone,
        "AC3: stale counter bumped on transition reject (no new query key)",
    )

    # AC4: fixtures present + registered.
    must(
        test.count("test_ac3311_soft_to_production_transition()") >= 2,
        "AC4: structural transition fixture defined + registered",
    )
    must(
        test.count("test_ac3311_live_soft_canary_then_prod_requery()") >= 2,
        "AC4: live Soft→Prod re-query fixture defined + registered",
    )
    must(
        "apply_production_audit_defaults()" in test and "apply_dev_audit_defaults()" in test,
        "AC4: fixture arms/disarms production_defaults mid-session",
    )
    must("#3311" in test, "AC4: test cites #3311")

    # AC5: no new public query key introduced for #3311 (no add("query:*3311*")).
    must(
        not re.search(r'add\(\s*"query:[^"]*3311', qw),
        "AC5: no new public query key",
    )

    # Gate wiring.
    must(
        "check_query_result_soft_prod_transition_3311" in build,
        "gate: build.py wires the linter",
    )

    if fails:
        for f in fails:
            print(f"FAIL: {f}")
        return 1
    print("OK: Issue #3311 Soft→Production schema-2 transition — all AC rows satisfied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
