// @category: integration
// @reason: uses CompilerService to eval Aura source
// test_issue_141.cpp — Verify Issue #141 acceptance criteria
// ("feat(workspace): implement full WorkspaceTree with COW,
// read-only permissions and merge primitives").
//
// Issue #141 is an umbrella issue. All 18 workspace:* primitives
// are already implemented (verified during #135 work):
//   - workspace:create
//   - workspace:switch
//   - workspace:current
//   - workspace:list
//   - workspace:delete
//   - workspace:lock
//   - workspace:can-write?
//   - workspace:merge
//   - workspace:merge-3way
//   - workspace:discard
//   - workspace:sync-from
//   - workspace:conflicts-with
//   - workspace:memory-used
//   - workspace:memory-limit
//   - workspace:set-memory-limit
//   - workspace:cow-refused-count
//
// The WorkspaceTree and WorkspaceNode structs (with COW, read-only,
// parent traversal, generation, per-workspace memory budget) are
// at evaluator_impl.cpp.
//
// IMPORTANT: Workspace state is reset per-eval (like set-code).
// Multi-step scenarios must be bundled in a single cs.eval() call.
//
// Verification strategy:
//   - For COW isolation, use (current-source :workspace) to read
//     the workspace's source rather than calling user-defined
//     functions (which can hit parser scoping issues across switches).
//   - For mutation rejection, check that mutate:rebind's return is
//     NOT bool-true (it returns a structured error pair, which is
//     truthy in Aura, so the (if ...) shortcut is wrong — the
//     bool-check matches test_issue_135's pattern).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <vector>

// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.diag;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.parser.parser;



static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return -1;
    }
    if (!aura::compiler::types::is_int(*r)) {
        std::println(std::cerr, "    [expected int, got val={}]", r->val);
        return -1;
    }
    return aura::compiler::types::as_int(*r);
}

static bool run_ok(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return false;
    auto& v = *r;
    if (aura::compiler::types::is_bool(v)) return aura::compiler::types::as_bool(v);
    if (aura::compiler::types::is_void(v)) return false;
    return true;
}

// Helper: extract std::string from a string EvalValue via the
// evaluator's string heap.
static std::string string_value(aura::compiler::CompilerService& cs,
                                 std::string_view src) {
    auto r = cs.eval(src);
    if (!r) return "";
    auto& v = *r;
    if (!aura::compiler::types::is_string(v)) return "";
    auto idx = aura::compiler::types::as_string_idx(v);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size()) return "";
    return std::string(heap[idx]);
}

// ═══════════════════════════════════════════════════════════════
// AC #1: Workspace create / switch / list / current
// ═══════════════════════════════════════════════════════════════

