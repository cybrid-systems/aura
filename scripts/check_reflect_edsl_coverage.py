#!/usr/bin/env python3
"""Issue #1907: reflect/EDSL bridge coverage gate.

Verifies that the #1907 instrumentation surface is wired in production
sources:

  - 6 new counters in src/compiler/observability_metrics.h
    (reflect_post_mutation_validate_total,
     reflect_post_mutation_validate_fail_total,
     reflect_hygiene_macro_reject_total,
     reflect_schema_query_total,
     reflect_validate_reflected_query_total,
     reflect_dirty_macro_nodes_total)
  - 6 new bump + 6 new getter helpers on Evaluator in src/compiler/evaluator.ixx
  - aura_validate_reflected_post_mutation bridge hook + 3 accessors
    in src/compiler/aura_jit_bridge.cpp
  - (engine:metrics "query:reflect-schema") + (mutate:validate-reflected)
    primitive registrations in src/compiler/evaluator_primitives_query.cpp

Fail the build when any of these is missing (regression gate).

Usage:
  python3 scripts/check_reflect_edsl_coverage.py
  python3 scripts/check_reflect_edsl_coverage.py --self-test

Exit 0 = OK, 1 = coverage violation.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COUNTERS = (
    "reflect_post_mutation_validate_total",
    "reflect_post_mutation_validate_fail_total",
    "reflect_hygiene_macro_reject_total",
    "reflect_schema_query_total",
    "reflect_validate_reflected_query_total",
    "reflect_dirty_macro_nodes_total",
)

GETTERS = tuple(f"get_{c}" for c in COUNTERS)
BUMPERS = tuple(f"bump_{c}" for c in COUNTERS)
BRIDGE_HOOKS = (
    "aura_validate_reflected_post_mutation",
    "aura_reflect_post_mutation_validate_total",
    "aura_reflect_post_mutation_validate_fail_total",
    "aura_reflect_hygiene_macro_reject_total",
)
PRIMITIVES = (
    "query:reflect-schema",
    "mutate:validate-reflected",
)


def _check_field(path, needles, rule, out):
    if not path.exists():
        out.append(f"MISSING {path.relative_to(ROOT)}")
        return
    text = path.read_text(encoding="utf-8")
    # Strip C++ comments before scanning so false positives don't fire
    # on documentation that mentions the symbols.
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
    _check_field(ROOT / "src/compiler/evaluator_primitives_query.cpp", PRIMITIVES, "primitive", findings)

    if findings:
        for f in findings:
            print(f"FAIL: {f}", file=sys.stderr)
        print(
            f"\nFAIL: {len(findings)} reflect/EDSL bridge coverage violation(s) "
            f"found. Issue #1907 mandate: every counter / helper / hook / "
            f"primitive must be wired in production sources.",
            file=sys.stderr,
        )
        return 1
    print("OK: scanned 4 file(s), all 6 counters + 12 helpers + 4 bridge hooks + 2 primitives wired (#1907).")
    return 0


def _self_test():
    """Verify the regex + strip logic with known-good + known-bad fixtures.
    Fixtures use the 3-tuple form (needle, text, expected) for clarity.
    """
    fixtures = [
        # 1. plain text containing the needle -> detected
        ("reflect_post_mutation_validate_total", "int x = 0; reflect_post_mutation_validate_total++;", True),
        # 2. needle not in text -> not detected
        ("reflect_post_mutation_validate_total", "int x = 0; unrelated_thing++;", False),
        # 3. needle in // line comment -> stripped out, not detected
        ("reflect_post_mutation_validate_total", "// reflect_post_mutation_validate_total dummy", False),
        # 4. needle in /* block */ comment -> stripped out, not detected
        ("reflect_post_mutation_validate_total", "/* reflect_post_mutation_validate_total */ dummy", False),
        # 5. plain text containing needle + separate /* */ comment ->
        #    strip the block comment but preserve the needle
        (
            "reflect_post_mutation_validate_total",
            "reflect_post_mutation_validate_total int /* ignored */ y; dummy",
            True,
        ),
        # 6. bridge hook name -> detected
        ("aura_validate_reflected_post_mutation", 'extern "C" int aura_validate_reflected_post_mutation(', True),
        # 7. primitive name in Aura primitive string -> detected
        ("query:reflect-schema", '"query:reflect-schema"', True),
        # 8. unrelated primitive needle NOT in text -> not detected
        ("query:envframe-dual-consistency-stats", "some_other_code_without_it dummy", False),
    ]
    failures = 0
    for needle, text, expected in fixtures:
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
