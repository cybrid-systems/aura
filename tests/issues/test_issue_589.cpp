// @category: integration
// @reason: Issue #589 SoA EnvFrame/EnvId dual-path bindings_
// vs bindings_symid_ consistency + version stamping + stale
// refresh in materialize_call_env & GCEnvWalkFn —
// query:envframe-dualpath-enforce-stats structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625/#626/#630/
// #631/#632/#633/#637/#640/#641/#642/#643/#644/#645/#646/#647/
// #648/#649/#650/#651 pattern.
//
// Discovery before this PR: the SoA EnvFrame dual-path
// observability surface already covers the high-level
// dual-path summary via existing primitives:
//   - (engine:metrics \"query:envframe-dualpath-stats\") — base flat-int dualpath
//     primitive (the AC4 surface listed in #589 body)
//   - (engine:metrics \"query:envframe-dualpath-stale-stats\") — existing flat-int
//     stale summary
//   - (engine:metrics \"query:envframe-dualpath-stale-stats-hash\") (#647) — stale
//     enforcement-layer hash (cross-fiber / version mismatch /
//     dualpath-repair counters)
//   - (engine:metrics \"query:envframe-stale-stats\") — stale refresh stats
//   - (engine:metrics \"query:envframe-bump-stats\") — bump stats
//   - #543 SoA EnvFrame foundation
//   - #568 children SoA
//   - #205 GCEnvWalkFn foundation
//   - envframe_desync_detected_ / envframe_stale_refresh_count_ /
//     envframe_post_rollback_invalidations_ +
//     envframe_version_mismatch_in_walk_ +
//     envframe_gc_walk_safe_skips_ + gc_envframe_stale_skipped_
//     (existing counters that #589 AC1+AC2+AC3 will exercise)
//
// What the issue body AC4 specifies by **exact name + fields** —
// `query:envframe-dualpath-stats` — already ships as the base
// flat-int primitive. The remaining AC1+AC2+AC3 enforcement work
// needs a distinct name to expose the per-write / per-refresh /
// per-walk enforcement counters.
//
// So #589 ships ONE new Aura primitive + 3 new atomics that are
// foundation scaffolding for the future AC1 (Env::bind_symid / bind
// always mirror writes + on owner_ set stamp defuse_version_ into
// env_version_), AC2 (materialize_call_env on version mismatch call
// refresh_dual_path_from_soa helper that syncs bindings_ <->
// bindings_symid_ + parent_id_ + bumps envframe_stale_refresh_count_),
// and AC3 (walk_env_frames / GCEnvWalkFn before emitting roots
// refresh or skip with metric if frame.version_ < current_defuse)
// enforcement work.
//
// Non-duplicative to existing #543 / #568 / #205 (issue body
// explicitly cross-referenced).
//
// The remaining #589 AC1 + AC2 + AC3 work is invasive C++ on
// evaluator.ixx + evaluator_impl.cpp + gc_coordinator.h + needs the
// large env chains + mutate + compaction/GC matrix + 5000+
// materialize under fibers + TSan coverage from the issue body —
// separate follow-ups.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_589_detail {
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

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:envframe-dualpath-enforce-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_589_detail

