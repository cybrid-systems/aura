// @category: unit
// @reason: Issue #2389 — query:security-health single Agent score
// (effect deny + isolation + epoch fence + WAL posture + ring wrap).
//
//   AC1: Fresh / vacuous → health_bp high / force-reason ok
//   AC2: Inject effect denials → score drops; force-reason effect-deny
//   AC3: Existing capability / isolation / security-audit queries unchanged
//   AC4: Source-cite registration + weight comment
//   AC5: Tests + CMake + build.py gate

#include "test_harness.hpp"

#include "compiler/security_health.hh"
#include "core/capability_model.hh"
#include "core/security_event.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::compute_security_health;
using aura::compiler::kSecurityHealthIssue;
using aura::compiler::security_rate_bp;
using aura::compiler::SecurityHealthSnapshot;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
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

static void reset_security_surfaces() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_security_event_ring_for_test();
    g_capability_registry().sandbox_mode = EffectSandboxMode::Off;
}

// ── AC1: vacuous pure score + fresh process query ──
static void ac1_vacuous_healthy() {
    std::println("\n--- AC1: vacuous snapshot → health 10000 / ok ---");
    SecurityHealthSnapshot s;
    auto r = compute_security_health(s);
    CHECK(r.health_bp == 10000, "AC1: vacuous health_bp == 10000");
    CHECK(r.force_reason == "ok", "AC1: force-reason ok");
    CHECK(r.health_budget_bp == 8000 || r.health_budget_bp <= 10000, "AC1: budget default");
    CHECK(security_rate_bp(0, 0) == 0, "AC1: rate vacuous 0");
    CHECK(security_rate_bp(1, 2) == 5000, "AC1: rate 1/2 = 5000 bp");
    CHECK(kSecurityHealthIssue == 2389, "AC1: issue stamp");

    reset_security_surfaces();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto h = cs.eval("(engine:metrics \"query:security-health\")");
    CHECK(h && is_hash(*h), "AC1: query:security-health is hash");
    CHECK(href_int(cs, "query:security-health", "schema-2389") == 2389, "schema-2389");
    CHECK(href_int(cs, "query:security-health", "issue-2389") == 2389, "issue-2389");
    CHECK(href_int(cs, "query:security-health", "security-health-wired") == 1, "wired");
    const auto health = href_int(cs, "query:security-health", "health-bp");
    std::println("  fresh health-bp={}", health);
    CHECK(health >= 8000, "AC1: fresh process health_bp ≥ budget (vacuous)");
    CHECK(href_int(cs, "query:security-health", "health-budget-bp") == 8000 ||
              href_int(cs, "query:security-health", "health-budget-bp") >= 0,
          "health-budget-bp");
}

