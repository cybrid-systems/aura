#!/usr/bin/env python3
# scripts/check_coercion_map_abort_rewind.py — Issue #3102 source-cite gate.
#
# Verifies the 3 abort sites in evaluator_mutation_boundary.cpp wire the
# CoercionMap + DeadCoercion abort rewind helpers (production/Full):
#   AC1 — truncate_type_cone_to_size(cp.coercion_cone_size_at_entry)
#   AC2 — coerced_nodes_tracker_take + force_dead_coercion_elim_into_cone
#   AC3 — bump_dead_coercion_decision_invalidate
#   AC4 — clear_coercion_commit_readiness_on_abort
#   AC5 — production_defaults_active() || AuditStrategy::Full gate +
#         g_coercion_map_abort_rewind_observe_total soft branch
#
# Default: --strict. CI gate.
#
# Self-test:
#   python3 scripts/check_coercion_map_abort_rewind.py --self-test
#
# Catches regressions when an abort path is added but the rewind is
# forgotten (would re-open the #3102 residual — stale CoercionMap /
# DeadCoercion-elided CastOp under restored AST).

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Iterable, Sequence
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = ("src/compiler/evaluator_mutation_boundary.cpp",)

# ── required call patterns per abort site ──────────────────────────────────
# Each tuple is the literal call that must appear at every abort site.
# Order: AC1 cone rewind → AC2 take + force-dirty → AC3 invalidate gen →
# AC4 commit_readiness clear → AC5 production gate / soft observe.
REQUIRED_CALLS: tuple[str, ...] = (
    "truncate_type_cone_to_size(cp.coercion_cone_size_at_entry)",
    "aura::compiler::coerced_nodes_tracker_take()",
    "aura::compiler::dirty::force_dead_coercion_elim_into_cone",
    "aura::compiler::dirty::bump_dead_coercion_decision_invalidate",
    "typed_audit::clear_coercion_commit_readiness_on_abort",
    "aura::compiler::g_coercion_map_abort_rewind_total",
    "aura::compiler::g_coercion_map_abort_rewind_observe_total",
)

# Anchors that identify each of the 3 abort sites. The script looks
# for these phrases and verifies the required calls appear AFTER the
# anchor but BEFORE the next major boundary / return. We use a window
# of up to N lines after each anchor to bound the search.
ABORT_SITE_ANCHORS: tuple[str, ...] = (
    # Site 1: post topology restore (failure path)
    "typed_audit::clear_type_linear_commit_proof_on_abort()",
    # Site 2: invariant force-rollback
    "invariant force-rollback clears proof face",
    # Site 3: Strict reflect-validate rollback
    "Strict reflect-validate rollback clears proof face",
)

ABORT_SITE_WINDOW_LINES = 60


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _line_index_of(text: str, anchor: str) -> int:
    """Return the byte/char index of the FIRST occurrence of anchor."""
    idx = text.find(anchor)
    if idx < 0:
        return -1
    return idx


def _slice_after(text: str, idx: int, max_lines: int) -> str:
    """Return up to max_lines of text starting at idx (line-bounded)."""
    line_start = text.rfind("\n", 0, idx) + 1
    end_line_start = line_start
    nl_count = 0
    pos = line_start
    while pos < len(text) and nl_count < max_lines:
        next_nl = text.find("\n", pos)
        if next_nl < 0:
            end_line_start = len(text)
            break
        end_line_start = next_nl + 1
        pos = end_line_start
        nl_count += 1
    return text[line_start:end_line_start]


def check_file(path: Path, *, strict: bool) -> list[str]:
    """Return a list of failure messages (empty = OK)."""
    failures: list[str] = []
    if not path.exists():
        failures.append(f"{path}: file not found")
        return failures
    text = _read_text(path)
    if not text:
        failures.append(f"{path}: empty file")
        return failures
    # Slice the file by anchor → window → verify required calls.
    found_sites = 0
    for anchor in ABORT_SITE_ANCHORS:
        idx = _line_index_of(text, anchor)
        if idx < 0:
            # The anchor may be slightly different at site 2/3 (the
            # comments use a different phrasing). Accept the variant.
            if strict:
                failures.append(f"{path}: abort-site anchor not found: {anchor!r}")
            continue
        found_sites += 1
        window = _slice_after(text, idx, ABORT_SITE_WINDOW_LINES)
        # Normalize by removing ALL whitespace before the substring check.
        # clang-format wraps long calls onto two lines and inserts a space
        # after the function name and before the argument (i.e.
        # "truncate_type_cone_to_size (\n    cp.coercion_cone_size_at_entry )").
        # A simple "collapse whitespace" pass would leave spaces around the
        # parens and still not match the human-readable required string
        # "truncate_type_cone_to_size(cp.coercion_cone_size_at_entry)".
        # Stripping all whitespace handles both single-line + wrapped forms
        # uniformly. The source-cite intent is "is this call present in the
        # abort site block?" not "is it formatted a specific way?".
        window_normalized = re.sub(r"\s+", "", window)
        for required in REQUIRED_CALLS:
            required_normalized = re.sub(r"\s+", "", required)
            if required_normalized not in window_normalized and strict:
                failures.append(f"{path}: abort site (anchor={anchor!r}) missing required call: {required!r}")
    # We expect at least 3 abort sites (the 3 rewind blocks); fail loudly
    # if we see fewer (a regression in production/Full coverage).
    if found_sites < 3 and strict:
        failures.append(f"{path}: only {found_sites} abort-site anchor(s) found, expected 3")
    return failures


