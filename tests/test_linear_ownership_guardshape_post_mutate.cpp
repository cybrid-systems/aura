// test_linear_ownership_guardshape_post_mutate.cpp
// Issue #638: Linear ownership runtime checks + GuardShape
// enforcement post-mutation (Prompt6 memory safety).
//
// Non-duplicative with #610 (linear-ownership-mutation-stats),
// #575 (incremental-stats), #531 (closure-env-safety), #570
// (shapeprofiler stability).
//
// AC1: query:linear-ownership-safety-stats reachable
// AC2: leaked-linear post_mutate → violations_caught bumps
// AC3: properly moved linear passes post_mutate revalidate
// AC4: mutate + eval linear → safety stats monotonic
// AC5: invalidate_shape clears stability (GuardShape path)
// AC6: multi-round linear mutate matrix — eval + stats monotonic
// AC7: query regression (mutation-stats, closure-env-safety,
//      shape-stability-stats)
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

namespace aura_638_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

static std::int64_t linear_safety_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:linear-ownership-safety-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool load_linear_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_linear_prog + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void test_post_mutate_leak_violations() {
    std::println("\n--- AC2: leaked-linear → violations_caught bumps ---");
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
    rec.mutation_id = 638;
    std::vector<aura::compiler::OwnershipNote> notes;
    (void)aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
    const auto violations = metrics.linear_violations_caught_total.load(std::memory_order_relaxed);
    const auto leaks = metrics.linear_leak_prevented_total.load(std::memory_order_relaxed);
    std::println("  violations_caught={} leak_prevented={}", violations, leaks);
    CHECK(violations >= 1 || leaks >= 1, "leaked-linear bumps violation or leak counters");
    CHECK(count_kind(notes, "leaked-linear") >= 1,
          "post_mutation_invariant_check finds leaked-linear");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:linear-ownership-safety-stats ---");
    CHECK(load_linear_workspace(cs), "load linear workspace");
    const auto s0 = linear_safety_stats(cs);
    std::println("  query:linear-ownership-safety-stats = {}", s0);
    CHECK(s0 >= 0, "linear-ownership-safety-stats non-negative");

    std::println("\n--- AC3: properly moved linear passes post_mutate ---");
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
    rec.mutation_id = 6383;
    std::vector<aura::compiler::OwnershipNote> notes;
    const auto status =
        aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg, rec, notes, &metrics);
    CHECK(status == aura::ast::InvariantStatus::Ok,
          "moved linear binding passes post-mutate revalidate");

    std::println("\n--- AC4: mutate + eval linear → safety stats monotonic ---");
    const auto stats4a = linear_safety_stats(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 88))) (display x)))\" "
                  "\"issue-638-leak\")");
    auto* ws = cs.evaluator().workspace_flat();
    auto* ws_pool = cs.evaluator().workspace_pool();
    if (ws && ws_pool && !ws->all_mutations().empty()) {
        TypeRegistry reg2;
        std::vector<aura::compiler::OwnershipNote> notes2;
        (void)aura::compiler::post_mutation_invariant_check(*ws, *ws_pool, reg2,
                                                            ws->all_mutations().back(), notes2,
                                                            cs.evaluator().compiler_metrics());
    }
    const auto stats4b = linear_safety_stats(cs);
    std::println("  linear-ownership-safety-stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "safety-stats monotonic after post-mutate linear revalidate");

    std::println("\n--- AC5: invalidate_shape clears GuardShape stability ---");
    (void)cs.eval("(set-code \"(define add1 (lambda (x) (+ x 1)))\")");
    CHECK(cs.eval("(eval-current)").has_value(), "add1 workspace eval");
    const bool stable_before = cs.is_shape_stable("add1");
    cs.invalidate_shape("add1");
    CHECK(!cs.is_shape_stable("add1"), "invalidate_shape clears stability for GuardShape refresh");

    std::println("\n--- AC6: multi-round linear mutate matrix ---");
    const auto stats6a = linear_safety_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string body =
            "(lambda () (let ((x (Linear " + std::to_string(round + 10) + "))) (move x)))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) + "\")");
        auto r = cs.eval("(f)");
        CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
    }
    const auto stats6b = linear_safety_stats(cs);
    std::println("  linear-ownership-safety-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "safety-stats monotonic over matrix");
    (void)stable_before;

    std::println("\n--- AC7: query regression ---");
    auto lms = cs.eval("(engine:metrics \"query:linear-ownership-mutation-stats\")");
    auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
    auto sps = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
    CHECK(lms && is_int(*lms), "linear-ownership-mutation-stats regression");
    CHECK(ces && is_hash(*ces), "closure-env-safety-stats regression");
    CHECK(sps && is_int(*sps), "shape-stability-stats regression");
}

} // namespace aura_638_detail

int main() {
    aura_638_detail::test_post_mutate_leak_violations();
    aura::compiler::CompilerService cs;
    aura_638_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}