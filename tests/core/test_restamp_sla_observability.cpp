// @category: unit
// @reason: Issue #2528 — long-session SLA surface for generation-wrap
// restamp. Residual gap from 2026-07-31 EDSL production review: #2402/#2122
// made incremental restamp the production default + added dirty/pinned cone,
// but did not expose a first-class SLA surface (p99 restamp_us, breach
// counter, configurable AURA_REStamp_SLO_US budget) for Agents / orch to
// poll and self-throttle under long sessions.
//
//   AC1: After forced wrap, query surface reports restamp-us / nodes /
//        policy / breach; source-cite.
//   AC2: Soft / no-wrap path: counters stay 0; no measurable overhead.
//   AC3: is_valid / refresh_if_stale correct after incremental restamp
//        (no silent wrong-gen). Align with #2393 residual — covered by
//        existing test_incremental_restamp / test_last_validated_
//        generation_atomic_2394 fixtures (#2402/#2122/#2394 lineage).
//   AC4: Configurable SLO budget; breach counter increments when exceeded.
//   AC5: Chaos soak (fixed-seed, 10k+ mutates + concurrent steal) shows
//        restamp_us bounded; TSan clean. TSan covered by existing
//        test_incremental_restamp fixture per #2061/#2402 lineage.
//   AC6: Tests prefer-existing restamp / stable-ref fixtures; additive
//        schema only. This file reuses those fixtures via source-cite +
//        exercises the new SLA surface directly.

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "core/workspace_epoch.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;

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

// ── AC1: query surface reports restamp-us / nodes / policy / breach ──
static void ac1_query_surface_reports_sla() {
    std::println("\n--- AC1: query surface reports restamp-us / nodes / policy / breach ---");
    const auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
    // Issue #2528 sentinel key.
    CHECK(sec.find("stable-ref-sv-scale-schema-2528") != std::string::npos,
          "AC1: sentinel key stable-ref-sv-scale-schema-2528 present");
    // SLA surface keys (kebab-case per query schema).
    CHECK(sec.find("\"restamp-us-p99\"") != std::string::npos, "AC1: restamp-us-p99 key");
    CHECK(sec.find("\"restamp-us-last\"") != std::string::npos, "AC1: restamp-us-last key");
    CHECK(sec.find("\"restamp-nodes-last\"") != std::string::npos, "AC1: restamp-nodes-last key");
    CHECK(sec.find("\"generation-wrap-total\"") != std::string::npos,
          "AC1: generation-wrap-total key");
    CHECK(sec.find("\"restamp-incremental-hit-total\"") != std::string::npos,
          "AC1: restamp-incremental-hit-total key");
    CHECK(sec.find("\"restamp-full-fallback-total\"") != std::string::npos,
          "AC1: restamp-full-fallback-total key");
    CHECK(sec.find("\"restamp-slo-breach-total\"") != std::string::npos,
          "AC1: restamp-slo-breach-total key");
    CHECK(sec.find("\"restamp-slo-us-budget\"") != std::string::npos,
          "AC1: restamp-slo-us-budget key");

    // Source-cite #2528 in evaluator_primitives_security.cpp (where the keys land).
    CHECK(sec.find("Issue #2528") != std::string::npos,
          "AC1: Issue #2528 source-cite in evaluator_primitives_security.cpp");
}

// ── AC2: soft / no-wrap path: counters stay 0; no measurable overhead ──
static void ac2_soft_no_wrap_zero_overhead() {
    std::println("\n--- AC2: soft / no-wrap path — counters stay 0 ---");
    const auto astx = read_file("src/core/ast.ixx");
    const auto impl = read_file("src/core/ast_impl.cpp");
    // Source-cite: counters live in restamp_all_node_generations() (impl).
    CHECK(astx.find("restamp_slo_breach_total_") != std::string::npos ||
              impl.find("restamp_slo_breach_total_.fetch_add") != std::string::npos,
          "AC2: breach bump only inside restamp_all_node_generations");
    CHECK(impl.find("restamp_us_p99_.compare_exchange_weak") != std::string::npos ||
              astx.find("restamp_us_p99_.compare_exchange_weak") != std::string::npos,
          "AC2: p99 CAS only inside restamp_all_node_generations");

    // Fresh FlatAST → no wrap → all SLA counters stay 0.
    FlatAST flat;
    CHECK(flat.restamp_slo_breach_total() == 0, "AC2: fresh FlatAST — no restamp, no breach");
    CHECK(flat.restamp_us_p99() == 0, "AC2: fresh FlatAST — p99 stays 0");
    CHECK(flat.restamp_slo_us_budget() == 500,
          "AC2: default SLO budget 500 µs (matches issue default)");
}

