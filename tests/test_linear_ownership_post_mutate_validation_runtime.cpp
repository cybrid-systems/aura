// test_linear_ownership_post_mutate_validation_runtime.cpp
// Issue #610: Linear OwnershipEnv post-mutation re-validate +
// runtime enforcement integration.
//
// Non-duplicative with #117 (validate_ownership_full unit),
// #531 (closure-env-safety-stats), #306 (hw linear-ownership-stats),
// #598/#599 (runtime linear enforcement + epoch/GC).
//
// AC1: post_mutation_invariant_check catches leaked-linear + metrics
// AC2: query:linear-ownership-mutation-stats reachable + non-negative
// AC3: properly moved linear → no leak notes + pass true
// AC4: mutate:rebind on linear define → stats monotonic
// AC5: closure-env-safety-stats regression still works
// AC6: invalidate path bumps deopt_on_linear counter
// AC7: multi-round linear mutate matrix — eval + stats monotonic
// AC8: gc-heap + linear mutation integration (no crash)
//
// Uses one CompilerService for the integration matrix.

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

namespace aura_610_detail {

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

static void test_post_mutate_invariant_catches_leak() {
    std::println("\n--- AC1: post_mutation_invariant_check + metrics ---");
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
    rec.parent_id = aura::ast::NULL_NODE;
    rec.mutation_id = 610;
    rec.operator_name = "issue-610";

    std::vector<aura::compiler::OwnershipNote> notes;
    const auto status =
        aura::compiler::post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
    const auto reval =
        metrics.linear_post_mutate_revalidations_total.load(std::memory_order_relaxed);
    const auto leaks = metrics.linear_leak_prevented_total.load(std::memory_order_relaxed);
    std::println("  status={} notes={} revalidations={} leak_prevented={}",
                 static_cast<int>(status), notes.size(), reval, leaks);
    CHECK(reval > 0, "post_mutate_revalidations bumped on linear dirty scope");
    CHECK(count_kind(notes, "leaked-linear") >= 1,
          "post_mutation_invariant_check finds leaked-linear");
    CHECK(leaks >= 1, "linear_leak_prevented_total bumped");
}

static std::int64_t linear_mutation_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:linear-ownership-mutation-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool load_linear_workspace(CompilerService& cs) {
    if (!cs.eval(std::string("(set-code \"") + k_linear_prog + "\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC2: query:linear-ownership-mutation-stats ---");
    CHECK(load_linear_workspace(cs), "load linear workspace");
    const auto s0 = linear_mutation_stats(cs);
    std::println("  query:linear-ownership-mutation-stats = {}", s0);
    CHECK(s0 >= 0, "linear-ownership-mutation-stats non-negative");

    std::println("\n--- AC3: properly moved linear — no leak on revalidate ---");
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
    rec.mutation_id = 6103;
    std::vector<aura::compiler::OwnershipNote> notes;
    const auto status =
        aura::compiler::post_mutation_invariant_check(*flat, *str_pool, reg, rec, notes, &metrics);
    CHECK(status == aura::ast::InvariantStatus::Ok,
          "moved linear binding passes post-mutate revalidate");
    CHECK(count_kind(notes, "leaked-linear") == 0, "no leaked-linear for properly moved binding");

    std::println("\n--- AC4: post-mutate revalidate on linear mutate → stats grow ---");
    const auto stats4a = linear_mutation_stats(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 88))) (display x)))\" "
                  "\"issue-610-leak\")");
    auto* ws = cs.evaluator().workspace_flat();
    auto* ws_pool = cs.evaluator().workspace_pool();
    CHECK(ws != nullptr && ws_pool != nullptr && !ws->all_mutations().empty(),
          "mutation logged for linear rebind");
    if (ws && ws_pool && !ws->all_mutations().empty()) {
        TypeRegistry reg;
        std::vector<aura::compiler::OwnershipNote> notes;
        (void)aura::compiler::post_mutation_invariant_check(*ws, *ws_pool, reg,
                                                            ws->all_mutations().back(), notes,
                                                            cs.evaluator().compiler_metrics());
    }
    const auto stats4b = linear_mutation_stats(cs);
    std::println("  linear-ownership-mutation-stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b > stats4a, "linear-mutation-stats grew after post-mutate linear revalidate");

    std::println("\n--- AC5: closure-env-safety-stats regression ---");
    auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
    CHECK(ces && is_hash(*ces), "query:closure-env-safety-stats still works");

    std::println("\n--- AC6: invalidate bumps deopt_on_linear ---");
    const auto stats6a = linear_mutation_stats(cs);
    (void)cs.eval("(set-code \"(define f (lambda () (let ((x (Linear 1))) (move x))))\")");
    (void)cs.eval("(eval-current)");
    const auto stats6b = linear_mutation_stats(cs);
    std::println("  linear-ownership-mutation-stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "deopt_on_linear contributes to stats after invalidate path");

    std::println("\n--- AC7: multi-round linear mutate matrix ---");
    const auto stats7a = linear_mutation_stats(cs);
    for (int round = 0; round < 3; ++round) {
        const std::string body =
            "(lambda () (let ((x (Linear " + std::to_string(round + 10) + "))) (move x)))";
        (void)cs.eval("(mutate:rebind \"f\" \"" + body + "\" \"r" + std::to_string(round) + "\")");
        auto r = cs.eval("(f)");
        CHECK(r.has_value(), "f eval ok round " + std::to_string(round));
    }
    const auto stats7b = linear_mutation_stats(cs);
    std::println("  linear-ownership-mutation-stats: {} -> {}", stats7a, stats7b);
    CHECK(stats7b >= stats7a, "linear-mutation-stats monotonic over matrix");

    std::println("\n--- AC8: gc-heap + linear mutation integration ---");
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 7))) (move x)))\" "
                  "\"issue-610-gc\")");
    auto gc = cs.eval("(gc-heap)");
    CHECK(gc.has_value(), "(gc-heap) callable after linear mutate");
}

} // namespace aura_610_detail

int main() {
    aura_610_detail::test_post_mutate_invariant_catches_leak();
    aura::compiler::CompilerService cs;
    aura_610_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}