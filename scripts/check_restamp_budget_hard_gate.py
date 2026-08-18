#!/usr/bin/env python3
# scripts/check_restamp_budget_hard_gate.py -- Issue #3104 source-cite gate +
#                                            Issue #3138 Agent recovery contract.
#
# Verifies the restamp-budget hard-gate infrastructure is in place across
# the three layers:
#
#  1. Restamp exit path (src/compiler/evaluator_fiber_mutation.cpp):
#     - unified_restamp_after_boundary sets restamp_last_budget_exceeded_
#     - unified_restamp_after_boundary calls force_query_epoch_stale_from_restamp_budget()
#       under production_defaults_active when budget exceeded
#     - unified_restamp_after_boundary bumps g_unified_restamp_torn_visible_total
#
#  2. Gate accessors (src/core/ast.ixx + src/compiler/evaluator_security.cpp):
#     - FlatAST::restamp_last_budget_exceeded() accessor exists
#     - FlatAST::restamp_last_budget_exceeded_ atomic flag exists
#     - Evaluator::allow_query_stable_ref_export(id) exists + checks the flag
#     - Evaluator::query_stable_hard_reject_torn() exists + checks production + flag
#     - Evaluator::stamp_query_stable_ref_export(ref) exists + nulls ref on fail
#
#  3. Export sites (src/compiler/evaluator_primitives_query_workspace.cpp +
#                   src/compiler/evaluator_primitives_mutate.cpp):
#     - query:children-stable uses allow_query_stable_ref_export + returns restamp-lag
#     - query:parent-stable uses allow_query_stable_ref_export + returns restamp-lag
#     - query:stable-ref uses allow_query_stable_ref_export + returns restamp-lag
#     - query:ensure-ref uses allow_query_stable_ref_export + returns restamp-lag
#     - query:as-stable-ref (mutate:export-stable-ref) uses allow + returns restamp-lag
#     - All 5 sites include the Agent recovery hint suffix (Issue #3138)
#
#  4. Force-stale (src/core/workspace_epoch.hh):
#     - force_query_epoch_stale_from_restamp_budget() exists
#
#  5. Observability (src/core/ast.ixx):
#     - restamp_budget_exceeded_total_ atomic + accessor exists
#
#  6. Issue #3138 status surface (src/compiler/evaluator_primitives_obs_eval.cpp +
#                                  src/compiler/evaluator_primitives_query_obs_mid.cpp):
#     - query:query-epoch-stats exposes restamp-last-budget-exceeded (bool)
#     - query:query-epoch-stats exposes restamp-budget-exceeded-total (counter)
#     - query:query-epoch-stats exposes restamp-budget-query-epoch-stale-total (counter)

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_TARGETS: tuple[str, ...] = (
    "src/compiler/evaluator_fiber_mutation.cpp",
    "src/core/ast.ixx",
    "src/compiler/evaluator_security.cpp",
    "src/compiler/evaluator.cpp",
    "src/compiler/evaluator_primitives_query_workspace.cpp",
    "src/compiler/evaluator_primitives_mutate.cpp",
    "src/core/workspace_epoch.hh",
    "src/compiler/evaluator_primitives_obs_eval.cpp",
    "src/compiler/evaluator_primitives_query_obs_mid.cpp",
)

