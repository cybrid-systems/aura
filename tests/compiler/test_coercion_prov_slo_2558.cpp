// @category: unit
// @reason: Issue #2558 — coercion provenance completeness SLO → force Full
//          audit on next MutationBoundary under production Sampled.
//
//   AC1: production + miss storm → bp < SLO → force pending; consume forces audit
//   AC2: Soft / non-production → observe-only breach; no force pending
//   AC3: no samples → bp 10000; no breach
//   AC4: #2512 stamp-at-add still preferred (wired flag); SLO is backstop
//   AC5: query:coercion-provenance-health schema-2558 + source-cite

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "compiler/coercion_provenance_policy.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::coercion_prov_slo_bp;
using aura::compiler::coercion_prov_slo_force_full_pending;
using aura::compiler::coercion_provenance_completeness_bp;
using aura::compiler::CompilerService;
using aura::compiler::consume_coercion_prov_slo_force_full;
using aura::compiler::consume_provenance_miss_for_boundary;
using aura::compiler::evaluate_coercion_provenance_slo;
using aura::compiler::g_coercion_prov_slo_breach_total;
using aura::compiler::g_coercion_prov_slo_force_armed_total;
using aura::compiler::g_coercion_prov_slo_force_consumed_total;
using aura::compiler::g_coercion_prov_slo_observe_only_total;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::g_coercion_stamp_at_add_wired;
using aura::compiler::kCoercionProvSloBpDefault;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_coercion_prov_slo_bp_for_test;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::production_defaults_active;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:coercion-provenance-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_slo_counters() {
    reset_coercion_provenance_miss_policy_for_test();
    g_coercion_provenance_complete_total.store(0, std::memory_order_relaxed);
    g_coercion_provenance_miss_total.store(0, std::memory_order_relaxed);
    g_coercion_prov_slo_breach_total.store(0, std::memory_order_relaxed);
    g_coercion_prov_slo_observe_only_total.store(0, std::memory_order_relaxed);
    g_coercion_prov_slo_force_armed_total.store(0, std::memory_order_relaxed);
    g_coercion_prov_slo_force_consumed_total.store(0, std::memory_order_relaxed);
    (void)consume_provenance_miss_for_boundary();
    (void)consume_coercion_prov_slo_force_full(); // clear pending
    set_coercion_prov_slo_bp_for_test(kCoercionProvSloBpDefault);
}

// ── AC1: production Sampled miss storm → force pending ──
static void ac1_production_slo_force() {
    std::println("\n--- #2558 AC1: production Sampled + miss storm → force Full ---");
    reset_slo_counters();
    // Production defaults + Sampled strategy (long AI session under Sampled).
    apply_production_audit_defaults(); // production_defaults_active=1, Full
    set_strategy(AuditStrategy::Sampled);
    CHECK(production_defaults_active(), "AC1: production defaults active");
    // Tight SLO so a few misses breach.
    set_coercion_prov_slo_bp_for_test(9000);
    // Drive miss ratio: 1 complete + 2 miss → bp = 3333 < 9000.
    g_coercion_provenance_complete_total.store(1, std::memory_order_relaxed);
    g_coercion_provenance_miss_total.store(2, std::memory_order_relaxed);
    const auto bp = coercion_provenance_completeness_bp();
    CHECK(bp < coercion_prov_slo_bp(), "AC1: bp below SLO");

    const auto breach0 = g_coercion_prov_slo_breach_total.load();
    const auto armed0 = g_coercion_prov_slo_force_armed_total.load();
    evaluate_coercion_provenance_slo(bp, /*production_active=*/true);
    CHECK(g_coercion_prov_slo_breach_total.load() > breach0, "AC1: breach_total bumped");
    CHECK(coercion_prov_slo_force_full_pending(), "AC1: force-full pending armed");
    CHECK(g_coercion_prov_slo_force_armed_total.load() > armed0, "AC1: force-armed bumped");

    // Consume = one-shot Full path (mirrors boundary exit).
    const auto cons0 = g_coercion_prov_slo_force_consumed_total.load();
    CHECK(consume_coercion_prov_slo_force_full(), "AC1: consume returns true");
    CHECK(!coercion_prov_slo_force_full_pending(), "AC1: pending cleared after consume");
    CHECK(g_coercion_prov_slo_force_consumed_total.load() > cons0, "AC1: consumed bumped");

    apply_dev_audit_defaults();
    reset_slo_counters();
}

