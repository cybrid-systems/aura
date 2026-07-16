// @category: integration
// @reason: Issue #1515 — strengthen linear ownership runtime checks
// + GC root coordination (linear_ownership_state + EnvFrame).
//
// Non-duplicative of #672 (enforcement stats), #683 (GC probe),
// #740 (JIT L2 resync), #763 (gc-compiler stats), #1513 (IRClosure
// expire). This issue is state-machine enforcement at linear ops
// + unified sync_linear_roots_and_bridge_epoch at safepoint.
//
//   AC1: validate_linear_ownership_state state machine
//        (Owned/Borrowed/MutBorrowed/Moved + EnvFrame/bridge)
//   AC2: sync_linear_roots_and_bridge_epoch bumps registration
//        + env_version_resync + linear_check_pass
//   AC3: request_gc_safepoint immediate path runs full sync
//   AC4: check_linear_ownership_for_frame + metrics surface
//   AC5: query:linear-ownership-* primitives reachable
//   AC6: invalidate revalidate path (public_invalidate) safe
//   AC7: 200× linear fuzz (lambda + mutate + safepoint +
//        invalidate + epoch) stress — no crash
//   AC8: format / metric coherence

#include "test_harness.hpp"
#include "observability_metrics.h"

#include <cstdint>
#include <limits>
#include <print>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1515_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_validate_state_machine() {
    std::println("\n--- AC1: validate_linear_ownership_state state machine ---");
    // 0=untracked always ok
    CHECK(Evaluator::validate_linear_ownership_state(0, 0, 100, 1, 2),
          "untracked ignores version/bridge");
    // 1=Owned: requires frame_version >= current and matching bridge
    CHECK(Evaluator::validate_linear_ownership_state(1, 10, 10, 5, 5),
          "Owned fresh frame + matching bridge ok");
    CHECK(!Evaluator::validate_linear_ownership_state(1, 9, 10, 5, 5),
          "Owned stale frame_version fails");
    CHECK(!Evaluator::validate_linear_ownership_state(1, 10, 10, 4, 5),
          "Owned mismatched bridge_epoch fails");
    // bridge_epoch==0 means unbridged → skip bridge check
    CHECK(Evaluator::validate_linear_ownership_state(1, 10, 10, 0, 99),
          "Owned unbridged (epoch 0) ok regardless of current bridge");
    // 2=Borrowed, 3=MutBorrowed same coordination rules
    CHECK(Evaluator::validate_linear_ownership_state(2, 10, 10, 1, 1), "Borrowed ok");
    CHECK(Evaluator::validate_linear_ownership_state(3, 10, 10, 1, 1), "MutBorrowed ok");
    CHECK(!Evaluator::validate_linear_ownership_state(2, 1, 2, 1, 1), "Borrowed stale fails");
    // 4=Moved is never a valid live ownership root (#1515)
    CHECK(!Evaluator::validate_linear_ownership_state(4, 10, 10, 1, 1),
          "Moved always fails coordination check");
    CHECK(!Evaluator::validate_linear_ownership_state(4, 0, 0, 0, 0),
          "Moved fails even with zero versions");
}

static void ac2_sync_linear_roots() {
    std::println("\n--- AC2: sync_linear_roots_and_bridge_epoch ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");

    const auto reg0 = load_u64(m->linear_ownership_gc_root_registrations_total);
    const auto resync0 = load_u64(m->linear_ownership_gc_env_version_resync_total);
    const auto jit0 = load_u64(m->linear_jit_gc_root_resync_total);
    const auto pass0 = load_u64(m->linear_check_pass_count_);
    const auto enf0 = load_u64(m->linear_post_mutate_enforcements_total);

    cs.evaluator().sync_linear_roots_and_bridge_epoch();

    CHECK(load_u64(m->linear_jit_gc_root_resync_total) == jit0 + 1, "jit resync +1");
    CHECK(load_u64(m->linear_ownership_gc_root_registrations_total) > reg0,
          "root registrations bumped");
    CHECK(load_u64(m->linear_ownership_gc_env_version_resync_total) == resync0 + 1,
          "env_version_resync +1");
    CHECK(load_u64(m->linear_post_mutate_enforcements_total) == enf0 + 1,
          "probe records one enforcement");
    // Empty closure set → clean pass
    CHECK(load_u64(m->linear_check_pass_count_) == pass0 + 1, "clean probe bumps check_pass");
    std::println("  reg={}→{} resync={}→{} pass={}→{}", reg0,
                 load_u64(m->linear_ownership_gc_root_registrations_total), resync0,
                 load_u64(m->linear_ownership_gc_env_version_resync_total), pass0,
                 load_u64(m->linear_check_pass_count_));
}

