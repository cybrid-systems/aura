// @category: unit
// @reason: pure C++ OwnershipEnv + TypeRegistry; no CompilerService
//
// test_issue_1417.cpp — Issue #1417: Linear ∩ Refinement
// type-driven discovery in validate_ownership_full.
//
// Background: validate_ownership_full must discover linear
// bindings from TypeRegistry (not only syntactic `(Linear e)`),
// and correctly treat move/drop as consuming ownership (no leak).
// Core type-driven discovery landed in #1387/#1410; this issue
// closes the AC surface: move path, drop path, TypeAnnotation
// carriers, Let-node type_id, defense-in-depth, and #117 baseline.
//
// ACs (from issue body):
//   AC1: type-driven + (move x) → no leaked-linear (Moved)
//   AC2: TypeAnnotation-style linear + (drop x) → no leaked-linear
//   AC3: type-driven without move/drop → leaked-linear (discovery)
//   AC4: defense-in-depth (syntactic + type-driven both fire)
//   AC5: Let-node type_id alone discovers linear (no value stamp)
//   AC6: no regression vs test_issue_117 syntactic baseline

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.type_checker;

namespace test_issue_1417_detail {

int count_kind(const std::vector<aura::compiler::OwnershipNote>& notes, const std::string& kind) {
    int n = 0;
    for (const auto& note : notes) {
        if (note.kind == kind)
            ++n;
    }
    return n;
}

// ── AC1: type-driven + move → no leak ───────────────────────────
//
// (let ((x <linear-typed 42>)) (move x))
// No Linear wrapper; value stamped with register_linear(Int).
// After move, ownership is Moved → no leaked-linear.
bool test_ac1_type_driven_move_no_leak() {
    std::println("\n--- AC1: type-driven + move → no leaked-linear ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    auto linear_tid = reg.register_linear(reg.int_type());
    const std::uint32_t linear_raw = static_cast<std::uint32_t>(linear_tid.index);

    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(42);
    flat->set_type(lit, linear_raw);
    auto x_var = flat->add_variable(x_sym);
    auto move_node = flat->add_move(x_var);
    auto root = flat->add_let(x_sym, lit, move_node);
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC1: pass={} leaks={}", pass, leaks);
    CHECK(leaks == 0, "AC1: type-driven linear + move → no leaked-linear");
    CHECK(pass, "AC1: overall pass=true after move");
    return true;
}

// ── AC2: TypeAnnotation carrier + drop → no leak ────────────────
//
// Simulates (let ((x (: LinearNumber 5))) (drop x)):
// value is TypeAnnotation with type_id stamped linear.
// drop consumes ownership → no leaked-linear.
bool test_ac2_type_annotation_drop_no_leak() {
    std::println("\n--- AC2: TypeAnnotation + drop → no leaked-linear ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    auto linear_tid = reg.register_linear(reg.int_type());
    const std::uint32_t linear_raw = static_cast<std::uint32_t>(linear_tid.index);

    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(5);
    // Type name matches register_linear's default name so name-lookup
    // path can also fire; type_id stamp is the primary post-infer signal.
    auto type_name = pool->intern(std::string(reg.name_of(linear_tid)));
    auto annot = flat->add_type_annotation(type_name, lit);
    flat->set_type(annot, linear_raw);

    auto x_var = flat->add_variable(x_sym);
    auto drop_node = flat->add_drop(x_var);
    auto root = flat->add_let(x_sym, annot, drop_node);
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC2: pass={} leaks={} type_name={}", pass, leaks, reg.name_of(linear_tid));
    CHECK(leaks == 0, "AC2: TypeAnnotation linear + drop → no leaked-linear");
    CHECK(pass, "AC2: overall pass=true after drop");
    return true;
}

// ── AC3: type-driven without consume → leak ─────────────────────
//
// Discovery must still fire: linear-typed binding that is only
// displayed (not moved/dropped) reports leaked-linear.
bool test_ac3_type_driven_leak_when_not_consumed() {
    std::println("\n--- AC3: type-driven without move/drop → leaked-linear ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    auto linear_tid = reg.register_linear(reg.int_type());
    const std::uint32_t linear_raw = static_cast<std::uint32_t>(linear_tid.index);

    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(42);
    flat->set_type(lit, linear_raw);
    auto disp_sym = pool->intern("display");
    auto disp_var = flat->add_variable(disp_sym);
    auto x_var = flat->add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat->add_call(disp_var, disp_args);
    auto root = flat->add_let(x_sym, lit, disp_call);
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC3: pass={} leaks={}", pass, leaks);
    CHECK(leaks >= 1, "AC3: type-driven path discovers linear and reports leak");
    CHECK(!pass, "AC3: overall pass=false when leak present");
    return true;
}

// ── AC4: defense-in-depth ───────────────────────────────────────
//
// Both Linear wrapper AND linear type_id present. With display
// body (no move): exactly one leak (set union, not double-count).
// With move body: no leak (either path would have tracked x).
bool test_ac4_defense_in_depth() {
    std::println("\n--- AC4: defense-in-depth (wrapper + type) ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    auto linear_tid = reg.register_linear(reg.int_type());
    const std::uint32_t linear_raw = static_cast<std::uint32_t>(linear_tid.index);

    auto x_sym = pool->intern("x");
    auto inner = flat->add_literal(42);
    flat->set_type(inner, linear_raw);
    auto lin_node = flat->add_linear(inner);

    // Leak case: display only
    {
        auto disp_sym = pool->intern("display");
        auto disp_var = flat->add_variable(disp_sym);
        auto x_var = flat->add_variable(x_sym);
        aura::ast::NodeId disp_args[] = {disp_var, x_var};
        auto disp_call = flat->add_call(disp_var, disp_args);
        auto root = flat->add_let(x_sym, lin_node, disp_call);
        flat->root = root;

        std::vector<aura::compiler::OwnershipNote> notes;
        (void)aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
        const int leaks = count_kind(notes, "leaked-linear");
        std::println("  AC4.leak: leaks={}", leaks);
        CHECK(leaks == 1, "AC4: set union → exactly 1 leak (not 2)");
    }

    // Consume case: move
    {
        auto arena2 = std::make_unique<aura::ast::ASTArena>();
        auto alloc2 = arena2->allocator();
        auto* flat2 = arena2->create<aura::ast::FlatAST>(alloc2);
        auto* pool2 = arena2->create<aura::ast::StringPool>(alloc2);
        aura::core::TypeRegistry reg2;
        auto linear_tid2 = reg2.register_linear(reg2.int_type());
        auto x_sym2 = pool2->intern("x");
        auto inner2 = flat2->add_literal(42);
        flat2->set_type(inner2, static_cast<std::uint32_t>(linear_tid2.index));
        auto lin2 = flat2->add_linear(inner2);
        auto x_var2 = flat2->add_variable(x_sym2);
        auto move2 = flat2->add_move(x_var2);
        auto root2 = flat2->add_let(x_sym2, lin2, move2);
        flat2->root = root2;

        std::vector<aura::compiler::OwnershipNote> notes;
        bool pass = aura::compiler::OwnershipEnv::validate_ownership_full(*flat2, *pool2, reg2,
                                                                          root2, notes);
        const int leaks = count_kind(notes, "leaked-linear");
        std::println("  AC4.move: pass={} leaks={}", pass, leaks);
        CHECK(leaks == 0, "AC4: both paths + move → no leak");
        CHECK(pass, "AC4: overall pass=true");
    }
    return true;
}

// ── AC5: Let-node type_id alone ─────────────────────────────────
//
// Value has no type_id and no Linear wrapper; only the Let node
// itself is stamped linear. Discovery pattern 3 must catch it.
bool test_ac5_let_node_type_id() {
    std::println("\n--- AC5: Let-node type_id discovers linear ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    auto linear_tid = reg.register_linear(reg.int_type());
    const std::uint32_t linear_raw = static_cast<std::uint32_t>(linear_tid.index);

    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(7); // no type stamp on value
    auto disp_sym = pool->intern("display");
    auto disp_var = flat->add_variable(disp_sym);
    auto x_var = flat->add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat->add_call(disp_var, disp_args);
    auto root = flat->add_let(x_sym, lit, disp_call);
    flat->set_type(root, linear_raw); // stamp Let node only
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC5: pass={} leaks={}", pass, leaks);
    CHECK(leaks >= 1, "AC5: Let-node type_id alone discovers linear binding");
    CHECK(!pass, "AC5: overall pass=false when leak present");
    return true;
}

// ── AC6: syntactic baseline (mirrors test_issue_117) ────────────
//
// No type registry linear entries; only (Linear 42) wrapper.
// Must still report leak (no regression).
bool test_ac6_syntactic_baseline() {
    std::println("\n--- AC6: syntactic Linear wrapper baseline (#117) ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg; // empty — type-driven silent

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
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  AC6: pass={} leaks={}", pass, leaks);
    CHECK(leaks >= 1, "AC6: syntactic path still reports leaked-linear");
    CHECK(!pass, "AC6: overall pass=false");
    return true;
}

// Bonus: TypeAnnotation name-lookup path without type_id stamp
// on the annotation node (only name in registry is linear).
bool test_type_annotation_name_lookup() {
    std::println("\n--- bonus: TypeAnnotation name lookup without type stamp ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);
    aura::core::TypeRegistry reg;

    auto linear_tid = reg.register_linear(reg.int_type());
    // Use the exact name register_linear put into name_to_id_.
    auto type_name = pool->intern(std::string(reg.name_of(linear_tid)));

    auto x_sym = pool->intern("x");
    auto lit = flat->add_literal(5);
    auto annot = flat->add_type_annotation(type_name, lit);
    // Intentionally do NOT set_type on annot — name lookup only.

    auto disp_sym = pool->intern("display");
    auto disp_var = flat->add_variable(disp_sym);
    auto x_var = flat->add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat->add_call(disp_var, disp_args);
    auto root = flat->add_let(x_sym, annot, disp_call);
    flat->root = root;

    std::vector<aura::compiler::OwnershipNote> notes;
    bool pass =
        aura::compiler::OwnershipEnv::validate_ownership_full(*flat, *pool, reg, root, notes);
    const int leaks = count_kind(notes, "leaked-linear");
    std::println("  name-lookup: pass={} leaks={} name={}", pass, leaks, reg.name_of(linear_tid));
    CHECK(leaks >= 1, "TypeAnnotation type-name lookup discovers linear without type_id stamp");
    return true;
}

} // namespace test_issue_1417_detail

int aura_issue_1417_run() {
    using namespace test_issue_1417_detail;
    std::println("=== Issue #1417: Linear ∩ Refinement type-driven discovery ===");
    bool all_ok = true;
    all_ok &= test_ac1_type_driven_move_no_leak();
    all_ok &= test_ac2_type_annotation_drop_no_leak();
    all_ok &= test_ac3_type_driven_leak_when_not_consumed();
    all_ok &= test_ac4_defense_in_depth();
    all_ok &= test_ac5_let_node_type_id();
    all_ok &= test_ac6_syntactic_baseline();
    all_ok &= test_type_annotation_name_lookup();
    if (all_ok && g_failed == 0) {
        std::println("\n=== ALL ACs PASS ===");
        return 0;
    }
    std::println("\n=== Some ACs FAILED (g_failed={}) ===", g_failed);
    return 1;
}

int main() {
    return aura_issue_1417_run();
}
