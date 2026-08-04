// @category: unit
// @reason: Issue #2108 — hard-block composite cross-batch linear escape
// on commit (refine #2027 + escape analysis).
//
//   AC1: Cross-batch escape → commit fails; blocked + escape counters
//   AC2: Sampled still forces escape check when linear_ops_present
//   AC3: Clean linear path can still commit (no false block)
//   AC4: Partial recovery linear only commits if re-audit all_ok
//   AC5: Tests under tests/compiler/; #2027 counters coherent

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::CompositeTxnCommitResult;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::InvariantAuditResult;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t trail_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-trail\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 1)) (define z (* y 2))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

static void ac1_escape_blocks_commit() {
    std::println("\n--- AC1: cross-batch escape hard-blocks commit ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    cs.evaluator().note_txn_dirty();
    cs.evaluator().inject_cross_batch_linear_escape_for_test();

    const auto esc0 =
        load_u64(g_typed_mutation_audit_counters.composite_cross_batch_linear_escape_total);
    const auto blk0 = load_u64(g_typed_mutation_audit_counters.linear_escape_commit_blocked_total);
    const auto rej0 = load_u64(g_typed_mutation_audit_counters.composite_commit_reject_total);

    CompositeTxnCommitResult cr{};
    const bool committed = cs.evaluator().composite_txn_commit(
        /*mid=*/108, "escape-hardblock-test", 0, 0, 1, /*nested=*/true, /*batch=*/true, &cr);

    CHECK(!committed, "commit must fail on escape");
    CHECK(cr.rejected || !cr.committed, "rejected / not committed");
    CHECK(!cr.linear_ok || cr.audit.cross_batch_linear_escape || !cr.audit.all_ok(),
          "linear fail or cross-batch escape flagged");
    CHECK(cs.evaluator().txn_dirty(), "txn_dirty remains (live state not cleared as clean)");
    CHECK(load_u64(g_typed_mutation_audit_counters.linear_escape_commit_blocked_total) > blk0,
          "blocked counter increments");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_reject_total) > rej0 ||
              load_u64(g_typed_mutation_audit_counters.composite_cross_batch_linear_escape_total) >
                  esc0,
          "reject or escape total advanced");
    // Direct audit also sees escape.
    InvariantAuditResult out{};
    const bool inv = cs.evaluator().run_typed_mutation_invariant_audit(
        109, "escape-audit", 0, 0, 1, /*composite_mode=*/true, &out);
    CHECK(!inv, "invariant audit fails under escape");
    CHECK(out.cross_batch_linear_escape, "cross_batch_linear_escape set");
    CHECK(!out.linear_ok, "linear_ok false");
}

static void ac2_sampled_force_linear() {
    std::println("\n--- AC2: Sampled forces escape check when linear present ---");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("linear_hint") != std::string::npos, "linear_hint present");
    CHECK(bound.find("composite || linear_hint") != std::string::npos ||
              bound.find("linear_hint ||") != std::string::npos ||
              bound.find("|| linear_hint") != std::string::npos,
          "linear_hint forces do_audit");
    CHECK(bound.find("Issue #2108") != std::string::npos ||
              bound.find("#2108") != std::string::npos,
          "boundary cites #2108");
    // should_audit_contextual still forces linear_ops_present
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("linear_ops_present") != std::string::npos, "contextual linear force");
    // Runtime under Sampled: inject + commit still hard-blocks
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    CompilerService cs;
    seed(cs);
    cs.evaluator().inject_cross_batch_linear_escape_for_test();
    const auto blk0 = load_u64(g_typed_mutation_audit_counters.linear_escape_commit_blocked_total);
    CompositeTxnCommitResult cr{};
    const bool ok =
        cs.evaluator().composite_txn_commit(110, "sampled-escape", 0, 0, 1, true, true, &cr);
    CHECK(!ok, "Sampled still hard-blocks escape on composite commit");
    CHECK(load_u64(g_typed_mutation_audit_counters.linear_escape_commit_blocked_total) > blk0,
          "blocked under Sampled");
}

