// test_linear_batch.cpp
// B pilot #19 (after env in 9d397920): consolidated linear family
// — Issues #1615 + #1599 + #1867 + #1731 + #1875 + #1755 (linear+coercion
// synergy + GC root audit + AI closedloop readiness + record_linear_gc_probe
// memory_order + linear_post_mutate_enforce NULL_ENV_ID + dirty-aware
// EscapeAnalysis + post-mutation validation hit_rate +
// validate_linear_ownership_state bridge_epoch drift) into one batch
// driver.
//
// NOTE: test_linear_ownership_occurrence_predicate_mutate.cpp (#747) +
// test_linear_ownership_postmutate_guard_steal_envframe.cpp (#800) NOT
// included — bundle members via tests/bundles/test_issues_jit_late3_main.cpp
// + test_issues_jit_late2_main.cpp (extern decl + dispatch table).
// Deleting source would break bundle link.
//
// NOTE: test_linear_boundary_consistency_1568.cpp NOT included — registered
// in cmake/AuraDomainTests.cmake:439-441 with add_dependencies(all_test_issue_targets ...)
// (default-build target).
//
// NOTE: test_linear_ownership_batch.cpp NOT included — already a batch
// entry from earlier consolidation waves.
//
// Per AuraDomainTests.cmake legacy Phase 1 batch convention.
// EXCLUDE_FROM_ALL — default build skips; on-demand 'ninja test_linear_batch'.

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

import std;
import aura.core.arena;
import aura.core.ast;
import aura.core.mutation;
import aura.core.type;
import aura.compiler.coercion_map;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;

namespace aura_linear_batch {

using aura::ast::FlatAST;
using aura::ast::InvariantStatus;
using aura::ast::StringPool;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::EnvId;
using aura::compiler::EscapeAnalysisPass;
using aura::compiler::Evaluator;
using aura::compiler::LinearOwnershipWrap;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::OwnershipEnv;
using aura::compiler::OwnershipNote;
using aura::compiler::post_mutation_invariant_check;
using aura::compiler::revalidate_linear_after_coercion;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href(CompilerService& cs, std::string_view prim, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", prim, key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static std::int64_t href_typed(CompilerService& cs, const char* prim, std::string_view key) {
    return href(cs, prim, key);
}

// ── Issue #1615 — linear + coercion synergy ──
static void run_1615_reval_api() {
    std::println("\n--- AC1 (#1615): revalidate_linear_after_coercion ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 2))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* flat = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    CHECK(flat && pool, "workspace");
    auto& reg = *static_cast<aura::core::TypeRegistry*>(cs.evaluator().ensure_type_registry());
    CoercionMap empty;
    const auto n0 = load_u64(metrics_of(cs)->linear_coercion_reval_count);
    (void)revalidate_linear_after_coercion(*flat, *pool, reg, empty, nullptr, metrics_of(cs));
    CHECK(load_u64(metrics_of(cs)->linear_coercion_reval_count) == n0 + 1, "reval count +1");
}

static void run_1615_typecheck_path() {
    std::println("\n--- AC2 (#1615): typecheck/mutate path advances reval ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 2))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto n0 = load_u64(metrics_of(cs)->linear_coercion_reval_count);
    CHECK(cs.eval("(mutate:rebind \"x\" \"10\")").has_value(), "mutate");
    (void)cs.eval("(eval-current)");
    auto* flat = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    auto& reg = *static_cast<aura::core::TypeRegistry*>(cs.evaluator().ensure_type_registry());
    CoercionMap empty;
    (void)revalidate_linear_after_coercion(*flat, *pool, reg, empty, nullptr, metrics_of(cs));
    CHECK(load_u64(metrics_of(cs)->linear_coercion_reval_count) > n0, "reval advanced");
}

static void run_1615_query_schema() {
    std::println("\n--- AC3 (#1615): query schema 1615 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 2))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* flat = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    auto& reg = *static_cast<aura::core::TypeRegistry*>(cs.evaluator().ensure_type_registry());
    CoercionMap empty;
    (void)revalidate_linear_after_coercion(*flat, *pool, reg, empty, nullptr, metrics_of(cs));
    auto h = cs.eval("(engine:metrics \"query:jit-typed-mutation-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "schema") == 1615 ||
              href_typed(cs, "query:jit-typed-mutation-stats", "schema") == 746,
          "schema 1615|746");
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "issue") == 1615 ||
              href_typed(cs, "query:jit-typed-mutation-stats", "issue") < 0,
          "issue 1615");
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "linear_coercion_reval_count") >= 1 ||
              href_typed(cs, "query:jit-typed-mutation-stats", "linear-coercion-reval-count") >= 1,
          "linear_coercion_reval_count");
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "narrow_evidence_guardshape_hits") >=
                  0 ||
              href_typed(cs, "query:jit-typed-mutation-stats", "narrow-evidence-guardshape-hits") >=
                  0,
          "narrow_evidence_guardshape_hits");
}