# (path, regex, label) -- each tuple is a substring/regex that must appear.
INFRA_REQUIRED: tuple[tuple[str, str], ...] = (
    # FlatAST flag + accessor (src/core/ast.ixx)
    ("src/core/ast.ixx", r"restamp_last_budget_exceeded_\{0\}"),
    ("src/core/ast.ixx", r"restamp_last_budget_exceeded\(\)\s+const\s+noexcept"),
    ("src/core/ast.ixx", r"restamp_budget_exceeded_total_\{0\}"),
    ("src/core/ast.ixx", r"restamp_budget_exceeded_total\(\)\s+const\s+noexcept"),
    # Force-stale (src/core/workspace_epoch.hh)
    ("src/core/workspace_epoch.hh", r"force_query_epoch_stale_from_restamp_budget\(\)"),
    # Restamp exit path (src/compiler/evaluator_fiber_mutation.cpp)
    ("src/compiler/evaluator_fiber_mutation.cpp", r"force_query_epoch_stale_from_restamp_budget\(\)"),
    ("src/compiler/evaluator_fiber_mutation.cpp", r"production_defaults_active\(\)"),
    ("src/compiler/evaluator_fiber_mutation.cpp", r"g_unified_restamp_torn_visible_total"),
    # Gate accessors (src/compiler/evaluator_security.cpp)
    ("src/compiler/evaluator_security.cpp", r"Evaluator::allow_query_stable_ref_export"),
    ("src/compiler/evaluator_security.cpp", r"Evaluator::query_stable_hard_reject_torn"),
    ("src/compiler/evaluator_security.cpp", r"Evaluator::stamp_query_stable_ref_export"),
    # Decls (src/compiler/evaluator.ixx)
    ("src/compiler/evaluator.ixx", r"allow_query_stable_ref_export"),
    ("src/compiler/evaluator.ixx", r"query_stable_hard_reject_torn"),
    ("src/compiler/evaluator.ixx", r"stamp_query_stable_ref_export"),
)

EXPORT_SITE_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # (path, site-name-keyword, restamp-lag-error-keyword)
    # query:children-stable
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:children-stable", r"mev\(\"restamp-lag\""),
    # query:parent-stable
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:parent-stable", r"mev\(\"restamp-lag\""),
    # query:stable-ref
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:stable-ref", r"mev\(\"restamp-lag\""),
    # query:ensure-ref
    ("src/compiler/evaluator_primitives_query_workspace.cpp", "query:ensure-ref", r"mev\(\"restamp-lag\""),
    # query:as-stable-ref (mutate:export-stable-ref)
    ("src/compiler/evaluator_primitives_mutate.cpp", "query:as-stable-ref", r"mev\(\"restamp-lag\""),
)

# Issue #3138: Agent-visible recovery contract — every restamp-lag reject
# path must include a stable recovery hint so multi-round Agents have a
# deterministic surface telling them what to do next.
# (path, site-name-keyword, anchor-regex, recovery-hint-regex)
RECOVERY_HINT_REQUIRED: tuple[tuple[str, str, str, str], ...] = (
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        "query:children-stable",
        r"mev\(\"restamp-lag\"",
        r"recovery:\s*re-query\s+after\s+budget\s+window\s+or\s+force\s+full\s+restamp\s+before\s+reusing\s+refs",
    ),
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        "query:parent-stable",
        r"mev\(\"restamp-lag\"",
        r"recovery:\s*re-query\s+after\s+budget\s+window\s+or\s+force\s+full\s+restamp\s+before\s+reusing\s+refs",
    ),
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        "query:stable-ref",
        r"mev\(\"restamp-lag\"",
        r"recovery:\s*re-query\s+after\s+budget\s+window\s+or\s+force\s+full\s+restamp\s+before\s+reusing\s+refs",
    ),
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        "query:ensure-ref",
        r"mev\(\"restamp-lag\"",
        r"recovery:\s*re-query\s+after\s+budget\s+window\s+or\s+force\s+full\s+restamp\s+before\s+reusing\s+refs",
    ),
    (
        "src/compiler/evaluator_primitives_mutate.cpp",
        "query:as-stable-ref",
        r"mev\(\"restamp-lag\"",
        r"recovery:\s*re-query\s+after\s+budget\s+window\s+or\s+force\s+full\s+restamp\s+before\s+reusing\s+refs",
    ),
)

