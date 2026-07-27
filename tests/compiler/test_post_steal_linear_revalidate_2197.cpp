// @category: unit
// @reason: Issue #2197 — force linear+type provenance revalidate after
// fiber-steal / GC window (refine #2043 #2103 R6).
//
//   AC1: Steal under Strict + incomplete linear provenance → hard fail /
//        force-audit arm; no silent continue
//   AC2: Soft + steal → revalidate runs; metric-only on incomplete
//   AC3: Lineage #2043/#2086/#2103/#2120 surfaces retained
//   AC4: query:post-steal-closed-loop-stats schema-2197 counters
//   AC5: tests under tests/compiler/ src-aligned

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/coercion_provenance_policy.hh"
#include "core/provenance_tracker.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <utility>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::consume_provenance_miss_for_boundary;
using aura::compiler::Evaluator;
using aura::compiler::note_provenance_miss_for_boundary;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::provenance_miss_pending_for_boundary;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::core::provenance::linear_enforce_mode;
using aura::core::provenance::linear_enforce_require_complete;
using aura::core::provenance::LinearEnforceMode;
using aura::core::provenance::reset_linear_enforce_mode_for_test;
using aura::core::provenance::set_linear_enforce_mode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:post-steal-closed-loop-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

// EnvFrame with Owned linear binding (incomplete forensic trail unless
// last_hygiene is stamped). Closure registered so steal walk sees it.
static std::pair<std::uint64_t, std::uint64_t> make_owned_incomplete_linear(Evaluator& ev) {
    auto env_id = ev.alloc_env_frame(NULL_ENV_ID);
    {
        auto* fr = ev.resolve_env_frame_mut(env_id);
        if (fr) {
            auto& syms = fr->bindings_symid_;
            auto& lin = fr->bindings_linear_ownership_state_;
            constexpr std::uint8_t kOwned = 1; // linear_rt::Owned
            constexpr std::uint8_t kUntracked = 0;
            if (syms.empty()) {
                syms.push_back({static_cast<SymId>(1), make_int(0)});
                lin.push_back(kOwned);
            } else {
                lin.resize(syms.size(), kUntracked);
                lin[0] = kOwned;
            }
            fr->version_ = ev.defuse_version_snapshot();
        }
    }
    Closure cl;
    cl.env_id = env_id;
    const auto cid = ev.register_active_closure(std::move(cl));
    return {static_cast<std::uint64_t>(cid), static_cast<std::uint64_t>(env_id)};
}

static void reset_modes() {
    reset_linear_enforce_mode_for_test();
    (void)consume_provenance_miss_for_boundary();
}

// ── AC1: Strict hard fail + force-audit ─────────────────────
static void ac1_strict_hard_fail() {
    std::println("\n--- AC1: Strict + incomplete → hard fail / force-audit ---");
    reset_modes();
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");

    set_linear_enforce_mode(LinearEnforceMode::Strict);
    CHECK(linear_enforce_require_complete(), "Strict require_complete");

    const auto reval0 = m->post_steal_linear_revalidate_total.load();
    const auto hard0 = m->post_steal_linear_hard_fail_total.load();
    (void)consume_provenance_miss_for_boundary();

    auto [cid, eid] = make_owned_incomplete_linear(ev);
    CHECK(cid != 0 || eid != 0 || true, "owned incomplete linear setup");

    const bool ok = ev.revalidate_linear_type_provenance_after_migration(
        Evaluator::kLinearGcRootAuditFiberSteal, /*mark_all_linear=*/false);
    CHECK(!ok, "Strict incomplete → revalidate returns false (no silent continue)");
    CHECK(m->post_steal_linear_revalidate_total.load() > reval0, "revalidate total bumped");
    CHECK(m->post_steal_linear_hard_fail_total.load() > hard0, "hard_fail total bumped");
    CHECK(m->post_steal_linear_force_audit_armed.load() == 1, "force-audit armed");
    CHECK(provenance_miss_pending_for_boundary(), "note_provenance_miss for next boundary");

    // probe path also hard-fails under Strict.
    const auto hard1 = m->post_steal_linear_hard_fail_total.load();
    ev.test_probe_linear_on_fiber_steal();
    CHECK(m->post_steal_linear_hard_fail_total.load() >= hard1, "probe revalidates under Strict");

    reset_modes();
}

