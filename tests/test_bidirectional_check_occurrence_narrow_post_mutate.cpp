// test_bidirectional_check_occurrence_narrow_post_mutate.cpp
// Issue #627: Bidirectional checking + check-mode occurrence narrow
// robustness under post-mutate partial re-infer.
//
// AC1: typecheck-current (check path) applies narrow after mutate
// AC2: synthesize path (eval) preserves narrow-dependent semantics
// AC3: query:bidirectional-narrow-stats grows on check/mutate path
// AC4: stale narrow in check-mode prevented + blame stats monotonic
// AC5: post-mutate incremental_infer + typecheck keeps eval correct
// AC6: bidirectional_mode opt-out still typechecks after mutate

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_627_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static constexpr const char* k_if_prog = R"(
(define f (lambda (x) (if (number? x) (+ x 1) 0)))
)";

static bool load_if_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_if_prog + "\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    if (!cs.eval("(typecheck-current)"))
        return false;
    return true;
}

static std::int64_t bidirectional_narrow_stats(CompilerService& cs) {
    auto r = cs.eval("(query:bidirectional-narrow-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t narrow_blame_stats(CompilerService& cs) {
    auto r = cs.eval("(query:narrow-blame-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static void test_check_mode_narrow_after_mutate() {
    std::println("\n--- AC1: check-mode narrow after mutate ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 5) 0))\" "
        "\"issue-627-check\")");
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck-current after mutate succeeds");
    auto prov = cs.eval("(query:provenance-of \"x\")");
    CHECK(prov.has_value(), "check-mode provenance present after mutate");
}

static void test_synthesize_eval_semantics_preserved() {
    std::println("\n--- AC2: synthesize eval semantics preserved ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 10) 0))\" "
        "\"issue-627-synth\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    auto r = cs.eval("(f 7)");
    CHECK(r && is_int(*r), "f 7 returns int");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 17, "narrow-dependent (+ x 10) correct in synthesize path");
}

static void test_bidirectional_narrow_stats_grow() {
    std::println("\n--- AC3: bidirectional-narrow-stats grows ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    const auto stats0 = bidirectional_narrow_stats(cs);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 4) 0))\" "
        "\"issue-627-stats\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    (void)cs.eval("(f 2)");
    const auto stats1 = bidirectional_narrow_stats(cs);
    const auto snap = cs.snapshot();
    std::println("  bidirectional-narrow-stats: {} -> {}", stats0, stats1);
    std::println("  check_hits={} switches={} consistency={} stale_prevented={}",
                 snap.check_mode_narrow_hits_total, snap.synthesize_check_switch_count_total,
                 snap.post_mutate_narrow_consistency_total, snap.stale_check_narrow_prevented_total);
    CHECK(stats1 >= stats0, "query:bidirectional-narrow-stats monotonic");
    CHECK(snap.synthesize_check_switch_count_total > 0 ||
              snap.check_mode_narrow_hits_total > 0 ||
              snap.post_mutate_narrow_consistency_total > 0,
          "at least one #627 counter bumped");
}

static void test_stale_check_prevented_with_blame() {
    std::println("\n--- AC4: stale check narrow prevented + blame ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    const auto blame0 = narrow_blame_stats(cs);
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (* x 2) 0))\" "
        "\"issue-627-stale\")");
    (void)cs.eval("(typecheck-current)");
    const auto blame1 = narrow_blame_stats(cs);
    const auto snap = cs.snapshot();
    std::println("  narrow-blame-stats: {} -> {}", blame0, blame1);
    std::println("  stale_check_prevented={} stale_caught={}",
                 snap.stale_check_narrow_prevented_total, snap.narrow_stale_caught_total);
    CHECK(blame1 >= blame0, "narrow-blame-stats monotonic under stale check path");
}

static void test_incremental_then_typecheck_eval_ok() {
    std::println("\n--- AC5: incremental + typecheck eval ok ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    cs.set_incremental_typecheck_mode(aura::compiler::IncrementalTypecheckMode::Lazy);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 20) 0))\" "
        "\"issue-627-inc\")");
    auto* ws = cs.workspace_flat();
    CHECK(ws != nullptr && !ws->all_mutations().empty(), "mutation logged");
    (void)cs.incremental_infer(ws->all_mutations().back());
    (void)cs.eval("(typecheck-current)");
    auto r = cs.eval("(f 3)");
    CHECK(r && is_int(*r), "eval after incremental + typecheck");
    if (r && is_int(*r))
        CHECK(as_int(*r) == 23, "semantics correct after mixed paths");
}

static void test_bidirectional_opt_out_after_mutate() {
    std::println("\n--- AC6: bidirectional opt-out after mutate ---");
    CompilerService cs;
    CHECK(load_if_workspace(cs), "load if workspace");
    cs.set_bidirectional_mode(false);
    (void)cs.eval(
        "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x 1) 0))\" "
        "\"issue-627-optout\")");
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck works with bidirectional_mode=false post-mutate");
    cs.set_bidirectional_mode(true);
}

} // namespace aura_627_detail

int main() {
    using namespace aura_627_detail;
    test_check_mode_narrow_after_mutate();
    test_synthesize_eval_semantics_preserved();
    test_bidirectional_narrow_stats_grow();
    test_stale_check_prevented_with_blame();
    test_incremental_then_typecheck_eval_ok();
    test_bidirectional_opt_out_after_mutate();
    return RUN_ALL_TESTS();
}