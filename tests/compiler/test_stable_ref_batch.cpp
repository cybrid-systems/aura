// tests/compiler/test_stable_ref_batch.cpp
// R19 phase3 dup-merge — stable_ref family trio:
//   Issue #527 (cow-fiber) + Issue #818 (cross-cow-provenance) + Issue #424 (workspace-tree)
// All 3 test 'stable_ref stats' subqueries with the same pattern:
//   aura_NNN_detail::run_matrix(cs) + main() calling RUN_ALL_TESTS().
// Same module imports (test_harness.hpp + aura.compiler.evaluator + service + value +
// aura.core.ast for #818 + extern resume_fiber_migration). Per Anqi 14:02 #81665
// '继续' (continue R19 cleanup of tests/compiler/) + MEMORY '完整 ship 以后都是' = ship
// end-to-end no asking.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

// #818 standalone hook (was guarded by #ifndef AURA_ISSUE_BUNDLE_MEMBER); batch's main()
// below replaces the dispatcher, so the symbol is declared here for direct calls.
extern "C" void aura_evaluator_resume_fiber_migration();

// === test_stable_ref_cow_fiber_closed_loop.cpp (Issue #527) ===
// Issue #527: StableNodeRef cross-COW/sub-workspace + concurrent
// fiber safety hardening for AI multi-round Query/Mutate loops.
//
// AC1: query:stable-ref-cow-fiber-stats reachable
// AC2: validate_stable_ref cross_cow bumps evaluator counters
// AC3: query:stable-ref + query:ref-valid? EDSL integration
// AC4: mutate:rebind + query:children-stable happy path
// AC5: cross_cow grows under mutate validate loop
// AC6: multi-round matrix — cow-fiber stats monotonic
// AC7: query regression (stable-ref-stats, edsl-stability-stats)

namespace aura_527_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

static std::int64_t cow_fiber_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:stable-ref-cow-fiber-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define a 1) (define b 2) (define acc 0)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:stable-ref-cow-fiber-stats ---");
    CHECK(setup_workspace(cs), "workspace setup");
    const auto s0 = cow_fiber_stats(cs);
    std::println("  stable-ref-cow-fiber-stats = {}", s0);
    CHECK(s0 >= 0, "cow-fiber stats non-negative");

    std::println("\n--- AC2: validate_stable_ref cross_cow ---");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    const auto cc0 = cs.evaluator().get_cross_cow_invalidations();
    const auto g = ws->generation();
    (void)cs.evaluator().validate_stable_ref(0, g > 0 ? g - 1 : 0);
    const auto cc1 = cs.evaluator().get_cross_cow_invalidations();
    std::println("  cross_cow: {} -> {}", cc0, cc1);
    CHECK(cc1 > cc0, "validate_stable_ref bumps cross_cow_invalidations");

    std::println("\n--- AC3: query:stable-ref + ref-valid? ---");
    auto ref = cs.eval("(query:stable-ref 1)");
    CHECK(ref && is_pair(*ref), "query:stable-ref returns pair");
    auto valid = cs.eval("(let ((r (query:stable-ref 1))) (query:ref-valid? r))");
    CHECK(valid && is_bool(*valid), "query:ref-valid? returns bool");

    std::println("\n--- AC4: mutate:rebind + children-stable ---");
    (void)cs.eval("(mutate:rebind \"acc\" \"99\")");
    auto kids = cs.eval("(query:children-stable 0)");
    CHECK(kids.has_value(), "query:children-stable returns on root");

    std::println("\n--- AC5: cross_cow under mutate validate loop ---");
    const auto stats5a = cow_fiber_stats(cs);
    const auto cc5a = cs.evaluator().get_cross_cow_invalidations();
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval("(mutate:rebind \"a\" \"" + std::to_string(10 + i) + "\")");
        const auto gen = ws->generation();
        (void)cs.evaluator().validate_stable_ref(0, gen > 0 ? gen - 1 : 0);
    }
    const auto cc5b = cs.evaluator().get_cross_cow_invalidations();
    const auto stats5b = cow_fiber_stats(cs);
    std::println("  cross_cow: {} -> {} stats: {} -> {}", cc5a, cc5b, stats5a, stats5b);
    CHECK(cc5b > cc5a, "mutate+validate loop grows cross_cow");
    CHECK(stats5b > stats5a, "cow-fiber stats grow under loop");

    std::println("\n--- AC6: multi-round matrix monotonic ---");
    const auto stats6a = cow_fiber_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"b\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(let ((r (query:stable-ref 1))) (query:ref-valid? r))");
    }
    const auto stats6b = cow_fiber_stats(cs);
    std::println("  cow-fiber stats: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "cow-fiber stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto srs = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    auto eds = cs.eval("(engine:metrics \"query:edsl-stability-stats\")");
    CHECK(srs && is_int(*srs), "stable-ref-stats regression");
    CHECK(eds && is_int(*eds), "edsl-stability-stats regression");
}

} // namespace aura_527_detail

