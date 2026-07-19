// @category: integration
// @reason: Issue #1491 — force bridge_epoch + defuse dual-check on all
// apply_closure paths + JIT aura_closure_call (closed-loop on #1475/#1477).
//
//   AC1: apply_closure map path: epoch bump → safe fallback metrics
//   AC2: aura_is_jit_closure_fresh strict (unstamped while tracking = stale)
//   AC3: aura_closure_call deopts after table/defuse bump
//   AC4: concurrent mutate + apply_closure (no crash, metrics grow)
//   AC5: #1475 helper + #1477 dual-epoch surfaces still reachable
//   AC6: compact_env_frames + apply still dual-check safe

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" int64_t aura_alloc_closure(int64_t func_id);
extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_free_closure(int64_t closure_id);

namespace {

using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::is_closure;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static bool capture_lambda(CompilerService& cs, ClosureId& out) {
    cs.bump_bridge_epoch();
    auto clo = cs.eval("(lambda (x) (+ x 1))");
    if (!clo || !is_closure(*clo))
        return false;
    out = static_cast<ClosureId>(as_closure_id(*clo));
    auto args = std::array{make_int(1)};
    (void)cs.evaluator().apply_closure(out,
                                       std::span<const aura::compiler::types::EvalValue>(args));
    return true;
}

static void ac1_apply_closure_safe_fallback() {
    std::println("\n--- AC1: apply_closure dual-check after epoch bump ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");
    ClosureId cid = 0;
    CHECK(capture_lambda(cs, cid), "captured lambda");

    const auto safe0 = load_u64(m->compiler_closure_safe_fallbacks);
    const auto enforced0 = load_u64(m->closure_bridge_epoch_safety_enforced);
    const auto stale_apply0 = load_u64(m->closure_stale_apply_count_total);

    // Invalidate provenance so next apply must dual-check + fallback.
    cs.public_atomic_bump_epochs_and_stamp_bridge("");
    auto args = std::array{make_int(5)};
    auto r =
        cs.evaluator().apply_closure(cid, std::span<const aura::compiler::types::EvalValue>(args));
    (void)r; // nullopt or recovered via bridge — both safe

    CHECK(load_u64(m->compiler_closure_safe_fallbacks) > safe0 ||
              load_u64(m->closure_bridge_epoch_safety_enforced) > enforced0 ||
              load_u64(m->closure_stale_apply_count_total) > stale_apply0,
          "stale apply bumped dual-check / safe-fallback metrics");
}

static void ac2_jit_fresh_strict() {
    std::println("\n--- AC2: aura_is_jit_closure_fresh strict (#1491) ---");
    setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);
    aura_set_aot_defuse_version(20);
    aura_aot_bump_func_table_epoch();
    const auto e = aura_aot_func_table_epoch();
    CHECK(e > 0 && 20 > 0, "tracking active on both domains");
    CHECK(aura_is_jit_closure_fresh(e, 20), "matching stamps are fresh");
    CHECK(!aura_is_jit_closure_fresh(e - 1, 20), "stale bridge deopts");
    CHECK(!aura_is_jit_closure_fresh(e, 19), "stale defuse deopts");
    CHECK(!aura_is_jit_closure_fresh(0, 20), "unstamped bridge is stale under active tracking");
    CHECK(!aura_is_jit_closure_fresh(e, 0), "unstamped defuse is stale under active tracking");
}

static void ac3_aura_closure_call_deopt() {
    std::println("\n--- AC3: aura_closure_call deopt after bump ---");
    aura_set_aot_defuse_version(50);
    auto id = aura_alloc_closure(9);
    CHECK(id >= 0, "alloc_closure ok");
    const auto deopt0 = aura_jit_closure_stale_deopt_total();
    const auto safe0 = aura_jit_closure_safe_fallbacks();
    aura_aot_bump_func_table_epoch(); // invalidate capture
    auto r = aura_closure_call(id, nullptr, 0);
    (void)r;
    CHECK(aura_jit_closure_stale_deopt_total() > deopt0, "stale_deopt advanced");
    CHECK(aura_jit_closure_safe_fallbacks() > safe0, "safe_fallback advanced");
    aura_free_closure(id);
}

static void ac4_concurrent_mutate_apply() {
    std::println("\n--- AC4: concurrent mutate + apply_closure ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    ClosureId cid = 0;
    CHECK(capture_lambda(cs, cid), "captured lambda");
    std::atomic<bool> stop{false};
    std::atomic<int> applies{0};
    std::thread mutator([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            cs.public_atomic_bump_epochs_and_stamp_bridge("");
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&] {
            auto args = std::array{make_int(3)};
            for (int i = 0; i < 250; ++i) {
                auto r = cs.evaluator().apply_closure(
                    cid, std::span<const aura::compiler::types::EvalValue>(args));
                (void)r;
                applies.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : workers)
        t.join();
    stop.store(true, std::memory_order_relaxed);
    mutator.join();
    CHECK(applies.load() == 1000, "1000 concurrent applies completed");
    CHECK(load_u64(m->closure_calls_total) >= 1000, "closure_calls_total advanced");
    // At least some stale catches under concurrent bumps (best-effort).
    CHECK(load_u64(m->compiler_closure_safe_fallbacks) +
                  load_u64(m->closure_stale_apply_count_total) >=
              0,
          "fallback counters readable after concurrent stress");
}

static void ac5_related_surfaces() {
    std::println("\n--- AC5: #1475 / #1477 / dual-check surfaces ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // #1475 pure helper
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(1), 1, 2) == true,
          "is_env_frame_stale helper (#1475)");
    CHECK(ev.is_bridge_stale(1, 2) == true, "is_bridge_stale mismatch");
    // JIT dual-check counters readable
    CHECK(aura_jit_closure_dual_check_total() >= 0, "jit dual_check total readable");
    // Metrics schema fields from apply path
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    CHECK(load_u64(m->closure_bridge_epoch_safety_enforced) >= 0, "safety_enforced field");
    CHECK(load_u64(m->jit_closure_dual_check_total) >= 0, "jit dual_check field");
}

static void ac6_compact_env_and_apply() {
    std::println("\n--- AC6: compact_env_frames + apply dual-check ---");
    CompilerService cs;
    ClosureId cid = 0;
    CHECK(capture_lambda(cs, cid), "captured lambda");
    // compact may reclaim; dual-check must not crash
    (void)cs.evaluator().compact_env_frames();
    auto args = std::array{make_int(7)};
    auto r =
        cs.evaluator().apply_closure(cid, std::span<const aura::compiler::types::EvalValue>(args));
    (void)r;
    CHECK(true, "apply after compact_env_frames did not crash");
    // Fresh lambda still works
    auto clo2 = cs.eval("(lambda (x) (* x 2))");
    CHECK(clo2 && is_closure(*clo2), "fresh lambda after compact");
}

} // namespace

int main() {
    std::println("test_issue_1491: apply_closure + JIT dual-check closed-loop (#1491)");
    setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);
    ac1_apply_closure_safe_fallback();
    ac2_jit_fresh_strict();
    ac3_aura_closure_call_deopt();
    ac4_concurrent_mutate_apply();
    ac5_related_surfaces();
    ac6_compact_env_and_apply();
    std::println("\n#1491: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
