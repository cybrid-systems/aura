// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_266.cpp — Issue #266: finer-grained SoA rollback for
// children_ / sym_id_ under MutationBoundaryGuard.

#include <cstdio>
#include <string>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import std;
import aura.compiler.evaluator;
import aura.core;
import aura.core.type;
import aura.diag;
import aura.compiler.value;

namespace aura_issue_266_detail {
#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

bool test_sym_id_field_rollback() {
    PRINTLN("\n--- Test 1: sym_id_ per-record rollback ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto x = flat.add_variable(pool.intern("x"));
    flat.root = x;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

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
    return true;
}

bool test_insert_child_children_rollback() {
    PRINTLN("\n--- Test 2: insert-child structural rollback ---");
    aura::compiler::Evaluator ev;
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

    auto before = flat.get(root).children.size();
    CHECK(before == 2, "call starts with func + arg");

    ev.enter_mutation_boundary();
    auto extra = flat.add_literal(2);
    flat.insert_child(root, 1, extra);
    CHECK(flat.get(root).children.size() == 3, "mid-boundary has inserted child");
    ev.exit_mutation_boundary(false);

    CHECK(flat.get(root).children.size() == before, "children count restored");
    CHECK(flat.get(root).child(1) == lit, "original arg preserved");
    return true;
}

bool test_rename_symbol_column_rollback() {
    PRINTLN("\n--- Test 3: rename-symbol sym_id column rollback ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto x = flat.add_variable(pool.intern("alpha"));
    auto body = flat.add_literal(1);
    auto lam = flat.add_lambda({pool.intern("alpha")}, body);
    flat.root = lam;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    auto alpha = pool.intern("alpha");
    auto beta = pool.intern("beta");
    CHECK(flat.sym_id(x) == alpha, "variable uses alpha");
    CHECK(flat.param_at(lam, 0) == alpha, "lambda param is alpha");

    bool ok = true;
    {
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok, true);
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            if (flat.sym_id(id) == alpha &&
                (flat.tag(id) == aura::ast::NodeTag::Variable ||
                 flat.tag(id) == aura::ast::NodeTag::Define))
                flat.sym_id(id) = beta;
        }
        flat.rename_param(lam, alpha, beta, nullptr);
        CHECK(flat.sym_id(x) == beta, "mid-boundary sym is beta");
        ok = false;
    }

    CHECK(flat.sym_id(x) == alpha, "variable sym restored to alpha");
    CHECK(flat.param_at(lam, 0) == alpha, "lambda param restored to alpha");
    auto stats = ev.last_boundary_rollback_stats();
    CHECK(stats.sym_id_column_restored, "stats report sym_id restore");
    CHECK(stats.param_columns_restored, "stats report param restore");
    CHECK(stats.children_column_restored, "stats report children restore");
    return true;
}

bool test_guard_enable_fine_rollback_late() {
    PRINTLN("\n--- Test 4: guard.enable_fine_rollback() before mutate ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto node = flat.add_variable(pool.intern("n"));
    flat.root = node;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    auto old_sym = flat.sym_id(node);
    auto new_sym = pool.intern("m");
    bool ok = true;
    {
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
        guard.enable_fine_rollback();
        flat.sym_id(node) = new_sym;
        ok = false;
    }

    CHECK(flat.sym_id(node) == old_sym, "late fine rollback restores sym_id");
    auto stats = ev.last_boundary_rollback_stats();
    CHECK(stats.sym_id_column_restored, "guard stats show sym restore");
    return true;
}

bool test_structural_record_rollback_count() {
    PRINTLN("\n--- Test 5: structural mutation log rollback count ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto fn = flat.add_variable(pool.intern("f"));
    auto a = flat.add_literal(1);
    auto b = flat.add_literal(2);
    auto root = flat.add_call(fn, {a});
    flat.root = root;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    ev.enter_mutation_boundary();
    flat.set_child(root, 1, b);
    ev.exit_mutation_boundary(false);

    CHECK(flat.get(root).child(1) == a, "set-child rolled back (arg slot)");
    auto stats = ev.last_boundary_rollback_stats();
    CHECK(stats.field_records_rolled >= 1, "at least one field record rolled");
    return true;
}

bool test_happy_path_keeps_mutation() {
    PRINTLN("\n--- Test 6: happy path keeps structural mutation ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto fn = flat.add_variable(pool.intern("g"));
    auto root = flat.add_call(fn, {});
    flat.root = root;
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    bool ok = true;
    {
        aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok, true);
        auto child = flat.add_literal(9);
        flat.insert_child(root, 0, child);
    }
    CHECK(flat.get(root).children.size() == 2, "insert committed on success");
    return true;
}

int run_tests() {
    std::fprintf(stdout, "═══ Issue #266 — fine-grained SoA rollback ═══\n");
    test_sym_id_field_rollback();
    test_insert_child_children_rollback();
    test_rename_symbol_column_rollback();
    test_guard_enable_fine_rollback_late();
    test_structural_record_rollback_count();
    test_happy_path_keeps_mutation();
    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
}  // namespace aura_issue_266_detail

int aura_issue_266_run() { return aura_issue_266_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_266_run(); }
#endif