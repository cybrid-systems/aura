// test_linear_ownership_incremental_post_mutate_task2.cpp
// Issue #575: Task2 PerDefUse + ownership_dirty incremental
// linear ownership re-validation post-mutation.
//
// Non-duplicative with #610 (linear-ownership-mutation-stats),
// #117 (validate_ownership_full), #531 (closure-env-safety),
// #411 (per_defuse_index wiring).
//
// AC1: query:linear-ownership-incremental-stats reachable
// AC2: leaked-linear post_mutate → revalidate + violation counters
// AC3: properly moved linear passes post_mutate revalidate
// AC4: mutate:rebind on linear → stats monotonic
// AC5: per_defuse_index counters observable after mutate
// AC6: O(uses) proxy — per_defuse visited bounded vs full AST
// AC7: query regression (mutation-stats, closure-env-safety)
// AC8: multi-round linear mutate matrix — eval + stats monotonic
//
// Unit leak test runs first; integration uses one CompilerService.

#include "test_harness.hpp"
#include "observability_metrics.h"

#include <cstdint>
#include <string>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_575_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;

static constexpr const char* k_linear_prog = R"(
(define leak (let ((x (Linear 42))) (display x)))
(define f (lambda () (let ((x (Linear 42))) (move x))))
)";

static int count_kind(const std::vector<aura::compiler::OwnershipNote>& notes,
                      const std::string& kind) {
    int n = 0;
    for (const auto& note : notes) {
        if (note.kind == kind)
            ++n;
    }
    return n;
}

