// @category: integration
// @reason: uses CompilerService + FlatAST dirty/defuse propagation APIs


#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_262_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

bool test_dirty_reason_constants() {
    std::println("\n--- AC1: Issue #262 DirtyReason constants ---");
    using D = aura::ast::FlatAST::DirtyReason;
    CHECK(D::kStructDirty == 0x20, "kStructDirty == 0x20");
    CHECK(D::kDefUseDirty == 0x40, "kDefUseDirty == 0x40");
    CHECK(D::kPpaHintDirty == 0x80, "kPpaHintDirty == 0x80");
    return true;
}

bool test_mark_dirty_upward_until() {
    std::println("\n--- AC2: mark_dirty_upward_until stops at boundary ---");
    auto alloc = std::pmr::polymorphic_allocator<std::byte>{};
    aura::ast::FlatAST flat(alloc);
    auto leaf = flat.add_variable(0);
    auto mid = flat.add_let(0, leaf, leaf);
    auto root = flat.add_begin({mid});
    flat.mark_dirty_upward_until(leaf, aura::ast::FlatAST::kStructDirty, mid);
    CHECK(flat.is_dirty_for(leaf, aura::ast::FlatAST::kStructDirty), "leaf marked struct-dirty");
    CHECK(!flat.is_dirty_for(mid, aura::ast::FlatAST::kStructDirty),
          "stop_at node (mid) not marked");
    CHECK(!flat.is_dirty_for(root, aura::ast::FlatAST::kStructDirty),
          "ancestor above stop_at not marked");
    return true;
}

bool test_mark_dirty_defuse_entries() {
    std::println("\n--- AC3: mark_dirty_defuse_entries sets kDefUseDirty ---");
    auto alloc = std::pmr::polymorphic_allocator<std::byte>{};
    aura::ast::FlatAST flat(alloc);
    auto leaf = flat.add_variable(0);
    auto parent = flat.add_let(0, leaf, leaf);
    std::vector<aura::ast::NodeId> entries{leaf};
    flat.mark_dirty_defuse_entries(entries, aura::ast::FlatAST::kGeneralDirty);
    CHECK(flat.is_dirty_for(leaf, aura::ast::FlatAST::kDefUseDirty), "entry has kDefUseDirty");
    CHECK(flat.is_dirty_for(parent, aura::ast::FlatAST::kDefUseDirty),
          "ancestor has kDefUseDirty propagated");
    return true;
}

bool test_dirty_counts_new_fields() {
    std::println("\n--- AC4: (stats:get \"dirty:counts\") exposes struct/defuse/ppa-hint ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define x 1)\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(hash-ref (stats:get \"dirty:counts\") \"defuse\")");
    CHECK(r.has_value(), "(stats:get \"dirty:counts\") defuse field readable");
    auto r2 = cs.eval("(hash-ref (stats:get \"dirty:counts\") \"struct\")");
    CHECK(r2.has_value(), "(stats:get \"dirty:counts\") struct field readable");
    auto r3 = cs.eval("(hash-ref (stats:get \"dirty:counts\") \"ppa-hint\")");
    CHECK(r3.has_value(), "(stats:get \"dirty:counts\") ppa-hint field readable");
    return true;
}

bool test_rebind_defuse_after_precise_dirty() {
    std::println("\n--- AC5: rebind + def-use query after precise dirty propagation ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1)) (define (g x) (f x))\")") ||
        !cs.eval("(query:def-use \"f\")")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"test262\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(query:def-use \"f\")");
    CHECK(r.has_value(), "query:def-use succeeds after rebind");
    if (r) {
        CHECK(aura::compiler::types::is_pair(*r),
              "query:def-use returns pair after precise dirty propagation");
    }
    return true;
}

int run_tests() {
    std::println("Issue #262 (precise dirty propagation + DefUse incremental)\n");
    test_dirty_reason_constants();
    test_mark_dirty_upward_until();
    test_mark_dirty_defuse_entries();
    test_dirty_counts_new_fields();
    test_rebind_defuse_after_precise_dirty();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_262_detail

int aura_issue_262_run() {
    return aura_issue_262_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_262_run();
}
#endif