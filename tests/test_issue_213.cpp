// test_issue_213.cpp — Issue #213 Cycle 1:
// rollback mechanism for the mutation boundary API.
//
// Verifies the new rollback path in
// `Evaluator::exit_mutation_boundary(false)`:
//   - When success=false, the mutations recorded between
//     enter and exit are rolled back via
//     `FlatAST::rollback_to_size(checkpoint_size)`.
//   - The defuse_index_ is invalidated.
//   - The defuse_version_ is bumped again.
//   - The mutation log shows the records as `RolledBack`.
//
// Test scenarios (matches the issue body's plan):
//   1. exit(success=true) keeps the mutation (no rollback)
//   2. exit(success=false) rolls back a single field-level mutation
//   3. exit(success=false) rolls back multiple mutations in one boundary
//   4. The defuse_version_ is bumped on rollback
//   5. The mutation log shows records as RolledBack
//   6. The mutation log size after rollback matches the checkpoint size
//   7. Nested boundaries: outer rollback rolls back inner commits too

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.core;
import aura.core.type;
import aura.diag;
import aura.compiler.value;

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "  FAIL: %s (line %d)\n", (msg), __LINE__); \
        ++g_failed; \
    } else { \
        std::fprintf(stdout, "  PASS: %s\n", (msg)); \
        ++g_passed; \
    } \
} while(0)

#define PRINTLN(msg) std::fprintf(stdout, "%s\n", (msg))

// Helper: build a workspace FlatAST with N Int literal nodes
// and return it. The literals are initialized with
// `set_int` to known values so we can verify rollback.
static void build_workspace_with_n_literals(
    aura::ast::FlatAST& flat,
    aura::ast::StringPool& pool,
    std::int64_t initial_value,
    std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        auto lit = flat.add_literal(initial_value);
        (void)lit;
    }
}

// Helper: count committed mutations in the log.
static std::size_t count_committed(aura::ast::FlatAST& flat) {
    std::size_t n = 0;
    for (const auto& r : flat.all_mutations()) {
        if (r.status == aura::ast::MutationStatus::Committed) ++n;
    }
    return n;
}
static std::size_t count_rolled_back(aura::ast::FlatAST& flat) {
    std::size_t n = 0;
    for (const auto& r : flat.all_mutations()) {
        if (r.status == aura::ast::MutationStatus::RolledBack) ++n;
    }
    return n;
}