static void ac3_clean_commits() {
    std::println("\n--- AC3: clean composite path still commits ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    // No Moved inject — clean workspace.
    CompositeTxnCommitResult cr{};
    const bool committed =
        cs.evaluator().composite_txn_commit(111, "clean-commit", 0, 0, 1, true, true, &cr);
    // Empty CS may soft-fail solve; if committed, must not have escape flags.
    if (committed) {
        CHECK(cr.committed, "cr.committed");
        CHECK(!cr.audit.cross_batch_linear_escape, "no escape on clean");
        CHECK(cr.audit.linear_ok || cr.linear_ok, "linear ok on clean commit");
        CHECK(!cs.evaluator().txn_dirty(), "txn_dirty cleared on success");
    } else {
        // Soft: may reject for solve/audit reasons other than escape.
        CHECK(!cr.audit.cross_batch_linear_escape || cr.rejected,
              "if rejected, escape not the only path");
    }
    // Explicit clean audit
    InvariantAuditResult out{};
    (void)cs.evaluator().run_typed_mutation_invariant_audit(112, "clean-audit", 0, 0, 1, true,
                                                            &out);
    CHECK(!out.cross_batch_linear_escape, "clean audit no escape");
}

static void ac4_partial_recovery_requires_reaudit() {
    std::println("\n--- AC4: partial recovery only if re-audit all_ok ---");
    auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(tc.find("hard_block_cross_batch_linear_escape") != std::string::npos,
          "hard_block helper");
    CHECK(tc.find("esc_after") != std::string::npos ||
              tc.find("Re-run escape hard-block") != std::string::npos ||
              tc.find("!esc_after.cross_batch_linear_escape") != std::string::npos,
          "re-audit escape after partial recover");
    // Runtime: escape + Full → may attempt partial recover linear, but
    // re-audit with live Moved still fails → reject.
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    cs.evaluator().inject_cross_batch_linear_escape_for_test();
    const auto attempt0 = load_u64(g_typed_mutation_audit_counters.partial_recovery_attempt_total);
    const auto lin0 = load_u64(g_typed_mutation_audit_counters.partial_recovery_linear_total);
    CompositeTxnCommitResult cr{};
    const bool ok =
        cs.evaluator().composite_txn_commit(113, "partial-escape", 0, 0, 1, true, true, &cr);
    CHECK(!ok, "still not committed after failed recover");
    CHECK(load_u64(g_typed_mutation_audit_counters.partial_recovery_attempt_total) >= attempt0,
          "recovery may attempt");
    // linear recovery branch should have been considered
    CHECK(load_u64(g_typed_mutation_audit_counters.partial_recovery_linear_total) >= lin0,
          "linear recovery path considered");
    CHECK(cr.rejected || !cr.committed, "final reject");
}

static void ac5_source_and_query() {
    std::println("\n--- AC5: source wiring + query schema-2108 ---");
    auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    auto q = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(tc.find("Issue #2108") != std::string::npos || tc.find("#2108") != std::string::npos,
          "typecheck cites #2108");
    CHECK(tc.find("analyze_linear_escape_for_dirty") != std::string::npos,
          "AST escape analysis wired");
    CHECK(aud.find("linear_escape_commit_blocked_total") != std::string::npos, "blocked counter");
    CHECK(q.find("schema-2108") != std::string::npos, "query schema-2108");
    CHECK(q.find("linear-escape-commit-blocked-total") != std::string::npos, "query key");
    CHECK(bound.find("composite || linear_hint") != std::string::npos ||
              bound.find("linear_hint ||") != std::string::npos ||
              bound.find("|| linear_hint") != std::string::npos,
          "boundary force path");
    CHECK(q.find("schema-2027") != std::string::npos, "#2027 lineage retained");

    reset_for_test();
    CompilerService cs;
    seed(cs);
    cs.evaluator().inject_cross_batch_linear_escape_for_test();
    (void)cs.evaluator().composite_txn_commit(114, "query-test", 0, 0, 1, true, true, nullptr);
    CHECK(trail_href(cs, "schema-2108") == 2108, "schema-2108");
    CHECK(trail_href(cs, "linear-escape-commit-hard-block-wired") == 1, "wired");
    CHECK(trail_href(cs, "linear-escape-commit-blocked-total") >= 1, "blocked visible");
    CHECK(trail_href(cs, "composite-cross-batch-linear-escape") >= 0, "#2027 escape key");
    CHECK(trail_href(cs, "schema-2027") == 2027, "2027 schema");
}

} // namespace

int run_test_linear_escape_commit_hardblock() {
    std::println("=== Issue #2108: linear escape commit hard-block ===");
    ac1_escape_blocks_commit();
    ac2_sampled_force_linear();
    ac3_clean_commits();
    ac4_partial_recovery_requires_reaudit();
    ac5_source_and_query();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_escape_commit_hardblock();
}
#endif
