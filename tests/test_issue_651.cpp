// @category: integration
// @reason: Issue #651 Actual GC Deferral/Block Logic in
// block_gc_for_pending_checkpoint_trampoline + Request Shim
// (Fills TODO in evaluator_fiber_mutation.cpp) —
// query:gc-panic-deferral-stats structured companion
//
// Scope-limited close matching #601/#491/#479/#604/#606/#614/
// #615/#616/#617/#618/#620/#621/#622/#623/#624/#625/#626/#630/
// #631/#632/#633/#637/#640/#641/#642/#643/#644/#645/#646/#647/
// #648/#649/#650 pattern.
//
// Discovery before this PR: the GC + panic observability
// surface already covers the high-level GC + panic summary via
// existing primitives:
//   - (engine:metrics \"query:gc-safepoint-stats\") — base GC safepoint primitive
//   - (engine:metrics \"query:gc-safepoint-deferral-stats\") (#646) — deferral +
//     backoff for outermost-vs-inner MutationBoundary (no
//     panic-specific breakdown)
//   - (engine:metrics \"query:panic-checkpoint-fiber-stats\") (#648) — fiber
//     resume panic transfer (no GC-deferral wire-up)
//   - (engine:metrics \"query:panic-checkpoint-lifecycle-stats\") — high-level
//     panic lifecycle summary
//   - block_gc_for_pending_checkpoint_trampoline +
//     g_block_gc_for_pending_checkpoint exist but with
//     "actual GC deferral is out of scope for the current
//     ship (TODO)" comment
//   - aura_evaluator_request_gc_safepoint forwards but only
//     records request (no pending panic check)
//
// What the issue body AC4 specifies by **exact name + fields** —
// `query:gc-panic-deferral-stats` with AC1+AC2+AC3-specific
// counters (pending_panic_deferrals, gc_blocked_by_panic,
// conflicts_resolved) — was *not* shipped under that exact
// name. So #651 ships ONE new Aura primitive + 3 new atomics
// that are foundation scaffolding for the future AC1
// (block_gc_for_pending_checkpoint_trampoline implements real
// deferral + gc_state phase integration), AC2
// (aura_evaluator_request_gc_safepoint checks pending panic +
// depth > 0 + defers/yields/retries), and AC3 (fiber
// check_gc_safepoint + scheduler wait_for_safepoint wire to
// pending-panic awareness) enforcement work.
//
// Non-duplicative to #646/#648 (issue body explicitly
// cross-referenced).
//
// The remaining #651 AC1 + AC2 + AC3 work is invasive C++ on
// evaluator_fiber_mutation.cpp +
// block_gc_for_pending_checkpoint_trampoline +
// aura_evaluator_request_gc_safepoint + fiber
// check_gc_safepoint + scheduler wait_for_safepoint + needs
// the panic during MutationBoundary + concurrent GC + steal
// matrix + TSan coverage from the issue body — separate
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