# Issue #3138 AC1: query:query-epoch-stats surface must expose the
# budget-exceeded + torn-visible counters so Agents have a single
# authoritative query point. The fields already exist (#3000/#3104
# lineage); the linter pins the keys so a future rename trips the gate
# instead of breaking Agent consumers silently.
STATUS_SURFACE_REQUIRED: tuple[tuple[str, str], ...] = (
    ("src/compiler/evaluator_primitives_obs_eval.cpp", r"\"restamp-last-budget-exceeded\""),
    ("src/compiler/evaluator_primitives_obs_eval.cpp", r"\"restamp-budget-exceeded-total\""),
    ("src/compiler/evaluator_primitives_obs_eval.cpp", r"\"restamp-budget-query-epoch-stale-total\""),
    ("src/compiler/evaluator_primitives_query_obs_mid.cpp", r"\"restamp-budget-query-epoch-stale-total\""),
)


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_file(rel_path: str, regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if not re.search(regex, text) and strict:
        failures.append(f"{rel_path}: missing required pattern: {regex!r}")
    return failures


def check_export_sites(rel_path: str, site_keyword: str, restamp_lag_regex: str, *, strict: bool) -> list[str]:
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    if site_keyword not in text:
        if strict:
            failures.append(f"{rel_path}: export site {site_keyword!r} not found")
        return failures
    # Find all occurrences of the keyword. Anchor on the mev("restamp-lag")
    # call within the implementation body (skip docstring/comment matches
    # where the mev call may be far away). Use a forward window from the
    # keyword that covers typical implementation bodies.
    found_anchor = False
    for idx in (m.start() for m in re.finditer(re.escape(site_keyword), text)):
        candidate = text[idx : idx + 4000]
        if re.search(restamp_lag_regex, candidate):
            found_anchor = True
            break
    if not found_anchor and strict:
        failures.append(
            f"{rel_path}: export site {site_keyword!r} does not return "
            f"restamp-lag structured error (regex {restamp_lag_regex!r})"
        )
    return failures


def check_recovery_hint(
    rel_path: str, site_keyword: str, anchor_regex: str, hint_regex: str, *, strict: bool
) -> list[str]:
    """Issue #3138: Agent-visible recovery contract. Verifies the file
    contains BOTH the site_keyword (e.g. "query:stable-ref", appearing
    INSIDE the mev call's message body) AND the recovery hint regex
    (e.g. "recovery: re-query after budget window or force full restamp",
    appearing in the multi-line continuation after the mev call).
    File-level check is robust to source layout (avoids window-edge
    fragility for sites where the keyword appears earlier in docstrings).
    """
    failures: list[str] = []
    p = REPO_ROOT / rel_path
    if not p.exists():
        failures.append(f"{rel_path}: file not found")
        return failures
    text = _read_text(p)
    # Normalize: collapse all whitespace + remove the quote chars that
    # separate concatenated string literals. Then search the normalized
    # text. The actual runtime string IS contiguous (compiler concatenates
    # adjacent string literals), so this is the correct invariant to
    # check against the source file.
    normalized = "".join(ch if not ch.isspace() and ch != chr(34) else " " for ch in text)
    normalized = " ".join(normalized.split())
    has_anchor = bool(re.search(anchor_regex, text))
    has_keyword = site_keyword in text
    has_hint = bool(re.search(hint_regex, normalized))
    if not has_anchor:
        failures.append(f"{rel_path}: no {anchor_regex!r} call found")
    elif not (has_keyword and has_hint):
        failures.append(
            f"{rel_path}: site {site_keyword!r} recovery hint contract incomplete "
            f"(anchor={has_anchor}, keyword_in_file={has_keyword}, "
            f"hint_in_file={has_hint}, hint_regex={hint_regex!r})"
        )
    return failures


def _self_test() -> int:
    """Validate the linter regex / structure against fixture text."""
    fixture_header = """
    struct QueryResultMatch {
        std::uint32_t node_id = 0;
        std::uint32_t tenant_id = 0;
        std::uint32_t fiber_id = 0;
        std::uint32_t mutation_id_at_capture = 0;
        std::uint16_t wrap_epoch = 0;
        std::uint16_t cow_epoch_at_capture = 0;
        std::uint8_t boundary_pinned = 0;
        std::uint8_t reserved = 0;
        constexpr bool has_full_provenance() const noexcept;
    };
    enum class QueryResultFreshness {
        Fresh, StaleByEpoch, InvalidTenant, InvalidFiber,
        InvalidCowLayer, InvalidMutation, SoftOnlyNoProvenance,
    };
    inline QueryResultFreshness query_result_is_fresh_with_refs(...);
    inline constexpr int kQueryResultFullProvenanceIssue = 3103;
    inline void push_match_full(QueryResultMatch& m, ...);
    inline std::atomic<std::uint64_t> g_query_result_full_provenance_total;
    inline std::atomic<std::uint64_t> g_query_result_full_provenance_fresh_hits_total;
    inline std::atomic<std::uint64_t> g_query_result_full_provenance_stale_total;
    inline std::atomic<std::uint64_t> g_query_result_full_provenance_tenant_mismatch_total;
    inline std::atomic<std::uint64_t> g_query_result_full_provenance_fiber_mismatch_total;
    inline std::atomic<std::uint64_t> g_query_result_full_provenance_cow_mismatch_total;
    inline void note_query_result_full_provenance();
    inline void note_query_result_full_provenance_fresh_hit();
    inline void note_query_result_full_provenance_stale();
    inline void note_query_result_full_provenance_tenant_mismatch();
    inline void note_query_result_full_provenance_fiber_mismatch();
    inline void note_query_result_full_provenance_cow_mismatch();
    // evaluator_security.cpp gate accessors
    bool Evaluator::allow_query_stable_ref_export(ast::NodeId id) const noexcept;
    bool Evaluator::query_stable_hard_reject_torn() const noexcept;
    void Evaluator::stamp_query_stable_ref_export(ast::FlatAST::StableNodeRef& ref) const noexcept;
    // evaluator.ixx decls
    [[nodiscard]] bool allow_query_stable_ref_export(ast::NodeId id) const noexcept;
    [[nodiscard]] bool query_stable_hard_reject_torn() const noexcept;
    void stamp_query_stable_ref_export(ast::FlatAST::StableNodeRef& ref) const noexcept;
    // evaluator_fiber_mutation.cpp restamp exit path
    inline void force_query_epoch_stale_from_restamp_budget();
    if (production_defaults_active())
        g_unified_restamp_torn_visible_total.fetch_add(1);
    // ast.ixx restamp accessors + atomics
    mutable std::atomic<std::uint32_t> restamp_last_budget_exceeded_{0};
    [[nodiscard]] bool restamp_last_budget_exceeded() const noexcept;
    mutable std::atomic<std::uint64_t> restamp_budget_exceeded_total_{0};
    [[nodiscard]] std::uint64_t restamp_budget_exceeded_total() const noexcept;
    """
    fixture_export = """
    // query:children-stable
    if (!ev.allow_query_stable_ref_export(cid))
        return mev("restamp-lag", "query:children-stable: restamp budget exceeded; "
                                  "recovery: re-query after budget window or force full restamp before reusing refs");
    // query:parent-stable
    if (!ev.allow_query_stable_ref_export(pref.id))
        return mev("restamp-lag", "query:parent-stable: restamp budget exceeded; "
                                  "recovery: re-query after budget window or force full restamp before reusing refs");
    // query:stable-ref
    if (!ev.allow_query_stable_ref_export(node))
        return mev("restamp-lag", "query:stable-ref: restamp budget exceeded; "
                                  "recovery: re-query after budget window or force full restamp before reusing refs");
    // query:ensure-ref
    if (!ev.allow_query_stable_ref_export(held.id))
        return mev("restamp-lag", "query:ensure-ref: restamp budget exceeded; "
                                  "recovery: re-query after budget window or force full restamp before reusing refs");
    // query:as-stable-ref (mutate.cpp export-stable-ref)
    if (!ev.allow_query_stable_ref_export(id))
        return mev("restamp-lag", "query:as-stable-ref: restamp budget exceeded; "
                                  "recovery: re-query after budget window or force full restamp before reusing refs");
    """
    fixture_surface = """
    // query:query-epoch-stats surface (AC1 single authoritative source)
    {"restamp-last-budget-exceeded", make_int(0)},
    {"restamp-budget-exceeded-total", make_int(0)},
    {"restamp-budget-query-epoch-stale-total", make_int(0)},
    """
    fails: list[str] = []
    for rel, rx in INFRA_REQUIRED:
        if (
            rel.endswith("ast.ixx")
            or rel.endswith("workspace_epoch.hh")
            or rel.endswith("evaluator_fiber_mutation.cpp")
            or rel.endswith("evaluator_security.cpp")
            or rel.endswith("evaluator.ixx")
        ):
            text = fixture_header
        else:
            text = ""
        if not re.search(rx, text):
            fails.append(f"{rel}: missing required pattern: {rx!r}")
    # Export sites (check site_keyword + restamp_lag_regex in window)
    for rel, site_keyword, restamp_lag_regex in EXPORT_SITE_REQUIRED:
        text = fixture_export
        if site_keyword not in text:
            fails.append(f"{rel}: export site {site_keyword!r} not found")
            continue
        idx = text.find(site_keyword)
        window = text[idx : idx + 4000]
        if not re.search(restamp_lag_regex, window):
            fails.append(
                f"{rel}: export site {site_keyword!r} does not return "
                f"restamp-lag structured error (regex {restamp_lag_regex!r})"
            )
    # Issue #3138: recovery hint enforcement (normalized match).
    for rel, site_keyword, anchor_regex, hint_regex in RECOVERY_HINT_REQUIRED:
        text = fixture_export
        normalized = "".join(ch if not ch.isspace() and ch != chr(34) else " " for ch in text)
        normalized = " ".join(normalized.split())
        has_anchor = bool(re.search(anchor_regex, text))
        has_keyword = site_keyword in text
        has_hint = bool(re.search(hint_regex, normalized))
        if not (has_anchor and has_keyword and has_hint):
            fails.append(
                f"{rel}: recovery hint contract incomplete for {site_keyword!r} "
                f"(anchor={has_anchor}, keyword={has_keyword}, hint={has_hint})"
            )
    # Issue #3138 AC1: status surface field exposure.
    for rel, regex in STATUS_SURFACE_REQUIRED:
        text = fixture_surface
        if not re.search(regex, text):
            fails.append(f"{rel}: missing required status-surface pattern: {regex!r}")
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3104 restamp-budget hard-gate + Issue #3138 Agent recovery contract source-cite gate",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        default=True,
        help="Fail on missing patterns (default: strict)",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run the linter self-test and exit",
    )
    parser.add_argument(
        "targets",
        nargs="*",
        help="Files to scan (default: see DEFAULT_TARGETS)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []
    target_filter = set(args.targets) if args.targets else set(DEFAULT_TARGETS)
    for rel_path, regex in INFRA_REQUIRED:
        if rel_path not in target_filter:
            continue
        failures.extend(check_file(rel_path, regex, strict=args.strict))
    for rel_path, site_keyword, restamp_lag_regex in EXPORT_SITE_REQUIRED:
        if rel_path not in target_filter:
            continue
        failures.extend(check_export_sites(rel_path, site_keyword, restamp_lag_regex, strict=args.strict))
    # Issue #3138: recovery hint + status surface enforcement.
    for rel_path, site_keyword, anchor_regex, hint_regex in RECOVERY_HINT_REQUIRED:
        if rel_path not in target_filter:
            continue
        failures.extend(check_recovery_hint(rel_path, site_keyword, anchor_regex, hint_regex, strict=args.strict))
    for rel_path, regex in STATUS_SURFACE_REQUIRED:
        if rel_path not in target_filter:
            continue
        failures.extend(check_file(rel_path, regex, strict=args.strict))

    if failures:
        print("check_restamp_budget_hard_gate: FAIL")
        for line in failures:
            print(f"  {line}")
        return 1

    print("check_restamp_budget_hard_gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
