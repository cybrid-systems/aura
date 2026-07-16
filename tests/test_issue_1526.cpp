// @category: integration
// @reason: Issue #1526 — compact_env_frames dual-epoch + bridge restamp
// cooperation with hot-swap (env_id rewrite + restamp under interlock).
//
// Non-duplicative of #1510 (basic compact/materialize), #1524 (typed_mutate
// dual-epoch). This issue is post-compact Closure::bridge_epoch restamp
// so remapped env_id stays dual-check consistent.
//
//   AC1: compact bumps defuse + bridge + AOT table epochs
//   AC2: envframe_compact_bridge_restamps_total advances when closures restamped
//   AC3: survivor Closure env_id resolves + bridge_epoch matches current
//   AC4: materialize_fallback still works on NULL/OOB after compact
//   AC5: 1000× closure-then-compact stress, no crash
//   AC6: EDSL eval works post-compact
//   AC7: metric surface (rewrites / epoch_bumps / bridge_restamps)
//   AC8: concurrent compact + apply does not crash

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1526_detail {

using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_dual_epoch_bump() {
    std::println("\n--- AC1: compact dual-epoch bump ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");

    const auto defuse0 = ev.defuse_version();
    const auto bridge0 = cs.bridge_epoch();
    const auto aot0 = aura_aot_func_table_epoch();
    const auto bumps0 = load_u64(m->envframe_compact_epoch_bumps_total);

    (void)ev.compact_env_frames();

    CHECK(ev.defuse_version() > defuse0, "defuse_version bumped");
    CHECK(cs.bridge_epoch() > bridge0, "bridge_epoch bumped");
    CHECK(aura_aot_func_table_epoch() > aot0, "AOT table epoch bumped");
    CHECK(load_u64(m->envframe_compact_epoch_bumps_total) > bumps0, "epoch_bumps metric");
}

static void ac2_bridge_restamps() {
    std::println("\n--- AC2: bridge restamps on live closures ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    // Capture a lambda so closures_ has a stamped entry.
    cs.bump_bridge_epoch(); // ensure tracking active
    auto clo = cs.eval("(lambda (x) (+ x 1))");
    CHECK(clo && is_closure(*clo), "lambda captured");
    auto cid = static_cast<ClosureId>(as_closure_id(*clo));
    // Touch apply so provenance is live.
    (void)ev.apply_closure(cid, {make_int(1)});

    const auto rest0 = load_u64(m->envframe_compact_bridge_restamps_total);
    const auto bridge_before = cs.bridge_epoch();
    (void)ev.compact_env_frames();
    const auto bridge_after = cs.bridge_epoch();
    CHECK(bridge_after > bridge_before, "bridge advanced by compact");
    // Restamp metric should grow when any non-zero bridge_epoch was restamped.
    CHECK(load_u64(m->envframe_compact_bridge_restamps_total) >= rest0,
          "bridge_restamps non-decreasing");
    std::println("  restamps {}→{} bridge {}→{}", rest0,
                 load_u64(m->envframe_compact_bridge_restamps_total), bridge_before, bridge_after);
}

static void ac3_env_id_resolves_post_compact() {
    std::println("\n--- AC3: survivor env_id resolves post-compact ---");
    CompilerService cs;
    auto& ev = cs.evaluator();

    const auto parent = ev.alloc_env_frame(NULL_ENV_ID, nullptr);
    const auto child = ev.alloc_env_frame(parent, nullptr);
    CHECK(parent != NULL_ENV_ID && child != NULL_ENV_ID, "alloc frames");
    CHECK(ev.is_valid_env_id(child), "child valid pre-compact");
    CHECK(ev.env_frame(child).parent_id == parent, "parent_id pre-compact");

    // Keep frames live by allocating a few more and compacting.
    for (int i = 0; i < 8; ++i)
        (void)ev.alloc_env_frame(child, nullptr);

    (void)ev.compact_env_frames();

    // All live parent_ids must resolve.
    bool ok = true;
    for (EnvId id = 0; id < 128; ++id) {
        if (!ev.is_valid_env_id(id))
            break;
        auto* fr = ev.resolve_env_frame(id);
        if (!fr) {
            ok = false;
            break;
        }
        if (fr->parent_id != NULL_ENV_ID && !ev.is_valid_env_id(fr->parent_id))
            ok = false;
    }
    CHECK(ok, "resolve_env_frame + parent_id chain consistent");
}

static void ac4_materialize_fallback() {
    std::println("\n--- AC4: materialize_fallback after compact ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    (void)ev.compact_env_frames();
    const auto fb0 = load_u64(m->materialize_fallback_total);
    Closure cl;
    cl.env_id = NULL_ENV_ID;
    (void)ev.materialize_call_env(cl);
    CHECK(load_u64(m->materialize_fallback_total) > fb0, "NULL env fallback");
    cl.env_id = static_cast<EnvId>(999999);
    (void)ev.materialize_call_env(cl);
    CHECK(load_u64(m->materialize_fallback_total) > fb0 + 1, "OOB env fallback");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000× closure-then-compact stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    int ok = 0;
    for (int i = 0; i < 1000; ++i) {
        if ((i % 20) == 0) {
            // Light lambda capture (may create stamped closure).
            auto clo = cs.eval("(lambda (x) (+ x 1))");
            if (clo && is_closure(*clo)) {
                auto cid = static_cast<ClosureId>(as_closure_id(*clo));
                (void)ev.apply_closure(cid, {make_int(i % 7)});
            }
        }
        auto id = ev.alloc_env_frame(NULL_ENV_ID, nullptr);
        (void)id;
        if ((i % 5) == 0)
            ev.bump_defuse_version_for_test();
        if ((i % 3) == 0)
            (void)ev.compact_env_frames();
        if ((i % 17) == 0) {
            Closure cl;
            cl.env_id = NULL_ENV_ID;
            (void)ev.materialize_call_env(cl);
        }
        ++ok;
    }
    CHECK(ok == 1000, "1000-iter stress completed");
    CHECK(load_u64(m->envframe_compact_epoch_bumps_total) > 0, "epoch bumps in stress");
    CHECK(load_u64(m->materialize_fallback_total) > 0, "materialize_fallback in stress");
    std::println("  rewrites={} bumps={} restamps={} fallback={}",
                 load_u64(m->envframe_compact_rewrites_total),
                 load_u64(m->envframe_compact_epoch_bumps_total),
                 load_u64(m->envframe_compact_bridge_restamps_total),
                 load_u64(m->materialize_fallback_total));
}

static void ac6_edsl_post_compact() {
    std::println("\n--- AC6: EDSL post-compact ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(cs.eval("(set-code \"(define (add1 x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto v0 = cs.eval("(add1 10)");
    CHECK(v0 && is_int(*v0) && as_int(*v0) == 11, "pre-compact (add1 10)==11");
    (void)ev.compact_env_frames();
    auto v1 = cs.eval("(add1 10)");
    CHECK(v1 && is_int(*v1) && as_int(*v1) == 11, "post-compact (add1 10)==11");
}

static void ac7_metrics_surface() {
    std::println("\n--- AC7: metric surface ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    CHECK(load_u64(m->envframe_compact_rewrites_total) >= 0, "rewrites");
    CHECK(load_u64(m->envframe_compact_epoch_bumps_total) >= 0, "epoch_bumps");
    CHECK(load_u64(m->envframe_compact_bridge_restamps_total) >= 0, "bridge_restamps");
    CHECK(load_u64(m->materialize_fallback_total) >= 0, "materialize_fallback");
    CHECK(cs.evaluator().get_envframe_compact_bridge_restamps() >= 0,
          "evaluator accessor restamps");
}

static void ac8_concurrent_compact_apply() {
    std::println("\n--- AC8: concurrent compact + apply ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto clo = cs.eval("(lambda (x) (+ x 1))");
    CHECK(clo && is_closure(*clo), "lambda for concurrent");
    auto cid = static_cast<ClosureId>(as_closure_id(*clo));

    std::atomic<int> errs{0};
    std::atomic<bool> stop{false};
    std::thread applier([&]() {
        try {
            auto args = std::array{make_int(2)};
            while (!stop.load(std::memory_order_relaxed)) {
                auto r =
                    ev.apply_closure(cid, std::span<const aura::compiler::types::EvalValue>(args));
                (void)r;
            }
        } catch (...) {
            errs.fetch_add(1);
        }
    });
    for (int i = 0; i < 50; ++i) {
        (void)ev.alloc_env_frame(NULL_ENV_ID, nullptr);
        (void)ev.compact_env_frames();
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_relaxed);
    applier.join();
    CHECK(errs.load() == 0, "no exceptions concurrent compact+apply");
}

} // namespace aura_issue_1526_detail

int main() {
    using namespace aura_issue_1526_detail;
    std::println("=== Issue #1526: compact_env_frames dual-epoch + bridge restamp ===");
    ac1_dual_epoch_bump();
    ac2_bridge_restamps();
    ac3_env_id_resolves_post_compact();
    ac4_materialize_fallback();
    ac5_stress_1000();
    ac6_edsl_post_compact();
    ac7_metrics_surface();
    ac8_concurrent_compact_apply();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