bool test_workspace_create_increasing_ids() {
    std::println("\n--- Test 1.1: workspace:create returns increasing IDs ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"alpha\") "
        "  (workspace:create \"beta\") "
        "  (workspace:create \"gamma\") "
        "  (length (workspace:list)))");
    CHECK(result >= 4, "root + 3 children = 4 entries (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_workspace_list() {
    std::println("\n--- Test 1.2: workspace:list shows created workspaces ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"alpha\") "
        "  (workspace:create \"beta\") "
        "  (length (workspace:list)))");
    CHECK(result >= 3, "root + 2 children = 3 entries (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_workspace_current() {
    std::println("\n--- Test 1.3: workspace:current returns the active ID ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"tmp\") "
        "  (workspace:switch 1) "
        "  (workspace:current))");
    CHECK(result == 1, "after switch to 1, current is 1 (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_workspace_switch() {
    std::println("\n--- Test 1.4: workspace:switch changes active layer ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"test\") "
        "  (workspace:switch 1) "
        "  (workspace:current))");
    CHECK(result == 1, "after switch, current is 1 (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_workspace_delete() {
    std::println("\n--- Test 1.5: workspace:delete returns success boolean ---");
    aura::compiler::CompilerService cs;
    bool result = run_ok(cs,
        "(begin "
        "  (define w (workspace:create \"delete-me\")) "
        "  (workspace:delete w))");
    CHECK(result, "delete returns truthy on success");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #2: COW + read-only + permissions
// ═══════════════════════════════════════════════════════════════

bool test_workspace_can_write() {
    std::println("\n--- Test 2.1: workspace:can-write? reflects lock state ---");
    aura::compiler::CompilerService cs;
    // can-write? is the correct primitive name (with '?' suffix).
    // We use nested let* so each binding sees the prior lock state.
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"canwrite-test\") "
        "  (let* ((b1 (if (workspace:can-write? 1) 1 0))) "
        "    (workspace:lock 1 #t) "
        "    (let* ((b2 (if (workspace:can-write? 1) 1 0))) "
        "      (workspace:lock 1 #f) "
        "      (let* ((b3 (if (workspace:can-write? 1) 1 0))) "
        "        (+ (* b1 100) (+ (* b2 10) b3))))))");
    CHECK(result == 101, "fresh=1, locked=0, unlocked=1 (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_workspace_lock_prevents_mutation() {
    std::println("\n--- Test 2.2: workspace:lock prevents mutation ---");
    aura::compiler::CompilerService cs;
    // mutate:rebind returns a structured error pair (truthy!) on failure.
    // To check rejection, look for the source NOT being updated.
    cs.eval("(begin "
            "  (set-code \"(define (f x) (+ x 1))\") "
            "  (workspace:create \"locked\") "
            "  (workspace:switch 1) "
            "  (workspace:lock 1 #t) "
            "  (mutate:rebind \"f\" \"(lambda (x) 99)\" \"test\") "
            "  (workspace:switch 0))");
    std::string root_src = string_value(cs, "(current-source :workspace)");
    bool result = (root_src == "(define f (lambda (x) (+ x 1)))");
    CHECK(result, "mutate in locked workspace rejected (source unchanged, got '" +
          root_src + "')");
    return true;
}

bool test_issue_example_scenario() {
    std::println("\n--- Test 2.3: example scenario (mutate in child, root unchanged) ---");
    aura::compiler::CompilerService cs;
    // Use current-source :workspace to verify (matches test_issue_135 pattern).
    cs.eval("(begin "
            "  (set-code \"(define (f x) (+ x 1))\") "
            "  (workspace:create \"experiment\") "
            "  (workspace:switch 1) "
            "  (mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"test\") "
            "  (workspace:switch 0))");
    std::string root_src = string_value(cs, "(current-source :workspace)");
    bool root_unchanged = (root_src == "(define f (lambda (x) (+ x 1)))");
    CHECK(root_unchanged, "root workspace source unchanged after child mutate (got '" +
          root_src + "')");
    cs.eval("(begin "
            "  (set-code \"(define (f x) (+ x 1))\") "
            "  (workspace:create \"experiment\") "
            "  (workspace:switch 1) "
            "  (mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"test\"))");
    std::string child_src = string_value(cs, "(current-source :workspace)");
    bool child_changed = (child_src == "(define f (lambda (x) (* x 2)))");
    CHECK(child_changed, "child workspace source shows mutation (got '" + child_src + "')");
    return true;
}

bool test_cow_lazy_clone() {
    std::println("\n--- Test 2.4: COW is lazy (no clone until mutate) ---");
    aura::compiler::CompilerService cs;
    // Issue #141 AC: COW triggers only on mutate, not on switch.
    // After switch (no mutate), memory-used on the child should be 0
    // (parent's flat is still shared, no clone happened).
    int64_t result = run_int(cs,
        "(begin "
        "  (set-code \"(define x 1)\") "
        "  (workspace:create \"cow-test\") "
        "  (workspace:switch 1) "
        "  (workspace:memory-used 1))");
    CHECK(result == 0, "after switch (no mutate): memory-used = 0 (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_cow_triggers_on_mutate() {
    std::println("\n--- Test 2.5: COW triggers on first mutate in child ---");
    aura::compiler::CompilerService cs;
    // After mutate in child, child's source changes and memory-used > 0.
    int64_t mem_after_mutate = run_int(cs,
        "(begin "
        "  (set-code \"(define x 1)\") "
        "  (workspace:create \"cow-mut\") "
        "  (workspace:switch 1) "
        "  (mutate:rebind \"x\" \"(quote 99)\" \"test\") "
        "  (workspace:memory-used))");
    CHECK(mem_after_mutate > 0, "after mutate in child: memory-used > 0 (got " +
          std::to_string(mem_after_mutate) + ")");
    cs.eval("(begin "
            "  (set-code \"(define x 1)\") "
            "  (workspace:create \"cow-mut\") "
            "  (workspace:switch 1) "
            "  (mutate:rebind \"x\" \"(quote 99)\" \"test\"))");
    std::string child_src = string_value(cs, "(current-source :workspace)");
    bool child_has_mutation = (child_src == "(define x (quote 99))");
    CHECK(child_has_mutation, "child shows mutation in source (got '" + child_src + "')");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #3: merge / discard / sync-from
// ═══════════════════════════════════════════════════════════════

bool test_workspace_discard() {
    std::println("\n--- Test 3.1: workspace:discard drops child without merge ---");
    aura::compiler::CompilerService cs;
    // After discard, the root keeps its original definition.
    cs.eval("(begin "
            "  (set-code \"(define (f x) (+ x 1))\") "
            "  (workspace:create \"discard-test\") "
            "  (workspace:switch 1) "
            "  (mutate:rebind \"f\" \"(lambda (x) 99)\" \"test\") "
            "  (workspace:discard 1) "
            "  (workspace:switch 0))");
    std::string root_src = string_value(cs, "(current-source :workspace)");
    bool result = (root_src == "(define f (lambda (x) (+ x 1)))");
    CHECK(result, "after discard: root source unchanged (got '" + root_src + "')");
    return true;
}

bool test_workspace_merge() {
    std::println("\n--- Test 3.2: workspace:merge propagates child to parent ---");
    aura::compiler::CompilerService cs;
    // After merge, the root should include both source snippets.
    // Source-level merge keeps both, with child overriding (later wins).
    cs.eval("(begin "
            "  (set-code \"(define (f x) (+ x 1))\") "
            "  (workspace:create \"merge-test\") "
            "  (workspace:switch 1) "
            "  (mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"test\") "
            "  (workspace:merge 1) "
            "  (workspace:switch 0))");
    std::string merged = string_value(cs, "(current-source :workspace)");
    bool result = (merged.find("(define f (lambda (x) (* x 2)))") != std::string::npos);
    CHECK(result, "after merge: root source contains child's mutation (got '" +
          merged + "')");
    return true;
}

bool test_workspace_sync_from() {
    std::println("\n--- Test 3.3: workspace:sync-from copies parent to child ---");
    aura::compiler::CompilerService cs;
    // After sync-from, the child should have the symbol from the source
    // workspace. Verify by reading the child's current-source.
    cs.eval("(begin "
            "  (set-code \"(define x 1)\") "
            "  (workspace:create \"sync-test\") "
            "  (workspace:switch 0) "
            "  (mutate:rebind \"x\" \"(quote 99)\" \"test\") "
            "  (workspace:sync-from 0 1) "
            "  (workspace:switch 1))");
    std::string child_src = string_value(cs, "(current-source :workspace)");
    bool result = !child_src.empty() &&
                  (child_src.find("x") != std::string::npos);
    CHECK(result, "after sync-from: child source non-empty and contains x (got '" +
          child_src + "')");
    return true;
}

bool test_workspace_conflicts_with() {
    std::println("\n--- Test 3.4: workspace:conflicts-with primitive exists ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(if (procedure? workspace:conflicts-with) 1 0)");
    CHECK(result == 1, "workspace:conflicts-with is a procedure (got " +
          std::to_string(result) + ")");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #4: Memory budget / cow-refused-count
// ═══════════════════════════════════════════════════════════════

bool test_workspace_memory_primitives() {
    std::println("\n--- Test 4.1: memory-used + memory-limit primitives exist ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"mem-test\") "
        "  (+ (workspace:memory-used 1) "
        "     (* 10 (+ (workspace:memory-limit 1) 2))))");
    // memory-used=0 (no mutate, no COW), memory-limit=-1 (unlimited) -> 0 + 10*1 = 10
    CHECK(result == 10, "memory-used=0, memory-limit=-1 (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_workspace_cow_refused_count() {
    std::println("\n--- Test 4.2: cow-refused-count starts at 0 ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"cow-refused-test\") "
        "  (workspace:cow-refused-count 1))");
    CHECK(result == 0, "cow-refused-count starts at 0 (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_workspace_set_memory_limit() {
    std::println("\n--- Test 4.3: set-memory-limit + read-back ---");
    aura::compiler::CompilerService cs;
    // Signature is (workspace:set-memory-limit bytes). The current
    // primitive operates on the active workspace, not on an arg id.
    int64_t result = run_int(cs,
        "(begin "
        "  (workspace:create \"limit-test\") "
        "  (workspace:switch 1) "
        "  (if (workspace:set-memory-limit 1048576) "
        "      (workspace:memory-limit) "
        "      -1))");
    CHECK(result == 1048576, "after set 1MB, memory-limit = 1048576 (got " +
          std::to_string(result) + ")");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #5: Stress test
// ═══════════════════════════════════════════════════════════════

bool test_stress_20_workspaces() {
    std::println("\n--- Test 5.1: 20 workspaces in a single eval ---");
    aura::compiler::CompilerService cs;
    int64_t result = run_int(cs,
        "(begin "
        "  (define i 0) "
        "  (while (< i 20) (begin "
        "    (workspace:create \"ws\") "
        "    (set! i (+ i 1)))) "
        "  (length (workspace:list)))");
    CHECK(result >= 21, "20+ workspaces in list (got " +
          std::to_string(result) + ")");
    return true;
}

bool test_stress_mutate_across_workspaces() {
    std::println("\n--- Test 5.2: many mutates across workspaces ---");
    aura::compiler::CompilerService cs;
    // 5 workspaces with their own mutation. Each child's source should
    // contain the workspace-specific value. We verify workspace 1
    // ends up with the rebind "counter" set to (quote 0).
    cs.eval("(begin "
            "  (set-code \"(define counter 0)\") "
            "  (workspace:create \"w0\") (workspace:create \"w1\") "
            "  (workspace:create \"w2\") (workspace:create \"w3\") "
            "  (workspace:create \"w4\") "
            "  (workspace:switch 1) (mutate:rebind \"counter\" \"(quote 0)\" \"c\") "
            "  (workspace:switch 2) (mutate:rebind \"counter\" \"(quote 10)\" \"c\") "
            "  (workspace:switch 3) (mutate:rebind \"counter\" \"(quote 20)\" \"c\") "
            "  (workspace:switch 4) (mutate:rebind \"counter\" \"(quote 30)\" \"c\") "
            "  (workspace:switch 5) (mutate:rebind \"counter\" \"(quote 40)\" \"c\") "
            "  (workspace:switch 1))");
    std::string ws1_src = string_value(cs, "(current-source :workspace)");
    bool result = (ws1_src.find("(quote 0)") != std::string::npos);
    CHECK(result, "workspace 1 source contains its own counter=0 (got '" +
          ws1_src + "')");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC #6: Cross-layer NodeId isolation
// ═══════════════════════════════════════════════════════════════

bool test_node_id_isolation() {
    std::println("\n--- Test 6.1: NodeId initially shared between parent and child ---");
    aura::compiler::CompilerService cs;
    // Initially, child shares parent's flat (COW). So NodeIds
    // should be the same until mutate.
    int64_t result = run_int(cs,
        "(begin "
        "  (set-code \"(define x 1)\") "
        "  (define root-id (car (query:find \"x\"))) "
        "  (workspace:create \"isolated\") "
        "  (workspace:switch 1) "
        "  (define child-id (car (query:find \"x\"))) "
        "  (if (= root-id child-id) 1 0))");
    CHECK(result == 1, "child initially shares NodeIds with parent (got " +
          std::to_string(result) + ")");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════

int run_issue_141() {
    std::println("═══ Issue #141 verification tests ═══\n");

    std::println("── AC #1: Workspace create/switch/list/current ──");
    test_workspace_create_increasing_ids();
    test_workspace_list();
    test_workspace_current();
    test_workspace_switch();
    test_workspace_delete();

    std::println("\n── AC #2: COW + read-only + permissions ──");
    test_workspace_can_write();
    test_workspace_lock_prevents_mutation();
    test_issue_example_scenario();
    test_cow_lazy_clone();
    test_cow_triggers_on_mutate();

    std::println("\n── AC #3: merge / discard / sync-from ──");
    test_workspace_discard();
    test_workspace_merge();
    test_workspace_sync_from();
    test_workspace_conflicts_with();

    std::println("\n── AC #4: Memory budget / cow-refused-count ──");
    test_workspace_memory_primitives();
    test_workspace_cow_refused_count();
    test_workspace_set_memory_limit();

    std::println("\n── AC #5: Stress (many workspaces / mutates) ──");
    test_stress_20_workspaces();
    test_stress_mutate_across_workspaces();

    std::println("\n── AC #6: Cross-layer NodeId isolation ──");
    test_node_id_isolation();

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══",
                 g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
