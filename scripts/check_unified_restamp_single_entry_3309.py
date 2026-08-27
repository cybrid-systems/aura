#!/usr/bin/env python3
# scripts/check_unified_restamp_single_entry_3309.py — Issue #3309 source-cite gate.
#
# All abort / steal-complete / densify-success restamp entries must route
# through Evaluator::unified_restamp_after_boundary (single budget/torn
# decision + node-gen / pinned-stable pairing). Catches regressions where
# a new or recovered call site pairs restamp_all_node_generations() with
# restamp_pinned_stable_refs() directly, re-opening the mixed-generation
# window the unified entry closes (#3058/#3287/#3259 residual).
#
# Contract rows:
#
#   R1 — evaluator_mutation_boundary.cpp outermost exit (success AND
#        abort) calls ev_->unified_restamp_after_boundary(BoundarySuccess
#        | AbortRestore).
#   R2 — steal-complete and densify-success paths call
#        unified_restamp_after_boundary (StealComplete | Densify).
#   R3 — no direct adjacent pairing of restamp_all_node_generations()
#        followed by restamp_pinned_stable_refs() outside the unified
#        entry (evaluator_fiber_mutation.cpp:unified_restamp_after_boundary
#        body itself is the exempt definition site).
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_unified_restamp_single_entry_3309.py --self-test

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

BOUNDARY = "src/compiler/evaluator_mutation_boundary.cpp"
FIBER = "src/compiler/evaluator_fiber_mutation.cpp"


def _read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _check_shared_entries() -> list[str]:
    failures: list[str] = []
    mb = _read(BOUNDARY)
    fm = _read(FIBER)
    if not re.search(
        r"unified_restamp_after_boundary\(\s*\n?\s*success\s*\?\s*Evaluator::UnifiedRestampSite::BoundarySuccess\s*\n?\s*:\s*Evaluator::UnifiedRestampSite::AbortRestore",
        mb,
    ):
        failures.append("R1: outermost exit must route success/abort through unified_restamp_after_boundary")
    if not re.search(r"unified_restamp_after_boundary\(\s*UnifiedRestampSite::StealComplete", fm):
        failures.append("R2: steal-complete path must call unified_restamp_after_boundary(StealComplete)")
    if not re.search(r"unified_restamp_after_boundary\(\s*UnifiedRestampSite::Densify", fm):
        failures.append("R2: densify-success path must call unified_restamp_after_boundary(Densify)")
    return failures


def _check_no_adjacent_pairing() -> list[str]:
    failures: list[str] = []
    exempt = re.search(
        r"UnifiedRestampResult\s*\n?aura::compiler::Evaluator::unified_restamp_after_boundary.*?(?=\n// Issue #1446)",
        _read(FIBER),
        re.S,
    )
    exempt_span = exempt.span() if exempt else (0, 0)
    for rel in (BOUNDARY, FIBER):
        src = _read(rel)
        for m in re.finditer(
            r"restamp_all_node_generations\(\)\s*;[^\n]*\n(?:[^\n]*\n){0,4}?[^\n]*?restamp_pinned_stable_refs\(\)",
            src,
        ):
            if rel == FIBER and exempt_span[0] <= m.start() < exempt_span[1]:
                continue
            failures.append(
                f"R3: adjacent direct pairing restamp_all_node_generations + "
                f"restamp_pinned_stable_refs in {rel} near offset {m.start()} "
                f"— route through unified_restamp_after_boundary"
            )
    return failures


def run_strict() -> list[str]:
    failures: list[str] = []
    failures.extend(_check_shared_entries())
    failures.extend(_check_no_adjacent_pairing())
    return failures


def _self_test() -> int:
    failures = run_strict()
    if failures:
        print("SELF-TEST FAIL:", file=sys.stderr)
        for f in failures:
            print("  -", f, file=sys.stderr)
        return 1
    print("SELF-TEST OK: all #3309 unified-restamp single-entry checks pass")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Issue #3309 unified restamp single-entry source-cite gate")
    ap.add_argument("--self-test", action="store_true", help="Run against current repo; expect zero failures.")
    ap.add_argument("--strict", action="store_true", help="Default mode; present for CI symmetry.")
    args = ap.parse_args(argv)
    if args.self_test:
        return _self_test()
    failures = run_strict()
    if failures:
        for f in failures:
            print("FAIL:", f, file=sys.stderr)
        return 1
    print("OK: unified restamp single-entry (#3309) checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
