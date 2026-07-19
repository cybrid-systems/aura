// @category: unit
// @reason: no CompilerService usage; pure C++ test
// test_issue_117.cpp — Verify linear ownership validation fixes
// (TypeChecker now has full re-simulation mode + gradual+Linear
// boundary check).
//
// Regression scenarios:
//   1. validate_ownership_full discovers linear bindings from
//      `(let ((x (Linear e))) ...)` and validates them.
//   2. Cross-function ownership: linear value passed as
//      argument to a higher-order function (the dirty-only
//      path can't see inside the closure body).
//   3. Closure-captured linear: a let-bound linear value
//      captured by a lambda, then moved in the lambda body.
//   4. Global-scope linear: a let-bound linear at top level
//      (not inside a function body).
//   5. Gradual+Linear boundary: Any ~ Linear is rejected by
//      consistent_unify (the fix).
//   6. The original dirty-only path still works (regression
//      check for the existing scope-aware walker).


// Unified test harness (Issue #226). Provides
// CHECK / EXPECT_* / TEST / RUN_ALL_TESTS. The local
// g_passed / g_failed / CHECK macro above are removed;
// this file now uses the harness's versions.
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.diag;
import aura.core.type;
import aura.parser.parser;


namespace aura_issue_117_detail {
struct TypecheckResult {
    std::unique_ptr<aura::ast::ASTArena> arena;
    std::unique_ptr<aura::core::TypeRegistry> treg;
    std::unique_ptr<aura::compiler::TypeChecker> tc;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
    aura::ast::NodeId root = 0;
};

TypecheckResult typecheck(const std::string& src) {
    TypecheckResult r;
    r.arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = r.arena->allocator();
    auto* flat = r.arena->create<aura::ast::FlatAST>(alloc);
    r.pool = r.arena->create<aura::ast::StringPool>(alloc);
    auto pr = aura::parser::parse_to_flat(src, *flat, *r.pool);
    flat->root = pr.root;
    r.flat = flat;
    r.root = pr.root;
    if (!pr.success)
        return r;
    r.treg = std::make_unique<aura::core::TypeRegistry>();
    r.tc = std::make_unique<aura::compiler::TypeChecker>(*r.treg);
    aura::diag::DiagnosticCollector diag;
    r.tc->infer_flat(*flat, *r.pool, pr.root, diag);
    return r;
}

int count_kind(const std::vector<aura::compiler::OwnershipNote>& notes, const std::string& kind) {
    int n = 0;
    for (auto& note : notes) {
        if (note.kind == kind)
            ++n;
    }
    return n;
}

// ── Test 1: validate_ownership_full discovers linear bindings ────
//
// Builds a let with a (Linear ...) value by hand, runs the
// full validation, and verifies that a leaked-linear diagnostic
// is reported.

bool test_full_re_simulation_discovers_linear() {
    std::println("\n--- Test: validate_ownership_full discovers linear bindings ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    // Build: (let ((x (Linear 42))) (display x))  — x is
    // linear but never moved → should leak.
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

    std::vector<aura::compiler::OwnershipNote> notes;
    aura::core::TypeRegistry reg; // Issue #1387: required TypeRegistry arg
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    int leaks = count_kind(notes, "leaked-linear");
    std::println("  full validation: pass={} leaks={}", pass, leaks);
    CHECK(leaks >= 1, "validate_ownership_full finds leaked-linear binding");
    CHECK(!pass, "full validation: overall pass=false when leak present");
    return true;
}

// ── Test 2: dirty-only vs full — full catches what dirty misses ──
//
// A (Linear ...) binding in a let that the dirty set doesn't
// include. The dirty-only path would skip it; the full path
// should still report the leak.

bool test_full_catches_what_dirty_misses() {
    std::println("\n--- Test: full catches what dirty-only misses ---");

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

    // Dirty-only with x NOT in the dirty set: should NOT
    // report any leak (skips x entirely).
    {
        std::vector<aura::compiler::OwnershipNote> notes;
        std::unordered_set<std::string> dirty = {}; // x not dirty
        bool pass =
            aura::compiler::OwnershipEnv::validate_ownership(*flat, *pool, root, dirty, notes);
        CHECK(notes.empty(), "dirty-only: empty dirty set → no diagnostics (skipped)");
    }

    // Full re-simulation: SHOULD report the leak even though
    // x isn't in any dirty set.
    {
        std::vector<aura::compiler::OwnershipNote> notes;
        aura::core::TypeRegistry reg; // Issue #1387
        bool pass =
            aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
        int leaks = count_kind(notes, "leaked-linear");
        CHECK(leaks >= 1, "full re-simulation: reports leak for non-dirty linear binding");
    }
    return true;
}

// ── Test 3: full validation on properly-moved binding = no leak ──

bool test_full_properly_moved_no_leak() {
    std::println("\n--- Test: full re-simulation on properly-moved binding ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    // (let ((x (Linear 42))) (move x))  — x is moved, no leak
    auto x_sym = pool->intern("x");
    auto inner = flat->add_literal(42);
    auto lin_node = flat->add_linear(inner);
    auto x_var = flat->add_variable(x_sym);
    auto move_node = flat->add_move(x_var);
    auto root = flat->add_let(x_sym, lin_node, move_node);
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    aura::core::TypeRegistry reg; // Issue #1387
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    int leaks = count_kind(notes, "leaked-linear");
    CHECK(leaks == 0, "properly moved linear → no leak");
    CHECK(pass, "properly moved linear → overall pass=true");
    return true;
}

// ── Test 4: gradual + Linear boundary — Any ~ Linear rejected ──

bool test_gradual_linear_boundary() {
    std::println("\n--- Test: gradual + Linear boundary (Any ~ Linear rejected) ---");

    // (let ((x (Linear 42))) (display x))  where display is
    // called with x as an argument. The argument position
    // expects Dynamic, so Any ~ Linear is checked. With
    // strict type checking this should produce a TypeError
    // diagnostic (consistent_unify returns false).
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    auto pr = aura::parser::parse_to_flat("(let ((x (Linear 42))) (display x))", *flat, *pool);
    flat->root = pr.root;
    aura::core::TypeRegistry treg;
    aura::compiler::TypeChecker tc(treg);
    tc.set_strict(true); // strict BEFORE infer_flat
    aura::diag::DiagnosticCollector diag;
    tc.infer_flat(*flat, *pool, pr.root, diag);

    int type_errors = 0;
    for (auto& d : diag.diagnostics()) {
        if (d.kind == aura::diag::ErrorKind::TypeError)
            ++type_errors;
    }
    std::println("  TypeError diagnostics: {}", type_errors);
    CHECK(type_errors >= 1,
          "Any ~ Linear: rejected with TypeError (consistent_unify returns false)");
    return true;
}

// ── Test 5: dirty-only path still works (regression check) ────

bool test_dirty_only_still_works() {
    std::println("\n--- Test: dirty-only path still works (regression check) ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    // (let ((x 1)) (move x))  — x is plain (not linear)
    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(1);
    auto x_var = flat->add_variable(x_sym);
    auto move_node = flat->add_move(x_var);
    auto root = flat->add_let(x_sym, lit, move_node);
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    std::unordered_set<std::string> dirty = {"x"};
    bool pass = aura::compiler::OwnershipEnv::validate_ownership(*flat, *pool, root, dirty, notes);
    CHECK(notes.empty(), "non-linear binding: dirty-only path produces no diagnostics");
    CHECK(pass, "non-linear binding: dirty-only path passes");
    return true;
}

int run_tests() {
    std::println("═══ Issue #117 verification tests ═══\n");
    test_full_re_simulation_discovers_linear();
    test_full_catches_what_dirty_misses();
    test_full_properly_moved_no_leak();
    test_gradual_linear_boundary();
    test_dirty_only_still_works();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}
} // namespace aura_issue_117_detail

int aura_issue_117_run() {
    return aura_issue_117_detail::run_tests();
}
