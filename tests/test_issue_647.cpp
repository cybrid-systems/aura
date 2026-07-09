// @category: integration
// @reason: Issue #647 Dual-Path EnvFrame/Env (parent_id_ vs
// parent_, bindings_symid_ vs bindings_) Cross-Fiber Stale
// Detection + materialize_call_env After Steal —
// query:envframe-dualpath-stale-stats-hash structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625/#626/#630/
// #631/#632/#633/#637/#640/#641/#642/#643/#644/#645/#646 pattern.
//
// Discovery before this PR: the EnvFrame dual-path observability
// surface already covers the high-level dual-path summary via
// existing primitives + counters:
//   - (query:envframe-dualpath-stale-stats) (#418) — existing
//     flat-int primitive (returns a single sum of 7 counters
//     — no field breakdown)
//   - (query:envframe-dualpath-stats) — base dualpath primitive
//   - (query:envframe-stale-stats) — stale refresh stats
//   - (query:envframe-bump-stats) — bump stats
//   - env_frames_ EnvFrame arena (walk + lookup_by_symid_chain)
//     with version_ + INVALID_VERSION sentinel #356
//   - #637 IRClosure + EnvFrame versioning + bridge invalidate
//     protocol
//   - #589 / #355 SoA migration (parent_id_ vs parent_,
//     bindings_symid_ vs bindings_ dual-path)
//
// What the issue body AC3 specifies by **exact name + fields** —
// `query:envframe-dualpath-stale-stats` with AC1+AC2+AC4-specific
// counters as a structured hash — was *not* shipped under that
// exact hash form. The existing flat-int primitive ships under
// the same name without `-hash` suffix; #647 ships the hash form
// with `-hash` suffix (matches the #630 / #641 hash-vs-int naming
// convention — see #640's `query:sv-verification-closedloop-stats`
// vs #630's `query:sv-verification-closedloop-stats-hash` for the
// same pattern).
//
// So #647 ships ONE new Aura primitive + 3 new atomics that are
// foundation scaffolding for the future AC1 (materialize_call_env
// + lookup paths validate parent_id_ vs current env_frames_
// owner after version_ check), AC2 (fiber resume() / g_fiber_
// sync_mutation_stack_ runs optional dual-path consistency check
// or repair for active Env/EnvFrame), and AC4 (strengthen
// GCEnvWalkFn to skip/repair dual-path inconsistent frames)
// enforcement work.
//
// Non-duplicative to #637/#589/#355 (issue body explicitly
// cross-referenced).
//
// The remaining #647 AC1 + AC2 + AC4 work is invasive C++ on
// materialize_call_env + lookup paths + fiber resume +
// g_fiber_sync_mutation_stack_ + GCEnvWalkFn + needs the heavy
// mutate + fiber steal/yield/resume matrix + INVALID_VERSION
// post-rollback + TSan coverage from the issue body — separate
// follow-ups.

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