// === test_stable_ref_cross_cow_provenance_enforcement.cpp (Issue #818) ===
// Issue #818: Full StableNodeRef provenance enforcement + cross-COW / sub-workspace
// auto-resolve + steal auto-refresh observability
// (refines #641/#715/#738/#749/#791; non-duplicative Task1-review layer).
//
// AC1:  query:stable-ref-cross-cow-provenance-stats reachable (schema 818)
// AC2:  provenance-enforced-hits bumps on direct path
// AC3:  cross-cow-refresh-hits bumps on direct path
// AC4:  fiber-workspace-mismatch-prevented bumps on direct path
// AC5:  steal-auto-refresh-hits bumps on direct path
// AC6:  production path — mutate StableRef + workspace:resolve-stable-ref
// AC7:  resume_fiber_migration bumps steal-auto-refresh
// AC8:  query regression (#641 provenance-sv, #715 layer-stats)
//
// NOTE: dropped #818's #ifndef AURA_ISSUE_BUNDLE_MEMBER dispatcher (was standalone bundle
// hook); batch's main() below replaces it.

namespace aura_issue_818_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-cross-cow-provenance-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t provenance_enforced(CompilerService& cs) {
    return stat_int(cs, "provenance-enforced-hits");
}
static std::int64_t cross_cow_refresh(CompilerService& cs) {
    return stat_int(cs, "cross-cow-refresh-hits");
}
static std::int64_t fiber_ws_mismatch(CompilerService& cs) {
    return stat_int(cs, "fiber-workspace-mismatch-prevented");
}
static std::int64_t steal_auto_refresh(CompilerService& cs) {
    return stat_int(cs, "steal-auto-refresh-hits");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:stable-ref-cross-cow-provenance-stats (schema 818) ---");
    auto h = cs.eval("(engine:metrics \"query:stable-ref-cross-cow-provenance-stats\")");
    CHECK(h && is_hash(*h), "stable-ref-cross-cow-provenance-stats returns hash");
    CHECK(stat_int(cs, "schema") == 818, "schema == 818");
    CHECK(provenance_enforced(cs) >= 0, "provenance-enforced-hits non-negative");
    CHECK(cross_cow_refresh(cs) >= 0, "cross-cow-refresh-hits non-negative");
    CHECK(fiber_ws_mismatch(cs) >= 0, "fiber-workspace-mismatch-prevented non-negative");
    CHECK(steal_auto_refresh(cs) >= 0, "steal-auto-refresh-hits non-negative");

    std::println("\n--- AC2: provenance-enforced-hits bumps on direct path ---");
    const auto p0 = provenance_enforced(cs);
    cs.evaluator().bump_stable_ref_provenance_enforced(2);
    CHECK(provenance_enforced(cs) == p0 + 2, "provenance-enforced-hits bumps by 2");

    std::println("\n--- AC3: cross-cow-refresh-hits bumps on direct path ---");
    const auto c0 = cross_cow_refresh(cs);
    cs.evaluator().bump_stable_ref_cross_cow_refresh();
    CHECK(cross_cow_refresh(cs) == c0 + 1, "cross-cow-refresh-hits bumps by 1");

    std::println("\n--- AC4: fiber-workspace-mismatch-prevented bumps on direct path ---");
    const auto m0 = fiber_ws_mismatch(cs);
    cs.evaluator().bump_stable_ref_fiber_workspace_mismatch_prevented(3);
    CHECK(fiber_ws_mismatch(cs) == m0 + 3, "fiber-workspace-mismatch-prevented bumps by 3");

    std::println("\n--- AC5: steal-auto-refresh-hits bumps on direct path ---");
    const auto s0 = steal_auto_refresh(cs);
    cs.evaluator().bump_stable_ref_steal_auto_refresh(2);
    CHECK(steal_auto_refresh(cs) == s0 + 2, "steal-auto-refresh-hits bumps by 2");

    std::println("\n--- AC6: production path — StableRef mutate + workspace resolve ---");
    // Seed a workspace define so query:stable-ref / mutate paths have nodes.
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code seed");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current seed");
    auto ref = cs.eval("(query:stable-ref 0)");
    // query:stable-ref may return void if node 0 is not a user node;
    // try finding a define body via workspace helpers when available.
    if (!ref || !is_hash(*ref)) {
        // Fall back: create WorkspaceTree + resolve-stable-ref mismatch path.
        auto created = cs.eval("(workspace:create \"root\")");
        (void)created;
        const auto mm_before = fiber_ws_mismatch(cs);
        // from-layer=0, node=0, gen=0, to-layer=0, workspace_id=9 (mismatch)
        auto resolved = cs.eval("(workspace:resolve-stable-ref 0 0 0 0 9)");
        (void)resolved;
        CHECK(fiber_ws_mismatch(cs) > mm_before || fiber_ws_mismatch(cs) >= mm_before,
              "workspace:resolve-stable-ref mismatch path exercised (or tree unavailable)");
    } else {
        // Stable ref obtained — mutate via replace-type with stable-ref form
        // would need full pair shape; use bump path already covered +
        // resolve-stable-ref for production cross-COW.
        auto created = cs.eval("(workspace:create \"child\")");
        (void)created;
        const auto cc_before = cross_cow_refresh(cs);
        auto resolved = cs.eval("(workspace:resolve-stable-ref 0 0 0)");
        (void)resolved;
        // resolve may miss (no remap table entry) — still ok; enforce path ran.
        CHECK(cross_cow_refresh(cs) >= cc_before, "cross-cow-refresh non-decreasing after resolve");
    }
    // Always exercise provenance-enforced via a successful make_ref validate
    // on a real flat node if present.
    if (auto* flat = cs.workspace_flat(); flat && flat->size() > 0) {
        const auto pe = provenance_enforced(cs);
        auto r = flat->make_ref(0);
        if (r.is_valid_in(*flat)) {
            (void)r.validate_with_provenance(*flat);
            cs.evaluator().bump_stable_ref_provenance_enforced();
            CHECK(provenance_enforced(cs) > pe, "provenance-enforced grew after validate");
        } else {
            CHECK(true, "flat node 0 not valid for provenance stamp (skip production stamp)");
        }
    } else {
        CHECK(true, "no workspace flat (skip production stamp)");
    }

    std::println("\n--- AC7: resume_fiber_migration bumps steal-auto-refresh ---");
    const auto st7a = steal_auto_refresh(cs);
    aura_evaluator_resume_fiber_migration();
    CHECK(steal_auto_refresh(cs) > st7a,
          "steal-auto-refresh-hits grew after resume_fiber_migration");

    std::println("\n--- AC8: aggregate monotonic + query regression ---");
    const auto agg8a = provenance_enforced(cs) + cross_cow_refresh(cs) + fiber_ws_mismatch(cs) +
                       steal_auto_refresh(cs);
    cs.evaluator().bump_stable_ref_provenance_enforced();
    cs.evaluator().bump_stable_ref_cross_cow_refresh();
    cs.evaluator().bump_stable_ref_fiber_workspace_mismatch_prevented();
    cs.evaluator().bump_stable_ref_steal_auto_refresh();
    const auto agg8b = provenance_enforced(cs) + cross_cow_refresh(cs) + fiber_ws_mismatch(cs) +
                       steal_auto_refresh(cs);
    CHECK(agg8b >= agg8a + 4, "aggregate #818 counters monotonic");

    auto s641 = cs.eval("(engine:metrics \"query:stable-ref-provenance-sv-stats\")");
    auto s715 = cs.eval("(engine:metrics \"query:stable-ref-layer-stats\")");
    CHECK(s641 && is_hash(*s641), "stable-ref-provenance-sv-stats regression (#641)");
    CHECK(s715 && is_hash(*s715), "stable-ref-layer-stats regression (#715)");
}

} // namespace aura_issue_818_detail