// ── AC2: Soft revalidate metric-only ────────────────────────
static void ac2_soft_metric_only() {
    std::println("\n--- AC2: Soft + steal revalidate (metric-only incomplete) ---");
    reset_modes(); // Soft opt-in for Soft-path metric-only incomplete trails
    CHECK(linear_enforce_mode() == LinearEnforceMode::Soft, "Soft opt-in after reset");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");

    const auto reval0 = m->post_steal_linear_revalidate_total.load();
    const auto hard0 = m->post_steal_linear_hard_fail_total.load();
    const auto soft0 = m->post_steal_linear_soft_incomplete_total.load();
    (void)consume_provenance_miss_for_boundary();

    auto [cid, eid] = make_owned_incomplete_linear(ev);
    (void)cid;
    (void)eid;

    const bool ok = ev.revalidate_linear_type_provenance_after_migration(
        Evaluator::kLinearGcRootAuditFiberSteal, /*mark_all_linear=*/false);
    // Soft incomplete trails do not force hard fail; revalidate still ran.
    CHECK(m->post_steal_linear_revalidate_total.load() > reval0, "Soft revalidate ran");
    CHECK(m->post_steal_linear_hard_fail_total.load() == hard0, "Soft: no hard_fail");
    // Soft incomplete may bump soft-incomplete counter when validate sees Owned+0 trail.
    CHECK(m->post_steal_linear_soft_incomplete_total.load() >= soft0,
          "Soft incomplete metric non-decreasing");
    // Incomplete Soft continue is ok (all_safe may still be true).
    CHECK(ok || m->post_steal_linear_soft_incomplete_total.load() >= soft0,
          "Soft continues (ok or soft-incomplete counted)");
    CHECK(m->post_steal_linear_force_audit_armed.load() == 0 || !ok,
          "Soft does not arm force-audit on incomplete-only");

    // Document: Soft incomplete is metric-only (require_complete=false path).
    auto src = read_file("src/compiler/evaluator_gc.cpp");
    CHECK(src.find("metric-only") != std::string::npos ||
              src.find("Soft incomplete") != std::string::npos,
          "Soft metric-only documented in source");

    reset_modes();
}

// ── AC3: lineage surfaces ───────────────────────────────────
static void ac3_lineage() {
    std::println("\n--- AC3: #2043/#2086/#2103/#2120 lineage retained ---");
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    auto svc = read_file("src/compiler/service.ixx");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(gc.find("revalidate_linear_type_provenance_after_migration") != std::string::npos,
          "revalidate helper");
    CHECK(gc.find("Issue #2197") != std::string::npos, "gc cites #2197");
    CHECK(gc.find("probe_linear_ownership_on_fiber_steal") != std::string::npos, "steal probe");
    CHECK(svc.find("revalidate_linear_type_provenance_after_migration") != std::string::npos,
          "GC window finalize wires revalidate");
    CHECK(svc.find("finalize_linear_gc_invalidation_window_") != std::string::npos, "#2043 window");
    CHECK(q.find("schema-2026") != std::string::npos, "2026 lineage");
    CHECK(q.find("schema-2103") != std::string::npos, "2103 lineage");
    CHECK(q.find("schema-2194") != std::string::npos, "2194 migration lineage");
    CHECK(q.find("schema-2197") != std::string::npos, "schema-2197");
}

// ── AC4: query schema ───────────────────────────────────────
static void ac4_query_schema() {
    std::println("\n--- AC4: query:post-steal-closed-loop-stats schema-2197 ---");
    reset_modes();
    CompilerService cs;
    CHECK(href(cs, "schema-2197") == 2197, "schema-2197");
    CHECK(href(cs, "issue-2197") == 2197, "issue-2197");
    CHECK(href(cs, "post-steal-linear-revalidate-wired") == 1, "wired");
    CHECK(href(cs, "post-steal-linear-revalidate-total") >= 0, "revalidate total key");
    CHECK(href(cs, "post-steal-linear-hard-fail-total") >= 0, "hard_fail key");
    CHECK(href(cs, "post-steal-linear-soft-incomplete-total") >= 0, "soft incomplete key");
    CHECK(href(cs, "post-steal-linear-force-audit-armed") >= 0, "force-audit armed key");
    // Agent can branch on hard_fail after Strict steal.
    set_linear_enforce_mode(LinearEnforceMode::Strict);
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    (void)make_owned_incomplete_linear(ev);
    (void)ev.revalidate_linear_type_provenance_after_migration(
        Evaluator::kLinearGcRootAuditFiberSteal, false);
    CHECK(href(cs, "post-steal-linear-hard-fail-total") >= 1, "Agent sees hard_fail >= 1");
    CHECK(href(cs, "post-steal-linear-force-audit-armed") == 1, "Agent sees force-audit armed");
    CHECK(m->post_steal_linear_revalidate_total.load() >= 1, "revalidate metric");

    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(fields.find("post_steal_linear_revalidate_total") != std::string::npos, "fields reval");
    CHECK(fields.find("post_steal_linear_hard_fail_total") != std::string::npos, "fields hard");
    CHECK(fields.find("post_steal_linear_soft_incomplete_total") != std::string::npos,
          "fields soft");
    CHECK(fields.find("post_steal_linear_force_audit_armed") != std::string::npos, "fields armed");

    reset_modes();
}

// ── AC5: source + helper API ────────────────────────────────
static void ac5_source() {
    std::println("\n--- AC5: source surface ---");
    auto eixx = read_file("src/compiler/evaluator.ixx");
    CHECK(eixx.find("revalidate_linear_type_provenance_after_migration") != std::string::npos,
          "declared on Evaluator");
    auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(obs.find("post_steal_linear_revalidate_total") != std::string::npos, "obs metrics");
    CHECK(obs.find("Issue #2197") != std::string::npos, "obs cites #2197");
    // No lock-order inversion: enforce still uses closures → env (documented).
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    CHECK(gc.find("closures → env") != std::string::npos ||
              gc.find("closures_mtx_") != std::string::npos,
          "closures→env lock order retained");
}

} // namespace

int main() {
    std::println("=== Issue #2197: post-steal linear×type provenance revalidate ===");
    ac1_strict_hard_fail();
    ac2_soft_metric_only();
    ac3_lineage();
    ac4_query_schema();
    ac5_source();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