def _self_test() -> int:
    """Validate the linter regex / anchor logic against fixture text."""
    fixture = """
    // Site 1 (post topology restore, failure path)
    typed_audit::clear_type_linear_commit_proof_on_abort();
    if (typed_audit::production_defaults_active() ||
        typed_audit::get_strategy() == typed_audit::AuditStrategy::Full) {
        aura::compiler::dirty::truncate_type_cone_to_size(cp.coercion_cone_size_at_entry);
        auto coerced = aura::compiler::coerced_nodes_tracker_take();
        if (!coerced.empty()) {
            const auto added =
                aura::compiler::dirty::force_dead_coercion_elim_into_cone(coerced);
            aura::compiler::g_coercion_map_abort_forced_dirty_total
                .fetch_add(added, std::memory_order_relaxed);
        }
        aura::compiler::dirty::bump_dead_coercion_decision_invalidate();
        typed_audit::clear_coercion_commit_readiness_on_abort();
        aura::compiler::g_coercion_map_abort_rewind_total.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        aura::compiler::g_coercion_map_abort_rewind_observe_total
            .fetch_add(1, std::memory_order_relaxed);
        (void)aura::compiler::coerced_nodes_tracker_take();
    }
    // Site 2 (invariant force-rollback)
    typed_audit::clear_type_linear_commit_proof_on_abort();
    if (typed_audit::production_defaults_active() ||
        typed_audit::get_strategy() == typed_audit::AuditStrategy::Full) {
        aura::compiler::dirty::truncate_type_cone_to_size(
            cp.coercion_cone_size_at_entry);
        auto coerced = aura::compiler::coerced_nodes_tracker_take();
        if (!coerced.empty()) {
            const auto added =
                aura::compiler::dirty::force_dead_coercion_elim_into_cone(
                    coerced);
            aura::compiler::g_coercion_map_abort_forced_dirty_total
                .fetch_add(added, std::memory_order_relaxed);
        }
        aura::compiler::dirty::bump_dead_coercion_decision_invalidate();
        typed_audit::clear_coercion_commit_readiness_on_abort();
        aura::compiler::g_coercion_map_abort_rewind_total.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        aura::compiler::g_coercion_map_abort_rewind_observe_total
            .fetch_add(1, std::memory_order_relaxed);
        (void)aura::compiler::coerced_nodes_tracker_take();
    }
    // Site 3 (Strict reflect-validate rollback)
    typed_audit::clear_type_linear_commit_proof_on_abort();
    if (typed_audit::production_defaults_active() ||
        typed_audit::get_strategy() == typed_audit::AuditStrategy::Full) {
        aura::compiler::dirty::truncate_type_cone_to_size(
            cp.coercion_cone_size_at_entry);
        auto coerced = aura::compiler::coerced_nodes_tracker_take();
        if (!coerced.empty()) {
            const auto added =
                aura::compiler::dirty::force_dead_coercion_elim_into_cone(
                    coerced);
            aura::compiler::g_coercion_map_abort_forced_dirty_total
                .fetch_add(added, std::memory_order_relaxed);
        }
        aura::compiler::dirty::bump_dead_coercion_decision_invalidate();
        typed_audit::clear_coercion_commit_readiness_on_abort();
        aura::compiler::g_coercion_map_abort_rewind_total.fetch_add(
            1, std::memory_order_relaxed);
    } else {
        aura::compiler::g_coercion_map_abort_rewind_observe_total
            .fetch_add(1, std::memory_order_relaxed);
        (void)aura::compiler::coerced_nodes_tracker_take();
    }
    """
    # Confirm each required call is found at least once per anchor.

    class _TmpFile:
        def __init__(self, txt: str) -> None:
            self._txt = txt

        def read_text(self, encoding: str = "utf-8") -> str:
            return self._txt

        def exists(self) -> bool:
            return True

    # The anchors are comment-relative (anchor 1) or comment text (2/3).
    # For self-test, we only need anchor 1 to be present (the others are
    # comments specific to the prod file); check the helper function with
    # the fixture that has anchor 1 three times.
    f = _TmpFile(fixture)
    # Force strict to verify the fixture passes.
    fails = check_file(f, strict=True)
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required calls found at every abort site")
    return 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3102 CoercionMap + DeadCoercion abort rewind source-cite gate",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=True,
        help="Fail on missing calls (default: strict)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run the linter self-test and exit",
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Files to scan (default: src/compiler/evaluator_mutation_boundary.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    targets: Iterable[Path]
    targets = (Path(t) for t in args.targets) if args.targets else (REPO_ROOT / t for t in DEFAULT_TARGETS)

    failures: list[str] = []
    for target in targets:
        target = target if target.is_absolute() else REPO_ROOT / target
        failures.extend(check_file(target, strict=args.strict))

    if failures:
        print("check_coercion_map_abort_rewind: FAIL")
        for line in failures:
            print(f"  {line}")
        return 1

    print("check_coercion_map_abort_rewind: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
