#!/usr/bin/env python3
"""Issue #3094: shared name_map ownership at top-level clone_macro_body.

Background
----------
`clone_macro_body_at_depth` already provides ConcurrentCloneGuard for
same-FlatAST reject (#3028) + per-fiber hygiene depth (#2806), but
name_map (caller-owned `std::unordered_map*`) is never exclusivity-checked.
Two fibers sharing the same name_map pointer mutate it concurrently →
data-race UB during rename + NameMapCheckpoint rollback erasing keys
another fiber still relies on.

#3094 closes the gap by adding a parallel lock-free hashed-slot side
table (g_name_map_slots) + claim_name_map_clone / release_name_map_clone
helpers (CAS-based, mirrors g_same_flat_slots pattern). At top-level
entry under production (Restricted/Strict or force_hygienic), the guard
claims the map; a concurrent second top-level clone sharing the same
map pointer rejects with stable reason 4 = hygiene-name-map-shared.

This linter is the regression guard:

  * PRESENCE — claim/release helpers + counter + ConcurrentCloneGuard
    integration + reject reason code 4 must exist (no removal of the
    exclusivity-checked path).
  * NO INVENT REGRESSION — no new docs/design/3094-* (per #1655),
    no new test_issue_3094.cpp (per #81934), no second guard struct.

Exit codes:
  0 — clean (helper + counter + guard integration + reject reason all present)
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_concurrent_clone_name_map_3094.py            # report
  python3 scripts/coverage/checks/check_concurrent_clone_name_map_3094.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_concurrent_clone_name_map_3094.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

MACRO_EXPANSION_CPP = ROOT / "src" / "compiler" / "macro_expansion.cpp"
TEST_DIR = ROOT / "tests" / "compiler" / "test_concurrent_clone_hygiene_depth.cpp"
DOCS_DIR = ROOT / "docs" / "design"


# Required patterns: presence checks. Use DOTALL so multi-line function
# bodies (with their own `{}` blocks) can match across newlines.
REQUIRED_PATTERNS = (
    # --- helpers ---
    (
        "claim_name_map_clone helper (CAS-based)",
        re.compile(r"\[\[nodiscard\]\]\s*bool\s+claim_name_map_clone\s*\("),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "release_name_map_clone helper",
        re.compile(r"void\s+release_name_map_clone\s*\("),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "g_name_map_slots side table (lock-free hashed slot)",
        re.compile(r"std::atomic<std::uintptr_t>\s+g_name_map_slots\s*\["),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- counter ---
    (
        "g_macro_clone_name_map_shared_reject_total counter declared",
        re.compile(r"std::atomic<std::uint64_t>\s+g_macro_clone_name_map_shared_reject_total"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- ConcurrentCloneGuard integration ---
    (
        "ConcurrentCloneGuard calls claim_name_map_clone",
        re.compile(r"struct\s+ConcurrentCloneGuard[\s\S]*?claim_name_map_clone\s*\("),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "claim gated on is_sandbox_active() (Soft/Off zero-cost)",
        re.compile(
            r"struct\s+ConcurrentCloneGuard[\s\S]*?is_sandbox_active\(\)[\s\S]*?"
            r"claim_name_map_clone",
            re.DOTALL,
        ),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- reject reason code 4 ---
    (
        "reject reason code 4 = hygiene-name-map-shared",
        re.compile(r"g_macro_clone_last_reject_reason\.store\(\s*4\s*,"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- test extension (AC1-AC5 in test_concurrent_clone_hygiene_depth.cpp) ---
    (
        "test #3094 AC1 (claim_name_map_clone helper)",
        re.compile(r"#3094 AC1"),
        1,
        TEST_DIR,
    ),
    (
        "test #3094 AC5 (no docs/design + no test_issue per #81934)",
        re.compile(r"#3094 AC5"),
        1,
        TEST_DIR,
    ),
)


def _forbidden_artefacts() -> list[dict]:
    hits: list[dict] = []
    if DOCS_DIR.is_dir():
        for p in DOCS_DIR.glob("3094-*"):
            hits.append({"kind": "docs", "path": str(p.relative_to(ROOT))})
    test_issue = ROOT / "tests" / "compiler" / "test_issue_3094.cpp"
    if test_issue.is_file():
        hits.append({"kind": "test_issue", "path": str(test_issue.relative_to(ROOT))})
    return hits


def _strip_comments(text: str) -> str:
    """Strip // line comments and /* */ block comments. Keep string
    literals intact so query key strings etc. remain visible."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Issue #3094 regression guard")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit 1 on any missing required pattern or forbidden artefact",
    )
    parser.add_argument("--json", action="store_true", help="JSON output")
    args = parser.parse_args(argv)

    for p in (MACRO_EXPANSION_CPP, TEST_DIR):
        if not p.is_file():
            print(f"target not found: {p}", file=sys.stderr)
            return 2

    per_file = {
        p: _strip_comments(p.read_text(encoding="utf-8", errors="replace")) for p in (MACRO_EXPANSION_CPP, TEST_DIR)
    }

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
                "[OK] #3094 regression guard clean "
                "(claim/release_name_map_clone helpers + g_name_map_slots + "
                "counter + ConcurrentCloneGuard integration gated on is_sandbox_active() + "
                "reject reason code 4 + test AC1/AC5 + no docs/design/ + no test_issue_3094.cpp)"
            )

    if args.strict and not ok:
        return 1
    return 0 if ok else (1 if args.strict else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
