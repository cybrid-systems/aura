#!/usr/bin/env python3
"""Issue #3093: cover-aware intermediate create (slot / pin / EXEMPT triad).

Background
----------
`ASTArena::note_intermediate_create_auto_wire_(void*)` (#2775) records
small-pool intermediates into `intermediate_creates_` (inventory) and
`register_external_root_for_densify(p)` (value-only prep set). Per #3017
comments in `arena.ixx` and `lifetime_pin.hh`, value-only prep is
OBSERVABILITY only, not safe cover. Cover requires one of:

- `LifetimePin` / `wire_general_object_create_pair*`
- `register_external_root_slot_for_densify(void**)` (slot rewrite)
- `GENERAL_OBJECT_PIN_EXEMPT(reason)` (stable reason string)

`#3093` closes the residual by adding a cover-aware helper
`note_intermediate_create_with_cover_(void* p, void** slot, const char* reason)`
that enforces the slot / EXEMPT triad. Value-only fallback bumps
`g_intermediate_create_value_only_total` so production call sites that
don't declare cover are visible to Agent dashboards.

This linter is the regression guard:

  * PRESENCE — the helper + counter declarations must exist
    (no removal of the cover-aware path).
  * NO INVENT REGRESSION — no new docs/design/3093-* (per #1655),
    no new test_issue_3093.cpp (per #81934), no second pin registry.
  * Quiet path — required-off / Soft / null p → no extra work
    (early-return on first null check).

Exit codes:
  0 — clean (helper + counters present; no forbidden artefacts)
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_intermediate_cover_3093.py            # report
  python3 scripts/coverage/checks/check_intermediate_cover_3093.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_intermediate_cover_3093.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

ARENA_IXX = ROOT / "src" / "core" / "arena.ixx"
TEST_DIR = ROOT / "tests" / "core" / "test_general_object_pin_coverage_gate.cpp"
DOCS_DIR = ROOT / "docs" / "design"


# Required patterns: presence checks. Use DOTALL so multi-line function
# bodies (with their own `{}` blocks) can match across newlines.
REQUIRED_PATTERNS = (
    # --- helper on ASTArena (multi-line signature — use [\s\S]*? to allow
    # newlines inside the parentheses, which the original [^)]*\) wouldn't) ---
    (
        "ASTArena::note_intermediate_create_with_cover_ helper",
        re.compile(
            r"void\s+note_intermediate_create_with_cover_"
            r"\s*\(\s*void\s*\*\s*p\s*,\s*void\s*\*\s*\*\s*slot"
            r"[\s\S]*?const\s+char\s*\*\s*reason\s*\)"
        ),
        1,
        ARENA_IXX,
    ),
    # --- counter declarations (compiled-in atomics) ---
    (
        "g_intermediate_create_with_cover_total counter",
        re.compile(r"std::atomic<std::uint64_t>\s+g_intermediate_create_with_cover_total"),
        1,
        ARENA_IXX,
    ),
    (
        "g_intermediate_create_value_only_total counter",
        re.compile(r"std::atomic<std::uint64_t>\s+g_intermediate_create_value_only_total"),
        1,
        ARENA_IXX,
    ),
    # --- v_read functions (query exposure) ---
    (
        "intermediate_create_with_cover_total_v_read",
        re.compile(r"intermediate_create_with_cover_total_v_read"),
        1,
        ARENA_IXX,
    ),
    (
        "intermediate_create_value_only_total_v_read",
        re.compile(r"intermediate_create_value_only_total_v_read"),
        1,
        ARENA_IXX,
    ),
    # --- helper has all three branches (slot / EXEMPT / value-only) ---
    # Use [\s\S]*? (DOTALL-equivalent) to match across newlines.
    (
        "helper has slot branch (register_external_root_slot_for_densify_)",
        re.compile(
            r"note_intermediate_create_with_cover_[\s\S]*?"
            r"if\s*\(\s*slot\s*!=\s*nullptr\s*\)"
            r"[\s\S]*?register_external_root_slot_for_densify_",
            re.DOTALL,
        ),
        1,
        ARENA_IXX,
    ),
    (
        "helper has EXEMPT branch (erase_intermediate_create_)",
        re.compile(
            r"note_intermediate_create_with_cover_[\s\S]*?"
            r"if\s*\(\s*reason\s*!=\s*nullptr\s*\)"
            r"[\s\S]*?erase_intermediate_create_",
            re.DOTALL,
        ),
        1,
        ARENA_IXX,
    ),
    (
        "helper has value-only fallback (note_intermediate_create_auto_wire_)",
        re.compile(
            r"note_intermediate_create_with_cover_[\s\S]*?"
            r"note_intermediate_create_auto_wire_\s*\(\s*p\s*\)",
            re.DOTALL,
        ),
        1,
        ARENA_IXX,
    ),
    # --- test extension (AC1-AC5 in test_general_object_pin_coverage_gate.cpp) ---
    (
        "test #3093 AC1 (source-cite helper)",
        re.compile(r"#3093 AC1"),
        1,
        TEST_DIR,
    ),
    (
        "test #3093 AC5 (no test_issue_3093.cpp per #81934)",
        re.compile(r"#3093 AC5"),
        1,
        TEST_DIR,
    ),
)


# Forbidden artefacts: no new docs/design/, no new test_issue_*.cpp, no
# second pin registry.
def _forbidden_artefacts() -> list[dict]:
    hits: list[dict] = []
    if DOCS_DIR.is_dir():
        for p in DOCS_DIR.glob("3093-*"):
            hits.append({"kind": "docs", "path": str(p.relative_to(ROOT))})
    test_issue = ROOT / "tests" / "core" / "test_issue_3093.cpp"
    if test_issue.is_file():
        hits.append({"kind": "test_issue", "path": str(test_issue.relative_to(ROOT))})
    # No second pin registry (slot / pin / EXEMPT is the triad).
    arena = ARENA_IXX.read_text(encoding="utf-8", errors="replace") if ARENA_IXX.is_file() else ""
    for bad in ("g_moving_pin_registry_3093", "class IntermediateCoverRegistry", "struct CoverRegistry3093"):
        if bad in arena:
            hits.append({"kind": "registry", "symbol": bad})
    return hits


def _strip_comments(text: str) -> str:
    """Strip // line comments and /* */ block comments. Keep string
    literals intact so query key strings ("audit-mid" etc.) remain
    visible to the regex."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Issue #3093 regression guard")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on any missing required pattern or forbidden artefact",
    )
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args(argv)

    for p in (ARENA_IXX, TEST_DIR):
        if not p.is_file():
            print(f"target not found: {p}", file=sys.stderr)
            return 2

    per_file = {p: _strip_comments(p.read_text(encoding="utf-8", errors="replace")) for p in (ARENA_IXX, TEST_DIR)}
    "\n".join(per_file.values())

    presence = []
    missing = []
    for label, pat, min_count, path_hint in REQUIRED_PATTERNS:
        haystack = per_file[path_hint]
        count = len(pat.findall(haystack))
        presence.append({"label": label, "count": count, "min": min_count})
        if count < min_count:
            missing.append(f"{label}: found {count}, need >= {min_count}")

    forbidden = _forbidden_artefacts()
    ok = not missing and not forbidden

    result = {
        "presence": presence,
        "missing": missing,
        "forbidden": forbidden,
        "ok": ok,
    }

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        if missing:
            print("[FAIL] missing required patterns:")
            for m in missing:
                print(f"  {m}")
        if forbidden:
            print("[FAIL] forbidden artefacts:")
            for f in forbidden:
                print(f"  {f}")
        if ok:
            print(
                "[OK] #3093 regression guard clean "
                "(note_intermediate_create_with_cover_ helper + 3 branches + "
                "2 counters + v_read + test AC1/AC5 + no docs/design/ + "
                "no test_issue_3093.cpp + no second registry)"
            )

    if args.strict and not ok:
        return 1
    return 0 if ok else (1 if args.strict else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
