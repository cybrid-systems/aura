// @category: integration
// @reason: Issue #507 — Task4 hot-path Contracts + consteval invariants

#include "test_harness.hpp"
#include "shape_profiler.h"

import std;
import aura.core.arena;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_507_detail {

using aura::compiler::CompilerService;
using aura::compiler::shape::inline_shape_of;
using aura::compiler::shape::is_known_inline_shape_id;
using aura::compiler::shape::SHAPE_INT;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t hash_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:task4-hotpath-contracts\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (+ a b)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

bool test_primitive_returns_hash() {
    std::println("\n--- AC1: query:task4-hotpath-contracts returns hash ---");
    CompilerService cs;
    auto r = cs.eval("(query:task4-hotpath-contracts)");
    CHECK(r.has_value(), "primitive eval succeeds");
    CHECK(is_hash(*r), "query:task4-hotpath-contracts returns hash");
    return true;
}

bool test_shape_dispatch_table_size() {
    std::println("\n--- AC2: shape-dispatch-table-size >= 5 ---");
    CompilerService cs;
    const auto v = hash_int(cs, "shape-dispatch-table-size");
    CHECK(v >= 5, std::format("shape-dispatch-table-size >= 5 (got {})", v));
    return true;
}

bool test_consteval_hits() {
    std::println("\n--- AC3: consteval-hits >= 30 ---");
    CompilerService cs;
    const auto v = hash_int(cs, "consteval-hits");
    CHECK(v >= 30, std::format("consteval-hits >= 30 (got {})", v));
    return true;
}

bool test_inline_shape_of_contract() {
    std::println("\n--- AC4: inline_shape_of post contract (in-bounds) ---");
    const auto int_shape = inline_shape_of(42);
    const auto bool_shape = inline_shape_of(3);
    CHECK(is_known_inline_shape_id(int_shape), "inline_shape_of(42) known shape");
    CHECK(is_known_inline_shape_id(bool_shape), "inline_shape_of(3) known shape");
    CHECK(int_shape == SHAPE_INT, "42 maps to SHAPE_INT");
    return true;
}

bool test_arena_create_contract() {
    std::println("\n--- AC5: ASTArena::create pre contract (in-bounds) ---");
    aura::ast::ASTArena arena(4096);
    auto* node = arena.create<int>(42);
    CHECK(node != nullptr, "ASTArena::create returns non-null");
    CHECK(*node == 42, "ASTArena::create value preserved");
    return true;
}

bool test_run_one_contract() {
    std::println("\n--- AC6: run_one contract (in-bounds) ---");
    aura::ir::IRModule mod;
    aura::compiler::ComputeKindWrap pass;
    const bool ok = aura::compiler::run_one(mod, pass);
    CHECK(ok, "run_one returns true for non-error pass");
    return true;
}

bool test_eval_cycle() {
    std::println("\n--- AC7: eval + mutate cycle ---");
    CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");
    CHECK(cs.eval("(mutate:rebind \"a\" \"10\")").has_value(), "mutate under Guard");
    (void)cs.eval("(eval-current)");
    auto stats = cs.eval("(query:task4-hotpath-contracts)");
    CHECK(stats && is_hash(*stats), "task4-hotpath-contracts hash after cycle");
    return true;
}

bool test_regression_primitives() {
    std::println("\n--- AC8: related primitive regression ---");
    CompilerService cs;
    auto cxx = cs.eval("(query:cxx26-hotpath-invariants)");
    auto pcs = cs.eval("(engine:metrics \"query:pass-contracts-stats\")");
    CHECK(cxx && is_hash(*cxx), "cxx26-hotpath-invariants regression");
    CHECK(pcs && is_int(*pcs), "pass-contracts-stats regression");
    return true;
}

bool test_stats_count() {
    std::println("\n--- AC9: stats:count ---");
    CompilerService cs;
    auto count = cs.eval("(stats:count)");
    CHECK(count && is_int(*count) && aura::compiler::types::as_int(*count) >= 211,
          "stats:count >= 211");
    return true;
}

} // namespace aura_issue_507_detail

int aura_issue_507_run() {
    using namespace aura_issue_507_detail;
    std::println("=== Issue #507: Task4 hot-path Contracts + consteval ===");
    test_primitive_returns_hash();
    test_shape_dispatch_table_size();
    test_consteval_hits();
    test_inline_shape_of_contract();
    test_arena_create_contract();
    test_run_one_contract();
    test_eval_cycle();
    test_regression_primitives();
    test_stats_count();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_507_run();
}
#endif
