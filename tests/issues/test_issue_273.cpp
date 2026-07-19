// @category: integration
// @reason: verifies FlatAST mutation/stability hot-path C++26 contracts

// test_issue_273.cpp — Issue #273: Contracts on FlatAST hot paths.
// Cycle 1: is_valid / get_safe / validate / rollback / mark_dirty_upward /
// add_mutation_with_rollback / bump_generation + PCV COW contracts.

#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.hpp"

using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;

namespace aura_issue_273_detail {

bool test_is_valid_and_get_safe() {
    std::println("\n--- AC1: is_valid + get_safe on live NodeId ---");
    aura::ast::FlatAST flat;
    auto sym = aura::ast::INVALID_SYM;
    auto id = flat.add_variable(sym);
    CHECK(flat.is_valid(id), "fresh NodeId is valid");
    auto opt = flat.get_safe(id);
    CHECK(opt.has_value(), "get_safe returns NodeView for valid id");
    CHECK(opt->tag == aura::ast::NodeTag::Variable, "get_safe view tag matches");
    return true;
}

bool test_stale_nodeid_after_generation_bump() {
    std::println("\n--- AC2: stale NodeId detected after bump_generation ---");
    aura::ast::FlatAST flat;
    auto id = flat.add_variable(0);
    auto gen_before = flat.generation();
    flat.bump_generation();
    CHECK(flat.generation() != gen_before, "generation advanced");
    CHECK(!flat.is_valid(id), "NodeId stale after generation bump");
    CHECK(!flat.get_safe(id).has_value(), "get_safe returns nullopt for stale id");
    return true;
}

bool test_stable_ref_invalidation() {
    std::println("\n--- AC3: StableNodeRef invalidated after structural bump ---");
    aura::ast::FlatAST flat;
    auto id = flat.add_literal(42);
    auto ref = flat.make_ref(id);
    CHECK(flat.is_valid(ref), "StableNodeRef valid at capture");
    flat.bump_generation();
    CHECK(!flat.is_valid(ref), "StableNodeRef stale after bump");
    CHECK(!flat.get_safe(ref).has_value(), "get_safe(StableNodeRef) nullopt when stale");
    return true;
}

bool test_validate_aborts_on_stale_nodeid() {
    std::println("\n--- AC4: validate() contract fires on stale NodeId (subprocess) ---");
    pid_t pid = fork();
    if (pid < 0) {
        ++g_failed;
        return false;
    }
    if (pid == 0) {
        aura::ast::FlatAST flat;
        auto id = flat.add_literal(1);
        flat.bump_generation();
        flat.validate(id);
        _exit(0); // should not reach
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    CHECK(aborted, "validate() aborts on stale NodeId (contract violation)");
    return aborted;
}

bool test_mutation_and_rollback_contracts() {
    std::println("\n--- AC5: add_mutation_with_rollback + rollback on sym_id ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto sym_a = pool.intern("a");
    auto sym_b = pool.intern("b");
    auto id = flat.add_variable(sym_a);
    auto old_sym = flat.sym_id(id);
    auto mid = flat.add_mutation_with_rollback(
        id, "rename-symbol", "", "", "test rename", aura::ast::MutationStatus::Committed,
        static_cast<std::uint32_t>(aura::ast::MutationSoAField::SymId),
        static_cast<std::uint64_t>(old_sym), static_cast<std::uint64_t>(sym_b), true);
    CHECK(mid >= 1, "mutation id assigned");
    flat.sym_id(id) = sym_b;
    // Capture StableNodeRef before rollback: #1441 restamps live
    // node_gen_ after bump, so raw NodeIds stay valid; refs with the
    // pre-rollback gen still go stale.
    auto ref_before = flat.make_ref(id);
    CHECK(flat.rollback(mid), "rollback succeeds for recorded mutation");
    CHECK(flat.sym_id(id) == old_sym, "sym_id restored after rollback");
    CHECK(flat.is_valid(id), "NodeId restamped after rollback generation bump");
    CHECK(!flat.is_valid(ref_before), "StableNodeRef stale after rollback generation bump");
    return true;
}

bool test_mark_dirty_upward_live_node() {
    std::println("\n--- AC6: mark_dirty_upward on valid node ---");
    aura::ast::FlatAST flat;
    auto id = flat.add_literal(7);
    flat.clear_all_dirty();
    flat.mark_dirty_upward(id);
    CHECK(flat.is_dirty(id), "target node marked dirty");
    return true;
}

int run_tests() {
    std::println("Issue #273 (FlatAST mutation/stability contracts)\n");
    test_is_valid_and_get_safe();
    test_stale_nodeid_after_generation_bump();
    test_stable_ref_invalidation();
    test_validate_aborts_on_stale_nodeid();
    test_mutation_and_rollback_contracts();
    test_mark_dirty_upward_live_node();
    // PCV COW contracts: see test_issue_221 (standalone header test).
    // Direct PCV use here conflicts with `import aura.core.ast` (std
    // module vs header include); FlatAST paths are the #273 focus.
    std::println("\n--- AC7: PCV COW contracts deferred to test_issue_221 ---");
    std::println("  (skipped — module/header std conflict)");
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_273_detail

int aura_issue_273_run() {
    return aura_issue_273_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_273_run();
}
#endif