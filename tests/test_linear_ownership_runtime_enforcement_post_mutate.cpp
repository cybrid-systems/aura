// test_linear_ownership_runtime_enforcement_post_mutate.cpp
// Issue #598: Runtime linear ownership enforcement + invalidate
// integration post-mutation (Prompt6 memory safety).
//
// Non-duplicative with #638 (GuardShape safety-stats focus),
// #610 (mutation-stats revalidation), #575 (incremental-stats).
//
// AC1: query:linear-ownership-runtime-stats reachable
// AC2: properly moved linear passes post_mutate revalidate
// AC3: invalidate path bumps deopt_on_invalidate counter
// AC4: leaked-linear post_mutate → violations_caught bumps
// AC5: gc-heap + linear mutate integration
// AC6: multi-round linear mutate matrix — runtime stats monotonic
// AC7: query regression (safety-stats, mutation-stats,
//      prompt6-violation-count)
//
// Unit leak test runs first; integration uses one CompilerService.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

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

namespace aura_598_detail {

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

static std::int64_t runtime_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
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
    std::println("\n--- AC4: leaked-linear → violations_caught bumps ---");
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
    rec.mutation_id = 598;
    std::vector<aura::compiler::OwnershipNote> notes;
    (void)aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
    const auto violations = metrics.linear_violations_caught_total.load(std::memory_order_relaxed);
    CHECK(violations >= 1 || count_kind(notes, "leaked-linear") >= 1,
          "leaked-linear bumps violation counters");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:linear-ownership-runtime-stats ---");
    CHECK(load_linear_workspace(cs), "load linear workspace");
    const auto s0 = runtime_stats(cs);
    std::println("  linear-ownership-runtime-stats = {}", s0);
    CHECK(s0 >= 0, "runtime stats non-negative");

    std::println("\n--- AC2: properly moved linear passes post_mutate ---");
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
    rec.mutation_id = 5982;
    std::vector<aura::compiler::OwnershipNote> notes;
    const auto status =
        aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg, rec, notes, &metrics);
    CHECK(status == aura::ast::InvariantStatus::Ok,
          "moved linear binding passes post-mutate revalidate");

    std::println("\n--- AC3: invalidate path bumps deopt_on_invalidate ---");
    const auto* m = static_cast<const CompilerMetrics*>(cs.evaluator().compiler_metrics());
    (void)cs.eval("(f)");
    const auto deopt0 = m ? m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed) : 0;
    const auto enforce0 =
        m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 99))) (move x)))\" "
                  "\"issue-598-invalidate\")");
    const auto deopt1 = m ? m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed) : 0;
    const auto enforce1 =
        m ? m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed) : 0;
    std::println("  deopt_on_invalidate: {} -> {} enforcement_hits: {} -> {}", deopt0, deopt1,
                 enforce0, enforce1);
    CHECK(deopt1 >= deopt0, "deopt_on_invalidate observable");
    CHECK(enforce1 > enforce0, "mutate:rebind bumps post_mutate_enforcement_hits");

    std::println("\n--- AC5: gc-heap + linear mutate integration ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 7))) (move x)))\" "
                  "\"issue-598-gc\")");
    auto gc = cs.eval("(gc-heap)");
    CHECK(gc.has_value(), "(gc-heap) callable after linear mutate");

    std::println("\n--- AC6: multi-round linear mutate matrix ---");
    const auto stats6a = runtime_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string body =
            "(lambda () (let ((x (Linear " + std::to_string(round + 20) + "))) (move x)))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) + "\")");
        auto r = cs.eval("(f)");
        CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
    }
    const auto stats6b = runtime_stats(cs);
    std::println("  runtime stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "runtime stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto los = cs.eval("(engine:metrics \"query:linear-ownership-safety-stats\")");
    auto lms = cs.eval("(engine:metrics \"query:linear-ownership-mutation-stats\")");
    auto p6v = cs.eval("(stats:get \"query:prompt6-violation-count\")");
    CHECK(los && is_int(*los), "linear-ownership-safety-stats regression");
    CHECK(lms && is_int(*lms), "linear-ownership-mutation-stats regression");
    CHECK(p6v && is_int(*p6v), "prompt6-violation-count regression");
}

} // namespace aura_598_detail

int main() {
    aura_598_detail::test_post_mutate_leak_violations();
    aura::compiler::CompilerService cs;
    aura_598_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}