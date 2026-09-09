#!/usr/bin/env python3
# scripts/check_render_fast_audit_3625.py -- Issue #3625 source-cite gate.
#
# AC1: the Guard-dtor RenderFastExit decision carries the Production/Full
#      metrics-only gate — render_fast_effective = render_fast &&
#      !(production_defaults_active() || strategy == Full) — so a
#      non-linear, non-match mutate under Production can no longer skip
#      the #2145/#2311 Full invariant suite (#2311/#3322 residual).
# AC2: #2311 linear/match suppress counters still fire BEFORE the
#      strategy gate (Soft-path gate unchanged); per-cause counters kept.
# AC3: the Soft skip site stays intact (render_fast_exit_skipped_audit_total
#      + note_invariant_enforcement_skipped) — Soft keeps the hotpath skip.
# AC4: #3614 persist/linear order untouched; #3517 force-rollback machinery
#      retained; render_fast_exit_total still bumps as an observe-only
#      hotpath signal (RenderFastExit not deleted). Runtime ACs live in
#      tests/compiler/test_typed_mutation_audit_decision.cpp (ac3625_1..4);
#      no docs/design/*3625*, no tests/**/test_issue_3625.cpp; linter
#      registered in build.py.

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

MB = "src/compiler/evaluator_mutation_boundary.cpp"
TEST = "tests/compiler/test_typed_mutation_audit_decision.cpp"
BUILD = "build.py"

LINTER = "check_render_fast_audit_3625"
ANCHOR = "const bool render_fast = render_fast_candidate && !linear_or_match_suppress;"
CITE = "Issue #3625 (#2311/#3322 residual)"
GATE = "render_fast_effective"


def _read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        return ""
    return p.read_text(encoding="utf-8", errors="replace")


def _rows(mb: str, test: str, build: str) -> list[str]:
    fails: list[str] = []

    def must(needle: str, label: str, hay: str) -> None:
        if needle not in hay:
            fails.append(f"{label}: missing {needle!r}")

    def must_not(needle: str, label: str, hay: str) -> None:
        if needle in hay:
            fails.append(f"{label}: forbidden {needle!r} present")

    # AC1 — the metrics-only gate sits right after the #2311 decision.
    pos = mb.find(ANCHOR)
    if pos == -1:
        fails.append("AC1: render_fast decision anchor not found")
        win = ""
    else:
        win = mb[pos : pos + 2400]
        must(CITE, "AC1 #3625 cite in gate", win)
        must("typed_audit::production_defaults_active()", "AC1 production knob", win)
        must(
            "typed_audit::get_strategy() == typed_audit::AuditStrategy::Full",
            "AC1 Full-strategy knob",
            win,
        )
        must(
            "const bool render_fast_effective = render_fast && !render_fast_audit_hard;",
            "AC1 effective computation",
            win,
        )
        must("ev_->render_fast_exit_this_boundary_ = render_fast_effective;", "AC1 boundary flag gated", win)
        must("render_fast_exit_total.fetch_add", "AC1 observe-only signal retained", win)

    # AC2 — #2311 suppress counters before the strategy gate.
    suppress = mb.find("m->render_fast_exit_suppressed_linear_or_match_total.fetch_add(")
    if suppress == -1:
        fails.append("AC2: #2311 suppress counter block not found")
    elif win:
        gate = mb.find(CITE)
        if gate != -1 and suppress > gate:
            fails.append("AC2: suppress counters must precede the #3625 gate")
    must("render_fast_exit_suppressed_linear_total", "AC2 per-cause linear counter", mb)
    must("render_fast_exit_suppressed_match_total", "AC2 per-cause match counter", mb)

    # AC3 — Soft skip site intact.
    must("render_fast_exit_skipped_audit_total", "AC3 skip counter present", mb)
    must("note_invariant_enforcement_skipped", "AC3 intentional-skip note present", mb)

    # AC4 — lineage + runtime ACs + registration + no docs / invented test.
    must("Issue #3614", "AC4 #3614 persist order untouched", mb)
    must("consume_outermost_audit_rollback_needs_fail", "AC4 #3517 rollback retained", mb)
    for fn in (
        "ac3625_1_production_render_still_audits",
        "ac3625_2_soft_render_fast_still_skips",
        "ac3625_3_suppress_counters_unchanged",
        "ac3625_4_source_and_linter",
    ):
        must(fn, "AC4 runtime AC", test)
    must("check_render_fast_audit_3625", "AC4 build.py registration", build)
    design = ROOT / "docs" / "design"
    if design.is_dir():
        hits = sorted(p.name for p in design.glob("*3625*"))
        if hits:
            fails.append(f"AC4: docs/design/*3625* present: {hits}")
    invented = sorted(str(p.relative_to(ROOT)) for p in ROOT.glob("tests/**/test_issue_3625.cpp"))
    if invented:
        fails.append(f"AC4: invented test_issue_3625.cpp present: {invented}")
    return fails


