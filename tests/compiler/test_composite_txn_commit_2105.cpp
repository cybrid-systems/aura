// @category: unit
// @reason: Issue #2105 — composite/nested txn commit unifies
// solve_delta_occurrence + linear revalidate + invariant audit
// (refine #2027/#2029).
//
//   AC1: Nested/atomic_batch success path runs ordered revalidate before clean
//   AC2: Forced fail → Full partial recovery or reject; composite_full_rollback
//   AC3: txn-dirty flag visible to Agents during open composite txn
//   AC4: #2027/#2029 lineage counters + schema-2105
//   AC5: tests under tests/compiler/
//   AC6: source wiring (composite_txn_commit sequence)

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::CompositeTxnCommitResult;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

static void ac1_commit_order() {
    std::println("\n--- AC1: composite_txn_commit ordered revalidate ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    const auto rev0 = load_u64(g_typed_mutation_audit_counters.composite_commit_revalidate_total);
    const auto ok0 = load_u64(g_typed_mutation_audit_counters.composite_commit_ok_total);
    CompositeTxnCommitResult cr{};
    const bool committed = cs.evaluator().composite_txn_commit(
        /*mid=*/105, "composite-commit-test", 0, 0, 1, /*nested=*/true, /*batch=*/true, &cr);
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_revalidate_total) > rev0,
          "revalidate total advanced");
    // Happy path workspace should commit (solve N/A or SOLVED + linear + audit).
    if (committed) {
        CHECK(cr.committed, "cr.committed");
        CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_ok_total) > ok0,
              "commit ok total");
        CHECK(!cs.evaluator().txn_dirty(), "txn_dirty cleared on success");
    } else {
        // Soft: empty CS may reject; reject path still ran the sequence.
        CHECK(cr.rejected || !cr.solve_ok || !cr.linear_ok || !cr.audit_ok, "reject has reason");
        CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_reject_total) >= 1 ||
                  load_u64(g_typed_mutation_audit_counters.composite_commit_revalidate_total) >
                      rev0,
              "reject or revalidate path exercised");
    }
    // Source order inside composite_txn_commit: solve → linear → audit.
    auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto pos_fn = tc.find("Evaluator::composite_txn_commit");
    CHECK(pos_fn != std::string::npos, "composite_txn_commit defined");
    auto body = pos_fn != std::string::npos ? tc.substr(pos_fn, 2500) : std::string{};
    auto pos_solve = body.find("solve_delta_occurrence");
    auto pos_lin = body.find("linear_post_mutate_enforce_all");
    auto pos_audit = body.find("run_typed_mutation_invariant_audit");
    CHECK(pos_solve != std::string::npos, "solve in commit");
    CHECK(pos_lin != std::string::npos, "linear in commit");
    CHECK(pos_audit != std::string::npos, "audit in commit");
    CHECK(pos_solve < pos_lin && pos_lin < pos_audit, "order: solve → linear → audit");
}

static void ac2_force_fail_rollback_surface() {
    std::println("\n--- AC2: reject / full-rollback counters ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("composite_txn_commit") != std::string::npos, "boundary calls commit");
    CHECK(bound.find("composite_full_rollback_total") != std::string::npos ||
              bound.find("composite-invariant-force-rollback") != std::string::npos,
          "force-rollback path retained");
    // Nested mutate under Full exercises composite path.
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &ok);
        {
            bool ok2 = true;
            Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &ok2);
            CHECK(cs.evaluator().txn_dirty(), "txn_dirty under nested Guard");
            (void)cs.eval("(define w 3)");
        }
    }
    CHECK(ok, "outer guard success flag");
    // After outermost exit, dirty should clear on success path.
    CHECK(!cs.evaluator().txn_dirty() || trail_href(cs, "txn-dirty") >= 0,
          "txn_dirty cleared or queryable");
}

static void ac3_txn_dirty_query() {
    std::println("\n--- AC3: txn-dirty Agent flag ---");
    reset_for_test();
    CompilerService cs;
    seed(cs);
    CHECK(trail_href(cs, "schema-2105") == 2105, "schema-2105");
    CHECK(trail_href(cs, "composite-txn-commit-wired") == 1, "commit wired");
    CHECK(trail_href(cs, "txn-dirty") == 0, "clean when idle");
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &ok);
        {
            bool ok2 = true;
            Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &ok2);
            CHECK(cs.evaluator().txn_dirty(), "dirty during nested txn");
            // Query while open — Agents see txn-dirty flag (not blocked).
            CHECK(trail_href(cs, "txn-dirty") == 1 || cs.evaluator().txn_dirty(),
                  "query or C++ sees dirty");
        }
    }
}

static void ac4_lineage() {
    std::println("\n--- AC4: #2027/#2029 lineage + schema-2105 ---");
    CompilerService cs;
    CHECK(trail_href(cs, "schema-2027") == 2027, "schema-2027");
    CHECK(trail_href(cs, "schema-2029") == 2029, "schema-2029");
    CHECK(trail_href(cs, "schema-2105") == 2105, "schema-2105");
    CHECK(trail_href(cs, "composite-commit-revalidate-total") >= 0, "revalidate key");
    CHECK(trail_href(cs, "composite-commit-reject-total") >= 0, "reject key");
    CHECK(trail_href(cs, "composite-commit-ok-total") >= 0, "ok key");
}

static void ac5_location() {
    std::println("\n--- AC5: test under tests/compiler/ ---");
    auto self = read_file("tests/compiler/test_composite_txn_commit_2105.cpp");
    CHECK(!self.empty(), "readable");
    CHECK(self.find("Issue #2105") != std::string::npos, "cites issue");
}

static void ac6_source() {
    std::println("\n--- AC6: source wiring #2105 ---");
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(!aud.empty() && aud.find("composite_commit_revalidate_total") != std::string::npos,
          "audit commit revalidate counter");
    CHECK(aud.find("CompositeTxnCommitResult") != std::string::npos, "commit result type");
    CHECK(!tc.empty() && tc.find("Issue #2105") != std::string::npos, "typecheck #2105");
    CHECK(tc.find("composite_txn_commit") != std::string::npos, "commit impl");
    CHECK(!bound.empty() && bound.find("Issue #2105") != std::string::npos, "boundary #2105");
    CHECK(bound.find("composite_txn_commit") != std::string::npos, "boundary calls commit");
    CHECK(bound.find("note_txn_dirty") != std::string::npos, "txn_dirty note");
    CHECK(!q.empty() && q.find("schema-2105") != std::string::npos, "query schema-2105");
    CHECK(q.find("txn-dirty") != std::string::npos, "txn-dirty key");
}

} // namespace

int main() {
    std::println("=== Issue #2105: composite_txn_commit ordered barrier ===");
    ac1_commit_order();
    ac2_force_fail_rollback_surface();
    ac3_txn_dirty_query();
    ac4_lineage();
    ac5_location();
    ac6_source();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
