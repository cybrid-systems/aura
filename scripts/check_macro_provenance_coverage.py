#!/usr/bin/env python3
"""Issue #1908: MutationBoundaryGuard + macro clone provenance hardening
coverage gate (refine #1014 / #1047).

Verifies that the #1908 instrumentation surface is wired in production
sources:

  - 2 new counters in src/compiler/observability_metrics.h
    (macro_provenance_repin_on_steal_total,
     hygiene_violation_prevented_on_boundary_total)
  - 2 new bump + 2 new getter helpers on Evaluator in src/compiler/evaluator.ixx
  - aura_macro_provenance_repin_on_steal bridge hook + 2 accessors
    in src/compiler/aura_jit_bridge.cpp
  - (engine:metrics "query:macro-provenance-stats") primitive registration
    in src/compiler/evaluator_primitives_query.cpp
  - 3 wire-up sites in src/compiler/evaluator_fiber_mutation.cpp
    (flush_mutation_boundary outermost exit +
     complete_post_resume_steal_refresh post probe +
     transfer_and_revalidate_panic_checkpoint post panic restamp)
  - 1 wire-up site in src/compiler/macro_expansion.cpp
    (clone_macro_body MacroIntroduced path via bridge hook)

Fail the build when any of these is missing (regression gate).

Usage:
  python3 scripts/check_macro_provenance_coverage.py
  python3 scripts/check_macro_provenance_coverage.py --self-test

Exit 0 = OK, 1 = coverage violation.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COUNTERS = (
    "macro_provenance_repin_on_steal_total",
    "hygiene_violation_prevented_on_boundary_total",
)

GETTERS = tuple(f"get_{c}" for c in COUNTERS)
BUMPERS = tuple(f"bump_{c}" for c in COUNTERS)
BRIDGE_HOOKS = (
    "aura_macro_provenance_repin_on_steal",
    "aura_macro_provenance_repin_on_steal_total",
    "aura_hygiene_violation_prevented_on_boundary_total",
)
PRIMITIVE = "query:macro-provenance-stats"

WIREUP_FIBER_MARKERS = (
    "flush_mutation_boundary",
    "complete_post_resume_steal_refresh",
    "transfer_and_revalidate_panic_checkpoint",
)
WIREUP_MACRO_MARKER = "clone_macro_body"


def _check_field(path, needles, rule, out):
    if not path.exists():
        out.append(f"MISSING {path.relative_to(ROOT)}")
        return
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"/\*[\s\S]*?\*/", "", text)
    text = re.sub(r"//[^\n]*", "", text)
    for needle in needles:
        if needle not in text:
            out.append(f"{path.relative_to(ROOT)}: [{rule}] missing `{needle}`")


def _self_test() -> int:
    fixtures = (
        (
            "struct C { std::atomic<std::uint64_t> macro_provenance_repin_on_steal_total{0}; "
            "std::atomic<std::uint64_t> hygiene_violation_prevented_on_boundary_total{0}; };",
            True,
        ),
        ("struct C { std::atomic<std::uint64_t> unrelated{0}; };", False),
    )
    for text, expected in fixtures:
        stripped = re.sub(r"/\*[\s\S]*?\*/", "", text)
        stripped = re.sub(r"//[^\n]*", "", stripped)
        has_all = all(n in stripped for n in COUNTERS)
        if has_all != expected:
            print(f"self-test FAIL: expected {expected}, got {has_all} for: {text!r}", file=sys.stderr)
            return 1
    print("OK: self-test passed (2 fixtures)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return _self_test()

    findings: list[str] = []

    _check_field(
        ROOT / "src" / "compiler" / "observability_metrics.h",
        COUNTERS,
        "counter",
        findings,
    )

    _check_field(
        ROOT / "src" / "compiler" / "evaluator.ixx",
        list(GETTERS) + list(BUMPERS),
        "getter+bumper",
        findings,
    )

    _check_field(
        ROOT / "src" / "compiler" / "aura_jit_bridge.cpp",
        list(BRIDGE_HOOKS),
        "bridge-hook",
        findings,
    )

    _check_field(
        ROOT / "src" / "compiler" / "evaluator_primitives_query.cpp",
        [PRIMITIVE],
        "primitive",
        findings,
    )

    _check_field(
        ROOT / "src" / "compiler" / "evaluator_fiber_mutation.cpp",
        list(WIREUP_FIBER_MARKERS) + list(BUMPERS),
        "fiber-wireup",
        findings,
    )

    _check_field(
        ROOT / "src" / "compiler" / "macro_expansion.cpp",
        [WIREUP_MACRO_MARKER, "aura_macro_provenance_repin_on_steal", "SyntaxMarker::MacroIntroduced"],
        "macro-wireup",
        findings,
    )

    if findings:
        for f in findings:
            print(f"FAIL: {f}", file=sys.stderr)
        print(f"\nMISS: {len(findings)} finding(s); #1908 instrumentation incomplete.", file=sys.stderr)
        return 1

    print(
        "OK: scanned 6 file(s), all 2 counters + 4 helpers + 3 bridge hooks "
        "+ 1 primitive + 3 fiber wire-ups + 1 macro wire-up wired (#1908)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
