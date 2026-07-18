#!/usr/bin/env python3
"""Issue #1905: AOT incremental hot-update / invalidation loop coverage gate.

Verifies that the #1905 instrumentation surface is wired in production
sources:

  - 6 new counters in src/compiler/observability_metrics.h
    (aot_live_closure_refresh_on_mutation_total,
     aot_live_closure_refresh_on_steal_total,
     aot_bridge_epoch_bump_on_mutation_total,
     aot_bridge_epoch_bump_on_steal_total,
     aot_region_mismatch_on_resume_total,
     aot_stale_deopt_on_steal_total)
  - 6 new bump + 6 new getter helpers on Evaluator in src/compiler/evaluator.ixx
  - aura_refresh_live_closures_for_mutated_define bridge hook in
    src/compiler/aura_jit_bridge.cpp (Step 2 of #1905 plan)
  - aura_post_steal_aot_revalidate bridge hook in aura_jit_bridge.cpp
    (Step 3 of #1905 plan)
  - (engine:metrics "query:aot-hot-update-stats") primitive registration
    in src/compiler/evaluator_primitives_query.cpp

Fail the build when any of these is missing (regression gate).

Usage:
  python3 scripts/check_aot_hot_update_coverage.py
  python3 scripts/check_aot_hot_update_coverage.py --self-test

Exit 0 = OK, 1 = coverage violation.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COUNTERS = (
    "aot_live_closure_refresh_on_mutation_total",
    "aot_live_closure_refresh_on_steal_total",
    "aot_bridge_epoch_bump_on_mutation_total",
    "aot_bridge_epoch_bump_on_steal_total",
    "aot_region_mismatch_on_resume_total",
    "aot_stale_deopt_on_steal_total",
)

GETTERS = tuple(f"get_{c}" for c in COUNTERS)
BUMPERS = tuple(f"bump_{c}" for c in COUNTERS)
BRIDGE_HOOKS = (
    "aura_refresh_live_closures_for_mutated_define",
    "aura_post_steal_aot_revalidate",
)
PRIMITIVE = "query:aot-hot-update-stats"


def _check_field(path, needles, rule, out):
    if not path.exists():
        out.append(f"MISSING {path.relative_to(ROOT)}")
        return
    text = path.read_text(encoding="utf-8")
    # Strip C++ comments before searching so false positives don't fire on
    # documentation comments that mention the legacy pattern.
    text = re.sub(r"/\*[\s\S]*?\*/", "", text)
    text = re.sub(r"//[^\n]*", "", text)
    for needle in needles:
        if needle not in text:
            out.append(f"{path.relative_to(ROOT)}: [{rule}] missing `{needle}`")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return _self_test()

    findings = []
    _check_field(ROOT / "src/compiler/observability_metrics.h", COUNTERS, "counter", findings)
    _check_field(ROOT / "src/compiler/evaluator.ixx", GETTERS + BUMPERS, "evaluator-helper", findings)
    _check_field(ROOT / "src/compiler/aura_jit_bridge.cpp", BRIDGE_HOOKS, "bridge-hook", findings)
    _check_field(ROOT / "src/compiler/evaluator_primitives_query.cpp", (PRIMITIVE,), "primitive", findings)

    if findings:
        for f in findings:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\nFAIL: {len(findings)} AOT hot-update coverage violation(s) "
            f"found. Issue #1905 mandate: every counter / helper / hook / "
            f"primitive must be wired in production sources.",
            file=sys.stderr,
        )
        return 1
    print("OK: scanned 4 file(s), all 6 counters + 12 helpers + 2 bridge hooks + 1 primitive wired (#1905).")
    return 0


def _self_test():
    """Verify the regex patterns + strip logic with known-good + known-bad
    fixtures. All fixtures use the 3-tuple form (needle, text, expected)
    for clarity - the strip logic is verified by construction.
    """
    fixtures = [
        # 1. plain text containing the needle - detected
        (
            "aot_live_closure_refresh_on_mutation_total",
            "int x = 0; aot_live_closure_refresh_on_mutation_total++; dummy",
            True,
        ),
        # 2. needle not in text - not detected
        ("aot_live_closure_refresh_on_mutation_total", "int x = 0; unrelated_thing++; dummy", False),
        # 3. needle in // line comment - stripped out, not detected
        #    (entire line is the comment, after strip = "")
        ("aot_live_closure_refresh_on_mutation_total", "// aot_live_closure_refresh_on_mutation_total dummy", False),
        # 4. needle in /* block */ comment - stripped out, not detected
        #    (block comment is removed, needle is gone)
        ("aot_live_closure_refresh_on_mutation_total", "/* aot_live_closure_refresh_on_mutation_total */ dummy", False),
        # 5. plain text containing needle + separate /* */ comment -
        #    strip the block comment but preserve the needle
        (
            "aot_live_closure_refresh_on_mutation_total",
            "aot_live_closure_refresh_on_mutation_total int /* ignored */ y; dummy",
            True,
        ),
        # 6. plain text with // line comment elsewhere - preserve needle
        (
            "aot_live_closure_refresh_on_mutation_total",
            "aot_live_closure_refresh_on_mutation_total // other comment",
            True,
        ),
        # 7. bridge hook names - each in a minimal declaration form
        (
            "aura_refresh_live_closures_for_mutated_define",
            'extern "C" void aura_refresh_live_closures_for_mutated_define(',
            True,
        ),
        ("aura_post_steal_aot_revalidate", 'extern "C" int aura_post_steal_aot_revalidate(', True),
        # 8. primitive name in Aura primitive string
        ("query:aot-hot-update-stats", '"query:aot-hot-update-stats"', True),
        # 9. unrelated primitive needle NOT in text - not detected
        ("query:envframe-dual-consistency-stats", "some_other_code_without_it dummy", False),
    ]
    failures = 0
    for needle, text, expected in fixtures:
        # Simulate the strip logic.
        stripped = re.sub(r"/\*[\s\S]*?\*/", "", text)
        stripped = re.sub(r"//[^\n]*", "", stripped)
        actual = needle in stripped
        if actual != expected:
            print(f"FAIL: needle={needle!r} text={text!r} expected={expected} got={actual}", file=sys.stderr)
            failures += 1
    if failures:
        print(f"\n{failures} self-test failure(s)", file=sys.stderr)
        return 1
    print(f"OK: self-test passed ({len(fixtures)} fixtures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
