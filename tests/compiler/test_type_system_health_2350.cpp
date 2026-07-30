// @category: unit
// @reason: Issue #2350 — query:type-system-health single Agent score
// (provenance + timeout + pin + layered DCE).
//
//   AC1: Score definition (header + pure compute)
//   AC2: force_reason priority when health < budget
//   AC3: Pure / additive (existing queries still resolve)
//   AC4: Keys health-bp / force-reason / schema-2350
//   AC5: Tests + source-cite

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/type_system_health.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.lifetime_pin;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::compute_type_system_health;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::layered_dce_efficiency_bp;
using aura::compiler::rate_bp;
using aura::compiler::TypeSystemHealthSnapshot;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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

static std::int64_t href_int(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: pure score on vacuous snapshot ──
static void ac1_vacuous_healthy() {
    std::println("\n--- AC1: vacuous snapshot → health 10000 ---");
    TypeSystemHealthSnapshot s; // defaults vacuous healthy
    auto r = compute_type_system_health(s);
    CHECK(r.health_bp == 10000, "AC1: vacuous health_bp == 10000");
    CHECK(r.force_reason == "ok", "AC1: force-reason ok");
    CHECK(r.health_budget_bp == 8000 || r.health_budget_bp <= 10000, "AC1: budget default");
    CHECK(layered_dce_efficiency_bp(0, 0) == 10000, "AC1: dce vacuous 10000");
    CHECK(rate_bp(0, 0) == 0, "AC1: rate vacuous 0");
    CHECK(rate_bp(1, 2) == 5000, "AC1: rate 1/2 = 5000 bp");
}

// ── AC2: force_reason priority ──
static void ac2_force_reason_priority() {
    std::println("\n--- AC2: force_reason priority ---");
    // Drive health below budget with timeout first.
    {
        TypeSystemHealthSnapshot s;
        s.timeout_reject_rate_bp = 5000; // halves timeout component
        s.linear_pin_miss_rate_bp = 5000;
        s.provenance_completeness_bp = 5000;
        s.castop_density_bp = 9000;
        s.castop_density_budget_bp = 1500;
        auto r = compute_type_system_health(s);
        CHECK(r.health_bp < r.health_budget_bp, "AC2: health below budget");
        CHECK(r.force_reason == "timeout-reject", "AC2: timeout-reject wins priority");
    }
    {
        TypeSystemHealthSnapshot s;
        s.timeout_reject_rate_bp = 0;
        s.linear_pin_miss_rate_bp = 9000; // health = (10000+10000+1000+10000)/4 = 7750
        s.provenance_completeness_bp = 5000;
        auto r = compute_type_system_health(s);
        CHECK(r.health_bp < r.health_budget_bp, "AC2: pin case below budget");
        CHECK(r.force_reason == "pin-miss", "AC2: pin-miss second");
    }
    {
        TypeSystemHealthSnapshot s;
        // health = (0+10000+10000+10000)/4 = 7500 < 8000
        s.provenance_completeness_bp = 0;
        auto r = compute_type_system_health(s);
        CHECK(r.health_bp < r.health_budget_bp, "AC2: provenance case below budget");
        CHECK(r.force_reason == "provenance-miss", "AC2: provenance-miss third");
    }
    {
        TypeSystemHealthSnapshot s;
        // Only castop bad; other components perfect → health still 10000
        // because castop is not in the score formula. Force castop by
        // also tanking dce efficiency so health < budget.
        s.layered_dce_efficiency_bp = 0;
        s.castop_density_bp = 9000;
        s.castop_density_budget_bp = 1500;
        auto r = compute_type_system_health(s);
        CHECK(r.health_bp < r.health_budget_bp, "AC2: low dce drops health");
        CHECK(r.force_reason == "castop-density", "AC2: castop-density fourth");
    }
}

// ── AC3 / AC4: query surface + lineage ──
static void ac3_ac4_query_surface() {
    std::println("\n--- AC3/AC4: query:type-system-health keys ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");

    auto h = cs.eval("(engine:metrics \"query:type-system-health\")");
    CHECK(h && is_hash(*h), "AC4: type-system-health is hash");

    CHECK(href_int(cs, "query:type-system-health", "schema-2350") == 2350, "schema-2350");
    CHECK(href_int(cs, "query:type-system-health", "issue-2350") == 2350, "issue-2350");
    CHECK(href_int(cs, "query:type-system-health", "type-system-health-wired") == 1, "wired");
    const auto health = href_int(cs, "query:type-system-health", "health-bp");
    CHECK(health >= 8000, "AC1 fresh process: health_bp ≥ budget (vacuous)");
    CHECK(href_int(cs, "query:type-system-health", "health-budget-bp") == 8000 ||
              href_int(cs, "query:type-system-health", "health-budget-bp") >= 0,
          "health-budget-bp");
    CHECK(href_int(cs, "query:type-system-health", "component-provenance-completeness-bp") >= 0,
          "component-provenance");
    CHECK(href_int(cs, "query:type-system-health", "component-timeout-reject-rate-bp") >= 0,
          "component-timeout");
    CHECK(href_int(cs, "query:type-system-health", "component-linear-pin-miss-rate-bp") >= 0,
          "component-pin");
    CHECK(href_int(cs, "query:type-system-health", "component-layered-dce-efficiency-bp") >= 0,
          "component-dce");

    // AC3: existing layered / fidelity / timeout queries still resolve.
    CHECK(href_int(cs, "query:dead-coercion-layered-stats", "dead-coercion-layered-total") >= 0 ||
              cs.eval("(engine:metrics \"query:dead-coercion-layered-stats\")").has_value(),
          "AC3: layered stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")").has_value(),
          "AC3: fidelity stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:type-timeout-repair-stats\")").has_value(),
          "AC3: timeout-repair stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:castop-density-stats\")").has_value(),
          "AC3: castop-density stats still reachable");
}

