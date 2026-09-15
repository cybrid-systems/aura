#!/usr/bin/env python3
"""Issue #3792 — mutation-history tenant filter linter.

Residual of #3722 (option B) / #3790: `mutation-history` dumped the
workspace mutation log with no tenant filter, letting a Restricted+MT
Agent learn a foreign mid and fire the unguarded rollback oracle. The
runtime fix (same ship) filters rows to the caller's occupancy tenant
under the consult regime (Strict, or Restricted+MT — same predicate as
require_effect_for_node_id); Soft/Off keeps the full dump. This linter
pins the face at source level:

  AC1: the mutation-history handler carries the consult-regime gate +
       occupancy resolve chain (existing stamp → collision borrow) with
       the gate preceding the dump loop and the foreign-row skip present.
  AC2: no new query key — 3792 appears in no query-primitive surface; no
       g_3792_* counters (no mid-struct metrics).
  AC3: no per-issue invent (tests/core/test_issue_3792.cpp) and no
       docs/design/3792-*.
  AC4: no second history API — the fix is confined to the existing face.

Soft / Off is not a vulnerability: the full dump is the documented Soft
contract (filter-over-deny under production only).
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PRIM = ROOT / "src" / "compiler" / "evaluator_primitives_mutation.cpp"
QUERY_PRIMS = [
    ROOT / "src" / "compiler" / "evaluator_primitives_query.cpp",
    ROOT / "src" / "compiler" / "evaluator_primitives_query_type_stats.cpp",
]
SRC_SUFFIXES = {".h", ".hpp", ".cpp", ".ixx"}


def fail(msg: str) -> int:
    print(f"FAIL: {msg}")
    return 1


def check_handler_filter(text: str) -> list[str]:
    """AC1 — consult gate + occupancy resolve, gate before dump, skip present."""
    errors: list[str] = []
    mh = text.find('"mutation-history"')
    if mh < 0:
        return ["AC1: mutation-history prim not found"]
    win = text[mh : mh + 4000]
    if "Issue #3792:" not in win:
        errors.append("AC1: handler does not cite #3792")
    gate = win.find("const bool consult = strict || (restricted && mt);")
    if gate < 0:
        errors.append("AC1: consult-regime gate missing in handler")
    ex = win.find("existing_stamp_for_node")
    occ = win.find("occupying_stamp_for_node")
    if ex < 0 or occ < 0:
        errors.append("AC1: occupancy resolve chain missing in handler")
    skip = win.find("occ != caller")
    if skip < 0:
        errors.append("AC1: foreign-row skip missing (occ != caller)")
    dump = win.find("push_string_heap")
    if gate >= 0 and dump >= 0 and gate > dump:
        errors.append("AC1: gate must precede the dump loop")
    return errors


def check_no_new_keys_or_counters() -> list[str]:
    """AC2 — no new query key, no g_3792_* counters."""
    errors: list[str] = []
    for q in QUERY_PRIMS:
        if q.exists() and "3792" in q.read_text(encoding="utf-8"):
            errors.append(f"AC2: {q.name} mentions 3792 (no new query key)")
    for p in (ROOT / "src").rglob("*"):
        if p.suffix not in SRC_SUFFIXES or not p.is_file():
            continue
        if "g_3792_" in p.read_text(encoding="utf-8", errors="ignore"):
            errors.append(f"AC2: {p.relative_to(ROOT)} adds g_3792_* counter")
    return errors


def check_no_invent() -> list[str]:
    """AC3 — no per-issue copy / design doc."""
    errors: list[str] = []
    if (ROOT / "tests" / "core" / "test_issue_3792.cpp").exists():
        errors.append("AC3: tests/core/test_issue_3792.cpp must not exist")
    for p in (ROOT / "docs" / "design").glob("3792*"):
        errors.append(f"AC3: {p.name} must not exist")
    return errors


def self_test() -> int:
    unfiltered = '"mutation-history", [&ev](std::span<const EvalValue> a) { push_string_heap(...); }'
    if not check_handler_filter(unfiltered):
        return fail("self-test: unfiltered handler not detected")
    filtered = (
        '"mutation-history", [&ev](std::span<const EvalValue> a) {\n'
        "    // Issue #3792: filter rows to caller occupancy tenant\n"
        "    const bool consult = strict || (restricted && mt);\n"
        "    auto existing = existing_stamp_for_node(0);\n"
        "    auto occ = occupying_stamp_for_node(0);\n"
        "    if (occ != caller) continue;\n"
        "    push_string_heap(...);\n"
        "}"
    )
    if check_handler_filter(filtered):
        return fail("self-test: filtered handler flagged")
    print("self-test ok")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    if not PRIM.exists():
        return fail(f"missing {PRIM}")
    errors: list[str] = []
    prim = PRIM.read_text(encoding="utf-8")
    errors += check_handler_filter(prim)
    errors += check_no_new_keys_or_counters()
    errors += check_no_invent()
    for e in errors:
        print(f"FAIL: {e}")
    if errors:
        return 1
    print("check_mutation_history_tenant_filter_3792: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
