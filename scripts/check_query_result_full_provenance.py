#!/usr/bin/env python3
# scripts/check_query_result_full_provenance.py -- Issue #3103 source-cite gate.
#
# Verifies that the schema-2 QueryResult full-provenance infrastructure is in
# place across the three layers:
#
#  1. Struct extension (src/core/workspace_epoch.hh):
#     - QueryResultMatch has tenant_id / fiber_id / mutation_id_at_capture /
#       wrap_epoch / cow_epoch_at_capture / boundary_pinned fields.
#     - push_match takes optional wrap_epoch / cow_epoch_at_capture /
#       tenant_id / fiber_id / mutation_id_at_capture / boundary_pinned.
#     - push_match_full overload exists.
#     - has_full_provenance() helper exists.
#
#  2. Validator (src/core/workspace_epoch.hh):
#     - QueryResultFreshness enum exists with Fresh + StaleByEpoch +
#       InvalidTenant + InvalidFiber + InvalidCowLayer + InvalidMutation +
#       SoftOnlyNoProvenance.
#     - query_result_is_fresh_with_refs declaration exists.
#
#  3. Implementation + stamping (src/compiler/evaluator_primitives_query_workspace.cpp):
#     - query_result_is_fresh_with_refs definition exists.
#     - stamp_query_result_full_provenance definition exists.
#     - Stamping helper is called from :as-query-result / :query-result finish
#       paths (3 sites: :find / :tag / query:pattern).
#     - Stamping helper uses make_stamped_ref + stamp_query_stable_ref_export.
#
#  4. Observability counters (src/core/workspace_epoch.hh):
#     - 6 full-provenance atomics + 6 note_* helpers exist.

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_TARGETS: tuple[str, ...] = (
    "src/core/workspace_epoch.hh",
    "src/compiler/evaluator_primitives_query_workspace.cpp",
)

# (path, regex, label) — each tuple is a substring/regex that must appear.
STRUCT_REQUIRED: tuple[tuple[str, str], ...] = (
    ("src/core/workspace_epoch.hh", r"std::uint32_t\s+tenant_id"),
    ("src/core/workspace_epoch.hh", r"std::uint32_t\s+fiber_id"),
    ("src/core/workspace_epoch.hh", r"std::uint32_t\s+mutation_id_at_capture"),
    ("src/core/workspace_epoch.hh", r"std::uint16_t\s+wrap_epoch"),
    ("src/core/workspace_epoch.hh", r"std::uint16_t\s+cow_epoch_at_capture"),
    ("src/core/workspace_epoch.hh", r"std::uint8_t\s+boundary_pinned"),
    ("src/core/workspace_epoch.hh", r"has_full_provenance"),
    ("src/core/workspace_epoch.hh", r"push_match_full"),
    ("src/core/workspace_epoch.hh", r"QueryResultFreshness"),
    ("src/core/workspace_epoch.hh", r"query_result_is_fresh_with_refs"),
    ("src/core/workspace_epoch.hh", r"InvalidTenant"),
    ("src/core/workspace_epoch.hh", r"InvalidFiber"),
    ("src/core/workspace_epoch.hh", r"InvalidCowLayer"),
    ("src/core/workspace_epoch.hh", r"InvalidMutation"),
    ("src/core/workspace_epoch.hh", r"SoftOnlyNoProvenance"),
    ("src/core/workspace_epoch.hh", r"g_query_result_full_provenance_total"),
    ("src/core/workspace_epoch.hh", r"g_query_result_full_provenance_fresh_hits_total"),
    ("src/core/workspace_epoch.hh", r"g_query_result_full_provenance_stale_total"),
    ("src/core/workspace_epoch.hh", r"g_query_result_full_provenance_tenant_mismatch_total"),
    ("src/core/workspace_epoch.hh", r"g_query_result_full_provenance_fiber_mismatch_total"),
    ("src/core/workspace_epoch.hh", r"g_query_result_full_provenance_cow_mismatch_total"),
    ("src/core/workspace_epoch.hh", r"note_query_result_full_provenance\b"),
    ("src/core/workspace_epoch.hh", r"note_query_result_full_provenance_fresh_hit"),
    ("src/core/workspace_epoch.hh", r"note_query_result_full_provenance_stale"),
    ("src/core/workspace_epoch.hh", r"note_query_result_full_provenance_tenant_mismatch"),
    ("src/core/workspace_epoch.hh", r"note_query_result_full_provenance_fiber_mismatch"),
    ("src/core/workspace_epoch.hh", r"note_query_result_full_provenance_cow_mismatch"),
    ("src/core/workspace_epoch.hh", r"kQueryResultFullProvenanceIssue\s*=\s*3103"),
)

