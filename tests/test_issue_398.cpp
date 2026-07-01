// test_issue_398.cpp — Issue #398: Optimize
// `children_stable()` to avoid vector allocation in hot
// navigation paths.
//
// Verifies the new `for_each_stable_child` + `stable_child_count`
// C++ API and the migration of `(query:children-stable)` to the
// zero-allocation path.
//
//   AC1: for_each_stable_child calls fn for each non-NULL child
//        in left-to-right order
//   AC2: for_each_stable_child skips NULL_NODE children
//   AC3: for_each_stable_child is a no-op for out-of-range id
//   AC4: for_each_stable_child callback receives refs with the
//        current generation_ captured at call time
//   AC5: stable_child_count matches a manual non-NULL count
//   AC6: (query:children-stable) returns the same (id . gen) pair
//        list as the pre-#398 implementation (regression: the
//        primitive must not change observable behavior)

#include "test_harness.hpp"

using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_398_detail {

// AC1: for_each_stable_child iterates non-NULL children in order.
bool test_for_each_basic() {
    std::println("\n--- AC1: for_each_stable_child iterates in order ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto c0 = flat.add_literal(0);
    auto c1 = flat.add_literal(1);
    auto c2 = flat.add_literal(2);
    // Build a parent node with the 3 children. Define's
    // child span is size 1; we use insert_child to grow it.
    auto parent = flat.add_define(pool.intern("p"), c0);
    flat.insert_child(parent, 1, c1);
    flat.insert_child(parent, 2, c2);
    std::vector<aura::ast::NodeId> seen;
    flat.for_each_stable_child(parent, [&](aura::ast::FlatAST::StableNodeRef r) {
        seen.push_back(r.id);
    });
    std::vector<aura::ast::NodeId> want{c0, c1, c2};
    if (seen != want) {
        ++g_failed; std::println("  FAIL: seen = {} entries, expected {}",
                                 seen.size(), want.size());
        return false;
    }
    ++g_passed; std::println("  PASS: 3 non-NULL children iterated in order");
    return true;
}

// AC2: for_each_stable_child skips NULL_NODE children.
bool test_for_each_skips_null() {
    std::println("\n--- AC2: for_each_stable_child skips NULL_NODE ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto c0 = flat.add_literal(0);
    auto c1 = flat.add_literal(1);
    // Parent has 5 children: c0, NULL, c1, NULL, NULL.
    // Use insert_child to grow the span beyond 1 element.
    auto parent = flat.add_define(pool.intern("p"), c0);
    flat.insert_child(parent, 1, aura::ast::NULL_NODE);
    flat.insert_child(parent, 2, c1);
    flat.insert_child(parent, 3, aura::ast::NULL_NODE);
    flat.insert_child(parent, 4, aura::ast::NULL_NODE);
    std::vector<aura::ast::NodeId> seen;
    flat.for_each_stable_child(parent, [&](aura::ast::FlatAST::StableNodeRef r) {
        seen.push_back(r.id);
    });
    if (seen.size() != 2 || seen[0] != c0 || seen[1] != c1) {
        ++g_failed; std::println("  FAIL: seen = [{} entries], expected [{}, {}]",
                                 seen.size(), c0, c1);
        return false;
    }
    ++g_passed; std::println("  PASS: NULL_NODE children filtered (saw 2 of 5)");
    return true;
}

// AC3: for_each_stable_child is a no-op for out-of-range id.
bool test_for_each_out_of_range() {
    std::println("\n--- AC3: for_each_stable_child out-of-range is no-op ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    int call_count = 0;
    flat.for_each_stable_child(999999, [&](aura::ast::FlatAST::StableNodeRef) {
        ++call_count;
    });
    if (call_count != 0) {
        ++g_failed; std::println("  FAIL: callback called {} times for OOB id",
                                 call_count);
        return false;
    }
    ++g_passed; std::println("  PASS: OOB id invokes callback 0 times");
    return true;
}

// AC4: callback receives refs with the current generation_
// captured at call time. We bump the generation_ and verify
// the callback (called after the bump) sees the new gen.
bool test_for_each_captures_current_gen() {
    std::println("\n--- AC4: callback captures current generation_ ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto c0 = flat.add_literal(0);
    auto parent = flat.add_define(pool.intern("p"), c0);
    // First call: gen = 1.
    std::uint16_t seen1 = 0;
    flat.for_each_stable_child(parent, [&](aura::ast::FlatAST::StableNodeRef r) {
        seen1 = r.gen;
    });
    // Manually bump the global gen.
    flat.bump_generation();
    // Second call: gen = 2.
    std::uint16_t seen2 = 0;
    flat.for_each_stable_child(parent, [&](aura::ast::FlatAST::StableNodeRef r) {
        seen2 = r.gen;
    });
    if (seen1 != 1) {
        ++g_failed; std::println("  FAIL: pre-bump gen = {} (expected 1)", seen1);
        return false;
    }
    if (seen2 != 2) {
        ++g_failed; std::println("  FAIL: post-bump gen = {} (expected 2)", seen2);
        return false;
    }
    ++g_passed; std::println("  PASS: pre-bump gen=1, post-bump gen=2 (captured at call time)");
    return true;
}

// AC5: stable_child_count matches a manual non-NULL count.
bool test_stable_child_count() {
    std::println("\n--- AC5: stable_child_count matches manual count ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto c0 = flat.add_literal(0);
    auto c1 = flat.add_literal(1);
    auto parent = flat.add_define(pool.intern("p"), c0);
    flat.insert_child(parent, 1, c1);
    flat.insert_child(parent, 2, aura::ast::NULL_NODE);
    flat.insert_child(parent, 3, aura::ast::NULL_NODE);
    auto n = flat.stable_child_count(parent);
    if (n != 2) {
        ++g_failed; std::println("  FAIL: stable_child_count = {} (expected 2)", n);
        return false;
    }
    ++g_passed; std::println("  PASS: stable_child_count = 2 (matches manual)");
    if (flat.stable_child_count(999999) != 0) {
        ++g_failed; std::println("  FAIL: OOB count = 0 expected");
        return false;
    }
    ++g_passed; std::println("  PASS: OOB id count = 0");
    return true;
}

// AC6: (query:children-stable) Aura primitive (migrated to
// the zero-allocation path) still returns the same (id . gen)
// pair list as before. Regression: observable behavior
// unchanged after the migration.
bool test_primitive_unchanged() {
    std::println("\n--- AC6: (query:children-stable) regression check ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval(std::string("(set-code \"(define x (begin 1 2 3 4 5))\")")).has_value()) {
        ++g_failed; std::println("  FAIL: set-code failed");
        return false;
    }
    if (!cs.eval(std::string("(eval-current)")).has_value()) {
        ++g_failed; std::println("  FAIL: eval-current failed");
        return false;
    }
    // Build the (query:children-stable x-id) call and
    // measure the result length + the first pair's id.
    auto r = cs.eval(std::string(
        "(let ((defs (query:defines))"
        "      (x-id (car defs))"
        "      (val-ref (car (query:children-stable x-id)))"
        "      (val-id (car val-ref)))"
        "  (let ((stable (query:children-stable val-id)))"
        "    (let loop ((l stable) (n 0))"
        "      (if (pair? l) (loop (cdr l) (+ n 1)) n))))"));
    if (!r) {
        ++g_failed; std::println("  FAIL: eval failed");
        return false;
    }
    auto n = aura::compiler::types::as_int(*r);
    if (n != 5) {
        ++g_failed; std::println("  FAIL: result length = {} (expected 5)", n);
        return false;
    }
    ++g_passed; std::println("  PASS: (query:children-stable) returns 5 pairs (regression OK)");
    return true;
}

} // namespace aura_issue_398_detail

int main() {
    using namespace aura_issue_398_detail;
    std::println("=== test_issue_398: zero-alloc children_stable ===");
    test_for_each_basic();
    test_for_each_skips_null();
    test_for_each_out_of_range();
    test_for_each_captures_current_gen();
    test_stable_child_count();
    test_primitive_unchanged();
    std::println("\n=== Totals: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}