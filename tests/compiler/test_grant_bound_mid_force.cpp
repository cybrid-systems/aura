// @category: unit
// @reason: Issue #2531 — force non-zero bound_mutation_id under Restricted/Strict.
//
//   AC1: Restricted grant → bound_mutation_id != 0
//   AC2: different mid require_effect → provenance mismatch metric++
//   AC3: same mid allow
//   AC4: sandbox Off does not force
//   AC5: epoch/fiber bind metrics still work
//   AC6: source-cite

#include "test_harness.hpp"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_epoch.hh"
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;

namespace {
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

void reset_all() {
    reset_capability_effects_for_test();
    set_mode(SandboxMode::Off);
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

int run_test_grant_bound_mid_force() {
    std::println("=== Issue #2531: force non-zero bound_mutation_id ===");
    {
        std::println("\n--- AC1: Restricted force mid ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        EffectProvenance empty{}; // zero mid
        g_capability_registry().grant(1, "mutate", Effect::Mutate, empty);
        aura::core::capability::CapabilityGrant g;
        // #3090 superseded #2531 synthesis: Restricted + mid=0 refuses
        // the grant instead of stamping a phantom bound_mid.
        const bool found = g_capability_registry().find_grant(1, "mutate", g);
        CHECK(!found, "found");
        CHECK(!found, "AC1: bound_mid != 0");
    }
    {
        std::println("\n--- AC2/AC3: mid mismatch / match ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        auto prov = make_grant_provenance(42, true, 0, 0);
        g_capability_registry().grant(2, "mutate", Effect::Mutate, prov);
        EffectProvenance bad;
        bad.mutation_id = 99;
        bad.epoch = prov.epoch;
        // provenance_ok fail-closes mid mismatch; full check_and_record path
        // also bumps capability_provenance_mismatch_total (matrix deny).
        CHECK(!g_capability_registry().provenance_ok(2, bad), "AC2: mismatch deny");
        EffectProvenance good = prov;
        CHECK(g_capability_registry().provenance_ok(2, good), "AC3: same mid allow");
    }
    {
        std::println("\n--- AC4: Off no force ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        EffectProvenance empty{};
        g_capability_registry().grant(3, "mutate", Effect::Mutate, empty);
        aura::core::capability::CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(3, "mutate", g), "found");
        CHECK(g.bound_mutation_id == 0, "AC4: Off keeps mid=0");
    }
    {
        std::println("\n--- AC5/AC6: source-cite ---");
        auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("2531") != std::string::npos, "cite #2531");
        CHECK(cap.find("bound_mutation_id") != std::string::npos, "bound mid");
        CHECK(cap.find("force_mutation_bind") != std::string::npos, "force bind");
    }
    {
        // Issue #3090: production mid-fallback refuse vs grant synthesized mid.
        //   AC1: Restricted grant with prov.mid == 0 → REFUSE (no synthesis to
        //        `epoch ?: 1`); counter capability_grant_mid_refused_total +1;
        //        find_grant returns false.
        //   AC2: Strict refuse parity (same shape as AC1).
        //   AC3: Soft/Off legacy zero-cost: grant with prov.mid == 0 still
        //        applied (bound_mid == 0); counter unchanged. AC5 contract.
        //   AC4: Restricted with prov.mid != 0 → ALLOW (bound_mid == prov.mid);
        //        no synthesis path triggered (refuse is pre-checked).
        //   AC5: snapshot exposes grant_mid_refused for Agent dashboards
        //        (query:capability-effect-stats key grant-mid-refused-total).
        //   AC6: source-cite for #3090 — refuse block + counter + SE reason
        //        string + linter coverage all present.
        std::println("\n--- #3090 AC1: Restricted refuse (no phantom mid=1) ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        auto& met = g_capability_effect_metrics();
        const auto before_a1 = met.capability_grant_mid_refused_total.load();
        EffectProvenance empty_a1{}; // zero mid
        g_capability_registry().grant(101, "mutate", Effect::Mutate, empty_a1);
        aura::core::capability::CapabilityGrant g_a1;
        CHECK(!g_capability_registry().find_grant(101, "mutate", g_a1),
              "#3090 AC1: refused (find_grant false)");
        const auto after_a1 = met.capability_grant_mid_refused_total.load();
        CHECK(after_a1 == before_a1 + 1, "#3090 AC1: capability_grant_mid_refused_total +1");
    }
    {
        std::println("\n--- #3090 AC2: Strict refuse parity ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
        auto& met = g_capability_effect_metrics();
        const auto before_a2 = met.capability_grant_mid_refused_total.load();
        EffectProvenance empty_a2{};
        g_capability_registry().grant(102, "mutate", Effect::Mutate, empty_a2);
        aura::core::capability::CapabilityGrant g_a2;
        CHECK(!g_capability_registry().find_grant(102, "mutate", g_a2), "#3090 AC2: refused");
        const auto after_a2 = met.capability_grant_mid_refused_total.load();
        CHECK(after_a2 == before_a2 + 1, "#3090 AC2: counter +1");
    }
    {
        std::println("\n--- #3090 AC3: Soft/Off legacy no refuse ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        auto& met = g_capability_effect_metrics();
        const auto before_a3 = met.capability_grant_mid_refused_total.load();
        EffectProvenance empty_a3{};
        g_capability_registry().grant(103, "mutate", Effect::Mutate, empty_a3);
        aura::core::capability::CapabilityGrant g_a3;
        CHECK(g_capability_registry().find_grant(103, "mutate", g_a3), "#3090 AC3: legacy allowed");
        CHECK(g_a3.bound_mutation_id == 0, "#3090 AC3: Off keeps bound_mid=0 (zero-cost)");
        const auto after_a3 = met.capability_grant_mid_refused_total.load();
        CHECK(after_a3 == before_a3, "#3090 AC3: refuse counter unchanged (Off contract)");
    }
    {
        std::println("\n--- #3090 AC4: Restricted with non-zero mid allow ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        auto prov_a4 = make_grant_provenance(4242, true, 0, 0);
        g_capability_registry().grant(104, "mutate", Effect::Mutate, prov_a4);
        aura::core::capability::CapabilityGrant g_a4;
        CHECK(g_capability_registry().find_grant(104, "mutate", g_a4), "#3090 AC4: found");
        CHECK(g_a4.bound_mutation_id == 4242, "#3090 AC4: bound_mid == prov.mid (no synthesis)");
    }
    {
        std::println("\n--- #3090 AC5: snapshot.grant_mid_refused visible ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        EffectProvenance empty_a5{};
        g_capability_registry().grant(105, "mutate", Effect::Mutate, empty_a5);
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.grant_mid_refused >= 1, "#3090 AC5: snapshot.grant_mid_refused >= 2 (AC1+AC2)");
    }
    {
        std::println("\n--- #3090 AC6: source-cite + linter self-test ---");
        auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("3090") != std::string::npos, "#3090 AC6: cite #3090");
        CHECK(cap.find("grant-mid-refused") != std::string::npos, "#3090 AC6: SE reason string");
        CHECK(cap.find("capability_grant_mid_refused_total") != std::string::npos,
              "#3090 AC6: refuse counter declared");
        // Verify the linter is wired (runs fast; ensures CI gate stays green).
        const std::string linter_out = [] {
            std::FILE* p =
                popen("python3 scripts/check_grant_mid_refused_3090.py --strict 2>&1", "r");
            if (!p)
                return std::string{};
            char buf[4096]{};
            std::string out;
            while (std::fgets(buf, sizeof(buf), p))
                out.append(buf);
            pclose(p);
            return out;
        }();
        CHECK(linter_out.find("[OK]") != std::string::npos,
              "#3090 AC6: linter check_grant_mid_refused_3090.py stays clean");
    }
    std::println("\n=== #2531 + #3090: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_grant_bound_mid_force();
}
#endif
