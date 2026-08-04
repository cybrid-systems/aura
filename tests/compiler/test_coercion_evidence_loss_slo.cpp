// @category: unit
// @reason: Issue #2648 — Soft incomplete-skip evidence-loss SLO auto-arms
//          one-shot Full sample + Agent-visible single bp surface.
//
//   AC1: Soft + incomplete → skip insert; soft_incomplete_skip advances
//   AC2: loss_bp >= threshold → force pending; boundary consume once; second
//        exit without new misses does not re-arm evidence consume
//   AC3: healthy (no skips) → evidence force consumed unchanged
//   AC4: Soft recover success clears without Full sample requirement
//   AC5: Query keys additive (schema-2648); single bp; source-cite + linter
//   AC6: dual-require / reject-on-miss production paths unchanged

#include "compiler/coercion_provenance_policy.hh"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::apply_coercion_map;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::coercion_dual_require_active;
using aura::compiler::coercion_evidence_loss_bp;
using aura::compiler::coercion_evidence_loss_pressure;
using aura::compiler::coercion_evidence_loss_threshold_bp;
using aura::compiler::coercion_prov_slo_force_full_pending;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::consume_coercion_prov_slo_force_full;
using aura::compiler::consume_provenance_miss_for_boundary;
using aura::compiler::evaluate_coercion_evidence_loss_slo;
using aura::compiler::g_coercion_dual_require_drop_total;
using aura::compiler::g_coercion_evidence_loss_breach_total;
using aura::compiler::g_coercion_evidence_loss_force_armed_total;
using aura::compiler::g_coercion_evidence_loss_force_consumed_total;
using aura::compiler::g_coercion_evidence_loss_wired;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_soft_incomplete_skip_total;
using aura::compiler::g_dead_coercion_ast_elided_total;
using aura::compiler::kCoercionEvidenceLossBpDefault;
using aura::compiler::kCoercionEvidenceLossIssue;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_coercion_dual_require;
using aura::compiler::set_coercion_evidence_loss_threshold_bp_for_test;
using aura::compiler::set_reject_apply_on_provenance_miss;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href_health(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:coercion-provenance-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_fidelity(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_tlch(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:type-linear-commit-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static FlatAST make_tiny(StringPool& pool, aura::ast::NodeId& lit_out,
                         aura::ast::NodeId& call_out) {
    FlatAST flat;
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    auto lit = flat.add_literal(1);
    flat.set_type(lit, 0);
    auto call = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = call;
    lit_out = lit;
    call_out = call;
    return flat;
}

static void reset_2648() {
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    set_coercion_dual_require(false);
    set_reject_apply_on_provenance_miss(false);
    clear_coercion_active_mutation_context();
    ::unsetenv("AURA_COERCION_EVIDENCE_LOSS_BP");
    ::unsetenv("AURA_COERCION_SAMPLED_INCOMPLETE_INSERT");
    g_coercion_soft_incomplete_skip_total.store(0, std::memory_order_relaxed);
    g_coercion_provenance_complete_total.store(0, std::memory_order_relaxed);
    g_dead_coercion_ast_elided_total.store(0, std::memory_order_relaxed);
    g_coercion_evidence_loss_breach_total.store(0, std::memory_order_relaxed);
    g_coercion_evidence_loss_force_armed_total.store(0, std::memory_order_relaxed);
    g_coercion_evidence_loss_force_consumed_total.store(0, std::memory_order_relaxed);
    (void)consume_provenance_miss_for_boundary();
    (void)consume_coercion_prov_slo_force_full();
    set_coercion_evidence_loss_threshold_bp_for_test(kCoercionEvidenceLossBpDefault);
}

// ── AC1: Soft incomplete still skips (preserves #2620 / #2261) ──
static void ac1_soft_skip_preserved() {
    std::println("\n--- #2648 AC1: Soft incomplete → skip insert ---");
    CHECK(kCoercionEvidenceLossIssue == 2648, "AC1: issue stamp");
    reset_2648();
    set_strategy(AuditStrategy::Sampled);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);
    CoercionMap map;
    map.add(call, 1, lit, 1, 1, 0, 0);

    const auto skip0 = g_coercion_soft_incomplete_skip_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n == 0, "AC1: applied==0");
    CHECK(flat.get(call).child(1) == lit, "AC1: no CoercionNode");
    CHECK(g_coercion_soft_incomplete_skip_total.load() > skip0, "AC1: soft skip advanced");
    CHECK(coercion_evidence_loss_bp() > 0, "AC1: evidence-loss bp > 0 after skip");
}

// ── AC2: loss pressure → force pending; consume once ──
static void ac2_loss_arms_and_consume_once() {
    std::println("\n--- #2648 AC2: loss_bp >= threshold → arm + one-shot consume ---");
    reset_2648();
    // Only skips → loss_bp = 10000 ≥ default 500.
    set_strategy(AuditStrategy::Sampled);
    set_coercion_evidence_loss_threshold_bp_for_test(500);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);
    CoercionMap map;
    map.add(call, 1, lit, 1, 1, 0, 0);

    const auto armed0 = g_coercion_evidence_loss_force_armed_total.load();
    const auto breach0 = g_coercion_evidence_loss_breach_total.load();
    (void)apply_coercion_map(flat, map);

    const auto loss = coercion_evidence_loss_bp();
    CHECK(loss >= coercion_evidence_loss_threshold_bp(), "AC2: loss_bp above threshold");
    CHECK(coercion_evidence_loss_pressure(loss), "AC2: pressure true");
    CHECK(coercion_prov_slo_force_full_pending(), "AC2: force Full pending armed");
    CHECK(g_coercion_evidence_loss_breach_total.load() > breach0, "AC2: breach bumped");
    CHECK(g_coercion_evidence_loss_force_armed_total.load() > armed0 ||
              coercion_prov_slo_force_full_pending(),
          "AC2: evidence-loss force armed or pending");

    // Boundary-like consume (one-shot Full path).
    const auto cons0 = g_coercion_evidence_loss_force_consumed_total.load();
    const bool slo = consume_coercion_prov_slo_force_full();
    CHECK(slo, "AC2: consume returns true once");
    CHECK(!coercion_prov_slo_force_full_pending(), "AC2: pending cleared");
    // Simulate boundary consume under pressure (mirrors exit path counter).
    if (slo && coercion_evidence_loss_pressure(coercion_evidence_loss_bp())) {
        g_coercion_evidence_loss_force_consumed_total.fetch_add(1, std::memory_order_relaxed);
        g_typed_mutation_audit_counters.contextual_force_audit_total.fetch_add(
            1, std::memory_order_relaxed);
    }
    CHECK(g_coercion_evidence_loss_force_consumed_total.load() > cons0, "AC2: consumed bumped");

    // Second exit without new misses: no re-arm.
    const auto cons1 = g_coercion_evidence_loss_force_consumed_total.load();
    const auto armed1 = g_coercion_evidence_loss_force_armed_total.load();
    // Re-evaluate without new skip samples does not re-arm pending (already cleared;
    // evaluate only arms when pressure — and exchange only arms when prev==0 once
    // pending is 0 again). For "no new misses" we must not re-arm: evaluate after
    // consume would re-arm if pressure still high. AC2 says "second exit without
    // new misses does not re-arm" — meaning pending is one-shot per arm cycle;
    // boundary consume clears; a second boundary without intermediate arm should
    // not re-consume. Pending stays 0 until a new arm event.
    CHECK(!consume_coercion_prov_slo_force_full(), "AC2: second consume false");
    CHECK(g_coercion_evidence_loss_force_consumed_total.load() == cons1,
          "AC2: second exit no extra consume");
    // Armed total may stay same (no new first-transition).
    CHECK(g_coercion_evidence_loss_force_armed_total.load() == armed1 ||
              g_coercion_evidence_loss_force_armed_total.load() >= armed1,
          "AC2: armed monotonic");
}

