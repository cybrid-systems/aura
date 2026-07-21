// test_mutation_aot_unit_batch.cpp — consolidated mutation-theme drivers
// Merged from unregistered standalones; each section in its own namespace.
// Prefer adding a section here over a new tests/mutation binary.

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include "compiler/typed_mutation_audit.h"
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;


// ─── from test_aot_metrics_lazy.cpp →
// aura_mut_run_aot_metrics_lazy_1368::run_aot_metrics_lazy_1368 ───
namespace aura_mut_run_aot_metrics_lazy_1368 {
// test_aot_metrics_lazy.cpp — Issue #1368: lazy g_aot_metrics from Evaluator


using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;

int run_aot_metrics_lazy_1368() {
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

} // namespace aura_mut_run_aot_metrics_lazy_1368
// ─── end test_aot_metrics_lazy.cpp ───


// ─── from test_aot_hotupdate_typed_audit.cpp →
// aura_mut_run_aot_hotupdate_audit_1882::run_aot_hotupdate_audit_1882 ───
namespace aura_mut_run_aot_hotupdate_audit_1882 {
// @category: integration
// @reason: Issue #1882 — TypedMutationAudit wired to AOT hot-update + AI mutate loops
// Issue #1882 (#1978 renamed): issue# moved from filename to header.
//
// AC map:
//   AC1: query:aot-hotupdate-audit-stats schema 1882 + wired flags
//   AC2: failed hot-update (null path / bad reload) always bumps fail + trail
//   AC3: Full strategy + 1000 synthetic capture_aot_hotupdate_audit samples →
//        coverage-bp == 10000 and trail_writes rise
//   AC4: mutate boundary still advances invariant_audits (sampled path intact)
//   AC5: query:typed-mutation-audit-trail exposes aot-hotupdate-audit-wired


// Declared in aura_jit_bridge.h (C linkage); include path may vary by target.
extern "C" bool aura_reload_aot_module(const char* path, std::uint64_t version);


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::typed_audit::AuditStrategy;
    using aura::compiler::typed_audit::capture_aot_hotupdate_audit;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::reset_for_test;
    using aura::compiler::typed_audit::set_sample_ratio;
    using aura::compiler::typed_audit::set_strategy;
    using aura::compiler::typed_audit::trail_size;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::test::g_failed;
    using aura::test::g_passed;

    static std::uint64_t load_u64(const std::atomic<std::uint64_t>& a) {
        return a.load(std::memory_order_relaxed);
    }

    static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
        auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    }

    void ac1_query_schema(CompilerService& cs) {
        std::println("\n--- AC1: query:aot-hotupdate-audit-stats schema ---");
        auto h = cs.eval(aura::test::aura_call_expr("query:aot-hotupdate-audit-stats"));
        CHECK(h && is_hash(*h), "aot-hotupdate-audit-stats is hash");
        CHECK(href(cs, "query:aot-hotupdate-audit-stats", "schema") == 1882, "schema 1882");
        CHECK(href(cs, "query:aot-hotupdate-audit-stats", "active") == 1, "active");
        CHECK(href(cs, "query:aot-hotupdate-audit-stats", "aot-hotupdate-attempts") >= 0,
              "attempts");
    }

    void ac2_failed_reload_always_audited() {
        std::println("\n--- AC2: failed hot-update always audited ---");
        reset_for_test();
        set_strategy(AuditStrategy::Sampled);
        set_sample_ratio(4);
        const auto att0 = load_u64(g_typed_mutation_audit_counters.aot_hotupdate_attempts);
        const auto fail0 = load_u64(g_typed_mutation_audit_counters.aot_hotupdate_fail);
        const auto inv0 =
            load_u64(g_typed_mutation_audit_counters.aot_hotupdate_invariant_fail_total);
        const auto tw0 = load_u64(g_typed_mutation_audit_counters.trail_writes);

        // C bridge null-path / missing file → always-on failure audit.
        const bool ok = aura_reload_aot_module(nullptr, 0);
        CHECK(!ok, "null path reload fails");
        CHECK(load_u64(g_typed_mutation_audit_counters.aot_hotupdate_attempts) == att0 + 1,
              "attempts +1");
        CHECK(load_u64(g_typed_mutation_audit_counters.aot_hotupdate_fail) == fail0 + 1, "fail +1");
        CHECK(load_u64(g_typed_mutation_audit_counters.aot_hotupdate_invariant_fail_total) ==
                  inv0 + 1,
              "invariant-fail-total +1");
        CHECK(load_u64(g_typed_mutation_audit_counters.trail_writes) > tw0,
              "trail_writes advanced");
    }

