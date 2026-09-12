// tests/orch/test_scope_join_tree_visibility.cpp
// @category: integration
// @reason: Issue #3671 — orch:scope-join-all computed tree_settled for the
//          #3496 drop gate and threw it away: the hash said ok=#t while
//          descendants still lived (three-plane collision with directory /
//          scope-resolve). The hash now carries tree-settled /
//          descendants-live blame fields, and production + !tree + live
//          descendants fail-closed (ok=#f, deny-class=other /
//          deny-detail=descendants-live, #3251 intern). Soft/Off is
//          observe-only (bools populate, no deny intern). C++ join_all
//          default stays local (#3496 AC1 unchanged); drop gate stays
//          settled-only.
//
// Member placement: this is a run_test_* member of test_orch_agent_batch.
// It deliberately does NOT live in test_orch_scope.cpp — that member is
// skip-listed by the batch driver (in-process hang history) and its checks
// would never execute.
//
// Liveness note: the AC1 descendant is driven through the C++ AgentScope
// API (spawn_child + a yield-spinning body behind a hold flag) because the
// language spawn face has no long-lived body primitive; empty-language
// bodies complete before the join window opens. The surface under test is
// still the language orch:scope-join-all deny path.
//
// Source-cite (issue #3671):
//   - src/compiler/evaluator_primitives_agent.cpp: orch:scope-join-all
//     tree_settled_now / descendants_live / join_guard_deny + deny intern
//     (by-value add_deny_class capture — a reference capture of the
//     register-scope local dangles after registration returns).
//   - scripts/check_scope_join_tree_visibility_3671.py: ship linter.
//
// No docs/design/ per #1655 / #1485.

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"

#include <atomic>
#include <format>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::orch::find_agent_scope;
using aura::orch::reset_all_agent_scopes_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