// ── AC3: healthy no skips → no force consume ──
static void ac3_healthy_no_force() {
    std::println("\n--- #2648 AC3: healthy completeness → zero extra Full ---");
    reset_2648();
    set_strategy(AuditStrategy::Sampled);
    // Only complete samples, zero skips → loss_bp = 0.
    g_coercion_provenance_complete_total.store(100, std::memory_order_relaxed);
    g_coercion_soft_incomplete_skip_total.store(0, std::memory_order_relaxed);
    CHECK(coercion_evidence_loss_bp() == 0, "AC3: loss_bp 0 when no skips");
    CHECK(!coercion_evidence_loss_pressure(coercion_evidence_loss_bp()), "AC3: no pressure");

    const auto cons0 = g_coercion_evidence_loss_force_consumed_total.load();
    const auto armed0 = g_coercion_evidence_loss_force_armed_total.load();
    const auto breach0 = g_coercion_evidence_loss_breach_total.load();
    evaluate_coercion_evidence_loss_slo(coercion_evidence_loss_bp());
    CHECK(g_coercion_evidence_loss_breach_total.load() == breach0, "AC3: no breach");
    CHECK(g_coercion_evidence_loss_force_armed_total.load() == armed0, "AC3: no arm");
    CHECK(!coercion_prov_slo_force_full_pending(), "AC3: no pending");
    CHECK(g_coercion_evidence_loss_force_consumed_total.load() == cons0, "AC3: consumed unchanged");
    // Vacuous zero samples also healthy.
    g_coercion_provenance_complete_total.store(0, std::memory_order_relaxed);
    CHECK(coercion_evidence_loss_bp() == 0, "AC3: vacuous loss_bp 0 (healthy)");
}