// ── AC2: Soft observe-only ──
static void ac2_soft_observe() {
    std::println("\n--- #2558 AC2: Soft / non-production observe-only ---");
    reset_slo_counters();
    apply_dev_audit_defaults(); // production_defaults_active=0
    CHECK(!production_defaults_active(), "AC2: not production");
    set_coercion_prov_slo_bp_for_test(9000);
    g_coercion_provenance_complete_total.store(0, std::memory_order_relaxed);
    g_coercion_provenance_miss_total.store(1, std::memory_order_relaxed);
    const auto bp = coercion_provenance_completeness_bp();
    const auto obs0 = g_coercion_prov_slo_observe_only_total.load();
    const auto breach0 = g_coercion_prov_slo_breach_total.load();
    evaluate_coercion_provenance_slo(bp, /*production_active=*/false);
    CHECK(g_coercion_prov_slo_breach_total.load() > breach0, "AC2: breach still counted");
    CHECK(g_coercion_prov_slo_observe_only_total.load() > obs0, "AC2: observe-only bumped");
    CHECK(!coercion_prov_slo_force_full_pending(), "AC2: no force pending under Soft");
    reset_slo_counters();
}

// ── AC3: vacuous 10000 ──
static void ac3_vacuous() {
    std::println("\n--- #2558 AC3: no samples → bp 10000; no breach ---");
    reset_slo_counters();
    apply_production_audit_defaults();
    g_coercion_provenance_complete_total.store(0, std::memory_order_relaxed);
    g_coercion_provenance_miss_total.store(0, std::memory_order_relaxed);
    CHECK(coercion_provenance_completeness_bp() == 10000, "AC3: vacuous 10000");
    const auto breach0 = g_coercion_prov_slo_breach_total.load();
    evaluate_coercion_provenance_slo(coercion_provenance_completeness_bp(), true);
    CHECK(g_coercion_prov_slo_breach_total.load() == breach0, "AC3: no breach");
    CHECK(!coercion_prov_slo_force_full_pending(), "AC3: no force pending");
    apply_dev_audit_defaults();
    reset_slo_counters();
}

// ── AC4: stamp-at-add still preferred ──
static void ac4_stamp_backstop() {
    std::println("\n--- #2558 AC4: #2512 stamp-at-add preferred (SLO backstop) ---");
    CHECK(g_coercion_stamp_at_add_wired.load() == 1, "AC4: stamp-at-add wired");
    const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("backstop") != std::string::npos || pol.find("#2512") != std::string::npos,
          "AC4: SLO documents stamp-at-add as preferred");
    const auto map = read_file("src/compiler/coercion_map.ixx");
    CHECK(map.find("evaluate_coercion_provenance_slo") != std::string::npos,
          "AC4: fill path evaluates SLO");
    CHECK(map.find("#2512") != std::string::npos, "AC4: #2512 lineage retained");
}

// ── AC5: query + source ──
static void ac5_source_schema() {
    std::println("\n--- #2558 AC5: query + source-cite ---");
    const auto pol = read_file("src/compiler/coercion_provenance_policy.hh");
    CHECK(pol.find("#2558") != std::string::npos, "AC5: policy cites #2558");
    CHECK(pol.find("evaluate_coercion_provenance_slo") != std::string::npos, "AC5: evaluate SLO");
    CHECK(pol.find("kCoercionProvSloBpDefault") != std::string::npos, "AC5: SLO default");

    const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("consume_coercion_prov_slo_force_full") != std::string::npos,
          "AC5: boundary consumes SLO force");
    CHECK(bound.find("#2558") != std::string::npos, "AC5: boundary cites #2558");

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:coercion-provenance-health") != std::string::npos, "AC5: health query");
    CHECK(q.find("schema-2558") != std::string::npos, "AC5: schema-2558");

    reset_slo_counters();
    CompilerService cs;
    CHECK(href(cs, "schema-2558") == 2558, "AC5: live schema-2558");
    CHECK(href(cs, "completeness-bp") == 10000, "AC5: vacuous completeness");
    CHECK(href(cs, "slo-bp") == static_cast<std::int64_t>(kCoercionProvSloBpDefault),
          "AC5: default SLO bp");
    CHECK(href(cs, "force-full-pending") == 0, "AC5: no pending");
    CHECK(href(cs, "slo-breach-total") >= 0, "AC5: breach total queryable");
    CHECK(href(cs, "miss-total") >= 0, "AC5: miss-total");
    CHECK(href(cs, "stamp-at-add-total") >= 0, "AC5: stamp-at-add total");
}

} // namespace

int main() {
    std::println("=== Issue #2558: coercion provenance completeness SLO ===");
    ac1_production_slo_force();
    ac2_soft_observe();
    ac3_vacuous();
    ac4_stamp_backstop();
    ac5_source_schema();
    apply_dev_audit_defaults();
    reset_slo_counters();
    std::println("\n=== #2558: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