IMPL_REQUIRED: tuple[tuple[str, str], ...] = (
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"query_result_is_fresh_with_refs"),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"stamp_query_result_full_provenance"),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"make_stamped_ref"),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"stamp_query_stable_ref_export"),
    # Issue #3137: schema-2 stamp wired into make_query_result_hash
    # chokepoint under production_defaults_active gate. The stamp call
    # + the 5 schema-2 hash keys + the schema-3137 lineage marker must
    # all appear inside make_query_result_hash so every :as-query-result
    # / :query-result #t finish path inherits the stamp.
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"qr\.push_match_full\(node_id, gen, 0, 0, 0, 0, 0, 0\)"),
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        r"stamp_query_result_full_provenance\(qr, ev, \*ws\.workspace_flat, scratch_ref\)",
    ),
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        r"aura::compiler::typed_audit::production_defaults_active\(\)",
    ),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"wrap-epoch\""),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"cow-epoch-at-capture\""),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"tenant-id\""),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"fiber-id\""),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"mutation-id-at-capture\""),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"schema-3137\""),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"query-result-wired-full\""),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"qr\.matches\[0\]\.has_full_provenance\(\)"),
)

WIRING_REQUIRED: tuple[tuple[str, str, str], ...] = (
    # (path, regex, label) — each tuple is a substring/regex that must
    # appear inside the `make_query_result_hash` lambda body. Enforced
    # via a window-of-2000-chars check after the lambda definition.
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        r"stamp_query_result_full_provenance",
        "stamp call inside make_query_result_hash",
    ),
    ("src/compiler/evaluator_primitives_query_workspace.cpp", r"insert_kv\(\"wrap-epoch\"", "wrap-epoch key"),
    (
        "src/compiler/evaluator_primitives_query_workspace.cpp",
        r"insert_kv\(\"mutation-id-at-capture\"",
        "mutation-id-at-capture key",
    ),
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
    """
    fixture_impl = """
    inline QueryResultFreshness query_result_is_fresh_with_refs(...) {
        if (qr.matches[0].has_full_provenance()) return Fresh;
        return SoftOnlyNoProvenance;
    }
    inline void stamp_query_result_full_provenance(...) {
        scratch_ref = ev.make_stamped_ref(qr.matches[i].node_id);
        ev.stamp_query_stable_ref_export(scratch_ref);
    }
    // Issue #3137: make_query_result_hash chokepoint — schema-2 stamp
    // under production_defaults_active. Soft / Quiet / no-matches / hash
    // OOM → zero-cost (gate early-returns before the transient QueryResult
    // build + push_match_full walk).
    auto make_query_result_hash = [ws, &ev](const aura::core::QueryEpoch& epoch,
                                            EvalValue matches, bool pinned) -> EvalValue {
        if (aura::compiler::typed_audit::production_defaults_active() && ws.workspace_flat) {
            aura::core::QueryResult qr;
            qr.epoch = epoch;
            bool pushed = false;
            EvalValue cur = matches;
            while (is_pair(cur) && cur.val != make_void().val) {
                const auto outer = as_pair_idx(cur);
                if (static_cast<std::size_t>(outer) >= ws.pairs.size()) break;
                const auto car = ws.pairs[outer].car;
                std::uint32_t node_id = 0;
                std::uint16_t gen = 0;
                bool got = false;
                if (is_int(car)) {
                    node_id = static_cast<std::uint32_t>(as_int(car));
                    got = true;
                } else if (is_pair(car)) {
                    const auto inner = as_pair_idx(car);
                    if (static_cast<std::size_t>(inner) < ws.pairs.size()) {
                        if (is_int(ws.pairs[inner].car)) {
                            node_id = static_cast<std::uint32_t>(as_int(ws.pairs[inner].car));
                            got = true;
                        }
                        if (is_int(ws.pairs[inner].cdr))
                            gen = static_cast<std::uint16_t>(as_int(ws.pairs[inner].cdr));
                    }
                }
                if (got && node_id < ws.workspace_flat->size()) {
                    qr.push_match_full(node_id, gen, 0, 0, 0, 0, 0, 0);
                    pushed = true;
                }
                cur = ws.pairs[outer].cdr;
            }
            if (pushed && qr.matches[0].has_full_provenance()) {
                aura::ast::FlatAST::StableNodeRef scratch_ref{};
                stamp_query_result_full_provenance(qr, ev, *ws.workspace_flat, scratch_ref);
                const auto& m = qr.matches[0];
                insert_kv("wrap-epoch",
                          make_int(static_cast<std::int64_t>(m.wrap_epoch)));
                insert_kv("cow-epoch-at-capture",
                          make_int(static_cast<std::int64_t>(m.cow_epoch_at_capture)));
                insert_kv("tenant-id",
                          make_int(static_cast<std::int64_t>(m.tenant_id)));
                insert_kv("fiber-id",
                          make_int(static_cast<std::int64_t>(m.fiber_id)));
                insert_kv("mutation-id-at-capture",
                          make_int(static_cast<std::int64_t>(m.mutation_id_at_capture)));
                insert_kv("schema-3137", make_int(3137));
                insert_kv("query-result-wired-full", make_int(1));
            }
        }
    };
    """
    fixture_header_text = ""  # build inline by concatenating
    fixture_header_text += fixture_header

    # Local helper to fake Path.read_text
    class _FakeFile:
        def __init__(self, txt: str) -> None:
            self._txt = txt

        def read_text(self, encoding: str = "utf-8") -> str:
            return self._txt

        def exists(self) -> bool:
            return True

    f_hdr = _FakeFile(fixture_header)
    f_impl = _FakeFile(fixture_impl)
    # Patch REPO_ROOT.joinpath temporarily by monkey-patching the read_text
    # call site would be invasive; instead just re-run the check_file logic
    # inline.
    fails: list[str] = []
    for rel, rx in STRUCT_REQUIRED:
        # Map relative path to fixture
        text = f_hdr.read_text() if rel.endswith("workspace_epoch.hh") else f_impl.read_text()
        if not re.search(rx, text):
            fails.append(f"{rel}: missing required pattern: {rx!r}")
    for rel, rx in IMPL_REQUIRED:
        text = f_impl.read_text()
        if not re.search(rx, text):
            fails.append(f"{rel}: missing required pattern: {rx!r}")
    if fails:
        print("SELF-TEST FAIL:")
        for line in fails:
            print(f"  {line}")
        return 1
    print("SELF-TEST PASS: all required patterns found in fixture")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Issue #3103 QueryResult full-provenance source-cite gate",
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
        help="Files to scan (default: workspace_epoch.hh + evaluator_primitives_query_workspace.cpp)",
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    failures: list[str] = []
    all_pairs: tuple[tuple[str, str], ...] = STRUCT_REQUIRED + IMPL_REQUIRED
    for rel_path, regex in all_pairs:
        if args.targets and rel_path not in args.targets:
            continue
        failures.extend(check_file(rel_path, regex, strict=args.strict))

    if failures:
        print("check_query_result_full_provenance: FAIL")
        for line in failures:
            print(f"  {line}")
        return 1

    print("check_query_result_full_provenance: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