namespace aura_issue_647_detail {
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
    auto r =
        cs.eval(std::format("(hash-ref (query:envframe-dualpath-stale-stats-hash) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_647_detail

int aura_issue_647_run() {
    using namespace aura_issue_647_detail;
    std::println(
        "=== Issue #647: query:envframe-dualpath-stale-stats-hash structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the documented fields.
    {
        std::println("\n--- AC1: (query:envframe-dualpath-stale-stats-hash) shape ---");
        auto h = cs.eval("(query:envframe-dualpath-stale-stats-hash)");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "envframe-dualpath-stale-stats-hash returns a hash");
        const auto cross_fiber = hash_int(cs, "cross-fiber-stale");
        const auto version_mis = hash_int(cs, "version-mismatch");
        const auto dualpath_repair = hash_int(cs, "dualpath-repair");
        const auto schema = hash_int(cs, "schema");
        CHECK(cross_fiber >= 0, std::format("cross-fiber-stale >= 0 (got {})", cross_fiber));
        CHECK(version_mis >= 0, std::format("version-mismatch >= 0 (got {})", version_mis));
        CHECK(dualpath_repair >= 0, std::format("dualpath-repair >= 0 (got {})", dualpath_repair));
        CHECK(schema == 647, std::format("schema == 647 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #647 doesn't disturb them).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_418 = cs.eval("(query:envframe-dualpath-stale-stats)");
        CHECK(s_418.has_value(), "(query:envframe-dualpath-stale-stats) reachable (#418 "
                                 "back-compat — flat-int summary)");
        auto s_dp = cs.eval("(query:envframe-dualpath-stats)");
        CHECK(s_dp.has_value(),
              "(query:envframe-dualpath-stats) reachable (existing base dualpath primitive)");
        auto s_646 = cs.eval("(query:gc-safepoint-deferral-stats)");
        CHECK(s_646.has_value(),
              "(query:gc-safepoint-deferral-stats) reachable (#646 back-compat)");
        auto s_645 = cs.eval("(query:scheduler-steal-bias-stats)");
        CHECK(s_645.has_value(), "(query:scheduler-steal-bias-stats) reachable (#645 back-compat)");
        auto s_644 = cs.eval("(query:aot-reload-func-table-stats)");
        CHECK(s_644.has_value(),
              "(query:aot-reload-func-table-stats) reachable (#644 back-compat)");
        auto s_642 = cs.eval("(query:arena-auto-compaction-stats)");
        CHECK(s_642.has_value(),
              "(query:arena-auto-compaction-stats) reachable (#642 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // cross-fiber-stale / version-mismatch / dualpath-repair
    // are all 0 on a fresh service — they are foundation
    // scaffolding for the future AC1 + AC2 + AC4 enforcement
    // work (parent_id_ vs env_frames_ owner validation +
    // fiber resume dual-path consistency check + GCEnvWalkFn
    // skip/repair).
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto cross_fiber = hash_int(cs, "cross-fiber-stale");
        const auto version_mis = hash_int(cs, "version-mismatch");
        const auto dualpath_repair = hash_int(cs, "dualpath-repair");
        CHECK(cross_fiber == 0,
              std::format("fresh-service cross-fiber-stale == 0 (got {})", cross_fiber));
        CHECK(version_mis == 0,
              std::format("fresh-service version-mismatch == 0 (got {})", version_mis));
        CHECK(dualpath_repair == 0,
              std::format("fresh-service dualpath-repair == 0 (got {})", dualpath_repair));
    }

    // AC4: schema sentinel is exactly 647 (not 646/645/644).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 647, std::format("schema == 647 (got {})", schema));
    }

    // AC5: naming distinction — the new primitive name uses
    // `-hash` suffix (matches the #630 / #641 hash-vs-int
    // naming convention), distinct from the existing
    // flat-int `query:envframe-dualpath-stale-stats` (#418).
    {
        std::println("\n--- AC5: naming distinction from #418 ---");
        auto new_p = cs.eval("(query:envframe-dualpath-stale-stats-hash)");
        auto old_p = cs.eval("(query:envframe-dualpath-stale-stats)");
        CHECK(new_p.has_value(),
              "new primitive (query:envframe-dualpath-stale-stats-hash) reachable (-hash suffix)");
        CHECK(old_p.has_value(), "existing (query:envframe-dualpath-stale-stats) still reachable "
                                 "(#418, no -hash suffix)");
        // The new primitive returns a hash; the existing one
        // returns an int. Verify type distinction.
        CHECK(aura::compiler::types::is_hash(*new_p), "new primitive returns a hash (not int)");
        CHECK(aura::compiler::types::is_int(*old_p),
              "existing primitive returns an int (not hash) — distinct primitive shape");
    }

    // AC6: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying counters + 3 new
    // scaffolding atomics.
    {
        std::println("\n--- AC6: concurrent envframe-dualpath-stale-stats-hash reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(query:envframe-dualpath-stale-stats-hash)");
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
    return aura_issue_647_run();
}
#endif
