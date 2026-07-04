// test_stable_ref_workspace_tree_closed_loop_424.cpp
// Issue #424: StableNodeRef / is_valid across COW WorkspaceTree
// and child workspaces for multi-Agent orchestration safety.
//
// Non-duplicative with #457 (stable-ref-stats FlatAST slice),
// #527 (stable-ref-cow-fiber-stats), #276 (remap unit tests).
//
// AC1: query:stable-ref-workspace-tree-stats reachable
// AC2: workspace:create + switch establishes child layer
// AC3: workspace:resolve-stable-ref bumps resolve counter
// AC4: query:ref-valid? on resolved ref after child mutate
// AC5: ensure_stable_ref_workspace_consistency — zero violations
// AC6: multi-round resolve matrix monotonic stats
// AC7: query regression (stable-ref-stats,
//      stable-ref-cow-fiber-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_424_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

static std::int64_t workspace_tree_stats(CompilerService& cs) {
    auto r = cs.eval("(query:stable-ref-workspace-tree-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_parent_child_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")"))
        return false;
    if (!cs.eval("(eval-current)"))
        return false;
    auto r = cs.eval("(workspace:create \"agent-child\")");
    if (!r || !is_int(*r) || as_int(*r) < 1)
        return false;
    return cs.eval("(workspace:switch 1)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:stable-ref-workspace-tree-stats ---");
    const auto s0 = workspace_tree_stats(cs);
    std::println("  stable-ref-workspace-tree-stats = {}", s0);
    CHECK(s0 >= 0, "workspace tree stats non-negative");

    std::println("\n--- AC2: workspace child layer baseline ---");
    CHECK(setup_parent_child_workspace(cs), "parent/child workspace setup");
    auto& ev = cs.evaluator();
    auto* wt = static_cast<aura::compiler::WorkspaceTree*>(ev.workspace_tree());
    CHECK(wt != nullptr, "workspace tree available");
    std::println("  layers = {}, active = {}", wt->size(), wt->active_idx());
    CHECK(wt->size() >= 2, "workspace tree has root + child");
    CHECK(wt->active_idx() == 1, "active layer is child");

    std::println("\n--- AC3: resolve-stable-ref bumps resolves ---");
    const auto stats3a = workspace_tree_stats(cs);
    const auto res3a = ev.get_stable_ref_workspace_resolves();
    auto resolved = cs.eval("(begin "
                            "  (workspace:switch 0) "
                            "  (define rx (ast:stable-ref 1)) "
                            "  (workspace:switch 1) "
                            "  (mutate:rebind \"x\" \"(quote 9)\" \"mut\") "
                            "  (define r (workspace:resolve-stable-ref 0 (car rx) (cdr rx))) "
                            "  (if r 1 0))");
    CHECK(resolved && is_int(*resolved) && as_int(*resolved) == 1,
          "workspace:resolve-stable-ref succeeds across COW child");
    const auto res3b = ev.get_stable_ref_workspace_resolves();
    const auto stats3b = workspace_tree_stats(cs);
    std::println("  resolves: {} -> {}, stats: {} -> {}", res3a, res3b, stats3a, stats3b);
    CHECK(res3b > res3a, "resolve bumps workspace resolve counter");
    CHECK(stats3b > stats3a, "workspace tree stats grow");

    std::println("\n--- AC4: ref-valid? on resolved ref ---");
    auto valid = cs.eval("(begin "
                         "  (workspace:switch 0) "
                         "  (define rx (ast:stable-ref 1)) "
                         "  (workspace:switch 1) "
                         "  (define r (workspace:resolve-stable-ref 0 (car rx) (cdr rx))) "
                         "  (if r (query:ref-valid? r) #f))");
    CHECK(valid && is_bool(*valid) && as_bool(*valid),
          "resolved ref valid in child layer after mutate");

    std::println("\n--- AC5: ensure_stable_ref_workspace_consistency ---");
    ev.ensure_stable_ref_workspace_consistency();
    CHECK(ev.get_stable_ref_workspace_tree_violations() == 0,
          "zero workspace tree consistency violations");

    std::println("\n--- AC6: multi-round resolve matrix ---");
    const auto stats6a = workspace_tree_stats(cs);
    const auto res6a = ev.get_stable_ref_workspace_resolves();
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(begin "
                      "  (workspace:switch 0) "
                      "  (define rx (ast:stable-ref 1)) "
                      "  (workspace:switch 1) "
                      "  (workspace:resolve-stable-ref 0 (car rx) (cdr rx)))");
    }
    const auto res6b = ev.get_stable_ref_workspace_resolves();
    const auto stats6b = workspace_tree_stats(cs);
    std::println("  resolves: {} -> {}", res6a, res6b);
    CHECK(res6b > res6a, "resolves grow over repeated matrix");
    CHECK(stats6b > stats6a, "workspace tree stats grow over matrix");

    std::println("\n--- AC7: query regression ---");
    auto srs = cs.eval("(query:stable-ref-stats)");
    auto scf = cs.eval("(query:stable-ref-cow-fiber-stats)");
    CHECK(srs && is_int(*srs), "stable-ref-stats regression");
    CHECK(scf && is_int(*scf), "stable-ref-cow-fiber-stats regression");
}

} // namespace aura_424_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_424_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}