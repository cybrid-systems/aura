// test_occurrence_mutate_narrowing.cpp — Issue #518 P0 Phase 1:
// Post-Mutation Occurrence Typing Re-Narrowing.
//
// Non-duplicative with #550/#555 (Guard stub counters),
// #434 (dirty recovery plumbing), #536/#537 (dirty +
// occurrence infra). Validates infer_flat_partial wires
// reanalyze_occurrence_contexts + propagate_narrowing_to_uses.
//
//   AC1: reanalyze_occurrence_contexts reachable via
//        incremental_infer after mutate on if-context
//   AC2: narrowing_dirty_recovery + narrowing_reanalyzed
//        bump when kOccurrenceDirty If is in scope
//   AC3: occurrence stale bit cleared after re-narrow
//   AC4: multi-round mutate + if-predicate still typechecks
//   AC5: query:occurrence-stale-count decreases after re-narrow
//   AC6: regression — existing eval still works

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_518_detail {

using aura::compiler::CompilerService;

static constexpr const char* k_if_prog = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
)";

static bool load_if_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_if_prog + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    (void)cs.typecheck("(define f (lambda (x) (if (number? x) (+ x 1) 0)))");
    return true;
}

// ── AC1: incremental_infer reaches re-narrow path ────────
bool test_reanalyze_via_incremental_infer() {
    std::println("\n--- AC1: reanalyze via incremental_infer after mutate ---");
    CompilerService cs;
    if (!load_if_workspace(cs)) {
        CHECK(false, "load if workspace");
        return false;
    }
    const auto n0 = cs.evaluator().get_narrowing_refresh_count();
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 2) 0))\" "
        "\"issue-518\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(),
          "workspace has mutation after rebind");
    if (!ws || ws->all_mutations().empty())
        return false;
    const auto reinferred = cs.incremental_infer(ws->all_mutations().back());
    const auto n1 = cs.evaluator().get_narrowing_refresh_count();
    std::println("  reinferred={} narrowing_refresh: {} -> {}",
                 reinferred, n0, n1);
    CHECK(reinferred >= 0, "incremental_infer returns count");
    CHECK(n1 > n0,
          "narrowing_refresh bumped on actual re-narrow path (#518)");
    return true;
}

// ── AC2: dirty recovery + reanalyzed counters bump ───────
bool test_dirty_recovery_counters_bump() {
    std::println("\n--- AC2: narrowing_dirty_recovery + reanalyzed bump ---");
    CompilerService cs;
    if (!load_if_workspace(cs)) {
        CHECK(false, "load if workspace");
        return false;
    }
    const auto snap0 = cs.snapshot();
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (* x 3) 0))\" "
        "\"issue-518-dirty\")");
    auto* ws = cs.workspace_flat();
    if (!ws || ws->all_mutations().empty()) {
        CHECK(false, "mutation log non-empty");
        return false;
    }
    (void)cs.incremental_infer(ws->all_mutations().back());
    const auto snap1 = cs.snapshot();
    std::println("  dirty_recovery: {} -> {}",
                 snap0.narrowing_dirty_recovery_total,
                 snap1.narrowing_dirty_recovery_total);
    std::println("  reanalyzed: {} -> {}",
                 snap0.narrowing_reanalyzed_total,
                 snap1.narrowing_reanalyzed_total);
    CHECK(snap1.narrowing_dirty_recovery_total >
              snap0.narrowing_dirty_recovery_total,
          "narrowing_dirty_recovery_total bumped after re-narrow");
    CHECK(snap1.narrowing_reanalyzed_total > snap0.narrowing_reanalyzed_total,
          "narrowing_reanalyzed_total bumped after re-narrow");
    return true;
}

