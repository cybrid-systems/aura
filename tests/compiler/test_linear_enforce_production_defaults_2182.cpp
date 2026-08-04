// @category: unit
// @reason: Issue #2182 — production defaults Strict LinearEnforceMode
// (refine #2103 + #2053). Soft under AURA_SANDBOX=off; optional
// AURA_LINEAR_ENFORCE=soft|strict canary override.
//
//   AC1: Production defaults → Strict; incomplete trail → hard error
//   AC2: Dev / AURA_SANDBOX=off → Soft; metric-only incomplete trail
//   AC3: #2103 Soft/Strict unit semantics preserved; defaults integration
//   AC4: Query surfaces mode + hard-fail totals (schema-2182)
//   AC5: src-aligned tests under tests/compiler/

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kLinearEnforceProductionDefaultsIssue;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::provenance::g_linear_soft_incomplete_continue_total;
using aura::core::provenance::g_linear_strict_hard_fail_total;
using aura::core::provenance::g_provenance_enforcement;
using aura::core::provenance::kLinearOwned;
using aura::core::provenance::linear_enforce_mode;
using aura::core::provenance::linear_enforce_require_complete;
using aura::core::provenance::LinearEnforceMode;
using aura::core::provenance::reset_linear_enforce_mode_for_test;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::restore_linear_enforce_production_default_for_test;
using aura::core::provenance::set_linear_enforce_mode;
using aura::core::provenance::validate_linear_provenance;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
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

static void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static void reset_process() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_audit_wal_for_test();
    reset_for_test(); // typed audit → Sampled/4
    reset_provenance_enforcement_for_test();
    reset_linear_enforce_mode_for_test(); // Soft
    set_mode(SandboxMode::Off);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_TYPED_AUDIT");
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_PERSIST_DIR");
    clear_env("AURA_LINEAR_ENFORCE");
    clear_env("AURA_HARD_FIBER_ISOLATION");
    clear_env("AURA_GRANT_EPOCH_RETAIN");
}

