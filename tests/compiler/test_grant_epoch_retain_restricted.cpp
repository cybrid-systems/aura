// @category: unit
// @reason: Issue #2529 — Restricted single-tenant grant_epoch_retain K=16.
//
//   AC1: Restricted + no multi-tenant → K==16
//   AC2: multi-tenant or Strict → still 64
//   AC3: AURA_GRANT_EPOCH_RETAIN=N overrides
//   AC4: AURA_SANDBOX=off → K=0 + min_valid=0
//   AC5: Restricted bump epoch >K → old grant provenance_ok false + fence++
//   AC6: source-cite + linter

#include "test_harness.hpp"
#include "compiler/security_defaults.hh"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_epoch.hh"
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;

namespace {
using aura::compiler::security::apply_production_security_defaults;
using aura::core::bump_mutation_epoch;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::kDefaultGrantEpochRetainWindowMultiTenant;
using aura::core::capability::kDefaultGrantEpochRetainWindowRestricted;
using aura::core::capability::kGrantEpochRetainRestrictedIssue;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
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
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_GRANT_EPOCH_RETAIN");
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
} // namespace

int run_test_grant_epoch_retain_restricted() {
    std::println("=== Issue #2529: Restricted grant epoch retain K=16 ===");
    CHECK(kGrantEpochRetainRestrictedIssue == 2529, "issue stamp");
    CHECK(kDefaultGrantEpochRetainWindowRestricted == 16, "K=16 constant");
    CHECK(kDefaultGrantEpochRetainWindowMultiTenant == 64, "K=64 multi-tenant");

    {
        std::println("\n--- AC1: Restricted only → K=16 ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 16, "AC1: K=16");
    }
    {
        std::println("\n--- AC2: multi-tenant / Strict → 64 ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 64, "AC2: multi-tenant 64");
        reset_all();
        set_env("AURA_SANDBOX", "strict");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 64, "AC2: Strict 64");
    }
    {
        std::println("\n--- AC3: env override ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_GRANT_EPOCH_RETAIN", "3");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 3, "AC3: env=3");
    }
    {
        std::println("\n--- AC4: sandbox=off ---");
        reset_all();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 0, "AC4: K=0");
        CHECK(g_capability_registry().grant_min_valid_epoch() == 0, "AC4: min_valid=0");
    }
    {
        std::println("\n--- AC5: fence after >K bumps ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(g_capability_registry().grant_epoch_retain_window() == 16, "K=16");
        EffectProvenance prov = make_grant_provenance(0, true, 0, 0);
        g_capability_registry().sandbox_mode =
            aura::core::capability::EffectSandboxMode::Restricted;
        g_capability_registry().grant(1, "mutate", Effect::Mutate, prov);
        const auto fence0 = g_capability_effect_metrics().capability_epoch_fence_hit_total.load();
        // Advance past window: grant epoch ~1, bump far ahead.
        bump_mutation_epoch(100);
        EffectProvenance call;
        call.mutation_id = prov.mutation_id;
        call.epoch = aura::core::current_mutation_epoch();
        CHECK(!g_capability_registry().provenance_ok(1, call), "AC5: old grant fenced");
        CHECK(g_capability_effect_metrics().capability_epoch_fence_hit_total.load() > fence0,
              "AC5: fence metric++");
    }
    {
        std::println("\n--- AC6: source-cite ---");
        auto def = read_file("src/compiler/security_defaults.hh");
        auto cap = read_file("src/core/capability_model.hh");
        CHECK(def.find("2529") != std::string::npos, "defaults cite #2529");
        CHECK(def.find("kDefaultGrantEpochRetainWindowRestricted") != std::string::npos ||
                  def.find("Restricted") != std::string::npos,
              "Restricted branch");
        CHECK(cap.find("kDefaultGrantEpochRetainWindowRestricted") != std::string::npos, "const");
        CHECK(cap.find("16") != std::string::npos, "16 present");
    }
    std::println("\n=== #2529: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_grant_epoch_retain_restricted();
}
#endif
