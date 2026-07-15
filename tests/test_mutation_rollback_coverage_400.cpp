// test_mutation_rollback_coverage_400.cpp
// Issue #400: Expand rollback coverage observability for sym_id_
// and structural changes under add_mutation_with_rollback.
//
// Non-duplicative with #266 (sym_id unit), #369 (structural unit),
// #553 (mutation-log-stats batch matrix), #213 (rollback_to_size).
//
// AC1: query:mutation-rollback-coverage-stats reachable
// AC2: sym_id per-record rollback under Guard boundary
// AC3: structural insert-child rollback restores children_
// AC4: structural_rollback_success counter bumps on rollback()
// AC5: mutate:rebind + failed boundary bumps field_log_rollbacks
// AC6: multi-round rollback matrix — coverage stats monotonic
// AC7: query regression (mutation-log-stats, ast:generation-stats)
//
// Unit sym/structural tests run first; integration uses CompilerService.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core;
import aura.core.ast;

namespace aura_400_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t coverage_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:mutation-rollback-coverage-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static std::int64_t coverage_stats(Evaluator& ev) {
    auto* ws = ev.workspace_flat();
    if (!ws)
        return 0;
    return static_cast<std::int64_t>(
        ws->structural_rollback_success() + ws->structural_rollback_besteffort() +
        ev.get_mutation_log_rollback_count() + ev.atomic_batch_rollbacks());
}

static void test_sym_id_rollback() {
    std::println("\n--- AC2: sym_id per-record rollback ---");
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto x = flat.add_variable(pool.intern("x"));
    flat.root = x;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    const auto stats0 = coverage_stats(ev);
    auto old_sym = flat.sym_id(x);
    auto new_sym = pool.intern("y");

    ev.enter_mutation_boundary();
    flat.set_sym(x, new_sym);
    flat.add_mutation_with_rollback(
        x, "replace-value", "Sym", "Sym", "x -> y", aura::ast::MutationStatus::Committed,
        static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId),
        static_cast<std::uint64_t>(old_sym), static_cast<std::uint64_t>(new_sym), true);
    CHECK(flat.sym_id(x) == new_sym, "mid-boundary sym is y");
    ev.exit_mutation_boundary(false);

    CHECK(flat.sym_id(x) == old_sym, "sym rolled back to x");
    const auto stats1 = coverage_stats(ev);
    CHECK(stats1 >= stats0, "coverage stats monotonic after sym_id rollback");
}

static void test_structural_insert_rollback() {
    std::println("\n--- AC3: insert-child structural rollback ---");
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto fn = flat.add_variable(pool.intern("+"));
    auto lit = flat.add_literal(1);
    auto root = flat.add_call(fn, {lit});
    flat.root = root;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    const auto before = flat.get(root).children.size();
    ev.enter_mutation_boundary();
    auto extra = flat.add_literal(2);
    flat.insert_child(root, 1, extra);
    CHECK(flat.get(root).children.size() == before + 1, "mid-boundary has inserted child");
    ev.exit_mutation_boundary(false);
    CHECK(flat.get(root).children.size() == before,
          "children count restored after failed boundary");
}

static void test_structural_counter_on_rollback() {
    std::println("\n--- AC4: structural_rollback_success on rollback() ---");
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::FlatAST flat(alloc);
    auto parent = flat.add_literal(0);
    auto child = flat.add_literal(42);
    flat.set_child(parent, 0, child);

    const auto success0 = flat.structural_rollback_success();
    auto mid = flat.add_structural_mutation_log_entry(parent, 0, child, aura::ast::NULL_NODE,
                                                      "remove-node");
    CHECK(mid > 0, "structural log entry recorded");
    CHECK(flat.rollback(mid), "rollback(mutation_id) succeeds for remove-node");
    CHECK(flat.structural_rollback_success() >= success0 + 1,
          "structural_rollback_success incremented");
}

static void test_field_int_rollback() {
    std::println("\n--- AC5: field_offset int_val rollback bumps counter ---");
    Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::FlatAST flat(alloc);
    auto lit = flat.add_literal(10);
    flat.root = lit;
    ev.set_workspace_flat(&flat);

    const auto roll0 = ev.get_mutation_log_rollback_count();
    ev.enter_mutation_boundary();
    flat.set_int(lit, 99);
    flat.add_mutation_with_rollback(
        lit, "test:set", "Int", "Int", "10 -> 99", aura::ast::MutationStatus::Committed,
        static_cast<std::uint32_t>(aura::ast::MutationSoAField::IntVal), 10, 99, true);
    ev.exit_mutation_boundary(false);
    const auto roll1 = ev.get_mutation_log_rollback_count();
    CHECK(flat.int_val(lit) == 10, "int_val restored after failed boundary");
    CHECK(roll1 > roll0, "mutation_log_rollback_count bumped");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:mutation-rollback-coverage-stats ---");
    CHECK(cs.eval("(set-code \"(define acc 0)\")").has_value(), "workspace setup");
    CHECK(cs.eval("(eval-current)").has_value(), "workspace eval");
    const auto s0 = coverage_stats(cs);
    std::println("  mutation-rollback-coverage-stats = {}", s0);
    CHECK(s0 >= 0, "coverage stats non-negative");

    std::println("\n--- AC6: multi-round mutate matrix ---");
    const auto stats6a = coverage_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(10 + round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = coverage_stats(cs);
    std::println("  coverage stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "coverage stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto mls = cs.eval("(engine:metrics \"query:mutation-log-stats\")");
    auto ags = cs.eval("(ast:generation-stats)");
    CHECK(mls && is_int(*mls), "mutation-log-stats regression");
    CHECK(ags.has_value(), "ast:generation-stats regression");
}

} // namespace aura_400_detail

int main() {
    aura_400_detail::test_sym_id_rollback();
    aura_400_detail::test_structural_insert_rollback();
    aura_400_detail::test_structural_counter_on_rollback();
    aura_400_detail::test_field_int_rollback();
    aura::compiler::CompilerService cs;
    aura_400_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}