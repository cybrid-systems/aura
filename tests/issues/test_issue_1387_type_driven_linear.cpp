// @category: integration
// @reason: uses OwnershipEnv + TypeRegistry + FlatAST to verify
//          type-driven linear binding discovery.
//
// test_issue_1387_type_driven_linear.cpp — Issue #1387:
// validate_ownership_full discovers type-driven linear bindings
// (not just syntactic `(Linear ...)` wrappers).
//
// Background: validate_ownership_full walked for `(let ((x
// (Linear e))) ...)` and missed bindings whose type was registered
// linear in TypeRegistry but whose AST value had no Linear wrapper.
// The fix adds type-driven discovery via `reg.linear_of(type_id)`
// alongside the existing syntactic check (defense in depth).
//
// Tests:
//   AC1: type-driven — binding whose value's type is linear in
//        registry, no Linear wrapper → discovered.
//   AC2: syntactic — original `(Linear e)` wrapper still works.
//   AC3: defense in depth — both paths combined via set union.
//   AC4: non-linear type — binding whose type is NOT linear →
//        not discovered (no false positive).

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core;
import aura.core.type;
import aura.compiler.type_checker;

namespace aura_issue_1387_detail {

// Helper: count notes of a specific kind.
static int count_kind(const std::vector<aura::compiler::OwnershipNote>& notes,
                      const std::string& kind) {
    int n = 0;
    for (const auto& nt : notes)
        if (nt.kind == kind)
            ++n;
    return n;
}

// Build (let ((x INNER)) (display x)) and return root. x is
// interned as "x", display as "display".
static aura::ast::NodeId build_let_x_display(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                             aura::ast::NodeId inner) {
    auto x_sym = pool.intern("x");
    auto disp_sym = pool.intern("display");
    auto disp_var = flat.add_variable(disp_sym);
    auto x_var = flat.add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat.add_call(disp_var, disp_args);
    auto root = flat.add_let(x_sym, inner, disp_call);
    flat.root = root;
    return root;
}

// ── AC1: type-driven discovery ─────────────────────────────────
//
// Build: (let ((x 42)) (display x))
// No Linear wrapper. But set x's type_id to a TypeId registered
// as linear in TypeRegistry. validate_ownership_full must
// discover x as linear and report a leak.
bool test_ac1_type_driven_discovers_linear() {
    std::println("\n--- AC1: type-driven discovery (no Linear wrapper) ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    // Build AST first (need a flat to operate on).
    auto inner = flat->add_literal(42);
    auto root = build_let_x_display(*flat, *pool, inner);

    // Stamp x's type_id with a linear type. Register on a
    // separate TypeRegistry (matches the parameter signature).
    aura::core::TypeRegistry reg;
    auto linear_tid = reg.register_linear(reg.int_type());
    flat->set_type(inner, linear_tid.index);

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC1: pass=*** leaks={}", pass, leaks);
    CHECK(leaks >= 1, "AC1: type-driven linear binding discovered (leak reported)");
    CHECK(!pass, "AC1: overall pass=*** when leak present");
    return true;
}

// ── AC2: syntactic Linear wrapper still works ───────────────────
//
// Regression: existing Linear wrapper discovery must not break.
bool test_ac2_syntactic_still_works() {
    std::println("\n--- AC2: syntactic Linear wrapper still works ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto inner = flat->add_literal(42);
    auto lin_node = flat->add_linear(inner);
    auto root = build_let_x_display(*flat, *pool, lin_node);

    std::vector<aura::compiler::OwnershipNote> notes;
    aura::core::TypeRegistry reg; // empty — type-driven path silent
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC2: pass=*** leaks={}", pass, leaks);
    CHECK(leaks >= 1, "AC2: syntactic Linear wrapper still discovered");
    CHECK(!pass, "AC2: overall pass=***");
    return true;
}

// ── AC3: defense in depth (both paths via set union) ────────────
//
// Build a binding with BOTH a Linear wrapper AND a linear-typed
// value. validate_ownership_full must discover x once (set union,
// not double-counted).
bool test_ac3_defense_in_depth() {
    std::println("\n--- AC3: defense in depth (Linear wrapper + linear type) ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto inner = flat->add_literal(42);
    auto lin_node = flat->add_linear(inner); // ALSO wrap
    auto root = build_let_x_display(*flat, *pool, lin_node);

    aura::core::TypeRegistry reg;
    auto linear_tid = reg.register_linear(reg.int_type());
    flat->set_type(inner, linear_tid.index);

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC3: pass=*** leaks={}", pass, leaks);
    CHECK(leaks == 1, "AC3: set union → exactly 1 leak (not 2)");
    return true;
}

// ── AC4: non-linear type → no false positive ────────────────────
//
// Binding whose value's type is NOT linear in registry, no
// Linear wrapper. Must NOT report a leak (x isn't linear).
bool test_ac4_non_linear_no_false_positive() {
    std::println("\n--- AC4: non-linear type → no false positive ---");

    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto inner = flat->add_literal(42);
    auto root = build_let_x_display(*flat, *pool, inner);

    aura::core::TypeRegistry reg;
    // Don't register as linear. Stamp with plain int type id.
    flat->set_type(inner, reg.int_type().index);

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC4: pass=*** leaks={}", pass, leaks);
    CHECK(leaks == 0, "AC4: non-linear type → no false leak report");
    CHECK(pass, "AC4: overall pass=***");
    return true;
}

} // namespace aura_issue_1387_detail

int main() {
    using namespace aura_issue_1387_detail;
    bool ok = true;
    ok &= test_ac1_type_driven_discovers_linear();
    ok &= test_ac2_syntactic_still_works();
    ok &= test_ac3_defense_in_depth();
    ok &= test_ac4_non_linear_no_false_positive();

    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1387 type-driven linear: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}