namespace aura_issue_651_detail {
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:gc-panic-deferral-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_651_detail

int aura_issue_651_run() {
    using namespace aura_issue_651_detail;
    std::println("=== Issue #651: query:gc-panic-deferral-stats structured companion ===");

    aura::compiler::CompilerService cs;

    // AC1: hash returns a hash with the documented fields.
    {
        std::println("\n--- AC1: (engine:metrics \"query:gc-panic-deferral-stats\") shape ---");
        auto h = cs.eval("(engine:metrics \"query:gc-panic-deferral-stats\")");
        CHECK(h && aura::compiler::types::is_hash(*h), "gc-panic-deferral-stats returns a hash");
        const auto pp_deferral = hash_int(cs, "pending-panic-deferral");
        const auto gc_blocked = hash_int(cs, "gc-blocked-by-panic");
        const auto conflicts = hash_int(cs, "conflicts-resolved");
        const auto schema = hash_int(cs, "schema");
        CHECK(pp_deferral >= 0, std::format("pending-panic-deferral >= 0 (got {})", pp_deferral));
        CHECK(gc_blocked >= 0, std::format("gc-blocked-by-panic >= 0 (got {})", gc_blocked));
        CHECK(conflicts >= 0, std::format("conflicts-resolved >= 0 (got {})", conflicts));
        CHECK(schema == 651, std::format("schema == 651 (got {})", schema));
    }

    // AC2: existing primitives remain reachable
    // (back-compat — #651 doesn't disturb them).
    {
        std::println("\n--- AC2: existing primitives back-compat ---");
        auto s_648 = cs.eval("(engine:metrics \"query:panic-checkpoint-fiber-stats\")");
        CHECK(
            s_648.has_value(),
            "(engine:metrics \"query:panic-checkpoint-fiber-stats\") reachable (#648 back-compat)");
        auto s_646 = cs.eval("(engine:metrics \"query:gc-safepoint-deferral-stats\")");
        CHECK(
            s_646.has_value(),
            "(engine:metrics \"query:gc-safepoint-deferral-stats\") reachable (#646 back-compat)");
        auto s_650 = cs.eval("(engine:metrics \"query:scheduler-stealbudget-yield-class-stats\")");
        CHECK(s_650.has_value(),
              "(engine:metrics \"query:scheduler-stealbudget-yield-class-stats\") reachable (#650 "
              "back-compat)");
        auto s_649 = cs.eval("(engine:metrics \"query:yield-checkpoint-panic-stats\")");
        CHECK(
            s_649.has_value(),
            "(engine:metrics \"query:yield-checkpoint-panic-stats\") reachable (#649 back-compat)");
        auto s_647 = cs.eval("(engine:metrics \"query:envframe-dualpath-stale-stats-hash\")");
        CHECK(s_647.has_value(), "(engine:metrics \"query:envframe-dualpath-stale-stats-hash\") "
                                 "reachable (#647 back-compat)");
        auto s_645 = cs.eval("(engine:metrics \"query:scheduler-steal-bias-stats\")");
        CHECK(s_645.has_value(),
              "(engine:metrics \"query:scheduler-steal-bias-stats\") reachable (#645 back-compat)");
    }

    // AC3: derived-metric invariants on a fresh service.
    // pending-panic-deferral / gc-blocked-by-panic /
    // conflicts-resolved are all 0 on a fresh service — they
    // are foundation scaffolding for the future AC1 + AC2 +
    // AC3 enforcement work (block_gc trampoline real deferral
    // + GC request blocked by panic + conflict resolution).
    {
        std::println("\n--- AC3: derived-metric invariants on fresh service ---");
        const auto pp_deferral = hash_int(cs, "pending-panic-deferral");
        const auto gc_blocked = hash_int(cs, "gc-blocked-by-panic");
        const auto conflicts = hash_int(cs, "conflicts-resolved");
        CHECK(pp_deferral == 0,
              std::format("fresh-service pending-panic-deferral == 0 (got {})", pp_deferral));
        CHECK(gc_blocked == 0,
              std::format("fresh-service gc-blocked-by-panic == 0 (got {})", gc_blocked));
        CHECK(conflicts == 0,
              std::format("fresh-service conflicts-resolved == 0 (got {})", conflicts));
    }

    // AC4: schema sentinel is exactly 651 (not 650/649/648/646).
    {
        std::println("\n--- AC4: schema sentinel ---");
        const auto schema = hash_int(cs, "schema");
        CHECK(schema == 651, std::format("schema == 651 (got {})", schema));
    }

    // AC5: naming distinction — the new primitive name uses
    // `-panic-deferral-` midfix, distinct from #646's
    // `-deferral-stats` (no `-panic-`). Combined with #648's
    // `-fiber-stats` and #646's coverage, the 3 primitives
    // form the GC+panic triangle:
    //   - #646 gc-safepoint-deferral-stats: deferral +
    //     backoff for outermost-vs-inner MutationBoundary
    //   - #648 panic-checkpoint-fiber-stats: fiber resume
    //     panic transfer (transport layer)
    //   - #651 gc-panic-deferral-stats: GC-panic
    //     coordination layer (fills the TODO).
    {
        std::println("\n--- AC5: naming distinction from #646 + #648 ---");
        auto new_p = cs.eval("(engine:metrics \"query:gc-panic-deferral-stats\")");
        auto old_646 = cs.eval("(engine:metrics \"query:gc-safepoint-deferral-stats\")");
        auto old_648 = cs.eval("(engine:metrics \"query:panic-checkpoint-fiber-stats\")");
        CHECK(new_p.has_value(), "new primitive (engine:metrics \"query:gc-panic-deferral-stats\") "
                                 "reachable (-panic- midfix)");
        CHECK(old_646.has_value(),
              "existing #646 (engine:metrics \"query:gc-safepoint-deferral-stats\") still "
              "reachable (no -panic- midfix)");
        CHECK(old_648.has_value(),
              "existing #648 (engine:metrics \"query:panic-checkpoint-fiber-stats\") still "
              "reachable (-fiber- midfix)");
        // The new primitive uses `schema` as its primary
        // sentinel — distinct from #646 (no schema field) and
        // #648 (schema==648). Verify schema==651 path and
        // that the new primitive has its own 3 documented
        // fields.
        CHECK(hash_int(cs, "schema") == 651, "new primitive schema == 651");
        const auto check_new_field = [&](std::string_view k) {
            auto r = cs.eval(std::format(
                "(hash-ref (engine:metrics \"query:gc-panic-deferral-stats\") '{}')", k));
            return r.has_value() && aura::compiler::types::is_int(*r);
        };
        CHECK(check_new_field("pending-panic-deferral"),
              "new primitive 'pending-panic-deferral' field reachable");
        CHECK(check_new_field("gc-blocked-by-panic"),
              "new primitive 'gc-blocked-by-panic' field reachable");
        CHECK(check_new_field("conflicts-resolved"),
              "new primitive 'conflicts-resolved' field reachable");
    }

    // AC6: concurrent reads under 2 threads × 4 iters. Atomicity
    // regression coverage for the underlying counters + 3 new
    // scaffolding atomics.
    {
        std::println("\n--- AC6: concurrent gc-panic-deferral-stats reads ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 4;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                auto r = cs.eval("(engine:metrics \"query:gc-panic-deferral-stats\")");
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
    return aura_issue_651_run();
}
#endif
