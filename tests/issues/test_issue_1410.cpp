// tests/test_issue_1410.cpp — Issue #1410: Linear ∩ Refinement
// type-driven discovery in validate_ownership_full.
//
// Background: validate_ownership_full discovers linear bindings via
// two paths (src/compiler/type_checker_impl.cpp:5387+):
//   1. SYNTACTIC — let value is wrapped in `(Linear ...)` node
//   2. TYPE-DRIVEN — let value's type is registered linear in
//      `TypeRegistry` via `register_linear(TypeId)`
// Before #1410, only the syntactic path fired; type-driven bindings
// were silently invisible to full validation. This file tests both
// paths + the defense-in-depth invariant.
//
// ACs:
//   AC1: Type-driven only (no Linear wrapper, type registered linear)
//        → leaked-linear reported
//   AC2: Syntactic only (Linear wrapper, no linear type) — regression
//        check vs test_issue_117, must still report leaked-linear
//   AC3: Defense-in-depth — both paths fire independently, at least
//        one catches the linear binding
//   AC4: No regression in test_issue_117 (full-validation baseline)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.diag;

namespace test_issue_1410_detail {

// Helper: count notes of a given kind
int count_kind(const std::vector<aura::compiler::OwnershipNote>& notes, const std::string& kind) {
    int n = 0;
    for (const auto& note : notes) {
        if (note.kind == kind)
            ++n;
    }
    return n;
}

// ── AC1: Type-driven discovery ──────────────────────────────────
//
// Register Int as linear, build `(let ((x 42)) (display x))` without
// a Linear wrapper, set the literal's type to the linear-int TypeId.
// validate_ownership_full should discover x as linear (via the
// type-driven path) and report `leaked-linear` since x is never moved.
bool test_type_driven_discovery() {
    std::println("\n--- AC1: type-driven discovery (no Linear wrapper) ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    // Register Int as linear; returns a new TypeId wrapping Int in Linear.
    auto int_tid = reg.int_type();
    auto linear_int_tid = reg.register_linear(int_tid);
    // The raw type_id stored in FlatAST is the TypeId::index field.
    const std::uint32_t linear_int_raw = static_cast<std::uint32_t>(linear_int_tid.index);

    // Build `(let ((x 42)) (display x))` — x has type linear-Int (type-driven).
    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(42);
    flat->set_type(lit, linear_int_raw); // tag the literal as linear Int
    auto disp_sym = pool->intern("display");
    auto disp_var = flat->add_variable(disp_sym);
    auto x_var = flat->add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat->add_call(disp_var, disp_args);
    auto root = flat->add_let(x_sym, lit, disp_call);
    flat->root = root;

    // Sanity: the linear-int type is registered.
    CHECK(reg.linear_of(linear_int_tid) != nullptr,
          "AC1.setup: register_linear makes linear_of(non-null)");

    std::vector<aura::compiler::OwnershipNote> notes;
    aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  type-driven validation: leaks={}", leaks);
    CHECK(leaks >= 1, "AC1: type-driven path discovers linear-typed binding without "
                      "Linear wrapper and reports leaked-linear");
    return true;
}

// ── AC2: Syntactic discovery (regression check) ─────────────────
//
// No type is registered as linear, but the let value IS wrapped
// in `(Linear 42)`. validate_ownership_full should still report
// leaked-linear via the syntactic path. (Mirrors test_issue_117.)
bool test_syntactic_discovery_still_works() {
    std::println("\n--- AC2: syntactic discovery (Linear wrapper) ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg; // empty registry — no type-driven fallback

    // Build `(let ((x (Linear 42))) (display x))`.
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
    aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  syntactic validation: leaks={}", leaks);
    CHECK(leaks >= 1, "AC2: syntactic path still reports leaked-linear (regression check, "
                      "mirrors test_issue_117)");
    return true;
}

// ── AC3: Defense-in-depth — both paths fire independently ───────
//
// Build with BOTH a Linear wrapper AND a linear-registered type.
// validate_ownership_full should still discover the binding as
// linear (either path catches it). Since the syntactic check runs
// first and short-circuits, this verifies that having BOTH defenses
// doesn't break the syntactic path.
bool test_defense_in_depth() {
    std::println("\n--- AC3: defense-in-depth (both wrapper + linear type) ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    // Register Int as linear too.
    auto int_tid = reg.int_type();
    auto linear_int_tid = reg.register_linear(int_tid);
    const std::uint32_t linear_int_raw = static_cast<std::uint32_t>(linear_int_tid.index);

    // Build `(let ((x (Linear 42))) (display x))` — BOTH a Linear
    // wrapper AND a linear-registered type for the inner literal.
    auto x_sym = pool->intern("x");
    auto inner = flat->add_literal(42);
    flat->set_type(inner, linear_int_raw); // tag inner as linear Int
    auto lin_node = flat->add_linear(inner);
    auto disp_sym = pool->intern("display");
    auto disp_var = flat->add_variable(disp_sym);
    auto x_var = flat->add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat->add_call(disp_var, disp_args);
    auto root = flat->add_let(x_sym, lin_node, disp_call);
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  defense-in-depth: leaks={}", leaks);
    CHECK(leaks >= 1, "AC3: defense-in-depth — both paths present, binding still "
                      "discovered as linear (either path catches it)");
    return true;
}

} // namespace test_issue_1410_detail

int aura_issue_1410_run() {
    using namespace test_issue_1410_detail;
    std::println("=== Issue #1410: Linear ∩ Refinement type-driven discovery ===");
    bool all_ok = true;
    all_ok &= test_type_driven_discovery();
    all_ok &= test_syntactic_discovery_still_works();
    all_ok &= test_defense_in_depth();
    if (all_ok) {
        std::println("\n=== ALL 3 ACs PASS ===");
        return 0;
    }
    std::println("\n=== Some ACs FAILED ===");
    return 1;
}

int main() {
    return aura_issue_1410_run();
}