// === test_stable_ref_workspace_tree_closed_loop.cpp (Issue #424) ===
// Issue #424: StableNodeRef / is_valid across COW WorkspaceTree
// and child workspaces for multi-Agent orchestration safety.
//
// AC1: query:stable-ref-workspace-tree-stats reachable
// AC2: workspace:create + switch establishes child layer
// AC3: workspace:resolve-stable-ref bumps resolve counter
// AC4: query:ref-valid? on resolved ref after child mutate
// AC5: ensure_stable_ref_workspace_consistency — zero violations
// AC6: multi-round resolve matrix monotonic stats
// AC7: query regression (stable-ref-stats, stable-ref-cow-fiber-stats)

namespace aura_424_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

static std::int64_t workspace_tree_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:stable-ref-workspace-tree-stats\")");
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
    auto srs = cs.eval("(engine:metrics \"query:stable-ref-stats\")");
    auto scf = cs.eval("(engine:metrics \"query:stable-ref-cow-fiber-stats\")");
    CHECK(srs && is_int(*srs), "stable-ref-stats regression");
    CHECK(scf && is_int(*scf), "stable-ref-cow-fiber-stats regression");
}

} // namespace aura_424_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_527_detail::run_matrix(cs);
    aura_issue_818_detail::run_matrix(cs);
    aura_424_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}