def _self_test() -> int:
    mb_ok = (
        "const bool render_fast = render_fast_candidate && !linear_or_match_suppress;\n"
        "        if (render_fast_candidate && linear_or_match_suppress) {\n"
        "            m->render_fast_exit_suppressed_linear_or_match_total.fetch_add(\n"
        "                1, std::memory_order_relaxed);\n"
        "            m->render_fast_exit_suppressed_linear_total.fetch_add(1);\n"
        "            m->render_fast_exit_suppressed_match_total.fetch_add(1);\n"
        "        }\n"
        "        // Issue #3625 (#2311/#3322 residual): metrics-only under Production/Full.\n"
        "        const bool render_fast_audit_hard =\n"
        "            typed_audit::production_defaults_active() ||\n"
        "            typed_audit::get_strategy() == typed_audit::AuditStrategy::Full;\n"
        "        const bool render_fast_effective = render_fast && !render_fast_audit_hard;\n"
        "        ev_->render_fast_exit_this_boundary_ = render_fast_effective;\n"
        "        if (render_fast) {\n"
        "            m->render_fast_exit_total.fetch_add(1, std::memory_order_relaxed);\n"
        "        }\n"
        "    if (render_fast_exit_this_boundary_ && !nested_boundary) {\n"
        "            m->render_fast_exit_skipped_audit_total.fetch_add(1);\n"
        "            typed_audit::note_invariant_enforcement_skipped(mid);\n"
        "    }\n"
        "    // Issue #3614: linear deny + pending_full_solve drain gate BEFORE the\n"
        "    consume_outermost_audit_rollback_needs_fail();\n"
    )
    test_ok = " ".join(
        f"static void ac3625_{i}_{n}() {{}}"
        for i, n in enumerate(
            (
                "production_render_still_audits",
                "soft_render_fast_still_skips",
                "suppress_counters_unchanged",
                "source_and_linter",
            ),
            start=1,
        )
    )
    build_ok = 'ROOT / "scripts" / "check_render_fast_audit_3625.py"'

    good = _rows(mb_ok, test_ok, build_ok)
    if good:
        print(f"self-test: synthetic-good fixture unexpectedly failed: {good}")
        return 1
    stripped = mb_ok.replace(
        "typed_audit::production_defaults_active() ||\n"
        "            typed_audit::get_strategy() == typed_audit::AuditStrategy::Full;\n",
        "false;\n",
    )
    bad = _rows(stripped, test_ok, build_ok)
    if not any("production knob" in r or "Full-strategy knob" in r for r in bad):
        print("self-test: stripped fixture did not trip the AC1 gate rows")
        return 1
    print("ok check_render_fast_audit_3625 self-test")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Issue #3625 source-cite gate")
    ap.add_argument("--strict", action="store_true", help="fail on any contract row")
    ap.add_argument("--self-test", action="store_true", help="run synthetic fixtures")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()
    rows = _rows(_read(MB), _read(TEST), _read(BUILD))
    if rows:
        for r in rows:
            print(f"FAIL {LINTER}: {r}")
        return 1
    print(f"ok {LINTER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
