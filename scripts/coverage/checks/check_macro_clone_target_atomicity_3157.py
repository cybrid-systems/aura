#!/usr/bin/env python3
"""Issue #3157: clone_macro_body target FlatAST atomicity.

Background
----------
`ASTArena::clone_macro_body_at_depth` (and its public wrapper
`clone_macro_body`) installs a `NameMapCheckpoint` for the rename table at
top-level entry, but on steal-abort / depth-limit fail only the
`name_map` is rolled back. Nodes allocated via `target.add_*` during the
recursive clone walk remain reachable via size growth / free-list gaps,
even though the Agent sees `NULL_NODE` / deny reason. Subsequent query /
densify may observe partial trees.

`#3157` closes the residual under production surface by installing a
lightweight `ExpandCheckpointGuard` (the same machinery from #3062
family already used by `macro_expand_all_body` for pass-limit rollback)
at top-level clone entry. On steal-abort / depth-limit fail,
`try_restore()` rolls back `target.add_*` allocations so orphan
MacroIntroduced nodes never reach subsequent query / densify. Soft / Off
keeps historical half-write (zero-cost contract).

This linter is the regression guard for `#3157`:

  * INSTALL_PRESENT — `clone_macro_body_at_depth` installs
    `expand_ckpt.ensure_installed()` at top-level entry (hygiene_depth == 0
    AND production_surface = aura::core::sandbox::is_sandbox_active()).

  * RESTORE_PRESENT — steal-fail return path calls
    `expand_ckpt.try_restore()` BEFORE `return NULL_NODE`, so target.add_*
    allocations are rolled back.

  * EXISTING_STEAL_COUNTERS — `g_macro_clone_steal_abort_total.fetch_add`
    + `g_macro_clone_last_reject_reason.store(3)` still fire on steal-fail
    (existing observability counters preserved).

  * NO_NEW_ROLLBACK_ENGINE — no `g_3157_*` atomic counter; the existing
    expand checkpoint machinery from #3062 family is reused. Existing C
    symbols `install_macro_expand_checkpoint()`,
    `aura_evaluator_try_restore_macro_expand_checkpoint()`,
    `aura_evaluator_commit_macro_expand_checkpoint()` all referenced.

  * NO_INVENT — no new `docs/design/3157-*` (per #1655), no new
    `tests/issues/test_issue_3157.cpp` (per #81934 — src/-aligned suite
    instead, i.e. `tests/compiler/test_macro_clone_target_atomicity.cpp`).

  * NESTED_NO_DOUBLE_ROLLBACK — install is gated on
    `hygiene_depth == 0` so nested recursion (depth > 0) inherits the
    top-level checkpoint for the whole subtree (no second rollback).

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_macro_clone_target_atomicity_3157.py            # report
  python3 scripts/coverage/checks/check_macro_clone_target_atomicity_3157.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_macro_clone_target_atomicity_3157.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

MACRO_EXPANSION_CPP = ROOT / "src" / "compiler" / "macro_expansion.cpp"
TEST_FILE = ROOT / "tests" / "compiler" / "test_macro_clone_target_atomicity.cpp"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_FILE = ROOT / "tests" / "issues" / "test_issue_3157.cpp"


# Required patterns: presence checks.
# Note on regex: function signatures in macro_expansion.cpp commonly have
# trailing qualifiers like `noexcept` between `)` and `{`. The `[^{]*\{`
# idiom allows arbitrary non-`{` text (including `noexcept`, comments, ws)
# before the body open, while still keeping the match tight (won't cross
# function boundaries since `{` only appears once per body).
_BODY_OPEN = r"[^{]*\{[\s\S]*?"  # once `{` is consumed, non-greedy to next anchor


def _read_text(path: Path) -> tuple[str, bool]:
    if not path.exists():
        return ("", False)
    try:
        return (path.read_text(encoding="utf-8", errors="replace"), True)
    except OSError:
        return ("", False)


REQUIRED_PATTERNS = (
    # --- AC1: ExpandCheckpointGuard struct + install in clone_macro_body_at_depth ---
    (
        "clone_macro_body_at_depth has ExpandCheckpointGuard struct",
        re.compile(
            r"static\s+aura::ast::NodeId\s+clone_macro_body_at_depth\s*\("
            + _BODY_OPEN
            + r"struct\s+ExpandCheckpointGuard\s*\{"
        ),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "clone_macro_body_at_depth installs expand_ckpt.ensure_installed",
        re.compile(
            r"static\s+aura::ast::NodeId\s+clone_macro_body_at_depth\s*\("
            + _BODY_OPEN
            + r"expand_ckpt\.ensure_installed\s*\(\s*\)"
        ),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "install gated on hygiene_depth == 0 && production_surface",
        re.compile(
            r"if\s*\(\s*hygiene_depth\s*==\s*0\s*&&\s*production_surface\s*\)\s*"
            r"\s*expand_ckpt\.ensure_installed\s*\(\s*\)"
        ),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- AC2: steal-fail path calls try_restore before return NULL_NODE ---
    (
        "steal-fail return path calls try_restore() before return NULL_NODE",
        re.compile(
            r"if\s*\(\s*steal1\s*>\s*steal0\s*\)\s*\{[\s\S]*?"
            r"expand_ckpt\.try_restore\s*\(\s*\)[\s\S]*?"
            r"return\s+NULL_NODE"
        ),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- AC4: existing steal counters preserved ---
    (
        "g_macro_clone_steal_abort_total.fetch_add(1) preserved",
        re.compile(r"g_macro_clone_steal_abort_total\.fetch_add\s*\(\s*1"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "g_macro_clone_last_reject_reason.store(3) preserved",
        re.compile(r"g_macro_clone_last_reject_reason\.store\s*\(\s*3"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- AC5: no new rollback engine, expand ckpt machinery reused ---
    (
        "install_macro_expand_checkpoint() reused",
        re.compile(r"install_macro_expand_checkpoint\s*\(\s*\)"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "aura_evaluator_try_restore_macro_expand_checkpoint reused",
        re.compile(r"aura_evaluator_try_restore_macro_expand_checkpoint\s*\(\s*\)"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "aura_evaluator_commit_macro_expand_checkpoint reused",
        re.compile(r"aura_evaluator_commit_macro_expand_checkpoint\s*\(\s*\)"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- AC9: src/-aligned test present ---
    (
        "src/-aligned test tests/compiler/test_macro_clone_target_atomicity.cpp",
        re.compile(r"run_test_macro_clone_target_atomicity"),
        1,
        TEST_FILE,
    ),
)


# Forbidden patterns: presence checks.
FORBIDDEN_PATTERNS = (
    # --- AC5: no new g_3157_* atomic counter (no second rollback engine) ---
    (
        "forbidden g_3157_* atomic counter (per issue non-goals — reuse #3062 family)",
        re.compile(r"g_3157_"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    (
        "forbidden atomic<std::uint64_t> g_3157_* declaration",
        re.compile(r"atomic<std::uint64_t>\s+g_3157_"),
        1,
        MACRO_EXPANSION_CPP,
    ),
    # --- AC7: no docs/design/3157-* (per #1655) ---
    (
        "forbidden docs/design/3157-* (per #1655)",
        re.compile(r"3157-"),
        1,
        DOCS_DESIGN_DIR,
    ),
    # --- AC8: no tests/issues/test_issue_3157.cpp (per #81934) ---
    (
        "forbidden tests/issues/test_issue_3157.cpp (per #81934 — src/-aligned suite instead)",
        re.compile(r"^."),
        1,
        ISSUES_TEST_FILE,
    ),
)


def collect_hits(patterns, root: Path) -> list[dict]:
    hits: list[dict] = []
    for label, regex, _min, relpath in patterns:
        text, ok = _read_text(relpath)
        rel = str(relpath.relative_to(root)) if relpath.is_relative_to(root) else str(relpath)
        if not ok:
            hits.append(
                {
                    "label": label,
                    "path": rel,
                    "status": "MISSING",
                    "matches": 0,
                }
            )
            continue
        count = len(regex.findall(text))
        if count >= 1:
            hits.append(
                {
                    "label": label,
                    "path": rel,
                    "status": "PASS",
                    "matches": count,
                }
            )
        else:
            hits.append(
                {
                    "label": label,
                    "path": rel,
                    "status": "FAIL",
                    "matches": 0,
                }
            )
    return hits


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--strict", action="store_true", help="exit 1 on any required-missing or forbidden-present hit")
    parser.add_argument("--json", action="store_true", help="emit JSON report on stdout")
    parser.add_argument("--root", type=Path, default=ROOT, help="override repo root (default: script parent ^3)")
    args = parser.parse_args(argv)

    root = args.root.resolve()

    required_hits = collect_hits(REQUIRED_PATTERNS, root)
    forbidden_hits = collect_hits(FORBIDDEN_PATTERNS, root)

    required_missing = [h for h in required_hits if h["status"] != "PASS"]
    forbidden_present = [h for h in forbidden_hits if h["status"] == "PASS"]

    report = {
        "issue": 3157,
        "topic": "clone_macro_body target FlatAST atomicity (depth/steal fail-closed)",
        "required_total": len(required_hits),
        "required_pass": len(required_hits) - len(required_missing),
        "required_missing": required_missing,
        "forbidden_total": len(forbidden_hits),
        "forbidden_pass": len(forbidden_hits) - len(forbidden_present),
        "forbidden_present": forbidden_present,
        "verdict": "clean" if not required_missing and not forbidden_present else "violation",
    }

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"#3157 linter — {report['verdict']}")
        print(f"  required: {report['required_pass']}/{report['required_total']} pass")
        for h in required_hits:
            mark = "✓" if h["status"] == "PASS" else "✗"
            print(f"    [{mark}] {h['label']}  ({h['path']}, {h['matches']} match(es))")
        print(f"  forbidden: {report['forbidden_pass']}/{report['forbidden_total']} absent")
        for h in forbidden_hits:
            mark = "✗ HIT" if h["status"] == "PASS" else "✓ absent"
            print(f"    [{mark}] {h['label']}  ({h['path']})")

    if args.strict and (required_missing or forbidden_present):
        return 1
    if required_missing or forbidden_present:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