int run_test_scope_join_tree_visibility() {
    // ── Issue #3671: join-all publishes tree_settled; production
    // fail-closed on non-tree joins with live descendants ──
    std::atomic<bool> hold{true};
    {
        std::println("\n--- #3671: join-all tree visibility + descendants-live guard ---");
        reset_all_agent_scopes_for_test();
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CompilerService cs;

        // Root scope via language, then a live descendant driven from C++:
        // child scope + yield-spinning body held open by `hold`.
        CHECK(
            cs.eval(R"((let ((r (orch:scope-spawn "3671-root"))) (hash-ref r "ok")))").has_value(),
            "3671: root spawn");
        auto* root = find_agent_scope(static_cast<void*>(&cs.evaluator()));
        CHECK(root != nullptr, "3671: root scope resolvable");
        if (root) {
            auto& child = root->spawn_child();
            child.spawn({.name = std::format("3671-child-agent"), .body = [&] {
                             while (hold.load(std::memory_order_relaxed)) {
                                 if (aura::serve::g_current_fiber &&
                                     aura::serve::g_current_fiber->is_cancel_requested())
                                     break;
                                 aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
                             }
                         }});
            CHECK(child.size() == 1, "3671: live descendant in child scope");
        }

        // AC1: local join (no :tree) under production — the root handle
        // joins but the descendant stays live: ok=#f, blame fields
        // populated, deny interned (#3251), and NO drop (#3496 AC1).
        const auto ok1 = cs.eval(R"((hash-ref (orch:scope-join-all :timeout-ms 200) "ok"))");
        CHECK(ok1 && is_bool(*ok1) && !as_bool(*ok1),
              "3671 AC1: ok=#f while descendants live (production, no :tree)");
        const auto settled1 =
            cs.eval(R"((hash-ref (orch:scope-join-all :timeout-ms 200) "tree-settled"))");
        CHECK(settled1 && is_bool(*settled1) && !as_bool(*settled1),
              "3671 AC1: tree-settled=#f (descendant live)");
        const auto desc1 =
            cs.eval(R"((hash-ref (orch:scope-join-all :timeout-ms 200) "descendants-live"))");
        CHECK(desc1 && is_bool(*desc1) && as_bool(*desc1), "3671 AC1: descendants-live=#t");
        const auto deny1 = cs.eval(
            R"((let ((r (orch:scope-join-all :timeout-ms 200))) (hash-ref r "schema-3251")))");
        CHECK(deny1 && is_int(*deny1) && as_int(*deny1) == 3251,
              "3671 AC1: deny-class interned (#3251 — descendants-live deny fired)");
        // No drop: the child scope still resolves (spawn into :path 0 again).
        CHECK(
            cs.eval(
                  R"((let ((r (orch:scope-spawn :path 0 "3671-child-agent-2"))) (hash-ref r "ok")))")
                .has_value(),
            "3671 AC1: no drop_agent_scope — child scope still live (#3496)");

        // AC2: :tree #t still folds the subtree (#3643 preserved). The
        // held descendant makes the subtree join wait to its timeout — the
        // "tree" flag row is present regardless of the aggregate status.
        const auto tree_flag =
            cs.eval(R"((hash-ref (orch:scope-join-all :tree #t :timeout-ms 200) "tree"))");
        CHECK(tree_flag && is_bool(*tree_flag) && as_bool(*tree_flag),
              "3671 AC2: tree join flag propagates");

        // Drain: release the held descendant, cancel, and tree-join so the
        // spinner exits before AC3 resets the tree.
        hold.store(false, std::memory_order_relaxed);
        CHECK(cs.eval(R"((orch:scope-cancel-all))").has_value(), "3671: cancel-all for drain");
        CHECK(cs.eval(R"((orch:scope-join-all :tree #t :timeout-ms 5000))").has_value(),
              "3671: tree join drains held descendant");
    }

    // ── AC3: fresh root with no children — settled tree, no deny. The
    // join drops the settled scope (#3496 AC2); the returned hash still
    // carries the fields. No cancel: the empty body completes → Ok. ──
    {
        reset_all_agent_scopes_for_test();
        CompilerService cs3;
        CHECK(cs3.eval(R"((let ((r (orch:scope-spawn "3671-root-3"))) (hash-ref r "ok")))")
                  .has_value(),
              "3671 AC3: fresh root spawn");
        const auto j3 = cs3.eval(
            R"((let ((r (orch:scope-join-all :timeout-ms 1000)))
               (list (hash-ref r "tree-settled")
                     (hash-ref r "descendants-live")
                     (hash-ref r "deny-detail" "")
                     (hash-ref r "ok"))))");
        CHECK(j3.has_value(), "3671 AC3: join-all hash readable");
        {
            // List walk: (tree-settled descendants-live deny-detail ok).
            using aura::compiler::types::as_pair_idx;
            using aura::compiler::types::as_string_idx;
            using aura::compiler::types::is_pair;
            using aura::compiler::types::is_string;
            auto& ev3 = cs3.evaluator();
            auto cur = *j3;
            int guard = 0;
            int seen = 0;
            bool tree_settled_v = false, descendants_v = false, ok_v = false;
            std::string deny_v = "unset";
            auto& pairs = ev3.pairs();
            auto heap = ev3.string_heap();
            while (is_pair(cur) && guard++ < 16) {
                const auto i = as_pair_idx(cur);
                if (i >= pairs.size())
                    break;
                const auto& car = pairs[i].car;
                if (is_bool(car)) {
                    ++seen;
                    if (seen == 1)
                        tree_settled_v = as_bool(car);
                    if (seen == 2)
                        descendants_v = as_bool(car);
                    if (seen == 4)
                        ok_v = as_bool(car);
                } else if (is_string(car)) {
                    ++seen;
                    const auto si = as_string_idx(car);
                    if (si < heap.size())
                        deny_v = std::string(heap[si]);
                }
                cur = pairs[i].cdr;
            }
            CHECK(seen == 4, "3671 AC3: all four hash fields readable");
            CHECK(tree_settled_v, "3671 AC3: tree-settled=#t (no children)");
            CHECK(!descendants_v, "3671 AC3: descendants-live=#f");
            CHECK(deny_v.empty(), "3671 AC3: no deny-detail (no deny interned)");
            CHECK(ok_v, "3671 AC3: ok=#t (settled join, no deny)");
        }
    }
    aura::compiler::typed_audit::apply_dev_audit_defaults();

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_scope_join_tree_visibility();
}
#endif