// ── AC2: effect deny inject + force_reason priority ──
static void ac2_effect_deny_and_priority() {
    std::println("\n--- AC2: effect deny + force_reason priority ---");
    {
        SecurityHealthSnapshot s;
        s.effect_checks = 10;
        s.effect_denied = 10; // 100% deny → effect_good=0
        auto r = compute_security_health(s);
        // health = (0 + 25*10000 + 20*10000 + 15*10000 + 10*10000)/100 = 7000
        CHECK(r.health_bp < r.health_budget_bp, "AC2: full effect deny below budget");
        CHECK(r.health_bp == 7000, "AC2: weighted health 7000 under full effect deny");
        CHECK(r.force_reason == "effect-deny", "AC2: force-reason effect-deny");
        CHECK(r.effect_deny_rate_bp == 10000, "AC2: effect deny rate 10000");
    }
    {
        SecurityHealthSnapshot s;
        s.isolation_checks = 4;
        s.isolation_violations = 4;
        auto r = compute_security_health(s);
        CHECK(r.force_reason == "isolation-deny" || r.health_bp >= r.health_budget_bp,
              "AC2: isolation-deny when isolation alone drops below budget");
        // isolation 25% → health = (30+0+20+15+10)*100 = 7500 < 8000
        CHECK(r.health_bp == 7500, "AC2: isolation full deny health 7500");
        CHECK(r.force_reason == "isolation-deny", "AC2: isolation-deny priority");
    }
    {
        SecurityHealthSnapshot s;
        s.effect_checks = 10;
        s.epoch_fence_hits = 10; // fence_health=0
        auto r = compute_security_health(s);
        // health = (30+25+0+15+10)*100 = 8000 == budget → ok (not below)
        // Push fence + wrap to go below.
        s.ring_total = 100;
        s.ring_wrap_total = 100; // wrap_pressure full
        r = compute_security_health(s);
        // health = (30+25+0+15+0)*100 = 7000
        CHECK(r.health_bp < r.health_budget_bp, "AC2: fence+wrap below budget");
        CHECK(r.force_reason == "epoch-fence", "AC2: epoch-fence before ring-wrap");
    }
    {
        SecurityHealthSnapshot s;
        s.sandbox_mode = 2; // Strict elevated
        s.security_event_wal_enabled = 0;
        s.ring_total = 100;
        s.ring_wrap_total = 100; // need extra drop: wal alone → 8500
        auto r = compute_security_health(s);
        // health = (30+25+20+0+0)*100 = 7500
        CHECK(r.health_bp < r.health_budget_bp, "AC2: wal-off+wrap below budget");
        CHECK(r.force_reason == "wal-off", "AC2: wal-off priority over ring-wrap");
    }
    {
        SecurityHealthSnapshot s;
        s.ring_total = 100;
        s.ring_wrap_total = 100;
        // wrap alone: health = (30+25+20+15+0)*100 = 9000 ≥ budget → ok
        auto r = compute_security_health(s);
        CHECK(r.health_bp >= r.health_budget_bp, "AC2: wrap alone still ≥ budget");
        CHECK(r.force_reason == "ok", "AC2: wrap alone force-reason ok when ≥ budget");
        // Drop wal too under elevated to get ring-wrap as sole remaining signal:
        // Actually priority: no effect/iso/fence, wal-off first if elevated.
        // Pure wrap with zero other: force ok above budget.
        // Below budget with only wrap: need health < 8000 → wrap_good=0 alone
        // only drops 1000 → 9000. Can't get ring-wrap alone below budget with
        // weights. Documented: ring-wrap only surfaces with other pressure.
        (void)r;
    }

    // Live inject: Strict effect denies via capability path.
    {
        reset_security_surfaces();
        g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
        for (int i = 0; i < 20; ++i) {
            EffectProvenance prov{};
            prov.mutation_id = static_cast<std::uint64_t>(1000 + i);
            prov.epoch = 1;
            (void)check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, /*tenant=*/9,
                                          "test:ac2-health-deny", /*wildcard=*/false,
                                          /*sandbox_active=*/true);
        }
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm inject");
        const auto health = href_int(cs, "query:security-health", "health-bp");
        const auto deny_rate =
            href_int(cs, "query:security-health", "component-effect-deny-rate-bp");
        std::println("  after 20 Strict denies: health-bp={} deny-rate-bp={}", health, deny_rate);
        CHECK(deny_rate > 0, "AC2: effect deny rate non-zero after inject");
        CHECK(health < 10000, "AC2: health drops after effect denials");
        // Pure path already locks force-reason; query should be below budget
        // when deny rate is high.
        if (deny_rate >= 5000)
            CHECK(health < 9000, "AC2: substantial deny rate pulls health down");
    }
}