// ── Test 1: exit(success=true) keeps the mutation ──
//
// Pre-#213 behavior was "both success and failure are commits".
// The new path keeps this for success=true. We just verify
// that the new code path doesn't accidentally roll back on
// success=true.
bool test_exit_success_keeps_mutation() {
    PRINTLN("\n--- Test 1: exit(success=true) keeps the mutation ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    build_workspace_with_n_literals(flat, pool, 0, 1);
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    auto v0 = ev.defuse_version_snapshot();
    ev.enter_mutation_boundary();
    // Simulate a field-level mutation: change node 0 from 0 to 42.
    flat.set_int(0, 42);
    flat.add_mutation_with_rollback(0, "test:set", "Int", "Int", "0 → 42",
                                    aura::ast::MutationStatus::Committed,
                                    /*field_offset=*/0, /*old=*/0, /*new=*/42,
                                    /*has_rollback=*/true);
    ev.exit_mutation_boundary(true);

    CHECK(flat.int_val(0) == 42, "node 0 stays at 42 after exit(true)");
    CHECK(count_committed(flat) == 1, "1 committed mutation in log");
    CHECK(count_rolled_back(flat) == 0, "0 rolled-back mutations in log");
    CHECK(ev.defuse_version_snapshot() == v0 + 2, "version bumped by 2 (enter + exit, success path)");
    return true;
}

// ── Test 2: exit(success=false) rolls back a single mutation ──
bool test_exit_failure_rolls_back_single() {
    PRINTLN("\n--- Test 2: exit(success=false) rolls back single ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    build_workspace_with_n_literals(flat, pool, 10, 1);  // node 0 starts at 10
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    ev.enter_mutation_boundary();
    flat.set_int(0, 99);  // change node 0 from 10 to 99
    flat.add_mutation_with_rollback(0, "test:set", "Int", "Int", "10 → 99",
                                    aura::ast::MutationStatus::Committed,
                                    /*field_offset=*/0, /*old=*/10, /*new=*/99,
                                    /*has_rollback=*/true);
    CHECK(flat.int_val(0) == 99, "mid-boundary: node 0 is 99");
    ev.exit_mutation_boundary(false);

    CHECK(flat.int_val(0) == 10, "post-rollback: node 0 is back to 10");
    CHECK(count_rolled_back(flat) == 1, "1 rolled-back mutation in log");
    CHECK(count_committed(flat) == 0, "0 committed mutations (the only one was rolled back)");
    return true;
}

// ── Test 3: exit(success=false) rolls back multiple mutations ──
bool test_exit_failure_rolls_back_multiple() {
    PRINTLN("\n--- Test 3: exit(success=false) rolls back multiple ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    build_workspace_with_n_literals(flat, pool, 100, 4);  // 4 nodes at 100
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    ev.enter_mutation_boundary();
    flat.set_int(0, 200);
    flat.add_mutation_with_rollback(0, "test:set", "Int", "Int", "100 → 200",
                                    aura::ast::MutationStatus::Committed, 0, 100, 200, true);
    flat.set_int(1, 300);
    flat.add_mutation_with_rollback(1, "test:set", "Int", "Int", "100 → 300",
                                    aura::ast::MutationStatus::Committed, 0, 100, 300, true);
    flat.set_int(2, 400);
    flat.add_mutation_with_rollback(2, "test:set", "Int", "Int", "100 → 400",
                                    aura::ast::MutationStatus::Committed, 0, 100, 400, true);
    ev.exit_mutation_boundary(false);

    CHECK(flat.int_val(0) == 100, "node 0 back to 100");
    CHECK(flat.int_val(1) == 100, "node 1 back to 100");
    CHECK(flat.int_val(2) == 100, "node 2 back to 100");
    CHECK(flat.int_val(3) == 100, "node 3 untouched (was 100, still 100)");
    CHECK(count_rolled_back(flat) == 3, "3 rolled-back mutations in log");
    return true;
}

// ── Test 4: defuse_version_ is bumped on rollback ──
bool test_defuse_version_bump_on_rollback() {
    PRINTLN("\n--- Test 4: defuse_version_ bump on rollback ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    build_workspace_with_n_literals(flat, pool, 0, 1);
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    auto v0 = ev.defuse_version_snapshot();
    ev.enter_mutation_boundary();
    flat.set_int(0, 42);
    flat.add_mutation_with_rollback(0, "test:set", "Int", "Int", "0 → 42",
                                    aura::ast::MutationStatus::Committed, 0, 0, 42, true);
    ev.exit_mutation_boundary(false);

    auto v1 = ev.defuse_version_snapshot();
    CHECK(v1 == v0 + 2, "version bumped by 2 (one enter + one rollback bump)");
    return true;
}

// ── Test 5: mutation log size matches checkpoint after rollback ──
bool test_log_size_after_rollback() {
    PRINTLN("\n--- Test 5: log size after rollback ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    build_workspace_with_n_literals(flat, pool, 0, 1);
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    // Pre-existing mutations: add 2 records
    flat.set_int(0, 50);
    flat.add_mutation_with_rollback(0, "test:pre1", "Int", "Int", "0 → 50",
                                    aura::ast::MutationStatus::Committed, 0, 0, 50, true);
    flat.set_int(0, 60);
    flat.add_mutation_with_rollback(0, "test:pre2", "Int", "Int", "50 → 60",
                                    aura::ast::MutationStatus::Committed, 0, 50, 60, true);
    auto size_at_entry = flat.all_mutations().size();
    CHECK(size_at_entry == 2, "log has 2 entries before boundary");

    ev.enter_mutation_boundary();
    flat.set_int(0, 999);
    flat.add_mutation_with_rollback(0, "test:rb", "Int", "Int", "60 → 999",
                                    aura::ast::MutationStatus::Committed, 0, 60, 999, true);
    CHECK(flat.all_mutations().size() == 3, "log has 3 entries mid-boundary");
    ev.exit_mutation_boundary(false);

    // The 3rd entry should be marked RolledBack but the log
    // size is preserved (records are not truncated, just
    // status-changed). This is the standard convention for
    // audit logs.
    CHECK(flat.all_mutations().size() == 3, "log still has 3 entries after rollback");
    CHECK(flat.int_val(0) == 60, "node 0 back to 60 (the value at boundary entry)");
    CHECK(flat.all_mutations()[2].status == aura::ast::MutationStatus::RolledBack,
          "3rd entry is RolledBack");
    return true;
}

// ── Test 6: nested boundaries — outer rollback rolls back inner commits ──
bool test_nested_boundaries_outer_rollback() {
    PRINTLN("\n--- Test 6: nested boundaries ---");
    aura::compiler::Evaluator ev;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    build_workspace_with_n_literals(flat, pool, 0, 1);
    ev.set_workspace_flat(&flat);
    ev.set_workspace_pool(&pool);

    // Outer boundary (rollback at the end).
    ev.enter_mutation_boundary();
    flat.set_int(0, 10);
    flat.add_mutation_with_rollback(0, "outer:1", "Int", "Int", "0 → 10",
                                    aura::ast::MutationStatus::Committed, 0, 0, 10, true);
    // Inner boundary (commits successfully).
    ev.enter_mutation_boundary();
    flat.set_int(0, 20);
    flat.add_mutation_with_rollback(0, "inner:1", "Int", "Int", "10 → 20",
                                    aura::ast::MutationStatus::Committed, 0, 10, 20, true);
    ev.exit_mutation_boundary(true);  // inner commits
    CHECK(flat.int_val(0) == 20, "after inner commit: node 0 is 20");

    // Outer fails → both outer:1 and inner:1 are rolled back.
    ev.exit_mutation_boundary(false);

    CHECK(flat.int_val(0) == 0, "after outer rollback: node 0 is back to 0");
    CHECK(count_rolled_back(flat) == 2, "both inner:1 and outer:1 are RolledBack");
    CHECK(count_committed(flat) == 0, "no committed mutations remain");
    return true;
}

// ── Test 7: rollback is idempotent (calling rollback_to_size twice is safe) ──
bool test_rollback_to_size_idempotent() {
    PRINTLN("\n--- Test 7: rollback_to_size is idempotent ---");
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::FlatAST flat(alloc);
    flat.add_literal(0);
    flat.set_int(0, 10);
    flat.add_mutation_with_rollback(0, "test:set", "Int", "Int", "0 → 10",
                                    aura::ast::MutationStatus::Committed, 0, 0, 10, true);
    CHECK(flat.int_val(0) == 10, "node 0 is 10");

    // First rollback: should restore 0.
    auto n1 = flat.rollback_to_size(0);
    CHECK(n1 == 1, "first rollback_to_size(0) returns 1");
    CHECK(flat.int_val(0) == 0, "node 0 back to 0");

    // Second rollback at the same checkpoint: should be a no-op
    // (no records to roll back, since the only one is already
    // RolledBack). Returns 0.
    auto n2 = flat.rollback_to_size(0);
    CHECK(n2 == 0, "second rollback_to_size(0) is a no-op (returns 0)");
    CHECK(flat.int_val(0) == 0, "node 0 still at 0");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ Issue #213 Cycle 1 — mutation boundary rollback ═══\n");
    std::fprintf(stdout, "  Verifies the new rollback path in\n");
    std::fprintf(stdout, "  `Evaluator::exit_mutation_boundary(false)`.\n\n");

    test_exit_success_keeps_mutation();
    test_exit_failure_rolls_back_single();
    test_exit_failure_rolls_back_multiple();
    test_defuse_version_bump_on_rollback();
    test_log_size_after_rollback();
    test_nested_boundaries_outer_rollback();
    test_rollback_to_size_idempotent();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