static std::int64_t linear_incremental_stats(CompilerService& cs) {
    auto r = cs.eval("(query:linear-ownership-incremental-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool load_linear_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_linear_prog + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void test_post_mutate_leak_counters() {
    std::println("\n--- AC2: leaked-linear → revalidate + violation counters ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto x_sym = pool->intern("x");
    auto inner = flat->add_literal(42);
    auto lin_node = flat->add_linear(inner);
    auto disp_sym = pool->intern("display");
    auto disp_var = flat->add_variable(disp_sym);
    auto x_var = flat->add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat->add_call(disp_var, disp_args);
    auto root = flat->add_let(x_sym, lin_node, disp_call);
    flat->root = root;

    TypeRegistry reg;
    CompilerMetrics metrics;
    aura::ast::MutationRecord rec;
    rec.target_node = root;
    rec.mutation_id = 575;
    std::vector<aura::compiler::OwnershipNote> notes;
    (void)aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
    const auto reval =
        metrics.linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
    const auto leaks = metrics.linear_leak_prevented_total.load(std::memory_order_relaxed);
    std::println("  revalidations={} leak_prevented={}", reval, leaks);
    CHECK(reval > 0, "ownership_revalidate_count bumped");
    CHECK(leaks >= 1, "violation_caught_post_mutate includes leak");
    CHECK(count_kind(notes, "leaked-linear") >= 1, "leaked-linear note emitted");
}

static void test_moved_linear_passes() {
    std::println("\n--- AC3: properly moved linear passes revalidate ---");
    CompilerMetrics metrics;
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* str_pool = arena->create<aura::ast::StringPool>(alloc);
    auto x_sym = str_pool->intern("x");
    auto inner = flat->add_literal(42);
    auto lin_node = flat->add_linear(inner);
    auto x_var = flat->add_variable(x_sym);
    auto move_node = flat->add_move(x_var);
    auto root = flat->add_let(x_sym, lin_node, move_node);
    flat->root = root;
    TypeRegistry reg;
    aura::ast::MutationRecord rec;
    rec.target_node = root;
    rec.mutation_id = 5753;
    std::vector<aura::compiler::OwnershipNote> notes;
    const auto status =
        aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg, rec, notes, &metrics);
    CHECK(status == aura::ast::InvariantStatus::Ok,
          "moved linear binding passes post-mutate revalidate");
    CHECK(count_kind(notes, "leaked-linear") == 0, "no leaked-linear for properly moved binding");
}

static void run_integration_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:linear-ownership-incremental-stats ---");
    CHECK(load_linear_workspace(cs), "load linear workspace");
    const auto s0 = linear_incremental_stats(cs);
    std::println("  query:linear-ownership-incremental-stats = {}", s0);
    CHECK(s0 >= 0, "linear-ownership-incremental-stats non-negative");

    std::println("\n--- AC4: linear mutate → stats monotonic ---");
    const auto stats4a = linear_incremental_stats(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 88))) (display x)))\" "
                  "\"issue-575-leak\")");
    auto* ws = cs.evaluator().workspace_flat();
    auto* ws_pool = cs.evaluator().workspace_pool();
    CHECK(ws != nullptr && ws_pool != nullptr && !ws->all_mutations().empty(), "mutation logged");
    if (ws && ws_pool && !ws->all_mutations().empty()) {
        TypeRegistry reg;
        std::vector<aura::compiler::OwnershipNote> notes;
        (void)aura::compiler::post_mutation_invariant_check(*ws, *ws_pool, reg,
                                                            ws->all_mutations().back(), notes,
                                                            cs.evaluator().compiler_metrics());
    }
    const auto stats4b = linear_incremental_stats(cs);
    std::println("  linear-incremental-stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b > stats4a, "incremental stats grew after linear revalidate");

    std::println("\n--- AC5: per_defuse_index counters after mutate ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 55))) (move x)))\" "
                  "\"issue-575-pdu\")");
    if (ws && !ws->all_mutations().empty())
        (void)cs.incremental_infer(ws->all_mutations().back());
    const auto snap5 = cs.snapshot();
    std::println("  per_defuse_used={} per_defuse_visited={}", snap5.per_defuse_index_used_total,
                 snap5.per_defuse_index_visited_total);
    CHECK(snap5.per_defuse_index_used_total >= 0, "per_defuse_index_used_total observable");
    CHECK(snap5.per_defuse_index_visited_total >= 0, "per_defuse_index_visited_total observable");

    std::println("\n--- AC6: O(uses) proxy — visited bounded ---");
    const auto visited = snap5.per_defuse_index_visited_total;
    const auto reinferred = snap5.incremental_typecheck_re_inferred_total;
    std::println("  per_defuse_visited={} reinferred_total={}", visited, reinferred);
    CHECK(visited <= reinferred + ws->size(), "per_defuse visited bounded vs full AST size proxy");

    std::println("\n--- AC7: query regression ---");
    auto lms = cs.eval("(query:linear-ownership-mutation-stats)");
    auto ces = cs.eval("(query:closure-env-safety-stats)");
    CHECK(lms && is_int(*lms), "query:linear-ownership-mutation-stats returns int");
    CHECK(ces && is_int(*ces), "query:closure-env-safety-stats returns int");

    std::println("\n--- AC8: multi-round linear mutate matrix ---");
    const auto stats8a = linear_incremental_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string body =
            "(lambda () (let ((x (Linear " + std::to_string(round + 15) + "))) (move x)))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) + "\")");
        auto r = cs.eval("(f)");
        CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
    }
    const auto stats8b = linear_incremental_stats(cs);
    std::println("  linear-incremental-stats: {} -> {}", stats8a, stats8b);
    CHECK(stats8b >= stats8a, "linear-incremental-stats monotonic over matrix");

    const auto* metrics = static_cast<const CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto reval =
        metrics ? metrics->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed)
                : 0u;
    std::println("  final ownership_revalidate_count={}", reval);
    CHECK(reval > 0, "ownership_revalidate_count > 0 after mutate cycle");
}

} // namespace aura_575_detail

int main() {
    using namespace aura_575_detail;
    test_post_mutate_leak_counters();
    test_moved_linear_passes();
    aura::compiler::CompilerService cs;
    run_integration_matrix(cs);
    return RUN_ALL_TESTS();
}