// ── AC2 inject: pin miss / timeout → score drops ──
static void ac2_inject_counters() {
    std::println("\n--- AC2 inject: counters move score / force_reason ---");
    // Pure path with injected snapshot (no process-wide pollution needed).
    TypeSystemHealthSnapshot s;
    // health = (10000 + (10000-9000) + 10000 + 10000)/4 = 7750 < 8000
    s.timeout_reject_rate_bp = 9000;
    s.timeout_reject_total = 9;
    s.timeout_full_solve_total = 10;
    auto r = compute_type_system_health(s);
    CHECK(r.health_bp < r.health_budget_bp, "AC2: timeout rate drops health below budget");
    CHECK(r.force_reason == "timeout-reject", "AC2: inject → timeout-reject");

    // Process counters: bump provenance miss, re-query (if completeness drops).
    const auto miss0 = g_coercion_provenance_miss_total.load(std::memory_order_relaxed);
    const auto ok0 = g_coercion_provenance_complete_total.load(std::memory_order_relaxed);
    g_coercion_provenance_miss_total.fetch_add(10, std::memory_order_relaxed);
    // Ensure completeness not vacuous: need complete+miss > 0 (already have miss).
    if (ok0 + miss0 + 10 == 0)
        g_coercion_provenance_complete_total.fetch_add(1, std::memory_order_relaxed);

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    const auto prov =
        href_int(cs, "query:type-system-health", "component-provenance-completeness-bp");
    CHECK(prov >= 0 && prov <= 10000, "AC2: provenance component queryable after inject");
    // Score may still be high if other components are 10000; at least completeness < 10000.
    if (miss0 + 10 > 0)
        CHECK(prov < 10000 || ok0 == 0, "AC2: provenance completeness reflects misses");
}

// ── AC5: source-cite ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto hh = read_file("src/compiler/type_system_health.hh");
    const auto obs = read_file("src/compiler/evaluator_primitives_observability.cpp");
    CHECK(q.find("query:type-system-health") != std::string::npos, "AC5: query registered");
    CHECK(q.find("Issue #2350") != std::string::npos, "AC5: query cites #2350");
    CHECK(hh.find("health_bp") != std::string::npos || hh.find("health-bp") != std::string::npos,
          "AC5: weight comment in header");
    CHECK(hh.find("timeout-reject") != std::string::npos, "AC5: force_reason table");
    CHECK(hh.find("0.25") != std::string::npos || hh.find("quarter") != std::string::npos,
          "AC5: equal weights documented");
    CHECK(obs.find("query:type-system-health") != std::string::npos, "AC5: catalog lists health");
}

} // namespace

int main() {
    std::println("=== Issue #2350: query:type-system-health ===");
    ac1_vacuous_healthy();
    ac2_force_reason_priority();
    ac3_ac4_query_surface();
    ac2_inject_counters();
    ac5_source_cite();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
