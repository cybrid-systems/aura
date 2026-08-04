// @category: unit
// @reason: Issue #2584 — AURA_COMMERCIAL_TENANT config profile (Restricted
// default hard-fiber isolation for commercial deployments; does not change
// today's defaults).
//
//   AC1: No AURA_COMMERCIAL_TENANT — Restricted default soft (#2536 regression)
//   AC2: AURA_COMMERCIAL_TENANT=1 + Restricted → hard deny + fiber_hard_deny++
//   AC3: AURA_COMMERCIAL_TENANT=1 + AURA_HARD_FIBER_ISOLATION=0 → soft (env override)
//   AC4: multi-tenant + Strict default hard unchanged (#2151 regression)
//   AC5: AURA_SANDBOX=off forces soft (unit Soft ergonomics)
//   AC6: query:security-posture exposes commercial-tenant-profile + source-cite

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
using aura::compiler::security::is_commercial_tenant_profile;
using aura::compiler::security::reset_commercial_tenant_profile_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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
    g_capability_registry().set_hard_fiber_isolation(false);
    reset_commercial_tenant_profile_for_test(false);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_HARD_FIBER_ISOLATION");
    clear_env("AURA_COMMERCIAL_TENANT");
}

std::int64_t posture(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:security-posture\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
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

int run_test_commercial_tenant_profile() {
    std::println("=== Issue #2584: AURA_COMMERCIAL_TENANT config profile ===");

    // ── AC1: No AURA_COMMERCIAL_TENANT — Restricted default soft (#2536) ──
    {
        std::println("\n--- AC1: Restricted default soft (#2536 regression) ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(), "AC1: Restricted soft default");
        CHECK(!is_commercial_tenant_profile(), "AC1: commercial profile off");

        // Fiber A holds a Mutate grant on tenant 10; fiber B (same tenant,
        // different fiber id) is allowed (soft share) and bumps mismatch metric.
        set_effect_fiber_id_override(1);
        auto prov = make_grant_provenance(/*mutation_id=*/0, /*force_bind=*/true,
                                          /*node_id=*/0, /*fiber_id=*/1);
        g_capability_registry().grant(/*tenant=*/10, "mutate", Effect::Mutate, prov);
        const auto m0 = g_capability_effect_metrics().capability_fiber_mismatch_total.load();
        const auto d0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        set_effect_fiber_id_override(2);
        EffectProvenance caller{};
        caller.fiber_id = 2;
        const bool ok = g_capability_registry().provenance_ok(/*tenant=*/10, caller);
        CHECK(ok, "AC1: fiber B same tenant soft allow");
        CHECK(g_capability_effect_metrics().capability_fiber_mismatch_total.load() > m0,
              "AC1: mismatch metric advanced");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() == d0,
              "AC1: hard_deny metric not advanced under soft");
    }

    // ── AC2: AURA_COMMERCIAL_TENANT=1 + Restricted → hard deny ──
    {
        std::println("\n--- AC2: commercial profile forces hard under Restricted ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_COMMERCIAL_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(), "AC2: commercial profile sets hard");
        CHECK(is_commercial_tenant_profile(), "AC2: profile flag on (cached)");

        set_effect_fiber_id_override(1);
        auto prov = make_grant_provenance(0, true, 0, 1);
        g_capability_registry().grant(10, "mutate", Effect::Mutate, prov);
        const auto m0 = g_capability_effect_metrics().capability_fiber_mismatch_total.load();
        const auto d0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        set_effect_fiber_id_override(2);
        EffectProvenance caller{};
        caller.fiber_id = 2;
        const bool ok = g_capability_registry().provenance_ok(10, caller);
        CHECK(!ok, "AC2: fiber B same tenant hard deny");
        CHECK(g_capability_effect_metrics().capability_fiber_mismatch_total.load() > m0,
              "AC2: mismatch metric still advanced");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() > d0,
              "AC2: hard_deny metric advanced");
    }

    // ── AC3: AURA_COMMERCIAL_TENANT=1 + AURA_HARD_FIBER_ISOLATION=0 → soft ──
    {
        std::println("\n--- AC3: explicit off overrides commercial profile ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_COMMERCIAL_TENANT", "1");
        set_env("AURA_HARD_FIBER_ISOLATION", "0");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(),
              "AC3: AURA_HARD_FIBER_ISOLATION=0 wins over commercial");
        CHECK(is_commercial_tenant_profile(),
              "AC3: commercial profile flag still on (env-only override, not a parse failure)");

        // Fiber B must be allowed under soft despite commercial flag set.
        set_effect_fiber_id_override(1);
        auto prov = make_grant_provenance(0, true, 0, 1);
        g_capability_registry().grant(10, "mutate", Effect::Mutate, prov);
        const auto d0 = g_capability_effect_metrics().capability_fiber_hard_deny_total.load();
        set_effect_fiber_id_override(2);
        EffectProvenance caller{};
        caller.fiber_id = 2;
        const bool ok = g_capability_registry().provenance_ok(10, caller);
        CHECK(ok, "AC3: fiber B allowed under soft override");
        CHECK(g_capability_effect_metrics().capability_fiber_hard_deny_total.load() == d0,
              "AC3: hard_deny metric not advanced under soft");
    }

    // ── AC4: multi-tenant + Strict default hard unchanged ──
    {
        std::println("\n--- AC4: multi-tenant+Strict default hard (#2151) ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(g_capability_registry().hard_fiber_isolation(),
              "AC4: multi-tenant+Strict hard default unchanged");
        CHECK(!is_commercial_tenant_profile(),
              "AC4: commercial profile off (env unset, no #2584 effect)");
    }

    // ── AC5: AURA_SANDBOX=off forces soft (unit Soft path) ──
    {
        std::println("\n--- AC5: dev_off forces soft ---");
        reset_all();
        set_env("AURA_SANDBOX", "off");
        set_env("AURA_COMMERCIAL_TENANT", "1");
        apply_production_security_defaults();
        CHECK(!g_capability_registry().hard_fiber_isolation(), "AC5: dev_off forces soft");
        CHECK(!is_commercial_tenant_profile(),
              "AC5: commercial profile off under dev_off (unit Soft)");
    }

    // ── AC6: posture exposes profile + source-cite ──
    {
        std::println("\n--- AC6: query:security-posture exposes commercial-tenant-profile ---");
        reset_all();
        set_env("AURA_SANDBOX", "restricted");
        set_env("AURA_COMMERCIAL_TENANT", "1");
        apply_production_security_defaults();
        CHECK(is_commercial_tenant_profile(), "AC6: profile flag cached on");

        // Live posture query — the new key is wired into the snapshot.
        CompilerService cs;
        const auto ct_flag = posture(cs, "commercial-tenant-profile");
        CHECK(ct_flag == 1, "AC6: query:security-posture returns commercial-tenant-profile=1");
        const auto hfi = posture(cs, "hard-fiber-isolation");
        CHECK(hfi == 1, "AC6: hard-fiber-isolation also surfaced under commercial");

        // Source-cite: security_defaults.hh step 6 documents the profile.
        const auto doc = read_file("src/compiler/security_defaults.hh");
        CHECK(doc.find("2584") != std::string::npos, "AC6: security_defaults.hh cites #2584");
        const auto posture_src = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture_src.find("commercial-tenant-profile") != std::string::npos,
              "AC6: posture key wired in evaluator_primitives_security.cpp");

        // Live security gate clean.
        const int rc = std::system("python3 scripts/coverage/checks/check_side_effect_security.py "
                                   "--strict >/dev/null 2>&1");
        const int rc2 =
            std::system("cd .. 2>/dev/null; "
                        "python3 scripts/coverage/checks/check_side_effect_security.py --strict "
                        ">/dev/null 2>&1");
        CHECK(rc == 0 || rc2 == 0, "AC6: live security gate passes");
    }

    std::println("\n=== #2584 commercial_tenant_profile: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_commercial_tenant_profile();
}
#endif