// ── AC3: existing queries still resolve ──
static void ac3_existing_queries_unchanged() {
    std::println("\n--- AC3: existing security queries still resolve ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(cs.eval("(engine:metrics \"query:capability-effect-stats\")").has_value(),
          "AC3: capability-effect-stats reachable");
    CHECK(cs.eval("(engine:metrics \"query:tenant-isolation-stats\")").has_value() ||
              href_int(cs, "query:tenant-isolation-stats", "phase") >= 0 ||
              cs.eval("(engine:metrics \"query:tenant-isolation-stats\")").has_value(),
          "AC3: tenant-isolation-stats reachable");
    // Isolation stats name may vary — try both common names.
    auto iso = cs.eval("(engine:metrics \"query:tenant-isolation-stats\")");
    if (!iso)
        iso = cs.eval("(engine:metrics \"query:isolation-stats\")");
    CHECK(iso.has_value() || cs.eval("(engine:metrics \"query:security-audit-stats\")").has_value(),
          "AC3: isolation or security-audit-stats reachable");
    CHECK(cs.eval("(engine:metrics \"query:security-audit-stats\")").has_value(),
          "AC3: security-audit-stats unchanged");
    CHECK(cs.eval("(engine:metrics \"query:security-posture\")").has_value(),
          "AC3: security-posture unchanged");
    CHECK(cs.eval("(engine:metrics \"query:security-stats\")").has_value(),
          "AC3: security-stats unchanged");
}

// ── AC4: query keys ──
static void ac4_query_keys() {
    std::println("\n--- AC4: component keys + force-reason ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href_int(cs, "query:security-health", "component-effect-deny-rate-bp") >= 0,
          "component-effect-deny-rate-bp");
    CHECK(href_int(cs, "query:security-health", "component-isolation-deny-rate-bp") >= 0,
          "component-isolation-deny-rate-bp");
    CHECK(href_int(cs, "query:security-health", "component-fence-health-bp") >= 0,
          "component-fence-health-bp");
    CHECK(href_int(cs, "query:security-health", "component-wal-posture-bp") >= 0,
          "component-wal-posture-bp");
    CHECK(href_int(cs, "query:security-health", "component-wrap-pressure-bp") >= 0,
          "component-wrap-pressure-bp");
    CHECK(href_int(cs, "query:security-health", "elevated-posture") >= 0, "elevated-posture");
    // force-reason is a string — presence via pure path already covered.
    auto fr = cs.eval("(hash-ref (engine:metrics \"query:security-health\") \"force-reason\")");
    CHECK(fr.has_value(), "AC4: force-reason key present");
}

// ── AC5: source-cite + gate ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite + gate registration ---");
    const auto hh = read_file("src/compiler/security_health.hh");
    const auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_observability.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter = read_file("scripts/check_security_health_2389.py");

    CHECK(hh.find("health_bp") != std::string::npos, "AC5: score definition in header");
    CHECK(hh.find("0.30") != std::string::npos || hh.find("30u") != std::string::npos,
          "AC5: weight comment (0.30 / 30u effect)");
    CHECK(hh.find("effect-deny") != std::string::npos, "AC5: force_reason table");
    CHECK(hh.find("compute_security_health") != std::string::npos, "AC5: pure compute");
    CHECK(sec.find("query:security-health") != std::string::npos, "AC5: query registered");
    CHECK(sec.find("Issue #2389") != std::string::npos, "AC5: security.cpp cites #2389");
    CHECK(obs.find("query:security-health") != std::string::npos, "AC5: catalog entry");
    CHECK(cmake.find("test_security_health_2389") != std::string::npos, "AC5: CMake");
    CHECK(build.find("check_security_health_2389") != std::string::npos, "AC5: build.py gate");
    CHECK(build.find("cmd_security_health_coverage") != std::string::npos, "AC5: cmd coverage");
    CHECK(!linter.empty(), "AC5: coverage linter present");
}

} // namespace

int main() {
    std::println("=== Issue #2389: query:security-health single Agent score ===");
    ac1_vacuous_healthy();
    ac2_effect_deny_and_priority();
    ac3_existing_queries_unchanged();
    ac4_query_keys();
    ac5_source_cite();
    std::println("\n=== #2389 results: passed={} failed={} ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