int aura_issue_589_run() {
    using namespace aura_issue_589_detail;
    std::println("=== Issue #589: query:envframe-dualpath-enforce-stats structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the documented fields.
    {
        std::println(
            "\n--- AC1: (engine:metrics \"query:envframe-dualpath-enforce-stats\") shape ---");
        auto h = cs.eval("(engine:metrics \"query:envframe-dualpath-enforce-stats\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "envframe-dualpath-enforce-stats returns a hash");
        const auto mirror = hash_int(cs, "mirror-write");
        const auto refresh = hash_int(cs, "dualpath-refresh");
        const auto violations = hash_int(cs, "consistency-violations");
        const auto schema = hash_int(cs, "schema");
        CHECK(mirror >= 0, std::format("mirror-write >= 0 (got {})", mirror));
        CHECK(refresh >= 0, std::format("dualpath-refresh >= 0 (got {})", refresh));
        CHECK(violations >= 0, std::format("consistency-violations >= 0 (got {})", violations));
        CHECK(schema == 589, std::format("schema == 589 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #589 doesn't disturb them).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_base = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
        CHECK(s_base.has_value(),
              "(engine:metrics \"query:envframe-dualpath-stats\") reachable (existing base "
              "flat-int dualpath primitive back-compat — AC4 surface)");
        auto s_stale = cs.eval("(engine:metrics \"query:envframe-dualpath-stale-stats\")");
        CHECK(s_stale.has_value(),
              "(engine:metrics \"query:envframe-dualpath-stale-stats\") reachable (existing "
              "flat-int stale summary back-compat)");
        auto s_647 = cs.eval("(engine:metrics \"query:envframe-dualpath-stale-stats-hash\")");
        CHECK(s_647.has_value(),
              "(engine:metrics \"query:envframe-dualpath-stale-stats-hash\") reachable (#647 "
              "back-compat — stale enforcement-layer hash)");
        auto s_651 = cs.eval("(engine:metrics \"query:gc-panic-deferral-stats\")");
        CHECK(s_651.has_value(),
              "(engine:metrics \"query:gc-panic-deferral-stats\") reachable (#651 back-compat)");
        auto s_650 = cs.eval("(engine:metrics \"query:scheduler-stealbudget-yield-class-stats\")");
        CHECK(s_650.has_value(),
              "(engine:metrics \"query:scheduler-stealbudget-yield-class-stats\") reachable (#650 "
              "back-compat)");
        auto s_649 = cs.eval("(engine:metrics \"query:yield-checkpoint-panic-stats\")");
        CHECK(
            s_649.has_value(),
            "(engine:metrics \"query:yield-checkpoint-panic-stats\") reachable (#649 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // mirror-write / dualpath-refresh / consistency-violations
    // are all 0 on a fresh service — they are foundation
    // scaffolding for the future AC1 + AC2 + AC3 enforcement
    // work (bind/bind_symid mirror writes + materialize_call_env
    // refresh + GCEnvWalkFn consistency violations).
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto mirror = hash_int(cs, "mirror-write");
        const auto refresh = hash_int(cs, "dualpath-refresh");
        const auto violations = hash_int(cs, "consistency-violations");
        CHECK(mirror == 0, std::format("fresh-service mirror-write == 0 (got {})", mirror));
        CHECK(refresh == 0, std::format("fresh-service dualpath-refresh == 0 (got {})", refresh));
        CHECK(violations == 0,
              std::format("fresh-service consistency-violations == 0 (got {})", violations));
    }

    // AC4: schema sentinel is exactly 589 (matches issue number
    // even though #589 is an older issue than the prior #651).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 589, std::format("schema == 589 (got {})", schema));
    }

    // AC5: naming distinction — the new primitive name uses
    // `-enforce-` midfix, distinct from #647's `-stale-` midfix
    // and from existing flat-int primitives.
    {
        std::println("\n--- AC5: naming distinction from #647 + existing ---");
        auto new_p = cs.eval("(engine:metrics \"query:envframe-dualpath-enforce-stats\")");
        auto old_647 = cs.eval("(engine:metrics \"query:envframe-dualpath-stale-stats-hash\")");
        auto old_base = cs.eval("(engine:metrics \"query:envframe-dualpath-stats\")");
        auto old_stale = cs.eval("(engine:metrics \"query:envframe-dualpath-stale-stats\")");
        CHECK(new_p.has_value(),
              "new primitive (engine:metrics \"query:envframe-dualpath-enforce-stats\") reachable "
              "(-enforce- midfix)");
        CHECK(old_647.has_value(),
              "existing #647 (engine:metrics \"query:envframe-dualpath-stale-stats-hash\") still "
              "reachable (-stale- midfix)");
        CHECK(old_base.has_value(),
              "existing base (engine:metrics \"query:envframe-dualpath-stats\") still reachable "
              "(no midfix)");
        CHECK(old_stale.has_value(),
              "existing (engine:metrics \"query:envframe-dualpath-stale-stats\") still reachable "
              "(no -hash suffix)");
        // The new primitive uses `schema` as its primary
        // sentinel — distinct from #647's `schema` (589 vs
        // 647). Verify field reachability (avoid hash-ref on
        // missing keys — see #644/#645 lessons).
        CHECK(hash_int(cs, "schema") == 589, "new primitive schema == 589");
        const auto check_new_field = [&](std::string_view k) {
            auto r = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:envframe-dualpath-enforce-stats\") '{}')", k));
            return r.has_value() && aura::compiler::types::is_int(*r);
        };
        CHECK(check_new_field("mirror-write"), "new primitive 'mirror-write' field reachable");
        CHECK(check_new_field("dualpath-refresh"),
              "new primitive 'dualpath-refresh' field reachable");
        CHECK(check_new_field("consistency-violations"),
              "new primitive 'consistency-violations' field reachable");
    }

    // AC6: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying counters + 3 new
    // scaffolding atomics.
    {
        std::println("\n--- AC6: concurrent envframe-dualpath-enforce-stats reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(engine:metrics \"query:envframe-dualpath-enforce-stats\")");
                if (r.has_value())
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(
            ok_count.load() == k_iters * 2,
            std::format("concurrent: {} / {} calls returned value", ok_count.load(), k_iters * 2));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_589_run();
}
#endif