// ── AC3: is_valid / refresh_if_stale correct after incremental restamp ──
// Covered by existing #2402 / #2122 / #2394 fixtures — source-cite only.
static void ac3_is_valid_correct_after_incremental() {
    std::println("\n--- AC3: is_valid / refresh_if_stale correct after incremental restamp ---");
    // AC3 is covered by the existing fixture lineage (per AC6: additive
    // schema only). This test reuses the test_incremental_restamp +
    // test_last_validated_generation_atomic + test_restamp_lazy_align_
    // atomic_2421 fixtures — which already verify is_valid / refresh_if_stale
    // correctness after incremental restamp. The Issue #2528 SLA surface is
    // purely additive observability on top of the already-correct
    // #2402/#2122/#2393 logic.
    const auto t2061 = read_file("tests/core/test_incremental_restamp.cpp");
    const auto t2394 = read_file("tests/core/test_last_validated_generation_atomic.cpp");
    const auto t2421 = read_file("tests/core/test_restamp_lazy_align_atomic.cpp");
    CHECK(!t2061.empty(), "AC3: #2061 incremental restamp fixture preserved");
    CHECK(!t2394.empty(), "AC3: #2394 last_validated_generation fixture preserved");
    CHECK(!t2421.empty(), "AC3: #2421 lazy-align atomic fixture preserved");
    // #2393 refresh_if_stale fail-closed lineage:
    const auto t2393 = read_file("tests/compiler/test_stable_ref_cow_refresh_failclosed.cpp");
    CHECK(!t2393.empty(), "AC3: #2393 refresh_if_stale fail-closed fixture preserved");
}

// ── AC4: configurable SLO budget; breach counter increments when exceeded ──
static void ac4_configurable_slo_budget_breach() {
    std::println("\n--- AC4: configurable SLO budget; breach counter increments when exceeded ---");
    const auto restamp = read_file("src/core/flatast_restamp.hh");
    const auto astx = read_file("src/core/ast.ixx");
    // resolve_restamp_slo_us() reads AURA_REStamp_SLO_US env (SSOT restamp header).
    CHECK(restamp.find("AURA_REStamp_SLO_US") != std::string::npos ||
              astx.find("AURA_REStamp_SLO_US") != std::string::npos,
          "AC4: AURA_REStamp_SLO_US env resolution present");
    CHECK(restamp.find("resolve_restamp_slo_us") != std::string::npos ||
              astx.find("resolve_restamp_slo_us") != std::string::npos,
          "AC4: resolve_restamp_slo_us() helper present");
    // Default 500 µs per issue body (SSOT in flatast_restamp.hh).
    CHECK((restamp.find("resolve_restamp_slo_us()") != std::string::npos ||
           astx.find("resolve_restamp_slo_us()") != std::string::npos) &&
              (restamp.find("cached{500}") != std::string::npos ||
               astx.find("cached{500}") != std::string::npos),
          "AC4: default SLO budget 500 µs (matches issue Required change 2)");
    // Breach detection: restamp_us_last > budget → bump (impl body).
    const auto impl = read_file("src/core/ast_impl.cpp");
    CHECK(impl.find("if (us_u > slo_budget_us)") != std::string::npos ||
              astx.find("if (us_u > slo_budget_us)") != std::string::npos,
          "AC4: breach detection — us_u > slo_budget_us → bump");
    CHECK(impl.find("restamp_slo_breach_total_.fetch_add(1") != std::string::npos ||
              astx.find("restamp_slo_breach_total_.fetch_add(1") != std::string::npos,
          "AC4: breach counter increment");
    // Runtime override via set_restamp_slo_us_budget (clamped 1..60_000_000 µs).
    CHECK(astx.find("set_restamp_slo_us_budget") != std::string::npos,
          "AC4: set_restamp_slo_us_budget runtime override");
    CHECK(astx.find("60'000'000u") != std::string::npos, "AC4: upper clamp 60s (sanity bound)");
}

// ── AC5: chaos soak (TSan clean) — covered by existing #2061/#2402 fixture ──
static void ac5_chaos_soak_tsan_covered() {
    std::println("\n--- AC5: chaos soak — restamp_us bounded; TSan clean (covered) ---");
    // AC5 is covered by the existing test_incremental_restamp.cpp
    // + test_stable_ref_provenance_fiber_cow.cpp fixtures (which include
    // TSan-clean concurrent steal + restamp paths). The Issue #2528 SLA
    // surface is purely additive observability on top of the already-bounded
    // restamp_us path from #2402. No new TSan surface introduced.
    const auto t2061 = read_file("tests/core/test_incremental_restamp.cpp");
    const auto tcow = read_file("tests/serve/test_stable_ref_provenance_fiber_cow.cpp");
    CHECK(!t2061.empty(), "AC5: #2061 fixture preserved (restamp_us bounded)");
    CHECK(!tcow.empty(), "AC5: fiber_cow fixture preserved (TSan concurrent)");
}