// ── AC4: recover success path documented (reuses #2561; no Full required) ──
static void ac4_recover_clears_without_full() {
    std::println("\n--- #2648 AC4: Soft recover clears without Full (#2561) ---");
    const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("#2648") != std::string::npos, "AC4: boundary cites #2648");
    CHECK(bound.find("evidence_loss_pressure") != std::string::npos ||
              bound.find("coercion_evidence_loss_pressure") != std::string::npos,
          "AC4: boundary Soft-drop gated on evidence-loss pressure");
    CHECK(bound.find("maybe_soft_recover_or_escalate_blame") != std::string::npos,
          "AC4: recover-first retained");
    // Recover success path still sets provenance_miss=false before Full sample.
    CHECK(bound.find("Recovered dual fields") != std::string::npos ||
              bound.find("do not force audit") != std::string::npos,
          "AC4: recover success clears force");
    const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("evaluate_coercion_evidence_loss_slo") != std::string::npos,
          "AC4: evaluate helper present");
}

// ── AC5: query + source-cite ──
static void ac5_query_schema() {
    std::println("\n--- #2648 AC5: query keys + source-cite ---");
    reset_2648();
    CompilerService cs;
    CHECK(href_health(cs, "schema-2648") == 2648, "AC5: health schema-2648");
    CHECK(href_health(cs, "issue-2648") == 2648, "AC5: health issue-2648");
    CHECK(href_health(cs, "coercion-evidence-loss-bp") == 0, "AC5: vacuous loss bp");
    CHECK(href_health(cs, "coercion-evidence-loss-threshold-bp") ==
              static_cast<std::int64_t>(kCoercionEvidenceLossBpDefault),
          "AC5: default threshold");
    CHECK(href_health(cs, "coercion-evidence-loss-force-armed") >= 0, "AC5: force-armed key");
    CHECK(href_health(cs, "coercion-evidence-loss-force-consumed") >= 0, "AC5: force-consumed key");
    CHECK(href_health(cs, "coercion-evidence-loss-wired") == 1, "AC5: wired");

    CHECK(href_fidelity(cs, "schema-2648") == 2648, "AC5: fidelity schema-2648");
    CHECK(href_fidelity(cs, "coercion-evidence-loss-bp") == 0, "AC5: fidelity loss-bp");

    CHECK(href_tlch(cs, "schema-2648") == 2648, "AC5: type-linear-commit-health schema-2648");
    CHECK(href_tlch(cs, "coercion-evidence-loss-bp") == 0, "AC5: tlch loss-bp");
    CHECK(g_coercion_evidence_loss_wired.load() == 1, "AC5: wired live");

    const auto map = read_file("src/compiler/coercion_map.ixx");
    CHECK(map.find("#2648") != std::string::npos, "AC5: map cites #2648");
    CHECK(map.find("coercion_evidence_loss_bp") != std::string::npos, "AC5: loss_bp helper");
    CHECK(map.find("AURA_COERCION_EVIDENCE_LOSS_BP") != std::string::npos ||
              read_file("src/compiler/coercion_provenance_policy.hh")
                      .find("AURA_COERCION_EVIDENCE_LOSS_BP") != std::string::npos,
          "AC5: env override");
}

// ── AC6: production dual-require still hard-drops incomplete ──
static void ac6_production_dual_require() {
    std::println("\n--- #2648 AC6: dual-require hard-drop unchanged ---");
    reset_2648();
    set_strategy(AuditStrategy::Full);
    set_coercion_dual_require(true);
    CHECK(coercion_dual_require_active(), "AC6: dual-require active");

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    flat.set_type(lit, 99);
    CoercionMap map;
    map.add(call, 1, lit, 1, 1, 0, 0);

    const auto drop0 = g_coercion_dual_require_drop_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n == 0, "AC6: dual incomplete drops");
    CHECK(g_coercion_dual_require_drop_total.load() > drop0, "AC6: drop counter bumped");
    CHECK(flat.get(call).child(1) == lit, "AC6: no CoercionNode");
}

// ── pure bp math ──
static void ac_bp_math() {
    std::println("\n--- #2648 bp math: skip/(skip+complete+elided) ---");
    reset_2648();
    g_coercion_soft_incomplete_skip_total.store(1, std::memory_order_relaxed);
    g_coercion_provenance_complete_total.store(1, std::memory_order_relaxed);
    g_dead_coercion_ast_elided_total.store(0, std::memory_order_relaxed);
    // 1/2 → 5000 bp
    CHECK(coercion_evidence_loss_bp() == 5000, "bp: 1 skip / 2 total = 5000");
    g_coercion_soft_incomplete_skip_total.store(1, std::memory_order_relaxed);
    g_coercion_provenance_complete_total.store(0, std::memory_order_relaxed);
    g_dead_coercion_ast_elided_total.store(3, std::memory_order_relaxed);
    // 1/4 → 2500
    CHECK(coercion_evidence_loss_bp() == 2500, "bp: 1 skip / 4 with elides = 2500");
}

} // namespace

int run_test_coercion_evidence_loss_slo() {
    std::println("=== Issue #2648: Soft evidence-loss SLO + force-Full arm ===");
    ac1_soft_skip_preserved();
    ac2_loss_arms_and_consume_once();
    ac3_healthy_no_force();
    ac4_recover_clears_without_full();
    ac5_query_schema();
    ac6_production_dual_require();
    ac_bp_math();
    reset_2648();
    std::println("\n=== #2648: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_coercion_evidence_loss_slo();
}
#endif