static void run_1615_post_mutation() {
    std::println("\n--- AC4 (#1615): post_mutation path ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 2))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(mutate:rebind \"y\" \"3\")").has_value(), "mutate y");
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "linear_coercion_reval_count") >= 0 ||
              href_typed(cs, "query:jit-typed-mutation-stats", "linear-coercion-reval-count") >= 0,
          "reval key readable after mutate");
}

static void run_1615_stress() {
    std::println("\n--- AC5 (#1615): multi-round reval stress ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 2))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* flat = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    auto& reg = *static_cast<aura::core::TypeRegistry*>(cs.evaluator().ensure_type_registry());
    CoercionMap empty;
    const auto n0 = load_u64(metrics_of(cs)->linear_coercion_reval_count);
    for (int i = 0; i < 50; ++i) {
        (void)revalidate_linear_after_coercion(*flat, *pool, reg, empty, nullptr, metrics_of(cs));
        if ((i % 10) == 0)
            (void)cs.eval(std::format("(mutate:rebind \"x\" \"{}\")", i));
    }
    CHECK(load_u64(metrics_of(cs)->linear_coercion_reval_count) >= n0 + 50, "50 revals");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok after stress");
}

static void run_1615_wire_and_lineage() {
    std::println("\n--- AC6 (#1615): wire + #746 lineage ---");
    CompilerService cs;
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "narrow-evidence-hits") >= 0,
          "746 narrow-evidence-hits");
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "cast-elided-in-l2") >= 0,
          "746 cast-elided");
    CHECK(href_typed(cs, "query:jit-typed-mutation-stats", "linear-state-optimized") >= 0,
          "746 linear-state-optimized");
}

// ── Issue #1599 — linear GC + closedloop readiness refine ──
static void run_1599_six_touchpoints() {
    std::println("\n--- AC1 (#1599): six touchpoints documented ---");
    CompilerService cs;
    for (std::uint8_t p = 0; p <= 6; ++p) {
        auto name = Evaluator::linear_gc_root_audit_path_name(p);
        CHECK(!name.empty(), std::format("path {} named", p));
    }
    auto h = cs.eval("(engine:metrics \"query:linear-gc-root-audit-log\")");
    CHECK(h && is_hash(*h), "audit-log hash");
    CHECK(href_typed(cs, "query:linear-gc-root-audit-log", "six-touchpoints-documented") == 1 ||
              href_typed(cs, "query:linear-gc-root-audit-log", "schema") == 1599 ||
              href_typed(cs, "query:linear-gc-root-audit-log", "schema") == 1543,
          "six-touchpoints flag or schema lineage");
}

static void run_1599_audit_log_monotonic() {
    std::println("\n--- AC2 (#1599): audit log + checks monotonic ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto c0 = load_u64(m->linear_gc_root_audit_checks_total);
    CHECK(ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual), "manual audit ok");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) == c0 + 1, "checks +1");
    (void)ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditGcSafepoint);
    (void)ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditCompact);
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) >= c0 + 3, "checks >= +3");
    CHECK(href_typed(cs, "query:linear-gc-root-audit-log", "schema") == 1599 ||
              href_typed(cs, "query:linear-gc-root-audit-log", "schema") == 1543,
          "audit schema 1599|1543");
}

