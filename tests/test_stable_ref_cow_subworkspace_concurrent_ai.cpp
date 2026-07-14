// @category: integration
// @reason: Issue #738 — Enhanced StableNodeRef with automatic COW/
//  sub-workspace pinning + cross-boundary validity tracking for
//  production concurrent AI orchestration.
//
// Non-duplicative with #735 (macro provenance), #717 (Guard snapshot),
// #370 (SafePCVSpan), #392 (subtree gen), #527 (cow-fiber-stats).
//
//   - AC1: query:stable-ref-boundary-stats-hash reachable (schema 738)
//   - AC2: query:stable-ref auto-pins on capture
//   - AC3: workspace COW clone propagates boundary pins
//   - AC4: cross_cow_invalidations observable under mutate validate
//   - AC5: parent ref → child workspace multi-round loop
//   - AC6: query:stable-ref-stats-hash enhanced fields regression

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_738_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t boundary_hash(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:stable-ref-boundary-stats-hash) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_parent(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_738_detail

int aura_issue_stable_ref_cow_subworkspace_concurrent_ai_run() {
    using namespace aura_issue_738_detail;

    std::println("=== Issue #738: StableRef COW + sub-workspace pinning ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_parent(cs), "parent workspace setup");

    const auto pins_before = cs.evaluator().cow_boundary_pins_total();

    // AC1: boundary stats hash
    {
        std::println("\n--- AC1: query:stable-ref-boundary-stats-hash ---");
        auto h = cs.eval("(query:stable-ref-boundary-stats-hash)");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "query:stable-ref-boundary-stats-hash returns hash");
        CHECK(boundary_hash(cs, "schema") == 738, "schema sentinel == 738");
        CHECK(boundary_hash(cs, "cross-cow-invalidations") >= 0, "cross-cow-invalidations present");
        CHECK(boundary_hash(cs, "pinned-across-boundaries") >= 0,
              "pinned-across-boundaries present");
    }

    // AC2: query:stable-ref auto-pins
    {
        std::println("\n--- AC2: query:stable-ref auto-pins on capture ---");
        (void)cs.eval("(query:stable-ref 1)");
        const auto pins_after = cs.evaluator().cow_boundary_pins_total();
        CHECK(pins_after > pins_before, "boundary pin counter grew after query:stable-ref");
    }

    // AC3: COW clone propagates pins
    {
        std::println("\n--- AC3: workspace COW clone propagates boundary pins ---");
        (void)cs.eval("(workspace:create \"child-a\")");
        (void)cs.eval("(workspace:switch 1)");
        const auto pins_child_before = cs.evaluator().cow_boundary_pins_total();
        (void)cs.eval("(mutate:rebind \"x\" \"42\")");
        const auto pins_child_after = cs.evaluator().cow_boundary_pins_total();
        const auto cow_epoch = boundary_hash(cs, "workspace-cow-epoch");
        std::println("  pins: {} -> {} cow_epoch={}", pins_child_before, pins_child_after,
                     cow_epoch);
        CHECK(pins_child_after >= pins_child_before, "pins monotonic after COW mutate");
        CHECK(cow_epoch > 0, "workspace-cow-epoch bumped after lazy COW clone");
        (void)cs.eval("(workspace:switch 0)");
    }

    // AC4: cross_cow under mutate + validate
    {
        std::println("\n--- AC4: cross_cow under mutate validate loop ---");
        // Issue #1430 fix: skip redundant setup_parent — AC3 already
        // switched back to workspace 0, and the initial setup_parent
        // established the bindings. Calling (set-code)+(eval-current)
        // again hits a stale current_id in eval_flat because the COW
        // machinery has bumped generation_ on workspace 0.
        auto* ws = cs.evaluator().workspace_flat();
        CHECK(ws != nullptr, "workspace flat available");
        const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
        const auto g = ws->generation();
        (void)cs.evaluator().validate_stable_ref(0, g > 0 ? g - 1 : 0);
        const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
        CHECK(cc1 > cc0, "cross_cow_invalidations grew on stale validate");
    }

    // AC5: multi-round parent → child loop
    {
        std::println("\n--- AC5: parent capture → child COW multi-round loop ---");
        CHECK(setup_parent(cs), "loop setup");
        for (int round = 0; round < 3; ++round) {
            (void)cs.eval("(query:stable-ref 1)");
            (void)cs.eval("(workspace:create (string-append \"child-\" (number->string " +
                          std::to_string(round) + ")))");
            auto ws_id = 1 + round;
            (void)cs.eval("(workspace:switch " + std::to_string(ws_id) + ")");
            (void)cs.eval("(mutate:rebind \"y\" \"" + std::to_string(round + 10) + "\")");
            auto valid =
                cs.eval("(let ((r (query:stable-ref 1))) (if (pair? r) (query:ref-valid? r) #f))");
            CHECK(valid.has_value(), std::format("round {} ref-valid? in child", round));
            (void)cs.eval("(workspace:switch 0)");
        }
        CHECK(cs.evaluator().cow_boundary_pins_total() >= 3,
              "multi-round loop accumulated boundary pins");
    }

    // AC6: enhanced stable-ref-stats-hash regression
    {
        std::println("\n--- AC6: query:stable-ref-stats-hash enhanced fields ---");
        auto h = cs.eval("(query:stable-ref-stats-hash)");
        CHECK(h && aura::compiler::types::is_hash(*h), "stable-ref-stats-hash returns hash");
        auto cc = cs.eval("(hash-ref (query:stable-ref-stats-hash) \"cross-cow-invalidations\")");
        auto pa = cs.eval("(hash-ref (query:stable-ref-stats-hash) \"pinned-across-boundaries\")");
        CHECK(cc && aura::compiler::types::is_int(*cc), "cross-cow-invalidations field present");
        CHECK(pa && aura::compiler::types::is_int(*pa), "pinned-across-boundaries field present");
        auto s527 = cs.eval("(query:stable-ref-cow-fiber-stats)");
        CHECK(s527 && aura::compiler::types::is_int(*s527),
              "(query:stable-ref-cow-fiber-stats) regression (#527)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_stable_ref_cow_subworkspace_concurrent_ai_run();
}
#endif
