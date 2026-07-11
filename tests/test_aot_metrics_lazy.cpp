// test_aot_metrics_lazy.cpp — Issue #1368: lazy g_aot_metrics from Evaluator

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "runtime_shared.h"

#include <cstdint>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;

int main() {
    // Start from a clean metrics pointer (may be dirty after other tests)
    aura_set_aot_metrics(nullptr);
    CHECK(aura_get_aot_metrics() == nullptr, "metrics cleared to null");

    // ── set_compiler_metrics auto-wires AOT metrics ──
    {
        Evaluator ev;
        CompilerMetrics metrics;
        const auto expl0 = aura_aot_metrics_explicit_sets_total();
        ev.set_compiler_metrics(&metrics);
        CHECK(aura_get_aot_metrics() == &metrics, "auto-wire g_aot_metrics == &metrics");
        CHECK(aura_aot_metrics_explicit_sets_total() == expl0 + 1,
              "explicit sets +1 via auto-wire");
        // Reload missing file should bump counters on metrics
        const auto att0 = metrics.aot_reload_attempts_.load(std::memory_order_relaxed);
        const auto rb0 = metrics.aot_hot_update_atomic_rollback.load(std::memory_order_relaxed);
        bool ok = aura_reload_aot_module("/tmp/aura_metrics_lazy_missing_1368.so", 0);
        CHECK(!ok, "missing so fails");
        CHECK(metrics.aot_reload_attempts_.load(std::memory_order_relaxed) == att0 + 1,
              "reload_attempts increments with auto-wired metrics");
        CHECK(metrics.aot_hot_update_atomic_rollback.load(std::memory_order_relaxed) >= rb0 + 1,
              "atomic_rollback increments");
        aura_set_aot_metrics(nullptr);
    }

    // ── aura_ensure_aot_metrics is lazy (no overwrite) ──
    {
        CompilerMetrics m1;
        CompilerMetrics m2;
        aura_set_aot_metrics(nullptr);
        const auto lazy0 = aura_aot_metrics_lazy_init_total();
        aura_ensure_aot_metrics(&m1);
        CHECK(aura_get_aot_metrics() == &m1, "ensure binds m1");
        CHECK(aura_aot_metrics_lazy_init_total() == lazy0 + 1, "lazy_init +1");
        aura_ensure_aot_metrics(&m2);
        CHECK(aura_get_aot_metrics() == &m1, "ensure does not overwrite m1 with m2");
        aura_set_aot_metrics(&m2);
        CHECK(aura_get_aot_metrics() == &m2, "explicit set overwrites");
        aura_set_aot_metrics(nullptr);
    }

    // ── ensure with null is no-op ──
    {
        aura_set_aot_metrics(nullptr);
        aura_ensure_aot_metrics(nullptr);
        CHECK(aura_get_aot_metrics() == nullptr, "ensure null leaves null");
    }

    // ── Aura (aot:reload) uses ensure (CompilerService already has metrics) ──
    {
        CompilerService cs;
        // Service ctor wires metrics; ensure should not change pointer
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "CS has metrics");
        CHECK(aura_get_aot_metrics() == m || aura_get_aot_metrics() != nullptr,
              "AOT metrics bound after CS create");
        const auto att0 = m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed);
        auto r = cs.eval("(aot:reload \"/tmp/aura_metrics_lazy_missing2_1368.so\")");
        CHECK(r && is_bool(*r) && !as_bool(*r), "aot:reload missing → #f");
        CHECK(m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed) == att0 + 1,
              "via_primitive attempts +1");
    }

    // ── Bare Evaluator path: set metrics then region/reload counters work ──
    {
        aura_set_aot_metrics(nullptr);
        Evaluator ev;
        CompilerMetrics metrics;
        ev.set_compiler_metrics(&metrics);
        aura_set_aot_region_mask_for_eval(&ev, 3);
        CHECK(metrics.aot_per_eval_region_sets.load(std::memory_order_relaxed) >= 1,
              "region set bumps via auto-wired metrics");
        aura_cleanup_aot_state(&ev);
        aura_set_aot_metrics(nullptr);
    }

    // ── Backward compat: explicit set still works ──
    {
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        CHECK(aura_get_aot_metrics() == &metrics, "explicit set");
        const auto att0 = metrics.aot_reload_attempts_.load(std::memory_order_relaxed);
        (void)aura_reload_aot_module(nullptr, 0);
        CHECK(metrics.aot_reload_attempts_.load(std::memory_order_relaxed) == att0 + 1,
              "null path still counts attempts");
        aura_set_aot_metrics(nullptr);
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("aot metrics lazy #1368: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