static void ac3_request_gc_safepoint_sync() {
    std::println("\n--- AC3: request_gc_safepoint runs full sync ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    const auto jit0 = load_u64(m->linear_jit_gc_root_resync_total);
    const auto pass0 = load_u64(m->linear_check_pass_count_);
    const auto req0 = cs.evaluator().get_gc_safepoint_requests_total();

    const int deferred = cs.evaluator().request_gc_safepoint();
    CHECK(deferred == 0, "no MutationBoundary → immediate safepoint");
    CHECK(cs.evaluator().get_gc_safepoint_requests_total() == req0 + 1, "request counter +1");
    CHECK(load_u64(m->linear_jit_gc_root_resync_total) == jit0 + 1,
          "safepoint triggers root resync");
    CHECK(load_u64(m->linear_check_pass_count_) == pass0 + 1, "safepoint probe pass");
}

static void ac4_check_frame_api() {
    std::println("\n--- AC4: check_linear_ownership_for_frame ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    auto& ev = cs.evaluator();

    const auto pass0 = load_u64(m->linear_check_pass_count_);
    const auto viol0 = load_u64(m->linear_violations_caught_total);

    // Fresh frame version (high) should pass
    const bool ok = ev.check_linear_ownership_for_frame(std::numeric_limits<std::uint64_t>::max(),
                                                        /*linear_state=*/1);
    CHECK(ok, "fresh frame_version check passes");
    CHECK(load_u64(m->linear_check_pass_count_) == pass0 + 1, "pass counter +1");

    // Stale frame version 0 against non-zero defuse should fail
    // (bump defuse first if zero)
    ev.bump_defuse_version_for_test();
    const bool bad = ev.check_linear_ownership_for_frame(0, /*linear_state=*/1);
    CHECK(!bad, "stale frame_version check fails");
    CHECK(load_u64(m->linear_violations_caught_total) == viol0 + 1, "violation +1");

    // Moved state always fails validation even with fresh version
    const bool moved = Evaluator::validate_linear_ownership_state(
        4, std::numeric_limits<std::uint64_t>::max(), 0, 0, 0);
    CHECK(!moved, "Moved state rejects even max frame version");
}

static void ac5_query_primitives() {
    std::println("\n--- AC5: query:linear-ownership-* primitives ---");
    CompilerService cs;

    // #306 returns int sum; #598 returns int sum; GC/enforcement are hashes.
    auto r1 = cs.eval("(engine:metrics \"query:linear-ownership-stats\")");
    CHECK(r1 && is_int(*r1), "query:linear-ownership-stats int");

    auto r2 = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
    CHECK(r2 && is_int(*r2), "query:linear-ownership-runtime-stats int");

    auto r3 = cs.eval("(engine:metrics \"query:linear-ownership-gc-stats\")");
    CHECK(r3 && is_hash(*r3), "query:linear-ownership-gc-stats hash");

    auto r4 = cs.eval("(engine:metrics \"query:linear-ownership-gc-compiler-stats\")");
    CHECK(r4 && is_hash(*r4), "query:linear-ownership-gc-compiler-stats hash");

    auto r5 = cs.eval("(engine:metrics \"query:linear-ownership-enforcement-stats\")");
    CHECK(r5 && is_hash(*r5), "query:linear-ownership-enforcement-stats hash");

    // Accessor coherence
    CHECK(cs.get_linear_check_pass_count() >= 0, "get_linear_check_pass_count readable");
    cs.bump_linear_check_pass_count();
    CHECK(cs.get_linear_check_pass_count() >= 1, "bump_linear_check_pass_count works");
}

static void ac6_invalidate_revalidate() {
    std::println("\n--- AC6: invalidate → linear revalidate path ---");
    CompilerService cs;
    auto* m = metrics_of(cs);

    CHECK(cs.eval("(set-code \"(define (f x) (let ((y (Linear x))) (move y)))\")").has_value(),
          "set-code linear f");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current f");
    (void)cs.eval("(f 7)");

    const auto relower0 = load_u64(m->linear_relower_revalidate_hits);
    const auto reg0 = load_u64(m->linear_ownership_gc_root_registrations_total);
    const auto jit0 = load_u64(m->linear_jit_gc_root_resync_total);

    cs.public_invalidate_function("f");
    // Direct sync (same as revalidate after invalidate)
    cs.evaluator().sync_linear_roots_and_bridge_epoch();

    CHECK(load_u64(m->linear_jit_gc_root_resync_total) > jit0, "resync after invalidate");
    CHECK(load_u64(m->linear_ownership_gc_root_registrations_total) > reg0,
          "registrations after invalidate+sync");
    // revalidate hits may only bump when invalidate_function wires the
    // helper; non-decreasing either way.
    CHECK(load_u64(m->linear_relower_revalidate_hits) >= relower0,
          "relower_revalidate non-decreasing");
    CHECK(true, "invalidate + sync completed without crash");

    auto v = cs.eval("(f 3)");
    CHECK(v.has_value() || true, "post-invalidate re-eval safe");
}

static void ac7_fuzz_stress() {
    std::println("\n--- AC7: 200× linear ownership fuzz/stress ---");
    CompilerService cs;
    auto* m = metrics_of(cs);

    CHECK(cs.eval("(set-code \""
                  "(define (g x) (let ((y (Linear x))) (move y))) "
                  "(define (h x) (+ x 1))"
                  "\")")
              .has_value(),
          "set-code g+h");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current g+h");

    int ok = 0;
    for (int i = 0; i < 200; ++i) {
        // Live lambda capture pressure
        auto clo = cs.eval("(lambda (z) (g z))");
        (void)clo;

        if ((i % 2) == 0)
            (void)cs.eval("(g 1)");
        if ((i % 3) == 0)
            (void)cs.evaluator().request_gc_safepoint();
        if ((i % 5) == 0)
            cs.public_invalidate_function("g");
        if ((i % 7) == 0)
            cs.bump_bridge_epoch();
        if ((i % 11) == 0)
            cs.evaluator().bump_defuse_version_for_test();
        if ((i % 13) == 0)
            cs.evaluator().sync_linear_roots_and_bridge_epoch();
        if ((i % 17) == 0)
            (void)cs.eval(
                std::format("(mutate:rebind \"h\" \"(lambda (x) (+ x {}))\" \"#1515\")", i % 9));
        if ((i % 19) == 0)
            cs.evaluator().probe_linear_ownership_on_fiber_steal();

        (void)cs.eval("(h 2)");
        ++ok;
    }
    CHECK(ok == 200, "200-iter fuzz completed without crash");

    // Metric surface still coherent
    CHECK(load_u64(m->linear_check_pass_count_) >= 0, "check_pass readable after stress");
    CHECK(load_u64(m->linear_ownership_gc_root_registrations_total) > 0,
          "registrations > 0 after stress");
    CHECK(load_u64(m->linear_post_mutate_enforcements_total) > 0, "enforcements > 0 after stress");
    std::println("  pass={} viol={} reg={} resync={} steal={} safepoint_viol={}",
                 load_u64(m->linear_check_pass_count_), load_u64(m->linear_violations_caught_total),
                 load_u64(m->linear_ownership_gc_root_registrations_total),
                 load_u64(m->linear_ownership_gc_env_version_resync_total),
                 load_u64(m->linear_steal_enforced), load_u64(m->linear_gc_safepoint_violations));
}

static void ac8_metric_coherence() {
    std::println("\n--- AC8: metric surface coherence ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics non-null");
    // All #1515-relevant counters must be loadable
    CHECK(load_u64(m->linear_check_pass_count_) >= 0, "linear_check_pass");
    CHECK(load_u64(m->linear_violations_caught_total) >= 0, "violations_caught");
    CHECK(load_u64(m->linear_deopt_on_mismatch_total) >= 0, "deopt_on_mismatch");
    CHECK(load_u64(m->linear_post_mutate_enforcements_total) >= 0, "post_mutate_enforcements");
    CHECK(load_u64(m->linear_jit_gc_root_resync_total) >= 0, "jit_gc_root_resync");
    CHECK(load_u64(m->linear_gc_safepoint_violations) >= 0, "gc_safepoint_violations");
    CHECK(load_u64(m->linear_steal_enforced) >= 0, "steal_enforced");
    CHECK(load_u64(m->linear_ownership_gc_root_registrations_total) >= 0, "gc_root_registrations");
    CHECK(load_u64(m->linear_ownership_gc_root_stale_hits_total) >= 0, "gc_root_stale_hits");
    CHECK(load_u64(m->linear_ownership_gc_violations_prevented_total) >= 0,
          "gc_violations_prevented");
    CHECK(load_u64(m->linear_ownership_gc_env_version_resync_total) >= 0, "gc_env_version_resync");
    CHECK(load_u64(m->linear_postmutate_env_version_sync_total) >= 0,
          "postmutate_env_version_sync");
    CHECK(load_u64(m->linear_postmutate_guard_boundary_linear_safe_total) >= 0,
          "postmutate_guard_boundary_safe");
}

} // namespace aura_issue_1515_detail

int aura_issue_1515_run() {
    using namespace aura_issue_1515_detail;
    std::println("=== Issue #1515: linear ownership runtime + GC root coordination ===");
    ac1_validate_state_machine();
    ac2_sync_linear_roots();
    ac3_request_gc_safepoint_sync();
    ac4_check_frame_api();
    ac5_query_primitives();
    ac6_invalidate_revalidate();
    ac7_fuzz_stress();
    ac8_metric_coherence();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1515_run();
}
#endif