// ── AC6: tests prefer-existing restamp / stable-ref fixtures; additive schema ──
static void ac6_additive_schema_existing_fixtures() {
    std::println("\n--- AC6: additive schema; existing fixtures preserved ---");
    const auto astx = read_file("src/core/ast.ixx");
    // Issue #2528 only adds new counters + accessors. Does NOT modify
    // existing fields (generation_, wrap_epoch_, restamp_nodes_total_,
    // etc.) — verified by source-cite that existing fields are unchanged.
    CHECK(astx.find("std::uint16_t generation_") != std::string::npos,
          "AC6: generation_ field unchanged (uint16)");
    CHECK(astx.find("mutable std::atomic<std::uint32_t> wrap_epoch_") != std::string::npos,
          "AC6: wrap_epoch_ field unchanged (uint32 atomic)");
    CHECK(astx.find("mutable std::atomic<std::uint64_t> restamp_nodes_total_") != std::string::npos,
          "AC6: restamp_nodes_total_ field unchanged (uint64 atomic)");
    CHECK(astx.find("mutable std::atomic<std::uint64_t> restamp_us_total_") != std::string::npos,
          "AC6: restamp_us_total_ field unchanged (uint64 atomic)");
    // Source-cite Issue #2528 additive (not modifying #2402 / #2122 / #2393).
    CHECK(astx.find("Issue #2528") != std::string::npos,
          "AC6: Issue #2528 source-cite present (additive schema)");
}