static std::int64_t href_ps(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:post-steal-closed-loop-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_enf(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-ownership-enforcement-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_linear_enforce_production_defaults_2182() {
    std::println("=== Issue #2182: production LinearEnforceMode Strict defaults ===");
    CHECK(kLinearEnforceProductionDefaultsIssue == 2182, "issue stamp");

    // ── AC1: production → Strict; incomplete trail hard-fails ──
    {
        std::println("\n--- AC1: production defaults → Strict + hard fail ---");
        reset_process();
        apply_production_security_defaults();
        CHECK(linear_enforce_mode() == LinearEnforceMode::Strict, "AC1: mode Strict");
        CHECK(linear_enforce_require_complete(), "AC1: require_complete");

        const auto hard0 = g_linear_strict_hard_fail_total.load();
        auto r = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0,
                                            linear_enforce_require_complete());
        CHECK(!r.ok, "AC1: incomplete → !ok");
        CHECK(r.force_deopt, "AC1: force_deopt");
        CHECK(g_linear_strict_hard_fail_total.load() > hard0, "AC1: hard-fail total");

        // Complete trail still succeeds under production Strict.
        auto r2 = validate_linear_provenance(kLinearOwned, 1, /*prov=*/42, /*mut=*/99, 0, 0, 0, 0,
                                             linear_enforce_require_complete());
        CHECK(r2.ok, "AC1: complete trail ok under Strict");
    }

    // ── AC2: AURA_SANDBOX=off → Soft ──
    {
        std::println("\n--- AC2: AURA_SANDBOX=off → Soft ---");
        reset_process();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(linear_enforce_mode() == LinearEnforceMode::Soft, "AC2: Soft under sandbox=off");
        CHECK(!linear_enforce_require_complete(), "AC2: require_complete false");

        const auto soft0 = g_linear_soft_incomplete_continue_total.load();
        const auto hard0 = g_linear_strict_hard_fail_total.load();
        const auto inc0 = g_provenance_enforcement().linear_provenance_incomplete_total.load();
        auto r = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0,
                                            /*require_complete=*/false);
        CHECK(r.ok, "AC2: Soft incomplete continues");
        CHECK(g_provenance_enforcement().linear_provenance_incomplete_total.load() > inc0,
              "AC2: incomplete metric");
        CHECK(g_linear_soft_incomplete_continue_total.load() > soft0, "AC2: soft continue");
        CHECK(g_linear_strict_hard_fail_total.load() == hard0, "AC2: no hard-fail under Soft");
        clear_env("AURA_SANDBOX");
    }

    // ── AC3: AURA_LINEAR_ENFORCE override + #2103 Soft/Strict preserved ──
    {
        std::println("\n--- AC3: env override + Soft opt-in lineage ---");
        // Issue #2207: process-native default is Strict. Soft is explicit
        // opt-in (test harness reset / set_linear_enforce_mode / env).
        // Before reset, production atomics start Strict; reset_process
        // forces Soft for Soft-path unit ergonomics.
        restore_linear_enforce_production_default_for_test();
        CHECK(linear_enforce_mode() == LinearEnforceMode::Strict,
              "AC3: process default Strict (#2207)");
        reset_process();
        CHECK(linear_enforce_mode() == LinearEnforceMode::Soft,
              "AC3: Soft after test harness reset (opt-in)");

        // Production + canary Soft override.
        set_env("AURA_LINEAR_ENFORCE", "soft");
        apply_production_security_defaults();
        CHECK(linear_enforce_mode() == LinearEnforceMode::Soft,
              "AC3: AURA_LINEAR_ENFORCE=soft overrides prod Strict");
        clear_env("AURA_LINEAR_ENFORCE");

        // sandbox=off + force Strict canary.
        reset_process();
        set_env("AURA_SANDBOX", "off");
        set_env("AURA_LINEAR_ENFORCE", "strict");
        apply_production_security_defaults();
        CHECK(linear_enforce_mode() == LinearEnforceMode::Strict,
              "AC3: AURA_LINEAR_ENFORCE=strict under sandbox=off");
        clear_env("AURA_SANDBOX");
        clear_env("AURA_LINEAR_ENFORCE");

        // Manual Soft/Strict still works (#2103 unit semantics).
        reset_process();
        set_linear_enforce_mode(LinearEnforceMode::Strict);
        auto r = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0, true);
        CHECK(!r.ok, "AC3: manual Strict hard-fail");
        set_linear_enforce_mode(LinearEnforceMode::Soft);
        auto r2 = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0, false);
        CHECK(r2.ok, "AC3: manual Soft continue");
    }

    // ── AC4: query surfaces schema-2182 ──
    {
        std::println("\n--- AC4: query schema-2182 + mode keys ---");
        reset_process();
        apply_production_security_defaults();
        CompilerService cs;
        CHECK(href_ps(cs, "schema-2182") == 2182, "post-steal schema-2182");
        CHECK(href_ps(cs, "issue-2182") == 2182, "post-steal issue-2182");
        CHECK(href_ps(cs, "linear-enforce-mode") == 1, "post-steal mode Strict");
        CHECK(href_ps(cs, "linear-enforce-strict") == 1, "post-steal strict flag");
        CHECK(href_ps(cs, "production-defaults-linear-strict") == 1, "prod-defaults-strict");
        CHECK(href_ps(cs, "linear-strict-hard-fail-total") >= 0, "hard-fail key");
        CHECK(href_ps(cs, "schema-2103") == 2103, "2103 lineage retained");

        CHECK(href_enf(cs, "schema-2182") == 2182, "enforcement-stats schema-2182");
        CHECK(href_enf(cs, "linear-enforce-mode") == 1, "enforcement mode Strict");
        CHECK(href_enf(cs, "linear-enforce-strict") == 1, "enforcement strict");
        CHECK(href_enf(cs, "linear-strict-hard-fail-total") >= 0, "enforcement hard-fail");

        // Soft path after sandbox=off.
        reset_process();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CompilerService cs2;
        CHECK(href_ps(cs2, "linear-enforce-mode") == 0, "query Soft mode under sandbox=off");
        CHECK(href_ps(cs2, "production-defaults-linear-strict") == 0, "prod-strict flag 0");
        clear_env("AURA_SANDBOX");
    }

    // ── AC5: source wiring ──
    {
        std::println("\n--- AC5: source wiring ---");
        const auto sd = read_file("src/compiler/security_defaults.hh");
        const auto pe = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        const auto ps = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(!sd.empty() && sd.find("2182") != std::string::npos, "security_defaults cites 2182");
        CHECK(sd.find("set_linear_enforce_mode") != std::string::npos, "defaults set mode");
        CHECK(sd.find("AURA_LINEAR_ENFORCE") != std::string::npos, "env override");
        CHECK(!pe.empty() && pe.find("schema-2182") != std::string::npos, "post-steal schema");
        CHECK(!ps.empty() && ps.find("schema-2182") != std::string::npos, "enforcement schema");
    }

    // Restore Soft for any subsequent process consumers.
    reset_process();
    apply_dev_audit_defaults();

    std::println("\n=== #2182 production linear enforce defaults: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_enforce_production_defaults_2182();
}
#endif