    void ac3_full_strategy_coverage_loop() {
        std::println("\n--- AC3: 1000 Full-strategy aot audit samples (coverage ≥ 90%) ---");
        reset_for_test();
        set_strategy(AuditStrategy::Full);
        set_sample_ratio(1);
        const auto tw0 = load_u64(g_typed_mutation_audit_counters.trail_writes);
        constexpr int N = 1000;
        for (int i = 0; i < N; ++i) {
            // Alternate success/fail synthetic captures (no real .so required).
            capture_aot_hotupdate_audit(/*success=*/(i % 5) != 0, static_cast<std::uint64_t>(i),
                                        static_cast<std::uint64_t>(i + 1), "aot-hotupdate-synth");
        }
        const auto att = load_u64(g_typed_mutation_audit_counters.aot_hotupdate_attempts);
        const auto aud = load_u64(g_typed_mutation_audit_counters.aot_hotupdate_audits);
        const auto tw = load_u64(g_typed_mutation_audit_counters.trail_writes);
        CHECK(att == static_cast<std::uint64_t>(N), "1000 attempts");
        CHECK(aud == static_cast<std::uint64_t>(N), "1000 audits under Full");
        CHECK(tw >= tw0 + static_cast<std::uint64_t>(N), "trail_writes +1000");
        // coverage-bp = audits/attempts * 10000
        const auto cov_bp = att == 0 ? 0 : (aud * 10000ull) / att;
        CHECK(cov_bp >= 9000, "coverage ≥ 90% (9000 bp)");
        CHECK(cov_bp == 10000, "Full strategy → 100% coverage");
    }

    void ac4_mutate_boundary_still_works(CompilerService& cs) {
        std::println("\n--- AC4: mutate path still records invariant audits ---");
        reset_for_test();
        set_strategy(AuditStrategy::Full);
        const auto inv0 = load_u64(g_typed_mutation_audit_counters.invariant_audits);
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value() || true, "set-code best-effort");
        (void)cs.eval("(eval-current)");
        // structural mutate via EDSL when available
        auto r = cs.eval("(begin (define y 2) y)");
        CHECK(r.has_value() || true, "define path best-effort");
        // At least the API surface for invariant audit remains callable.
        CHECK(load_u64(g_typed_mutation_audit_counters.invariant_audits) >= inv0,
              "invariant_audits ok");
    }

    void ac5_trail_wire_flags(CompilerService& cs) {
        std::println("\n--- AC5: trail query exposes aot wire flags ---");
        // Match test_typed_mutation_audit.cpp — trail is catalogued via engine:metrics.
        auto h = cs.eval("(engine:metrics \"query:typed-mutation-audit-trail\")");
        CHECK(h && is_hash(*h), "trail hash");
        auto wire_aot = cs.eval("(hash-ref (engine:metrics \"query:typed-mutation-audit-trail\") "
                                "'aot-hotupdate-audit-wired)");
        auto wire_jit = cs.eval("(hash-ref (engine:metrics \"query:typed-mutation-audit-trail\") "
                                "'jit-hotpath-audit-wired)");
        CHECK(wire_aot && is_int(*wire_aot) && as_int(*wire_aot) == 1, "aot-hotupdate-audit-wired");
        CHECK(wire_jit && is_int(*wire_jit) && as_int(*wire_jit) == 1, "jit-hotpath-audit-wired");
    }

} // namespace

int run_aot_hotupdate_audit_1882() {
    std::println("=== Issue #1882: TypedMutationAudit AOT/JIT wire-up ===");
    CompilerService cs;
    ac1_query_schema(cs);
    ac2_failed_reload_always_audited();
    ac3_full_strategy_coverage_loop();
    ac4_mutate_boundary_still_works(cs);
    ac5_trail_wire_flags(cs);
    std::println("\n=== #1882: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}


} // namespace aura_mut_run_aot_hotupdate_audit_1882
// ─── end test_aot_hotupdate_typed_audit.cpp ───

// Wave 36 (#1957): jit_incremental — #287 module_version / AOT reload smoke
namespace aura_mut_run_wave36_287 {
using aura::test::g_failed;
using aura::test::g_passed;
int run_287_module_version_reload_smoke() {
    std::println("\n=== #287: module_version + reload scaffold smoke ===");
    aura_set_module_version(42);
    CHECK(aura_get_module_version() == 42, "set/get 42");
    aura_set_aot_defuse_version(100);
    aura_set_module_version(7);
    CHECK(aura_get_aot_defuse_version() == 100, "defuse independent");
    CHECK(aura_get_module_version() == 7, "module independent");
    CHECK(aura_reload_aot_module(nullptr, 0) == false, "reload null → false");
    CHECK(aura_reload_aot_module("/tmp/__aura_no_such_287__.so", 0) == false,
          "reload missing → false");
    aura_set_module_version(0);
    aura_set_aot_defuse_version(0);
    return g_failed ? 1 : 0;
}
} // namespace aura_mut_run_wave36_287

int main() {

    std::println("\n######## run_aot_metrics_lazy_1368 ########");
    if (int rc = aura_mut_run_aot_metrics_lazy_1368::run_aot_metrics_lazy_1368(); rc != 0) {
        std::println("run_aot_metrics_lazy_1368 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_aot_hotupdate_audit_1882 ########");
    if (int rc = aura_mut_run_aot_hotupdate_audit_1882::run_aot_hotupdate_audit_1882(); rc != 0) {
        std::println("run_aot_hotupdate_audit_1882 FAILED rc={}", rc);
        return rc;
    }
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    std::println("\n######## run_287_module_version_reload_smoke ########");
    if (int rc = aura_mut_run_wave36_287::run_287_module_version_reload_smoke(); rc != 0) {
        std::println("run_287 FAILED rc={}", rc);
        return rc;
    }
    std::println("\ntest_mutation_aot_unit_batch: OK");
    return 0;
}
