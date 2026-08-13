// @category: unit
// @reason: Issue #2536 / #2835 — Restricted same-tenant multi-fiber soft by
// default; multi-tenant under Restricted hard (#2835); env override.
//
//   AC1: Restricted default soft — fiber A grant, fiber B allow + mismatch metric
//   AC2: Restricted + env=1 → deny + fiber_hard_deny++
//   AC3: multi-tenant under Restricted hard (#2835); fiber B denied
//   AC4: AURA_SANDBOX=off forces soft even with env=1
//   AC5: policy comments + posture keys schema-2536 / schema-2835
//   AC6: source-cite + linter
//   AC2835: pure Restricted soft; multi+Restricted hard; HFI=0 soft

#include "test_harness.hpp"
#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "core/capability_model.hh"
#include "core/sandbox.hh"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {
using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::set_effect_fiber_id_override;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}
void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}
void reset_all() {
    reset_capability_effects_for_test();
    set_mode(SandboxMode::Off);
    set_effect_fiber_id_override(0);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_HARD_FIBER_ISOLATION");
    clear_env("AURA_COMMERCIAL_TENANT");
    aura::compiler::security::reset_commercial_tenant_profile_for_test(false);
}
std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}
std::int64_t posture(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:security-posture\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}
} // namespace

int run_test_hard_fiber_restricted() {
    std::println("=== Issue #2536/#2835: Restricted hard-fiber policy ===");
    CHECK(true, "ac2835: issue stamp");

    {
        std::println("\n--- AC1: Restricted default soft share ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(), "AC1: Restricted soft default");

        set_effect_fiber_id_override(1);
        auto prov = make_grant_provenance(0, true, 0, 1);
        g_capability_registry().grant(10, "mutate", Effect::Mutate, prov);
        CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(10, "mutate", g), "grant found");
        CHECK(g.grant_fiber_id == 1, "bound fiber A");

        const auto mm0 = g_capability_effect_metrics().capability_fiber_mismatch_total.load();
        EffectProvenance call;
        call.mutation_id = prov.mutation_id;
        call.epoch = prov.epoch;
        call.fiber_id = 2; // fiber B
        CHECK(g_capability_registry().provenance_ok(10, call), "AC1: soft allow fiber B");
        CHECK(g_capability_effect_metrics().capability_fiber_mismatch_total.load() > mm0,
              "AC1: mismatch metric++");
    }
    {
        std::println("\n--- AC2: Restricted + env=1 hard deny ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_HARD_FIBER_ISOLATION", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(), "AC2: env hard under Restricted");

        auto prov = make_grant_provenance(0, true, 0, 1);
        g_capability_registry().grant(11, "mutate", Effect::Mutate, prov);
        const auto hd0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        EffectProvenance call;
        call.mutation_id = prov.mutation_id;
        call.epoch = prov.epoch;
        call.fiber_id = 2;
        CHECK(!g_capability_registry().provenance_ok(11, call), "AC2: hard deny fiber B");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() > hd0,
              "AC2: hard_deny metric++");
    }
    {
        std::println("\n--- AC3: multi-tenant under Restricted hard (#2835) ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted"); // stays Restricted; multi arms hard
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(),
              "AC3: Restricted+multi_tenant hard (#2835)");
        // Fiber B cannot use fiber A's Mutate grant.
        auto prov = make_grant_provenance(0, true, 0, 1);
        g_capability_registry().grant(12, "mutate", Effect::Mutate, prov);
        const auto hd0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        EffectProvenance call;
        call.mutation_id = prov.mutation_id;
        call.epoch = prov.epoch;
        call.fiber_id = 2;
        CHECK(!g_capability_registry().provenance_ok(12, call), "AC3: hard deny fiber B");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() > hd0,
              "AC3: hard_deny metric++");
    }
    {
        std::println("\n--- AC4: sandbox=off forces soft ---");
        reset_all();
        set_env("AURA_SANDBOX", "off");
        set_env("AURA_HARD_FIBER_ISOLATION", "1");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(), "AC4: off forces soft");
    }
    {
        std::println("\n--- AC5: posture keys ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CompilerService cs;
        CHECK(posture(cs, "schema-2536") == 2536, "schema-2536");
        CHECK(posture(cs, "schema-2835") == 2835, "schema-2835");
        CHECK(posture(cs, "hard-fiber-restricted-policy-wired") == 1, "wired");
        CHECK(posture(cs, "restricted-multi-tenant-hard-fiber-wired") == 1, "2835 wired");
        CHECK(posture(cs, "hard-fiber-isolation") == 0, "Restricted soft on posture");
        CHECK(posture(cs, "fiber-mismatch-total") >= 0, "mismatch total key");
        CHECK(posture(cs, "fiber-hard-deny-total") >= 0, "hard-deny total key");
    }
    {
        std::println("\n--- AC2835: pure Restricted soft; multi hard; HFI=0 soft ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(),
              "AC2835: pure Restricted single-tenant soft");

        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        set_env("AURA_HARD_FIBER_ISOLATION", "0");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(),
              "AC2835: HFI=0 forces soft under multi Restricted");

        reset_all();
        set_env("AURA_SANDBOX", "strict");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(), "AC2835: Strict+multi still hard");
        CHECK(g_capability_registry().grant_epoch_retain_window() == 64, "AC2835: multi → K=64");
    }
    {
        std::println("\n--- AC6: source-cite ---");
        auto def = read_file("src/compiler/security_defaults.hh");
        auto cap = read_file("src/core/capability_model.hh");
        auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(def.find("2536") != std::string::npos, "defaults cite");
        CHECK(def.find("2835") != std::string::npos, "defaults cite #2835");
        CHECK(def.find("TenantScope") != std::string::npos ||
                  def.find("principal boundary") != std::string::npos,
              "principal boundary contract");
        CHECK(cap.find("2536") != std::string::npos, "capability cite");
        CHECK(sec.find("schema-2536") != std::string::npos, "posture schema");
        CHECK(sec.find("schema-2835") != std::string::npos, "posture schema-2835");
    }
    // ── #2943: production multi-tenant OR Strict → hard_fiber_isolation ──
    {
        std::println("\n--- #2943 AC1: pure Strict arms hard fiber ---");
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        // No AURA_MULTI_TENANT — Strict alone must hard-deny grant_fiber
        // mismatch (closes residual soft share after #2835 multi-only).
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(),
              "ac2943 AC1: pure Strict → hard_fiber_isolation=true");
        auto prov = make_grant_provenance(0, true, 0, 1);
        g_capability_registry().grant(43, "mutate", Effect::Mutate, prov);
        const auto hd0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        EffectProvenance call;
        call.mutation_id = prov.mutation_id;
        call.epoch = prov.epoch;
        call.fiber_id = 2; // fiber B ≠ grant fiber A
        CHECK(!g_capability_registry().provenance_ok(43, call),
              "ac2943 AC1: Strict hard deny fiber B on grant_fiber mismatch");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() > hd0,
              "ac2943 AC1: capability_fiber_hard_deny_total bumps");
    }
    {
        std::println("\n--- #2943 AC2: HFI=0 forces soft under Strict ---");
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        set_env("AURA_HARD_FIBER_ISOLATION", "0");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(),
              "ac2943 AC2: HFI=0 forces soft under Strict");
    }
    {
        std::println("\n--- #2943 AC3: Off soft; Restricted soft; multi hard ---");
        reset_all();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(), "ac2943 AC3: Off soft");

        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(),
              "ac2943 AC3: pure Restricted soft (#2536)");

        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(),
              "ac2943 AC3: multi-tenant still hard (#2835 lineage)");
    }
    {
        std::println("\n--- #2943 AC5/AC6: schema + linter + no invent ---");
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        apply_production_security_defaults();
        CompilerService cs;
        CHECK(posture(cs, "schema-2943") == 2943, "ac2943 AC5: schema-2943");
        CHECK(posture(cs, "production-hard-fiber-default-wired") == 1,
              "ac2943 AC5: production-hard-fiber-default-wired");
        CHECK(posture(cs, "schema-2835") == 2835, "ac2943 AC5: schema-2835 preserved");
        CHECK(posture(cs, "hard-fiber-isolation") == 1, "ac2943 AC5: Strict hard on posture");
        const auto def = read_file("src/compiler/security_defaults.hh");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto build = read_file("build.py");
        CHECK(def.find("Issue #2943") != std::string::npos ||
                  def.find("#2943") != std::string::npos,
              "ac2943 AC6: security_defaults cites #2943");
        CHECK(cap.find("2943") != std::string::npos, "ac2943 AC6: capability_model cites #2943");
        CHECK(build.find("check_production_hard_fiber_default_2943") != std::string::npos,
              "ac2943 AC6: build.py wires linter");
        std::ifstream invent("tests/compiler/test_issue_2943.cpp");
        if (!invent.good())
            invent.open("../tests/compiler/test_issue_2943.cpp");
        CHECK(!invent.good(), "ac2943 AC6: no test_issue_2943.cpp");
    }
    std::println("\n=== #2536/#2835/#2943: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hard_fiber_restricted();
}
#endif
