// @category: integration
// @reason: Issue #737 — End-to-end atomic batch mutate primitive +
//  snapshot/rollback with StableNodeRef pinning for reliable
//  multi-round AI Agent edit loops.
//
// Non-duplicative with #717 (Guard impact snapshot), #488
// (post-mutate reflect validation), #732 (AOT hot-reload),
// #735 (macro provenance), #553/#529 (atomic-batch rollback
// matrix), #622 (atomic-batch-stats-hash).
//
//   - AC1: workspace:snapshot + workspace:rollback-to primitives
//   - AC2: (mutate:atomic-batch ... :snapshot? #t) happy path
//   - AC3: failed batch restores pre-batch snapshot
//   - AC4: pinned-refs + snapshot metrics observable
//   - AC5: multi-round query → batch mutate → eval loop
//   - AC6: existing atomic-batch primitives regression

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_737_detail {
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

static std::int64_t snap_hash(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:atomic-batch-snapshot-stats-hash) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2) (define acc 0)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_737_detail

int aura_issue_atomic_batch_snapshot_stable_ref_ai_loops_run() {
    using namespace aura_issue_737_detail;

    std::println("=== Issue #737: Atomic batch + snapshot + StableRef pinning ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: workspace:snapshot + workspace:rollback-to
    {
        std::println("\n--- AC1: workspace:snapshot + workspace:rollback-to ---");
        auto snap = cs.eval("(workspace:snapshot \"pre-edit\")");
        CHECK(snap && aura::compiler::types::is_int(*snap) &&
                  aura::compiler::types::as_int(*snap) >= 0,
              "workspace:snapshot returns non-negative id");
        (void)cs.eval("(mutate:rebind \"x\" \"99\")");
        auto restored = cs.eval("(workspace:rollback-to \"pre-edit\")");
        CHECK(restored && aura::compiler::types::is_bool(*restored) &&
                  aura::compiler::types::as_bool(*restored),
              "workspace:rollback-to by name succeeds");
        auto x_after = cs.eval("(begin (eval-current) x)");
        CHECK(x_after && aura::compiler::types::is_int(*x_after) &&
                  aura::compiler::types::as_int(*x_after) == 1,
              "rollback restored x to original value");
    }

    CHECK(setup_workspace(cs), "workspace re-setup for batch tests");

    const auto commits_before = cs.evaluator().atomic_batch_count();
    const auto pinned_before = cs.evaluator().atomic_batch_pinned_refs_total();

    // AC2: happy path with :snapshot?
    {
        std::println("\n--- AC2: mutate:atomic-batch :snapshot? happy path ---");
        auto ok = cs.eval("(mutate:atomic-batch "
                          "(list (list \"mutate:rebind\" \"x\" \"10\") "
                          "       (list \"mutate:rebind\" \"y\" \"20\")) "
                          ":snapshot? #t \"737-happy\")");
        CHECK(ok && aura::compiler::types::is_bool(*ok) && aura::compiler::types::as_bool(*ok),
              "atomic-batch with :snapshot? succeeds");
        const auto commits_after = cs.evaluator().atomic_batch_count();
        CHECK(commits_after > commits_before, "batch commit counter grew");
        const auto snap_captures = cs.evaluator().atomic_batch_snapshot_captures();
        CHECK(snap_captures > 0, "snapshot capture counter bumped");
    }

    // AC3: failed batch restores snapshot
    {
        std::println("\n--- AC3: failed batch restores pre-batch snapshot ---");
        (void)cs.eval("(mutate:rebind \"x\" \"55\")");
        const auto roll_before = cs.evaluator().atomic_batch_snapshot_rollbacks();
        (void)cs.eval("(mutate:atomic-batch "
                      "(list (list \"mutate:replace-value\" 999 \"bad\")) "
                      ":snapshot? #t \"737-fail\")");
        const auto roll_after = cs.evaluator().atomic_batch_snapshot_rollbacks();
        CHECK(roll_after > roll_before, "snapshot rollback counter grew on batch failure");
        auto x_val = cs.eval("(begin (eval-current) x)");
        CHECK(x_val && aura::compiler::types::is_int(*x_val) &&
                  aura::compiler::types::as_int(*x_val) == 55,
              "failed batch restored x via snapshot");
    }

    // AC4: pinning + snapshot metrics
    {
        std::println("\n--- AC4: pinning + snapshot metrics ---");
        auto h = cs.eval("(query:atomic-batch-snapshot-stats-hash)");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "query:atomic-batch-snapshot-stats-hash returns hash");
        CHECK(snap_hash(cs, "schema") == 737, "schema sentinel == 737");
        CHECK(snap_hash(cs, "batch-commits") >= 0, "batch-commits present");
        CHECK(snap_hash(cs, "rollback-triggers") >= 0, "rollback-triggers present");
        CHECK(snap_hash(cs, "pinned-refs-total") >= 0, "pinned-refs-total present");
        CHECK(snap_hash(cs, "snapshot-captures") >= 0, "snapshot-captures present");
        const auto pinned_after = cs.evaluator().atomic_batch_pinned_refs_total();
        CHECK(pinned_after >= pinned_before, "pinned refs total monotonic");
        auto abs = cs.eval("(hash-ref (atomic-batch:stats) \"pinned-refs-last-batch\")");
        CHECK(abs && aura::compiler::types::is_int(*abs),
              "atomic-batch:stats exposes pinned-refs-last-batch");
    }

    // AC5: multi-round AI loop — query stable-ref → batch → eval
    {
        std::println("\n--- AC5: multi-round query → batch → eval loop ---");
        CHECK(setup_workspace(cs), "loop workspace setup");
        for (int round = 0; round < 3; ++round) {
            (void)cs.eval("(workspace:snapshot (string-append \"round-\" (number->string " +
                          std::to_string(round) + ")))");
            auto ref_ok =
                cs.eval("(let ((r (query:stable-ref (car (query:defines-by-marker \"Define\"))))) "
                        "(if (pair? r) (query:ref-valid? r) #f))");
            CHECK(ref_ok.has_value(), std::format("round {} stable-ref query", round));
            auto batch = cs.eval("(mutate:atomic-batch "
                                 "(list (list \"mutate:rebind\" \"acc\" \"" +
                                 std::to_string(round + 1) +
                                 "\")) "
                                 ":snapshot? #t \"loop\")");
            CHECK(batch.has_value(), std::format("round {} atomic batch", round));
            CHECK(cs.eval("(eval-current)").has_value(),
                  std::format("round {} eval-current", round));
        }
        const auto commits = cs.evaluator().atomic_batch_count();
        CHECK(commits >= 3, "multi-round loop produced batch commits");
    }

    // AC6: regression — existing primitives still work
    {
        std::println("\n--- AC6: atomic-batch primitive regression ---");
        auto s192 = cs.eval("(atomic-batch:stats)");
        auto s622 = cs.eval("(query:atomic-batch-stats-hash)");
        auto s529 = cs.eval("(query:atomic-batch-rollback-stats)");
        CHECK(s192.has_value(), "(atomic-batch:stats) regression");
        CHECK(s622 && aura::compiler::types::is_hash(*s622),
              "(query:atomic-batch-stats-hash) regression");
        CHECK(s529.has_value(), "(query:atomic-batch-rollback-stats) regression");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_atomic_batch_snapshot_stable_ref_ai_loops_run();
}
#endif