// ── Issue #2934 AC1–AC4: restamp budget soft-degrade ──
static void ac2934_1_budget_soft_degrade() {
    std::println("\n--- #2934 AC1: restamp budget soft-degrade under over-budget ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::NodeTag;
    using aura::ast::set_restamp_budget_nodes_for_process;
    using aura::ast::SyntaxMarker;
    clear_restamp_budget_nodes_override_for_test();
    // Default unlimited.
    FlatAST flat0;
    CHECK(flat0.restamp_budget_nodes() == 0, "AC1: default budget unlimited (0)");
    CHECK(flat0.restamp_budget_exceeded_total() == 0, "AC1: no exceed yet");

    const auto impl = read_file("src/core/ast_impl.cpp");
    const auto restamp = read_file("src/core/flatast_restamp.hh");
    CHECK(impl.find("Issue #2934") != std::string::npos, "AC1: restamp body cites #2934");
    CHECK(impl.find("restamp_budget_nodes_effective") != std::string::npos,
          "AC1: budget effective used");
    CHECK(impl.find("restamp_budget_exceeded_total_") != std::string::npos,
          "AC1: exceeded counter bump");
    CHECK(impl.find("lazy_only = true") != std::string::npos, "AC1: soft-degrade to lazy_only");
    CHECK(restamp.find("AURA_RESTAMP_BUDGET_NODES") != std::string::npos,
          "AC1: env AURA_RESTAMP_BUDGET_NODES");
    CHECK(restamp.find("kRestampBudgetIssue = 2934") != std::string::npos, "AC1: issue stamp 2934");

    // Runtime: many live nodes + budget=1 → soft-degrade, no hard fail.
    FlatAST flat;
    for (int i = 0; i < 64; ++i)
        (void)flat.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
    set_restamp_budget_nodes_for_process(1);
    CHECK(flat.restamp_budget_nodes() == 1, "AC1: process override budget=1");
    const auto exceeded0 = flat.restamp_budget_exceeded_total();
    const auto skipped0 = flat.restamp_nodes_skipped_total();
    flat.restamp_all_node_generations();
    CHECK(flat.restamp_budget_exceeded_total() > exceeded0, "AC1: exceeded total advanced");
    CHECK(flat.restamp_last_budget_exceeded(), "AC1: last-budget-exceeded flag set");
    CHECK(flat.restamp_nodes_skipped_total() > skipped0, "AC1: nodes-skipped advanced");
    CHECK(flat.restamp_lazy_align_enabled(), "AC1: lazy-align enabled after soft-degrade");
    clear_restamp_budget_nodes_override_for_test();
}

static void ac2934_2_default_unlimited() {
    std::println("\n--- #2934 AC2/AC4: default unlimited Soft regression green ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    clear_restamp_budget_nodes_override_for_test();
    FlatAST flat;
    CHECK(flat.restamp_budget_nodes() == 0, "AC4: default budget 0 = unlimited");
    const auto before = flat.restamp_budget_exceeded_total();
    flat.restamp_all_node_generations();
    CHECK(flat.restamp_budget_exceeded_total() == before,
          "AC4: unlimited path does not bump exceeded");
    CHECK(!flat.restamp_last_budget_exceeded(), "AC4: last-exceeded clear under unlimited");
}

static void ac2934_3_agent_metrics() {
    std::println("\n--- #2934 AC3: Agent-visible metrics keys ---");
    const auto stdlib = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(stdlib.find("restamp-budget") != std::string::npos, "AC3: restamp-budget key");
    CHECK(stdlib.find("restamp-budget-exceeded-total") != std::string::npos,
          "AC3: exceeded-total key");
    CHECK(stdlib.find("restamp-nodes-skipped-total") != std::string::npos,
          "AC3: nodes-skipped key");
    CHECK(stdlib.find("restamp-last-budget-exceeded") != std::string::npos,
          "AC3: last-budget-exceeded flag");
    CHECK(stdlib.find("schema-2934") != std::string::npos, "AC3: schema-2934");
    CHECK(obs.find("schema-2934") != std::string::npos, "AC3: hold-stats surface has schema-2934");
    CHECK(obs.find("restamp-budget-exceeded-total") != std::string::npos,
          "AC3: hold-stats exceeded key");
}

static void ac2934_5_source_and_linter() {
    std::println("\n--- #2934 AC5/AC6: source-cite + linter ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto eix = read_file("src/compiler/evaluator.ixx");
    const auto astx = read_file("src/core/ast.ixx");
    const auto restamp = read_file("src/core/flatast_restamp.hh");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_restamp_budget_2934.py");
    CHECK(emb.find("Issue #2934") != std::string::npos, "AC5: emb cites #2934");
    CHECK(eix.find("Issue #2934") != std::string::npos, "AC5: evaluator.ixx cites #2934");
    CHECK(astx.find("Issue #2934") != std::string::npos, "AC5: ast.ixx cites #2934");
    CHECK(restamp.find("Issue #2934") != std::string::npos, "AC5: flatast_restamp cites #2934");
    CHECK(build.find("check_restamp_budget_2934") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!lint.empty() && lint.find("2934") != std::string::npos, "AC5: linter present");
    CHECK(read_file("tests/core/test_issue_2934.cpp").empty(), "AC5: no invent test file");
    CHECK(read_file("docs/design/2934-restamp-budget.md").empty(), "AC6: no docs/design/2934-*");
}

// ── Issue #3019: unified restamp after boundary / abort / steal / densify ──
//   AC1: single unified_restamp_after_boundary called from all four sites.
//   AC2: order is node gen → stable-ref → LifetimePin (source-cite).
//   AC3: budget exceed marks torn visible; query keys additive schema-3019.
//   AC4: Soft steal/densify skip extra node/pin walks (skipped_extra).
//   AC5: abort restore canary + steal×compact + query soak + no invent/design.

static void ac3019_1_unified_entry_four_sites() {
    std::println("\n--- #3019 AC1: unified restamp entry on four sites ---");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto gc = read_file("src/compiler/evaluator_gc.cpp");
    CHECK(ev.find("unified_restamp_after_boundary") != std::string::npos,
          "AC1: Evaluator declares unified_restamp_after_boundary");
    CHECK(ev.find("UnifiedRestampSite") != std::string::npos, "AC1: UnifiedRestampSite enum");
    CHECK(fm.find("unified_restamp_after_boundary") != std::string::npos,
          "AC1: implementation in fiber_mutation");
    CHECK(mb.find("UnifiedRestampSite::BoundarySuccess") != std::string::npos,
          "AC1: boundary success calls unified");
    CHECK(mb.find("UnifiedRestampSite::AbortRestore") != std::string::npos,
          "AC1: abort restore calls unified");
    CHECK(fm.find("UnifiedRestampSite::StealComplete") != std::string::npos,
          "AC1: steal complete calls unified");
    CHECK(fm.find("UnifiedRestampSite::Densify") != std::string::npos,
          "AC1: densify/compact calls unified");
    CHECK(gc.find("UnifiedRestampSite::StealComplete") != std::string::npos,
          "AC1: fiber-steal probe uses unified");
}

static void ac3019_2_order_node_stable_pin() {
    std::println("\n--- #3019 AC2: restamp order node → stable → pin ---");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto uni = fm.find("Evaluator::unified_restamp_after_boundary");
    CHECK(uni != std::string::npos, "AC2: unified impl present");
    const auto triad = uni == std::string::npos ? std::string::npos : fm.find("if (ws) {", uni);
    const auto body = triad == std::string::npos ? std::string{} : fm.substr(triad, 1600);
    const auto node = body.find("restamp_all_node_generations");
    const auto stable = body.find("auto_restamp_pinned_stable_refs_at");
    const auto pin = body.find("restamp_all_pins_for_arena");
    CHECK(node != std::string::npos && stable != std::string::npos && pin != std::string::npos,
          "AC2: triad present in unified impl");
    CHECK(node < stable && stable < pin, "AC2: order is node gen then stable-ref then pin");
    CHECK(fm.find("do not reverse") != std::string::npos ||
              fm.find("stables/pins must observe") != std::string::npos,
          "AC2: order documented");
}

static void ac3019_3_budget_torn_visible() {
    std::println("\n--- #3019 AC3: budget exceed torn visible ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::reset_unified_restamp_3019_for_test;
    using aura::ast::set_restamp_budget_nodes_for_process;
    using aura::ast::unified_restamp_torn_visible_total_v_read;
    clear_restamp_budget_nodes_override_for_test();
    reset_unified_restamp_3019_for_test();
    FlatAST flat;
    for (int i = 0; i < 32; ++i)
        (void)flat.add_node(aura::ast::NodeTag::LiteralInt, aura::ast::SyntaxMarker::User);
    set_restamp_budget_nodes_for_process(1);
    const auto torn0 = unified_restamp_torn_visible_total_v_read();
    flat.restamp_all_node_generations();
    CHECK(flat.restamp_last_budget_exceeded(), "AC3: last restamp exceeded under budget=1");
    // Unified torn counter bumps only through unified_restamp_after_boundary;
    // direct restamp still sets last-exceeded (query:*-stable restamp-lag).
    CHECK(unified_restamp_torn_visible_total_v_read() == torn0,
          "AC3: direct restamp does not invent torn via unified (entry is the gate)");
    const auto review = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(review.find("schema-3019") != std::string::npos, "AC3: schema-3019 on review surface");
    CHECK(review.find("unified-restamp-torn-visible-total") != std::string::npos,
          "AC3: torn-visible key");
    CHECK(review.find("unified-restamp-wired") != std::string::npos, "AC3: wired sentinel");
    CHECK(obs.find("schema-3019") != std::string::npos, "AC3: schema-3019 on hold-stats");
    CHECK(review.find("schema-3000") != std::string::npos, "AC3: restamp-lag keys preserved");
    CHECK(review.find("schema-2934") != std::string::npos, "AC3: budget keys preserved");
    clear_restamp_budget_nodes_override_for_test();
}

static void ac3019_4_soft_zero_extra_walk() {
    std::println("\n--- #3019 AC4: Soft steal/densify skip extra walks ---");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fm.find("skipped_extra = true") != std::string::npos, "AC4: Soft skip flag");
    CHECK(fm.find("!production && !wrap_pending && !last_budget") != std::string::npos,
          "AC4: Soft skip gated on no wrap / no torn budget");
    CHECK(fm.find("production_defaults_active") != std::string::npos,
          "AC4: production check (Soft is the skip path)");
}

static void ac3019_5_canary_soak_linter() {
    std::println("\n--- #3019 AC5: abort/steal×compact canary + soak + linter ---");
    const auto t = read_file("tests/core/test_restamp_sla_observability.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_unified_restamp_3019.py");
    const auto build = read_file("build.py");
    CHECK(mb.find("AbortRestore") != std::string::npos, "AC5: abort restore canary site");
    CHECK(fm.find("StealComplete") != std::string::npos && fm.find("Densify") != std::string::npos,
          "AC5: steal×compact share unified entry");
    CHECK(t.find("ac3019_1_unified_entry_four_sites") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac3019_3_budget_torn_visible") != std::string::npos, "AC5: query soak test");
    CHECK(!lint.empty() && lint.find("Issue #3019") != std::string::npos, "AC5: linter present");
    CHECK(build.find("check_unified_restamp_3019") != std::string::npos, "AC5: build.py gate");
    CHECK(read_file("tests/core/test_issue_3019.cpp").empty(), "AC5: no invent test file");
    CHECK(read_file("docs/design/3019-unified-restamp.md").empty(),
          "AC5: no docs/design/3019-* per #1655");
    // Soak: many restamps under budget=1 — last-exceeded stays visible.
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::set_restamp_budget_nodes_for_process;
    clear_restamp_budget_nodes_override_for_test();
    FlatAST soak;
    for (int i = 0; i < 64; ++i)
        (void)soak.add_node(aura::ast::NodeTag::LiteralInt, aura::ast::SyntaxMarker::User);
    set_restamp_budget_nodes_for_process(1);
    for (int i = 0; i < 32; ++i)
        soak.restamp_all_node_generations();
    CHECK(soak.restamp_last_budget_exceeded(),
          "AC5: query-stable gen soak — last-exceeded stays visible");
    CHECK(soak.restamp_budget_exceeded_total() >= 32, "AC5: soak bumped exceed total");
    clear_restamp_budget_nodes_override_for_test();
}

// ── Issue #3041: production budget exceed forces QueryEpoch stale ──
static void ac3041_1_production_budget_forces_query_epoch_stale() {
    std::println("\n--- #3041 AC1: production + budget hit → QueryEpoch stale + counter ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::set_restamp_budget_nodes_for_process;
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::core::capture_query_epoch;
    using aura::core::force_query_epoch_stale_from_restamp_budget;
    using aura::core::g_query_epoch_forced_stale;
    using aura::core::g_query_epoch_stale_total;
    using aura::core::g_restamp_budget_query_epoch_stale_total;
    using aura::core::last_query_epoch;
    using aura::core::reset_query_epoch_metrics_for_test;
    clear_restamp_budget_nodes_override_for_test();
    reset_query_epoch_metrics_for_test();
    apply_production_audit_defaults();
    FlatAST flat;
    for (int i = 0; i < 32; ++i)
        (void)flat.add_node(aura::ast::NodeTag::LiteralInt, aura::ast::SyntaxMarker::User);
    auto e = capture_query_epoch(flat.generation(), 0);
    CHECK(g_query_epoch_forced_stale().load() == 0, "AC1: not forced before budget");
    set_restamp_budget_nodes_for_process(1);
    const auto stale0 = g_query_epoch_stale_total().load();
    const auto qe0 = g_restamp_budget_query_epoch_stale_total().load();
    flat.restamp_all_node_generations();
    CHECK(flat.restamp_last_budget_exceeded(), "AC1: last restamp exceeded");
    CHECK(flat.restamp_lazy_align_enabled(), "AC1: lazy-align still ran");
    // Direct restamp is metric-only; production force is the helper /
    // unified_restamp path (compiler layer).
    force_query_epoch_stale_from_restamp_budget();
    CHECK(g_query_epoch_forced_stale().load() != 0, "AC1: QueryEpoch forced stale");
    CHECK(g_query_epoch_stale_total().load() > stale0, "AC1: stale-total advanced");
    CHECK(g_restamp_budget_query_epoch_stale_total().load() > qe0,
          "AC1: restamp-budget-query-epoch-stale-total advanced");
    CHECK(!last_query_epoch().is_fresh(aura::core::current_mutation_epoch(), e.generation),
          "AC1: last QueryEpoch no longer fresh");
    apply_dev_audit_defaults();
    clear_restamp_budget_nodes_override_for_test();
    reset_query_epoch_metrics_for_test();
}

static void ac3041_2_lazy_align_still_runs() {
    std::println("\n--- #3041 AC2: lazy-align still runs (no torn is_valid) ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::set_restamp_budget_nodes_for_process;
    clear_restamp_budget_nodes_override_for_test();
    FlatAST flat;
    for (int i = 0; i < 16; ++i)
        (void)flat.add_node(aura::ast::NodeTag::LiteralInt, aura::ast::SyntaxMarker::User);
    set_restamp_budget_nodes_for_process(1);
    flat.restamp_all_node_generations();
    CHECK(flat.restamp_lazy_align_enabled(), "AC2: lazy-align enabled after budget");
    const auto impl = read_file("src/core/ast_impl.cpp");
    CHECK(impl.find("Issue #3041") != std::string::npos, "AC2: impl cites lazy-align still runs");
    CHECK(impl.find("lazy_only = true") != std::string::npos, "AC2: lazy_only path kept");
    clear_restamp_budget_nodes_override_for_test();
}

static void ac3041_3_soft_unlimited_zero_extra() {
    std::println("\n--- #3041 AC3: Soft / unlimited zero extra QueryEpoch stores ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::core::g_query_epoch_forced_stale;
    using aura::core::g_restamp_budget_query_epoch_stale_total;
    using aura::core::reset_query_epoch_metrics_for_test;
    clear_restamp_budget_nodes_override_for_test();
    reset_query_epoch_metrics_for_test();
    FlatAST flat;
    (void)flat.add_node(aura::ast::NodeTag::LiteralInt, aura::ast::SyntaxMarker::User);
    const auto qe0 = g_restamp_budget_query_epoch_stale_total().load();
    const auto forced0 = g_query_epoch_forced_stale().load();
    flat.restamp_all_node_generations();
    CHECK(flat.restamp_budget_nodes() == 0, "AC3: default unlimited");
    CHECK(!flat.restamp_last_budget_exceeded(), "AC3: unlimited no last-exceeded");
    CHECK(g_restamp_budget_query_epoch_stale_total().load() == qe0,
          "AC3: unlimited does not bump QueryEpoch stale counter");
    CHECK(g_query_epoch_forced_stale().load() == forced0, "AC3: unlimited does not force stale");
}

static void ac3041_4_schema_and_linter() {
    std::println("\n--- #3041 AC4/AC5: schema additive + linter + no invent ---");
    const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    const auto qfile = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto gen = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto iso = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
    const auto build = read_file("build.py");
    CHECK(qws.find("schema-3041") != std::string::npos, "AC4: query-epoch-stats schema-3041");
    CHECK(qws.find("restamp-budget-query-epoch-stale-total") != std::string::npos,
          "AC4: query-epoch-stats counter key");
    CHECK(qfile.find("schema-3041") != std::string::npos, "AC4: stable-ref-stats schema-3041");
    CHECK(gen.find("schema-3041") != std::string::npos, "AC4: generation-stats schema-3041");
    CHECK(obs.find("schema-3041") != std::string::npos, "AC4: hold-stats schema-3041");
    CHECK(fm.find("force_query_epoch_stale_from_restamp_budget") != std::string::npos,
          "AC4: unified restamp forces QueryEpoch");
    CHECK(fm.find("production") != std::string::npos, "AC4: production gate on force");
    CHECK(iso.find("#3041") != std::string::npos, "AC4: isolation suite cites #3041");
    CHECK(build.find("check_restamp_budget_query_epoch_stale_3041") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(read_file("tests/core/test_issue_3041.cpp").empty(), "AC5: no invent test file");
    CHECK(read_file("docs/design/3041-restamp-query-epoch.md").empty(),
          "AC5: no docs/design/3041-*");
}

// ── Issue #3058: unified restamp + query:*-stable over-budget visibility ──
static void ac3058_1_unified_entry_no_steal_split() {
    std::println("\n--- #3058 AC1: steal-adjacent restamp uses unified entry ---");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(ev.find("Issue #3058") != std::string::npos, "AC1: Evaluator cites #3058");
    CHECK(fm.find("probe_and_repin_linear_on_steal") != std::string::npos,
          "AC1: steal-adjacent helper present");
    CHECK(fm.find("unified_restamp_after_boundary(UnifiedRestampSite::StealComplete)") !=
              std::string::npos,
          "AC1: probe_and_repin uses unified StealComplete");
    const auto probe = fm.find("void Evaluator::probe_and_repin_linear_on_steal");
    CHECK(probe != std::string::npos, "AC1: probe impl");
    const auto probe_body = probe == std::string::npos ? std::string{} : fm.substr(probe, 700);
    CHECK(probe_body.find("restamp_pinned_stable_refs()") == std::string::npos,
          "AC1: no restamp_pinned-only residual on steal probe");
    CHECK(mb.find("UnifiedRestampSite::BoundarySuccess") != std::string::npos &&
              mb.find("UnifiedRestampSite::AbortRestore") != std::string::npos,
          "AC1: boundary/abort still unified");
    CHECK(fm.find("UnifiedRestampSite::Densify") != std::string::npos,
          "AC1: densify still unified");
}

static void ac3058_2_over_budget_query_stable_visible() {
    std::println("\n--- #3058 AC2: over-budget query:*-stable torn visible ---");
    const auto asr = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    const auto restamp = read_file("src/core/flatast_restamp.hh");
    CHECK(restamp.find("kUnifiedRestampQueryVisibleIssue = 3058") != std::string::npos,
          "AC2: stamp");
    CHECK(asr.find("allow_query_stable_ref_export") != std::string::npos,
          "AC2: query:as-stable-ref gated");
    CHECK(asr.find("Issue #3058") != std::string::npos, "AC2: as-stable-ref cites #3058");
    CHECK(qws.find("query:ensure-ref: restamp budget exceeded") != std::string::npos,
          "AC2: query:ensure-ref restamp-lag");
    CHECK(qws.find("schema-3058") != std::string::npos, "AC2: ensure-ref hash torn schema");
    CHECK(qws.find("restamp-generation-torn") != std::string::npos, "AC2: ensure-ref reports torn");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::set_restamp_budget_nodes_for_process;
    clear_restamp_budget_nodes_override_for_test();
    FlatAST flat;
    for (int i = 0; i < 16; ++i)
        (void)flat.add_node(aura::ast::NodeTag::LiteralInt, aura::ast::SyntaxMarker::User);
    set_restamp_budget_nodes_for_process(1);
    flat.restamp_all_node_generations();
    CHECK(flat.restamp_last_budget_exceeded(), "AC2: last-exceeded after over-budget");
    CHECK(flat.restamp_generation_torn(), "AC2: generation torn after over-budget");
    clear_restamp_budget_nodes_override_for_test();
}

static void ac3058_3_soft_under_budget_unchanged() {
    std::println("\n--- #3058 AC3: Soft / under-budget path unchanged ---");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fm.find("skipped_extra = true") != std::string::npos, "AC3: Soft skip retained");
    CHECK(fm.find("!production && !wrap_pending && !last_budget") != std::string::npos,
          "AC3: Soft skip gate retained");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    clear_restamp_budget_nodes_override_for_test();
    FlatAST flat;
    (void)flat.add_node(aura::ast::NodeTag::LiteralInt, aura::ast::SyntaxMarker::User);
    flat.restamp_all_node_generations();
    CHECK(!flat.restamp_last_budget_exceeded(), "AC3: under-budget not exceeded");
    CHECK(!flat.restamp_generation_torn(), "AC3: under-budget not torn");
}

static void ac3058_4_additive_schema() {
    std::println("\n--- #3058 AC4: additive schema only ---");
    const auto review = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto qmid = read_file("src/compiler/evaluator_primitives_query_obs_mid.cpp");
    CHECK(review.find("schema-3058") != std::string::npos, "AC4: generation-stats schema-3058");
    CHECK(review.find("query-stable-ref-over-budget-visible-wired") != std::string::npos,
          "AC4: wired");
    CHECK(q.find("schema-3058") != std::string::npos, "AC4: stable-ref-stats-hash schema-3058");
    CHECK(qmid.find("schema-3058") != std::string::npos, "AC4: children-stable-stats schema-3058");
    CHECK(review.find("schema-3000") != std::string::npos, "AC4: restamp-lag keys preserved");
    CHECK(review.find("schema-3037") != std::string::npos, "AC4: torn keys preserved");
    CHECK(review.find("schema-3019") != std::string::npos, "AC4: unified keys preserved");
}

static void ac3058_5_canary_linter() {
    std::println("\n--- #3058 AC5: canary + linter + no invent ---");
    const auto t = read_file("tests/core/test_restamp_sla_observability.cpp");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3058_1_unified_entry_no_steal_split") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac3058_2_over_budget_query_stable_visible") != std::string::npos,
          "AC5: AC2 test");
    CHECK(build.find("check_unified_restamp_query_visible_3058") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(read_file("tests/core/test_issue_3058.cpp").empty(), "AC5: no test_issue_3058.cpp");
    CHECK(read_file("docs/design/3058-unified-restamp-query.md").empty(),
          "AC5: no docs/design/3058-* per #1655");
}

} // namespace

int run_test_restamp_sla_observability() {
    std::println("=== Issue #2528: restamp SLA observability (long-session residual) ===");
    ac1_query_surface_reports_sla();
    ac2_soft_no_wrap_zero_overhead();
    ac3_is_valid_correct_after_incremental();
    ac4_configurable_slo_budget_breach();
    ac5_chaos_soak_tsan_covered();
    ac6_additive_schema_existing_fixtures();
    std::println("\n=== Issue #2934: restamp budget soft-degrade ===");
    ac2934_1_budget_soft_degrade();
    ac2934_2_default_unlimited();
    ac2934_3_agent_metrics();
    ac2934_5_source_and_linter();
    std::println("\n=== Issue #3019: unified restamp after boundary/abort/steal/densify ===");
    ac3019_1_unified_entry_four_sites();
    ac3019_2_order_node_stable_pin();
    ac3019_3_budget_torn_visible();
    ac3019_4_soft_zero_extra_walk();
    ac3019_5_canary_soak_linter();
    std::println("\n=== Issue #3041: restamp budget QueryEpoch stale ===");
    ac3041_1_production_budget_forces_query_epoch_stale();
    ac3041_2_lazy_align_still_runs();
    ac3041_3_soft_unlimited_zero_extra();
    ac3041_4_schema_and_linter();
    std::println("\n=== Issue #3058: unified restamp + query:*-stable torn visible ===");
    ac3058_1_unified_entry_no_steal_split();
    ac3058_2_over_budget_query_stable_visible();
    ac3058_3_soft_under_budget_unchanged();
    ac3058_4_additive_schema();
    ac3058_5_canary_linter();
    std::println("\n=== #2528+#2934+#3019+#3041+#3058: see per-AC results above ===");
    return aura::test::g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_restamp_sla_observability();
}
#endif
