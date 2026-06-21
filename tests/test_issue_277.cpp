// @category: integration
// @reason: PPA dirty bitmask + hardware backend mutate hook

#include <atomic>
#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.hardware_backend;

namespace aura_issue_277_detail {

static int64_t run_int(aura::compiler::CompilerService& cs, const std::string& src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

bool test_ppa_dirty_constants() {
    std::println("\n--- AC1: PpaDirtyReason constants ---");
    using P = aura::ast::FlatAST::PpaDirtyReason;
    CHECK(P::kTimingDirty == 0x01, "kTimingDirty == 0x01");
    CHECK(P::kPowerDirty == 0x02, "kPowerDirty == 0x02");
    CHECK(P::kAreaDirty == 0x04, "kAreaDirty == 0x04");
    CHECK(P::kBackendHint == 0x08, "kBackendHint == 0x08");
    return true;
}

bool test_mark_dirty_upward_ppa_propagation() {
    std::println("\n--- AC2: mark_dirty_upward propagates PPA bits ---");
    aura::ast::FlatAST flat;
    auto leaf = flat.add_variable(0);
    auto mid = flat.add_let(0, leaf, leaf);
    auto root = flat.add_begin({mid});
    using P = aura::ast::FlatAST::PpaDirtyReason;
    flat.mark_dirty_upward(leaf, aura::ast::FlatAST::kGeneralDirty,
                           static_cast<std::uint8_t>(P::kTimingDirty | P::kAreaDirty));
    CHECK(flat.is_ppa_dirty_for(leaf, P::kTimingDirty), "leaf timing dirty");
    CHECK(flat.is_ppa_dirty_for(leaf, P::kAreaDirty), "leaf area dirty");
    CHECK(flat.is_ppa_dirty_for(mid, P::kTimingDirty), "ancestor timing dirty");
    CHECK(flat.is_dirty_for(root, aura::ast::FlatAST::kPpaHintDirty),
          "root has kPpaHintDirty mirror");
    (void)root;
    return true;
}

bool test_mark_subtree_dirty_ppa() {
    std::println("\n--- AC3: mark_subtree_dirty propagates PPA ---");
    aura::ast::FlatAST flat;
    auto leaf = flat.add_literal(1);
    auto parent = flat.add_let(0, leaf, leaf);
    using P = aura::ast::FlatAST::PpaDirtyReason;
    flat.mark_subtree_dirty(parent, aura::ast::FlatAST::kGeneralDirty, P::kPowerDirty);
    CHECK(flat.is_ppa_dirty_for(leaf, P::kPowerDirty), "subtree leaf power dirty");
    CHECK(flat.is_ppa_dirty_for(parent, P::kPowerDirty), "subtree root power dirty");
    return true;
}

bool test_replace_value_ppa_hint() {
    std::println("\n--- AC4: mutate:replace-value with ppa-hint ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (f x) (+ x 1))\")")) {
        ++g_failed;
        return false;
    }
    std::atomic<int> hook_calls{0};
    aura::compiler::hardware::register_structural_mutation_hook(
        [&](aura::ast::NodeId, std::uint8_t, std::uint8_t ppa) {
            if (ppa & aura::ast::FlatAST::PpaDirtyReason::kTimingDirty)
                hook_calls.fetch_add(1);
        });
    if (!cs.eval("(mutate:replace-value 2 42 \"ppa-test\" 1)")) {
        aura::compiler::hardware::clear_structural_mutation_hook();
        ++g_failed;
        return false;
    }
    CHECK(hook_calls.load() == 1, "hardware hook fired for timing hint");
    auto timing = run_int(cs, "(hash-ref (dirty:counts) \"timing\")");
    CHECK(timing >= 1, "(dirty:counts) timing >= 1 after replace-value");
    auto ppa_hint = run_int(cs, "(hash-ref (dirty:counts) \"ppa-hint\")");
    CHECK(ppa_hint >= 1, "(dirty:counts) ppa-hint >= 1");
    aura::compiler::hardware::clear_structural_mutation_hook();
    return true;
}

bool test_dirty_ppa_reasons_primitive() {
    std::println("\n--- AC5: (dirty:ppa-reasons) primitive ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (g x) (+ x 2))\")") ||
        !cs.eval("(mutate:replace-value 2 99 \"t\" 5)")) {
        ++g_failed;
        return false;
    }
    auto mask = run_int(cs, "(dirty:ppa-reasons 2)");
    CHECK(mask == 5, "(dirty:ppa-reasons 2) == 5 (timing|area)");
    return true;
}

bool test_dirty_counts_ppa_fields() {
    std::println("\n--- AC6: (dirty:counts) exposes timing/power/area/backend-hint ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define (h x) (+ x 3))\")")) {
        ++g_failed;
        return false;
    }
    CHECK(cs.eval("(hash-ref (dirty:counts) \"timing\")").has_value(),
          "timing field readable");
    CHECK(cs.eval("(hash-ref (dirty:counts) \"power\")").has_value(), "power field readable");
    CHECK(cs.eval("(hash-ref (dirty:counts) \"area\")").has_value(), "area field readable");
    CHECK(cs.eval("(hash-ref (dirty:counts) \"backend-hint\")").has_value(),
          "backend-hint field readable");
    return true;
}

int run_tests() {
    std::println("Issue #277 (PPA dirty bitmask + HW backend hook)\n");
    test_ppa_dirty_constants();
    test_mark_dirty_upward_ppa_propagation();
    test_mark_subtree_dirty_ppa();
    test_replace_value_ppa_hint();
    test_dirty_ppa_reasons_primitive();
    test_dirty_counts_ppa_fields();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_277_detail

int aura_issue_277_run() { return aura_issue_277_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_277_run(); }
#endif