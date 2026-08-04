// @category: unit
// @reason: Issue #2053 — production default Restricted sandbox + stronger
// TypedMutationAudit + optional WAL for multi-tenant AI workloads.
//
//   AC1: apply_production_security_defaults → Restricted (unset AURA_SANDBOX)
//   AC2: un-granted mutate denied under production defaults
//   AC3: TypedMutationAudit Full (production-defaults-active=1, strategy=2)
//   AC4: AURA_MULTI_TENANT escalates to Strict
//   AC5: AURA_SANDBOX=off restores Off + Sampled/4 (dev path)
//   AC6: AURA_MUTATION_AUDIT_WAL enables WAL with single env flag
//   AC7: schema-2053 keys on capability-effect-stats + typed-mutation-audit-stats
//   AC8: Full audit captures every should_audit id (no under-sample)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kProductionSecurityDefaultsIssue;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::get_sample_ratio;
using aura::compiler::typed_audit::get_strategy;
using aura::compiler::typed_audit::production_defaults_active;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::should_audit;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::core::audit_wal::g_mutation_audit_wal;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::g_sandbox_state;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href_cap(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_aud(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

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

void reset_process() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_audit_wal_for_test();
    reset_for_test(); // typed audit → Sampled/4
    set_mode(SandboxMode::Off);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_TYPED_AUDIT");
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_PERSIST_DIR");
}

} // namespace

