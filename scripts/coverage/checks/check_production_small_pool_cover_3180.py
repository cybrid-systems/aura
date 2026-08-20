#!/usr/bin/env python3
"""Issue #3180: drive production small-pool allocate call sites to true cover
(slot/EXEMPT) — uncovered_under_required happy-path residual of #3156.

Background
----------
`ASTArena::maybe_note_allocate_intermediate_(ptr, size)` (#3053 + #3156) bumped
`g_intermediate_create_uncovered_under_required_total` under production required
+ both null cover — fail-closed inventory + sticky-off. Hot-path call sites
still pass nullptr/nullptr to `try_allocate` / `allocate_checked` / `create<T>`,
so the implicit cover bump fires on every small-pool intermediate in production.

`#3180` closes the residual by:

  1. Threading optional `cover_slot` / `cover_reason` through
     `allocate_checked` → `allocate_raw` → `allocate_raw_impl` →
     `maybe_note_allocate_intermediate_` → `note_intermediate_create_with_cover_`
     so hot-path callers can declare cover at the allocate site.

  2. Migrating production hot-path call sites
     (`evaluator_eval_flat.cpp`, `service.ixx`,
     `evaluator_module_loader.cpp`, `evaluator_workspace_tree.cpp`) to pass
     either `reinterpret_cast<void**>(&ptr_var)` (long-lived, slot cover
     registers `*slot` for densify rewrite) or a stable reason string
     (transient, EXEMPT via `note_intermediate_create_with_cover_` erase branch).

  3. Promoting `note_intermediate_create_with_cover_` from private to public so
     production call sites (Evaluator / CompilerService / etc.) can declare
     cover at the allocate site without friend-class intrusion.

AC2 target after production soak:
  `intermediate_create_uncovered_under_required_total_v_read() == 0`

This linter is the regression guard:

  * AC1_PUBLIC_COVER_HELPER — `note_intermediate_create_with_cover_` is in the
    PUBLIC section of `ASTArena` (after a `public:` marker, before the next
    `private:`). External callers must reach it without friend-class.

  * AC2_FORWARD_COVER — `allocate_checked` / `allocate_raw` / `allocate_raw_impl`
    signatures all accept optional `cover_slot` / `cover_reason` parameters
    (with `= nullptr` defaults). Body forwards the cover to
    `maybe_note_allocate_intermediate_(ptr, size, cover_slot, cover_reason)`.

  * AC3_HOT_PATH_COVER_DECLS — each migrated hot-path file declares cover at
    every small-pool intermediate create site: long-lived (`pat_pool` /
    `pat_flat` / `pool_ptr` / `flat_ptr` / `mod_env` / `env`) pass slot
    `reinterpret_cast<void**>(&var), nullptr`; transient (closure body,
    import-parse, inst-env-cache) pass `nullptr, "<reason>"`.

  * AC4_ZERO_COST_PRESERVED — `maybe_note_allocate_intermediate_` keeps the
    single `general_object_pin_required_active()` load + `in_render_hotpath()`
    gate + `kMaxSmallSize` / `small_pool_.owns()` checks. Default `nullptr` /
    `nullptr` preserves legacy uncovered-bump behaviour (no API break).

  * AC5_NO_INVENT — no `docs/design/3180-*` (per #1655), no
    `tests/issues/test_issue_3180.cpp` (per #81934 — src/-aligned suite
    instead, i.e. `tests/core/test_arena_required_cover_no_value_only.cpp`).

Exit codes:
  0 — clean
  1 — at least one required pattern missing OR forbidden artefact present
  2 — invocation error

Usage:
  python3 scripts/coverage/checks/check_production_small_pool_cover_3180.py            # report
  python3 scripts/coverage/checks/check_production_small_pool_cover_3180.py --strict    # exit 1 on hit
  python3 scripts/coverage/checks/check_production_small_pool_cover_3180.py --json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

ARENA_IXX = ROOT / "src" / "core" / "arena.ixx"
EVAL_FLAT = ROOT / "src" / "compiler" / "evaluator_eval_flat.cpp"
SERVICE_IXX = ROOT / "src" / "compiler" / "service.ixx"
MOD_LOADER = ROOT / "src" / "compiler" / "evaluator_module_loader.cpp"
WS_TREE = ROOT / "src" / "compiler" / "evaluator_workspace_tree.cpp"
TEST_DIR = ROOT / "tests" / "core" / "test_arena_required_cover_no_value_only.cpp"
DOCS_DESIGN_DIR = ROOT / "docs" / "design"
ISSUES_TEST_DIR = ROOT / "tests" / "issues" / "test_issue_3180.cpp"


# --- AC1: public-section cover helper ---
#   The `note_intermediate_create_with_cover_` function must appear AFTER a
#   `public:` marker (its current home — line ~2478 — is after the private
#   `note_intermediate_create_auto_wire_` body at line ~2473). We verify by
#   locating the function definition and confirming the immediately preceding
#   access specifier on the same line is `public:` (not `private:` / `protected:`).
_REQUIRED_PUBLIC_COVER_HELPER = (
    "AC1: note_intermediate_create_with_cover_ is in public section",
    re.compile(
        # capture the most-recent access specifier line immediately above
        # the function definition; require it to be `public:`
        r"public:\s*\n"
        r"(?:\s*//[^\n]*\n)*"  # optional comment block(s)
        r"\s*void\s+note_intermediate_create_with_cover_\s*\("
    ),
    1,
    ARENA_IXX,
)

# --- AC2: forward cover through allocate chain ---
_REQUIRED_ALLOCATION_CHAIN = (
    # allocate_checked signature: cover_slot/cover_reason params with defaults
    # (multi-line — use [\s\S] not [^)] to skip alignof()'s closing paren)
    (
        "AC2: allocate_checked signature has cover_slot/cover_reason params",
        re.compile(r"allocate_checked\s*\([\s\S]*?cover_slot\s*=\s*nullptr[\s\S]*?cover_reason\s*=\s*nullptr"),
        1,
        ARENA_IXX,
    ),
    # allocate_raw_impl signature: cover_slot/cover_reason params with defaults
    (
        "AC2: allocate_raw_impl signature has cover_slot/cover_reason params",
        re.compile(r"allocate_raw_impl\s*\([^)]*cover_slot\s*=\s*nullptr[^)]*cover_reason\s*=\s*nullptr"),
        1,
        ARENA_IXX,
    ),
    # allocate_raw_impl forwards cover to maybe_note_allocate_intermediate_
    (
        "AC2: allocate_raw_impl forwards cover to maybe_note_allocate_intermediate_",
        re.compile(r"maybe_note_allocate_intermediate_\s*\(\s*ptr\s*,\s*size\s*,\s*cover_slot\s*,\s*cover_reason\s*\)"),
        1,
        ARENA_IXX,
    ),
    # allocate_checked forwards cover to allocate_raw_impl
    (
        "AC2: allocate_checked forwards cover to allocate_raw_impl",
        re.compile(r"allocate_raw_impl\s*\(\s*size\s*,\s*alignment\s*,\s*cover_slot\s*,\s*cover_reason\s*\)"),
        1,
        ARENA_IXX,
    ),
    # maybe_note_allocate_intermediate_ accepts slot/reason with defaults
    (
        "AC2: maybe_note_allocate_intermediate_ signature has slot/reason defaults",
        re.compile(
            r"maybe_note_allocate_intermediate_\s*\(\s*void\s*\*\s*ptr\s*,\s*std::size_t\s+size\s*,\s*void\s*\*\*(?:\s+)?slot\s*=\s*nullptr\s*,\s*const\s+char\s*\*\s*reason\s*=\s*nullptr"
        ),
        1,
        ARENA_IXX,
    ),
    # maybe_note_allocate_intermediate_ forwards slot/reason to note_intermediate_create_with_cover_
    (
        "AC2: maybe_note_allocate_intermediate_ forwards slot/reason to with_cover_",
        re.compile(r"note_intermediate_create_with_cover_\s*\(\s*ptr\s*,\s*slot\s*,\s*reason\s*\)"),
        1,
        ARENA_IXX,
    ),
)

# --- AC3: hot-path call-site cover declarations ---
_REQUIRED_HOT_PATH_COVER = (
    # evaluator_eval_flat.cpp: pat_pool/pat_flat long-lived slot cover
    (
        "AC3: evaluator_eval_flat declares pat_pool/pat_flat slot cover",
        re.compile(
            r"note_intermediate_create_with_cover_\(\s*\n?\s*pat_pool\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&pat_pool\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        EVAL_FLAT,
    ),
    (
        "AC3: evaluator_eval_flat declares pat_flat slot cover",
        re.compile(
            r"note_intermediate_create_with_cover_\(\s*\n?\s*pat_flat\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&pat_flat\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        EVAL_FLAT,
    ),
    # evaluator_eval_flat.cpp: transient EXEMPT (closure body, import, inst-env)
    (
        "AC3: evaluator_eval_flat declares cl_flat EXEMPT(eval-flat-closure-body-transient)",
        re.compile(r'"eval-flat-closure-body-transient"'),
        1,
        EVAL_FLAT,
    ),
    (
        "AC3: evaluator_eval_flat declares ipool/iflat EXEMPT(require-import-parse-transient)",
        re.compile(r'"require-import-parse-transient"'),
        1,
        EVAL_FLAT,
    ),
    (
        "AC3: evaluator_eval_flat declares cached_env EXEMPT(inst-env-cache-transient)",
        re.compile(r'"inst-env-cache-transient"'),
        1,
        EVAL_FLAT,
    ),
    # service.ixx: arena pool/flat + mod_arena pool/flat slot cover
    (
        "AC3: service.ixx declares arena parse_to_flat pool/flat slot cover",
        re.compile(
            r"note_intermediate_create_with_cover_\(\s*\n?\s*pool_ptr\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&pool_ptr\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        SERVICE_IXX,
    ),
    (
        "AC3: service.ixx declares arena parse_to_flat flat_ptr slot cover",
        re.compile(
            r"note_intermediate_create_with_cover_\(\s*\n?\s*flat_ptr\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&flat_ptr\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        SERVICE_IXX,
    ),
    (
        "AC3: service.ixx declares mod_arena parse_to_flat pool slot cover",
        re.compile(
            r"mod_arena\.note_intermediate_create_with_cover_\(\s*\n?\s*pool_ptr\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&pool_ptr\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        SERVICE_IXX,
    ),
    (
        "AC3: service.ixx declares mod_arena parse_to_flat flat slot cover",
        re.compile(
            r"mod_arena\.note_intermediate_create_with_cover_\(\s*\n?\s*flat_ptr\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&flat_ptr\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        SERVICE_IXX,
    ),
    # evaluator_module_loader.cpp: pool/flat/env slot cover
    (
        "AC3: evaluator_module_loader declares mod_arena pool slot cover",
        re.compile(
            r"mod_arena\.note_intermediate_create_with_cover_\(\s*\n?\s*pool_ptr\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&pool_ptr\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        MOD_LOADER,
    ),
    (
        "AC3: evaluator_module_loader declares mod_arena flat slot cover",
        re.compile(
            r"mod_arena\.note_intermediate_create_with_cover_\(\s*\n?\s*flat_ptr\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&flat_ptr\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        MOD_LOADER,
    ),
    (
        "AC3: evaluator_module_loader declares mod_arena mod_env slot cover",
        re.compile(
            r"mod_arena\.note_intermediate_create_with_cover_\(\s*\n?\s*mod_env\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&mod_env\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        MOD_LOADER,
    ),
    # evaluator_workspace_tree.cpp: env slot cover
    (
        "AC3: evaluator_workspace_tree declares workspace env slot cover",
        re.compile(
            r"ar->note_intermediate_create_with_cover_\(\s*\n?\s*env\s*,\s*reinterpret_cast<void\*\*>\s*\(\s*&env\s*\)\s*,\s*nullptr\s*\)"
        ),
        1,
        WS_TREE,
    ),
    # AC9: src/-aligned test extension present (ac3180_* functions called)
    (
        "AC3: test_arena_required_cover_no_value_only calls ac3180_cover_param_threading",
        re.compile(r"ac3180_cover_param_threading\s*\(\s*\)"),
        1,
        TEST_DIR,
    ),
    (
        "AC3: test_arena_required_cover_no_value_only calls ac3180_hot_path_cover_declarations",
        re.compile(r"ac3180_hot_path_cover_declarations\s*\(\s*\)"),
        1,
        TEST_DIR,
    ),
)

# --- AC4: zero-cost contract preserved ---
_REQUIRED_ZERO_COST = (
    (
        "AC4: maybe_note_allocate_intermediate_ keeps single required_active load",
        re.compile(
            r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)[^{]*\{[\s\S]*?general_object_pin_required_active\s*\(\)"
        ),
        1,
        ARENA_IXX,
    ),
    (
        "AC4: maybe_note_allocate_intermediate_ keeps in_render_hotpath gate",
        re.compile(r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)[^{]*\{[\s\S]*?in_render_hotpath\s*\(\)"),
        1,
        ARENA_IXX,
    ),
    (
        "AC4: maybe_note_allocate_intermediate_ keeps kMaxSmallSize check",
        re.compile(r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)[^{]*\{[\s\S]*?kMaxSmallSize"),
        1,
        ARENA_IXX,
    ),
    (
        "AC4: maybe_note_allocate_intermediate_ keeps small_pool_.owns check",
        re.compile(r"void\s+maybe_note_allocate_intermediate_\s*\([^)]*\)[^{]*\{[\s\S]*?small_pool_\.owns"),
        1,
        ARENA_IXX,
    ),
)

REQUIRED_PATTERNS = (
    (_REQUIRED_PUBLIC_COVER_HELPER,) + _REQUIRED_ALLOCATION_CHAIN + _REQUIRED_HOT_PATH_COVER + _REQUIRED_ZERO_COST
)


# Forbidden patterns
FORBIDDEN_PATTERNS = (
    # AC5: no docs/design/3180-* (per #1655)
    (
        "AC5: forbidden docs/design/3180-* (per #1655)",
        re.compile(r"3180-"),
        1,
        DOCS_DESIGN_DIR,
    ),
    # AC5: no tests/issues/test_issue_3180.cpp (per #81934 — src/-aligned suite)
    (
        "AC5: forbidden tests/issues/test_issue_3180.cpp (per #81934 — src/-aligned suite instead)",
        re.compile(r"^."),
        1,
        ISSUES_TEST_DIR,
    ),
)


def collect_hits(patterns, root: Path) -> list[dict]:
    hits: list[dict] = []
    for label, regex, _min, relpath in patterns:
        path = relpath
        if not path.exists():
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
                    "status": "MISSING",
                    "matches": 0,
                }
            )
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
                    "status": f"READ_ERROR: {exc}",
                    "matches": 0,
                }
            )
            continue
        count = len(regex.findall(text))
        if count >= 1:
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
                    "status": "PASS",
                    "matches": count,
                }
            )
        else:
            hits.append(
                {
                    "label": label,
                    "path": str(path.relative_to(root)) if path.is_relative_to(root) else str(path),
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
        "issue": 3180,
        "topic": "drive production small-pool allocate call sites to true cover (slot/EXEMPT)",
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
        print(f"#3180 linter — {report['verdict']}")
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