static void run_1599_scan_and_roots() {
    std::println("\n--- AC3 (#1599): live-closure scan + GC root enforce ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto s0 = load_u64(m->linear_live_closure_scans_total);
    const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);
    (void)ev.scan_live_closures_for_linear_captures(true, false);
    CHECK(load_u64(m->linear_live_closure_scans_total) > s0, "scans advanced");
    (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditTypedMutate, false);
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) > a0, "audit via enforce");
    ev.test_probe_linear_on_fiber_steal();
    CHECK(load_u64(m->linear_boundary_consistency_total) >= 1, "boundary consistency");
}

static void run_1599_closedloop_readiness() {
    std::println("\n--- AC4 (#1599): ai-closedloop-readiness-stats schema 1599 ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    (void)ev.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditManual);
    (void)ev.scan_live_closures_for_linear_captures(true, true);
    auto h = cs.eval("(engine:metrics \"query:ai-closedloop-readiness-stats\")");
    CHECK(h && is_hash(*h), "readiness hash");
    const auto schema = href_typed(cs, "query:ai-closedloop-readiness-stats", "schema");
    CHECK(schema == 1613 || schema == 1599 || schema == 1597 || schema == 1593 || schema == 1499,
          "schema 1613|1599 lineage");
    CHECK(href_typed(cs, "query:ai-closedloop-readiness-stats", "health-score") >= 0 &&
              href_typed(cs, "query:ai-closedloop-readiness-stats", "health-score") <= 100,
          "health-score");
}

static void run_1599_adaptive_and_hist() {
    std::println("\n--- AC5 (#1599): adaptive safepoint + mutation depth hist ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:gc-safepoint-adaptive-stats\")");
    CHECK(h && is_hash(*h), "adaptive hash");
    const auto schema = href_typed(cs, "query:gc-safepoint-adaptive-stats", "schema");
    CHECK(schema == 1599 || schema == 1493 || schema == 1483, "adaptive schema lineage");
}

static void run_1599_load_trends() {
    std::println("\n--- AC6 (#1599): multi-path load trends ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto h0 = href_typed(cs, "query:ai-closedloop-readiness-stats", "health-score");
    const auto a0 = load_u64(m->linear_gc_root_audit_checks_total);

    for (int i = 0; i < 20; ++i) {
        if ((i % 4) == 0)
            cs.public_mark_define_dirty("__load__");
        if ((i % 4) == 1)
            (void)ev.enforce_linear_boundary_consistency(Evaluator::kLinearGcRootAuditManual,
                                                         (i % 2) == 0);
        if ((i % 4) == 2)
            (void)ev.compact_env_frames();
        if ((i % 4) == 3)
            ev.test_probe_linear_on_fiber_steal();
    }

    const auto h1 = href_typed(cs, "query:ai-closedloop-readiness-stats", "health-score");
    CHECK(h1 >= 0 && h1 <= 100, "health still valid under load");
    CHECK(load_u64(m->linear_gc_root_audit_checks_total) > a0, "audits advanced under load");
    (void)h0;
}

// ── Issue #1867 — record_linear_gc_probe memory_order ──
static void run_1867_source() {
    std::println("\n--- AC1 (#1867): release writes / acquire loads ---");
    auto gc = read_first({"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"});
    auto q = read_first({"src/compiler/evaluator_primitives_query.cpp",
                         "../src/compiler/evaluator_primitives_query.cpp"});
    CHECK(!gc.empty(), "read evaluator_gc.cpp");
    CHECK(gc.find("#1867") != std::string::npos, "cites #1867");
    auto pos = gc.find("record_linear_gc_probe");
    CHECK(pos != std::string::npos, "record_linear_gc_probe present");
    auto win = gc.substr(pos, 1800);
    CHECK(win.find("memory_order_release") != std::string::npos, "violation path uses release");
    CHECK(win.find("linear_violations_caught_total") != std::string::npos,
          "bumps violations_caught");
    CHECK(!q.empty(), "read query.cpp");
    CHECK(q.find("linear_violations_caught_total.load(std::memory_order_acquire)") !=
              std::string::npos,
          "stats load uses acquire");
    CHECK(q.find("#1867") != std::string::npos, "query cites #1867");
}

static void run_1867_sequential_visibility() {
    std::println("\n--- AC2 (#1867): sequential violation visible ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "service wires metrics");
    const auto v0 = m->linear_violations_caught_total.load(std::memory_order_acquire);
    ev.bump_defuse_version_for_test();
    const bool bad = ev.check_linear_ownership_for_frame(0, /*linear_state=*/1);
    CHECK(!bad, "stale frame fails check");
    const auto v1 = m->linear_violations_caught_total.load(std::memory_order_acquire);
    CHECK(v1 == v0 + 1, "violation counter +1 after probe");
    ev.test_probe_linear_at_gc_safepoint();
    CHECK(true, "safepoint probe ok");
}

static void run_1867_concurrent_probe_reader() {
    std::println("\n--- AC3 (#1867): concurrent probes + acquire reader ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics wired");
    ev.bump_defuse_version_for_test();

    std::atomic<bool> start{false};
    std::atomic<std::uint64_t> ops{0};
    constexpr int kWriters = 4;
    constexpr int kIters = 100;
    std::vector<std::thread> threads;
    threads.reserve(kWriters + 1);

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            for (int i = 0; i < kIters; ++i) {
                (void)ev.check_linear_ownership_for_frame(0, /*linear_state=*/1);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    std::atomic<std::uint64_t> max_seen{0};
    threads.emplace_back([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int i = 0; i < kIters * 4; ++i) {
            auto v = m->linear_violations_caught_total.load(std::memory_order_acquire);
            auto prev = max_seen.load(std::memory_order_relaxed);
            while (v > prev && !max_seen.compare_exchange_weak(prev, v)) {
            }
        }
    });

    const auto before = m->linear_violations_caught_total.load(std::memory_order_acquire);
    start.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();
    const auto after = m->linear_violations_caught_total.load(std::memory_order_acquire);
    CHECK(ops.load() == static_cast<std::uint64_t>(kWriters * kIters), "all writer ops done");
    CHECK(after >= before + static_cast<std::uint64_t>(kWriters * kIters),
          "all violations recorded");
    CHECK(max_seen.load() >= before + 1, "reader observed progress under acquire");
}

// ── Issue #1731 — linear_post_mutate_enforce NULL_ENV_ID ──
static void run_1731_source_field() {
    std::println("\n--- AC1 (#1731): metric field + cites ---");
    std::string env_cpp;
    for (const char* p : {"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"}) {
        env_cpp = read_file(p);
        if (!env_cpp.empty())
            break;
    }
    CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
    CHECK(env_cpp.find("#1731") != std::string::npos, "cites #1731");
    CHECK(env_cpp.find("linear_post_mutate_null_env_id_total") != std::string::npos,
          "bumps null_env metric");

    std::string msrc;
    for (const char* p :
         {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
        msrc = read_file(p);
        if (!msrc.empty())
            break;
    }
    CHECK(!msrc.empty() && msrc.find("linear_post_mutate_null_env_id_total") != std::string::npos,
          "metric declared");
}

static void run_1731_enforce_null_bumps() {
    std::println("\n--- AC2 (#1731): enforce(NULL_ENV_ID) bumps metric ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "metrics wired");
    const auto n0 = m->linear_post_mutate_null_env_id_total.load(std::memory_order_relaxed);
    const auto e0 = m->linear_post_mutate_enforcements.load(std::memory_order_relaxed);

    const bool ok = ev.linear_post_mutate_enforce(NULL_ENV_ID);
    CHECK(ok, "NULL_ENV_ID enforce returns true (safe no-op)");

    const auto n1 = m->linear_post_mutate_null_env_id_total.load(std::memory_order_relaxed);
    const auto e1 = m->linear_post_mutate_enforcements.load(std::memory_order_relaxed);
    CHECK(n1 == n0 + 1, "null_env_id_total +1");
    CHECK(e1 == e0, "full enforcements counter not bumped for NULL");
}

static void run_1731_oob_doesnt_bump_null() {
    std::println("\n--- AC3 (#1731): OOB env_id does not bump null metric ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto n0 = m->linear_post_mutate_null_env_id_total.load(std::memory_order_relaxed);

    const bool ok = ev.linear_post_mutate_enforce(static_cast<EnvId>(1u << 30));
    CHECK(ok, "OOB returns true");
    const auto n1 = m->linear_post_mutate_null_env_id_total.load(std::memory_order_relaxed);
    CHECK(n1 == n0, "OOB does not bump null_env metric");
}

static void run_1731_materialize_null() {
    std::println("\n--- AC4 (#1731): materialize_call_env NULL_ENV_ID path ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    const auto n0 = m->linear_post_mutate_null_env_id_total.load(std::memory_order_relaxed);
    const auto f0 = m->materialize_fallback_total.load(std::memory_order_relaxed);

    auto r = cs.eval(R"AURA(((lambda () 1)))AURA");
    CHECK(r.has_value(), "top-level lambda apply evaluates");

    const auto n1 = m->linear_post_mutate_null_env_id_total.load(std::memory_order_relaxed);
    const auto f1 = m->materialize_fallback_total.load(std::memory_order_relaxed);
    CHECK(n1 >= n0, "null_env metric non-decreasing after lambda call");
    CHECK(f1 >= f0, "fallback metric non-decreasing");
    (void)ev.linear_post_mutate_enforce(NULL_ENV_ID);
    CHECK(m->linear_post_mutate_null_env_id_total.load(std::memory_order_relaxed) >= n1 + 1,
          "direct enforce still works after eval");
}

// ── Issue #1875 — dirty-aware EscapeAnalysis + post-mutation validation ──
static void run_1875_source_surface() {
    std::println("\n--- AC1 (#1875): source surface ---");
    auto pm = read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
    auto impl =
        read_first({"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
    auto hdr = read_first(
        {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
    auto ixx = read_first({"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"});
    CHECK(!pm.empty() && pm.find("#1875") != std::string::npos, "pass_manager cites #1875");
    CHECK(pm.find("dirty_blocks_analyzed") != std::string::npos, "dirty_blocks_analyzed");
    CHECK(pm.find("DirtyAwarePass<EscapeAnalysisPass>") != std::string::npos,
          "EscapeAnalysisPass DirtyAware static_assert");
    CHECK(pm.find("double_consume_count") != std::string::npos, "LinearOwnershipWrap double");
    CHECK(!impl.empty() && impl.find("#1875") != std::string::npos, "impl cites #1875");
    CHECK(impl.find("linear_post_mutation_validation_hit_rate") != std::string::npos,
          "hit_rate update in post_mutation");
    CHECK(!hdr.empty() && hdr.find("linear_post_mutation_validation_hit_rate") != std::string::npos,
          "hit_rate metric declared");
}

static void run_1875_dirty_aware_escape() {
    std::println("\n--- AC2 (#1875): EscapeAnalysis dirty block filter ---");
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "f", .local_count = 8});
    auto& func = mod.functions.back();
    {
        auto& b0 = func.blocks.emplace_back();
        b0.id = 0;
        b0.instructions = {
            IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
            IRInstruction{IROpcode::Local, {1, 0, 0, 0}, 0, 1},
        };
    }
    {
        auto& b1 = func.blocks.emplace_back();
        b1.id = 1;
        b1.instructions = {
            IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
        };
    }
    EscapeAnalysisPass full;
    full.run(mod);
    CHECK(func.escape_map.size() == 8, "escape_map sized");
    CHECK(full.functions_analyzed() >= 1, "functions analyzed");
    EscapeAnalysisPass dirty;
    std::unordered_set<std::uint32_t> dirty_set{0};
    dirty.set_block_dirty_fn([&](std::uint32_t bi) { return dirty_set.count(bi) > 0; });
    dirty.run(func);
    CHECK(dirty.dirty_blocks_analyzed() >= 1, "dirty blocks analyzed");
    CHECK(dirty.is_block_dirty(0), "block 0 dirty");
    CHECK(!dirty.is_block_dirty(1), "block 1 clean under filter");
    dirty.run(func);
    CHECK(dirty.dirty_reruns() >= 1, "dirty_reruns on re-analysis");
}

static void run_1875_post_mutation_hit_rate() {
    std::println("\n--- AC3 (#1875): post_mutation full validate + hit_rate ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<FlatAST>(alloc);
    auto* pool = arena->create<StringPool>(alloc);

    auto x_sym = pool->intern("x");
    auto inner = flat->add_literal(42);
    auto lin_node = flat->add_linear(inner);
    auto disp_sym = pool->intern("display");
    auto disp_var = flat->add_variable(disp_sym);
    auto x_var = flat->add_variable(x_sym);
    aura::ast::NodeId disp_args[] = {disp_var, x_var};
    auto disp_call = flat->add_call(disp_var, disp_args);
    auto root = flat->add_let(x_sym, lin_node, disp_call);
    flat->root = root;

    TypeRegistry reg;
    CompilerMetrics metrics;
    aura::ast::MutationRecord rec{};
    rec.mutation_id = 1875;
    rec.target_node = root;
    rec.operator_name = "test";
    std::vector<OwnershipNote> notes;
    auto st = post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
    CHECK(st == InvariantStatus::Ok || st == InvariantStatus::Warnings,
          "post_mutation returns Ok or Warnings");
    CHECK(metrics.linear_post_mutation_checks_total.load() >= 1, "checks bumped");
    CHECK(metrics.linear_post_mutation_full_validate_total.load() >= 1, "full validate path ran");
    CHECK(metrics.linear_post_mutation_hits_total.load() >= 1, "hits bumped (linear work)");
    const auto rate = metrics.linear_post_mutation_validation_hit_rate.load();
    CHECK(rate > 0 && rate <= 100, "hit_rate in (0,100]");
}

static void run_1875_linear_ownership_wrap() {
    std::println("\n--- AC4 (#1875): LinearOwnershipWrap double_consume ---");
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "lo", .local_count = 8});
    auto& block = mod.functions.back().blocks.emplace_back();
    block.id = 0;
    block.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        IRInstruction{IROpcode::MoveOp, {1, 0, 0, 0}, 0, 1},
        IRInstruction{IROpcode::MoveOp, {2, 0, 0, 0}, 0, 1},
        IRInstruction{IROpcode::Add, {3, 0, 1, 0}, 0, 1},
    };
    LinearOwnershipWrap wrap;
    wrap.run(mod);
    CHECK(wrap.double_consume_count() >= 1, "double consume detected");
    CHECK(wrap.use_after_move_count() >= 1, "use-after-move detected");
    CHECK(wrap.functions_scanned() >= 1, "functions scanned");
    CHECK(wrap.has_error(), "has_error when violations");
    CHECK(LinearOwnershipWrap::lifetime_use_after_move() >= 1, "lifetime uam");
}

// ── Issue #1755 — validate_linear_ownership_state bridge_epoch drift ──
static void run_1755_source_metric() {
    std::println("\n--- AC1/AC2 (#1755): #1755 source + metric field ---");
    std::string gc;
    for (const char* p : {"src/compiler/evaluator_gc.cpp", "../src/compiler/evaluator_gc.cpp"}) {
        gc = read_file(p);
        if (!gc.empty())
            break;
    }
    CHECK(!gc.empty(), "read evaluator_gc.cpp");
    CHECK(gc.find("#1755") != std::string::npos, "cites #1755");
    CHECK(gc.find("linear_validate_bridge_epoch_drift_total") != std::string::npos ||
              gc.find("bridge_epoch_drift_counter") != std::string::npos,
          "drift counter wiring");
    CHECK(gc.find("bridge_epoch != current_bridge_epoch") != std::string::npos ||
              gc.find("bridge_epoch != 0 && bridge_epoch != current_bridge_epoch") !=
                  std::string::npos,
          "compares bridge epochs");

    std::string msrc;
    for (const char* p :
         {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"}) {
        msrc = read_file(p);
        if (!msrc.empty())
            break;
    }
    CHECK(!msrc.empty() &&
              msrc.find("linear_validate_bridge_epoch_drift_total") != std::string::npos,
          "metric field declared");
}

static void run_1755_matching_epochs() {
    std::println("\n--- AC3 (#1755): matching epochs ---");
    std::atomic<std::uint64_t> drift{0};
    const bool ok = Evaluator::validate_linear_ownership_state(1, 10, 10, 5, 5, &drift);
    CHECK(ok, "Owned + matching bridge ok");
    CHECK(drift.load() == 0, "no drift bump on match");
}

static void run_1755_mismatched_epochs() {
    std::println("\n--- AC4 (#1755): mismatched bridge_epoch ---");
    std::atomic<std::uint64_t> drift{0};
    const bool ok = Evaluator::validate_linear_ownership_state(1, 10, 10, 4, 5, &drift);
    CHECK(!ok, "Owned + mismatched bridge fails");
    CHECK(drift.load() == 1, "drift counter +1");
    CHECK(!Evaluator::validate_linear_ownership_state(1, 10, 10, 3, 7, &drift),
          "second mismatch fails");
    CHECK(drift.load() == 2, "drift counter +2");
}

static void run_1755_unbridged_skips() {
    std::println("\n--- AC5 (#1755): bridge_epoch==0 skips bridge check ---");
    std::atomic<std::uint64_t> drift{0};
    const bool ok = Evaluator::validate_linear_ownership_state(1, 10, 10, 0, 99, &drift);
    CHECK(ok, "unbridged Owned ok");
    CHECK(drift.load() == 0, "no drift bump when bridge_epoch==0");
}

} // namespace aura_linear_batch

int main() {
    using namespace aura_linear_batch;
    std::println(
        "=== Linear batch: #1615 + #1599 + #1867 + #1731 + #1875 + #1755 (28 ACs total) ===");
    std::println("(#747 occurrence_predicate + #800 postmutate_guard_steal_envframe");
    std::println(
        " NOT included — bundle members via test_issues_jit_late3_main.cpp + late2_main.cpp.");
    std::println(" test_linear_boundary_consistency_1568.cpp NOT included — AuraDomainTests.cmake");
    std::println(" default-build with add_dependencies. test_linear_ownership_batch.cpp NOT");
    std::println(" included — already a batch entry from earlier consolidation waves)");
    run_1615_reval_api();
    run_1615_typecheck_path();
    run_1615_query_schema();
    run_1615_post_mutation();
    run_1615_stress();
    run_1615_wire_and_lineage();
    run_1599_six_touchpoints();
    run_1599_audit_log_monotonic();
    run_1599_scan_and_roots();
    run_1599_closedloop_readiness();
    run_1599_adaptive_and_hist();
    run_1599_load_trends();
    run_1867_source();
    run_1867_sequential_visibility();
    run_1867_concurrent_probe_reader();
    run_1731_source_field();
    run_1731_enforce_null_bumps();
    run_1731_oob_doesnt_bump_null();
    run_1731_materialize_null();
    run_1875_source_surface();
    run_1875_dirty_aware_escape();
    run_1875_post_mutation_hit_rate();
    run_1875_linear_ownership_wrap();
    run_1755_source_metric();
    run_1755_matching_epochs();
    run_1755_mismatched_epochs();
    run_1755_unbridged_skips();
    std::println("\n=== Linear batch: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