// ── AC3: occurrence stale cleared after re-narrow ───────
bool test_occurrence_stale_cleared() {
    std::println("\n--- AC3: occurrence stale cleared after re-narrow ---");
    CompilerService cs;
    if (!load_if_workspace(cs)) {
        CHECK(false, "load if workspace");
        return false;
    }
    // Lazy mode: defer auto-invoke so stale bits survive until
    // we explicitly call incremental_infer (#518).
    cs.set_incremental_typecheck_mode(
        aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 4) 0))\" "
        "\"issue-518-stale\")");
    auto stale_after_mutate = cs.eval("(query:occurrence-stale-count)");
    CHECK(stale_after_mutate.has_value() &&
              aura::compiler::types::is_int(*stale_after_mutate),
          "(query:occurrence-stale-count) after mutate");
    const auto stale_before =
        stale_after_mutate && aura::compiler::types::is_int(*stale_after_mutate)
            ? aura::compiler::types::as_int(*stale_after_mutate)
            : 0;
    std::println("  stale_count after mutate: {}", stale_before);
    CHECK(stale_before > 0,
          "occurrence-stale-count > 0 after mutate marks if-context");

    auto* ws = cs.workspace_flat();
    if (!ws || ws->all_mutations().empty()) {
        CHECK(false, "mutation log non-empty");
        return false;
    }
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto stale_after_renarrow = cs.eval("(query:occurrence-stale-count)");
    CHECK(stale_after_renarrow.has_value() &&
              aura::compiler::types::is_int(*stale_after_renarrow),
          "(query:occurrence-stale-count) after re-narrow");
    const auto stale_after =
        stale_after_renarrow && aura::compiler::types::is_int(*stale_after_renarrow)
            ? aura::compiler::types::as_int(*stale_after_renarrow)
            : stale_before;
    std::println("  stale_count after re-narrow: {}", stale_after);
    CHECK(stale_after == 0,
          "occurrence-stale-count == 0 after re-narrow");
    return true;
}

// ── AC4: multi-round mutate + typecheck still ok ─────────
bool test_multi_round_mutate_typechecks() {
    std::println("\n--- AC4: multi-round mutate + if typechecks ---");
    CompilerService cs;
    if (!load_if_workspace(cs)) {
        CHECK(false, "load if workspace");
        return false;
    }
    for (int round = 0; round < 3; ++round) {
        const std::string body =
            "(lambda (x) (if (number? x) (+ x " + std::to_string(round + 5) +
            ") 0))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"round-" +
                      std::to_string(round) + "\")");
        auto* ws = cs.workspace_flat();
        if (!ws || ws->all_mutations().empty()) {
            CHECK(false, "mutation log non-empty in multi-round");
            return false;
        }
        (void)cs.incremental_infer(ws->all_mutations().back());
        const auto tc = cs.typecheck(body);
        CHECK(!tc.empty(),
              "typecheck succeeds after round " + std::to_string(round));
    }
    return true;
}

// ── AC5: occurrence-stale-count decreases ────────────────
bool test_occurrence_stale_count_decreases() {
    std::println("\n--- AC5: occurrence-stale-count decreases ---");
    CompilerService cs;
    if (!load_if_workspace(cs)) {
        CHECK(false, "load if workspace");
        return false;
    }
    cs.set_incremental_typecheck_mode(
        aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (- x 1) 0))\" "
        "\"issue-518-dec\")");
    auto before = cs.eval("(query:occurrence-stale-count)");
    const auto count_before =
        before && aura::compiler::types::is_int(*before)
            ? aura::compiler::types::as_int(*before)
            : 0;
    auto* ws = cs.workspace_flat();
    if (!ws || ws->all_mutations().empty()) {
        CHECK(false, "mutation log non-empty");
        return false;
    }
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto after = cs.eval("(query:occurrence-stale-count)");
    const auto count_after =
        after && aura::compiler::types::is_int(*after)
            ? aura::compiler::types::as_int(*after)
            : count_before;
    std::println("  stale_count: {} -> {}", count_before, count_after);
    CHECK(count_after <= count_before,
          "occurrence-stale-count decreases or stays 0");
    CHECK(count_after == 0,
          "occurrence-stale-count is 0 after re-narrow");
    return true;
}

// ── AC6: regression — eval still works ───────────────────
bool test_regression_eval_works() {
    std::println("\n--- AC6: regression — eval still works ---");
    CompilerService cs;
    (void)cs.eval("(set-code \"(define g 99)\")");
    (void)cs.eval("(eval-current)");
    auto r = cs.eval("(eval-current)");
    CHECK(r.has_value() && aura::compiler::types::is_int(*r) &&
              aura::compiler::types::as_int(*r) == 99,
          "(eval-current) returns 99 after occurrence re-narrow wiring");
    return true;
}

int run_tests() {
    std::println("═══ Issue #518 P0 Phase 1: occurrence mutate narrowing ═══\n");
    test_reanalyze_via_incremental_infer();
    test_dirty_recovery_counters_bump();
    test_occurrence_stale_cleared();
    test_multi_round_mutate_typechecks();
    test_occurrence_stale_count_decreases();
    test_regression_eval_works();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_518_detail

int aura_issue_518_run() { return aura_issue_518_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_518_run(); }
#endif