int run_test_production_security_defaults_2053() {
    std::println("=== Issue #2053: production security defaults ===");
    CHECK(kProductionSecurityDefaultsIssue == 2053, "issue stamp");

    // ── AC1+AC3: production defaults Restricted + Full audit ──
    {
        std::println("\n--- AC1/AC3: Restricted + Full audit ---");
        reset_process();
        apply_production_security_defaults();
        CHECK(static_cast<std::uint8_t>(g_sandbox_state().mode) == 1, "sandbox Restricted");
        CHECK(static_cast<std::uint8_t>(g_capability_registry().sandbox_mode.load()) == 1,
              "effect sandbox Restricted");
        CHECK(get_strategy() == AuditStrategy::Full, "audit strategy Full");
        CHECK(get_sample_ratio() == 1, "sample ratio 1 under Full");
        CHECK(production_defaults_active(), "production_defaults_active");
    }

    // ── AC2: un-granted mutate denied ──
    {
        std::println("\n--- AC2: un-granted mutate denied ---");
        reset_process();
        apply_production_security_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Mirror process mode onto this Evaluator instance.
        ev.set_effect_sandbox_mode(
            static_cast<std::uint8_t>(g_capability_registry().sandbox_mode.load()));
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto r = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 9))\" \"prod-deny\")");
        // Deny is make_merr (Error) or legacy pair; success is a non-error value.
        const bool denied = !r || is_error(*r) || is_pair(*r);
        CHECK(denied, "Restricted without grant denies set-body");
        auto v = cs.eval("(f 1)");
        if (v && is_int(*v))
            CHECK(as_int(*v) == 2, "body unchanged after deny");
    }

    // ── AC4: multi-tenant → Strict ──
    {
        std::println("\n--- AC4: AURA_MULTI_TENANT → Strict ---");
        reset_process();
        set_env("AURA_MULTI_TENANT", "1");
        apply_production_security_defaults();
        CHECK(static_cast<std::uint8_t>(g_sandbox_state().mode) == 2, "Strict under multi-tenant");
        CHECK(static_cast<std::uint8_t>(g_capability_registry().sandbox_mode.load()) == 2,
              "effect Strict");
        clear_env("AURA_MULTI_TENANT");
    }

    // ── AC5: dev Off path ──
    {
        std::println("\n--- AC5: AURA_SANDBOX=off → Off + Sampled ---");
        reset_process();
        set_env("AURA_SANDBOX", "off");
        apply_production_security_defaults();
        CHECK(static_cast<std::uint8_t>(g_sandbox_state().mode) == 0, "sandbox Off");
        CHECK(get_strategy() == AuditStrategy::Sampled, "dev Sampled");
        CHECK(get_sample_ratio() == 4, "dev ratio 4");
        CHECK(!production_defaults_active(), "production inactive under off");
        clear_env("AURA_SANDBOX");
        // Explicit Off still allows mutate without grant
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        CHECK(cs.eval("(set-code \"(define (g x) x)\")").has_value(), "set-code g");
        CHECK(cs.eval("(eval-current)").has_value(), "eval g");
        auto r = cs.eval("(mutate:set-body \"g\" \"(lambda (x) (+ x 1))\" \"dev\")");
        CHECK(r.has_value(), "dev Off mutate returns");
    }

    // ── AC6: WAL env enable ──
    {
        std::println("\n--- AC6: AURA_MUTATION_AUDIT_WAL enable ---");
        reset_process();
        namespace fs = std::filesystem;
        auto dir = fs::temp_directory_path() / "aura-2053-wal";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        set_env("AURA_MUTATION_AUDIT_WAL", dir.string().c_str());
        apply_production_security_defaults();
        CHECK(g_mutation_audit_wal().is_enabled(), "WAL enabled via env");
        clear_env("AURA_MUTATION_AUDIT_WAL");
        reset_audit_wal_for_test();
        fs::remove_all(dir, ec);
    }

    // ── AC7: query schema-2053 ──
    {
        std::println("\n--- AC7: schema-2053 query keys ---");
        reset_process();
        apply_production_security_defaults();
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(
            static_cast<std::uint8_t>(g_capability_registry().sandbox_mode.load()));
        auto h = cs.eval("(engine:metrics \"query:capability-effect-stats\")");
        CHECK(h && is_hash(*h), "capability-effect-stats hash");
        CHECK(href_cap(cs, "schema-2053") == 2053, "cap schema-2053");
        CHECK(href_cap(cs, "issue-2053") == 2053, "cap issue-2053");
        CHECK(href_cap(cs, "production-security-wired") == 1, "production-security-wired");
        CHECK(href_cap(cs, "production-defaults-active") == 1, "cap production-defaults-active");
        CHECK(href_cap(cs, "typed-audit-strategy") == 2, "typed-audit-strategy Full=2");
        CHECK(href_cap(cs, "process-sandbox-mode") == 1, "process-sandbox-mode Restricted");
        auto ha = cs.eval("(engine:metrics \"query:typed-mutation-audit-stats\")");
        CHECK(ha && is_hash(*ha), "typed-mutation-audit-stats hash");
        CHECK(href_aud(cs, "schema-2053") == 2053, "audit schema-2053");
        CHECK(href_aud(cs, "strategy") == 2, "audit strategy Full");
        CHECK(href_aud(cs, "production-defaults-active") == 1, "audit production active");
    }

    // ── AC8: Full captures every id ──
    {
        std::println("\n--- AC8: Full captures every mutation id ---");
        reset_process();
        apply_production_audit_defaults();
        CHECK(get_strategy() == AuditStrategy::Full, "Full");
        int hits = 0;
        for (std::uint64_t id = 1; id <= 20; ++id) {
            if (should_audit(id))
                ++hits;
        }
        CHECK(hits == 20, std::format("Full audits all 20 ids (got {})", hits));
        // Dev Sampled/4 under-samples
        apply_dev_audit_defaults();
        int hits_dev = 0;
        for (std::uint64_t id = 1; id <= 20; ++id) {
            if (should_audit(id))
                ++hits_dev;
        }
        CHECK(hits_dev < 20 && hits_dev >= 1,
              std::format("Sampled under-samples (got {})", hits_dev));
    }

    // Leave process clean for any subsequent tests in the same binary.
    reset_process();

    std::println("\n#2053 production security defaults: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_production_security_defaults_2053();
}
#endif
