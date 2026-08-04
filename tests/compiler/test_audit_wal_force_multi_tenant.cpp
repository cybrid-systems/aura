// @category: unit
// @reason: Issue #2150 — Force mutation-audit WAL under multi-tenant / Strict
// production defaults (durable forensic trail without env tribal knowledge).
//
//   AC1: AURA_MULTI_TENANT=1 without WAL env → enabled + forced metric > 0
//   AC2: AURA_SANDBOX=off → WAL remains off (no spill in unit tests)
//   AC3: After disable/re-enable same dir, SecurityEvent ring has prior allow/deny
//   AC4: Explicit AURA_MUTATION_AUDIT_WAL wins over default dir
//   AC5: kFlushEvery batching unchanged (still 32); schema-2150 Agent keys

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "core/capability_model.hh"
#include "core/mutation_audit_wal.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::audit_wal::g_mutation_audit_wal;
using aura::core::audit_wal::kAuditWalForceMultiTenantIssue;
using aura::core::audit_wal::MutationAuditWal;
using aura::core::audit_wal::reset_audit_wal_for_test;
using aura::core::audit_wal::snapshot_audit_wal_stats;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::reset_security_event_ring_for_test;
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

void reset_process() {
    reset_capability_effects_for_test();
    reset_audit_wal_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
    clear_env("AURA_TYPED_AUDIT");
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_PERSIST_DIR");
}

static std::int64_t href_wal(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:audit-wal-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_cap(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: multi-tenant forces WAL without path env ────────────
static void ac1_multi_tenant_forces_wal() {
    std::println("\n--- AC1: AURA_MULTI_TENANT=1 forces WAL ---");
    reset_process();
    set_env("AURA_MULTI_TENANT", "1");
    // Ensure no explicit path.
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_PERSIST_DIR");
    apply_production_security_defaults();
    const auto snap = snapshot_audit_wal_stats();
    std::println("  enabled={} forced={} default_dir={}", snap.enabled, snap.forced_by_multi_tenant,
                 snap.using_default_dir);
    CHECK(snap.enabled == 1, "AC1: snapshot enabled == 1");
    CHECK(g_mutation_audit_wal().is_enabled(), "AC1: WAL is_enabled");
    CHECK(snap.forced_by_multi_tenant > 0, "AC1: forced metric > 0");
    CHECK(snap.using_default_dir == 1, "AC1: using default dir");
    CHECK(!g_mutation_audit_wal().directory().empty(), "AC1: directory non-empty");
    clear_env("AURA_MULTI_TENANT");
}

// ── AC2: sandbox off → no WAL ────────────────────────────────
static void ac2_sandbox_off_no_wal() {
    std::println("\n--- AC2: AURA_SANDBOX=off → WAL off ---");
    reset_process();
    set_env("AURA_SANDBOX", "off");
    set_env("AURA_MULTI_TENANT", "1"); // must not force under off
    apply_production_security_defaults();
    CHECK(!g_mutation_audit_wal().is_enabled(), "AC2: WAL remains off under sandbox=off");
    CHECK(snapshot_audit_wal_stats().enabled == 0, "AC2: enabled==0");
    CHECK(snapshot_audit_wal_stats().forced_by_multi_tenant == 0, "AC2: not forced");
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
}

// ── AC3: restart / re-enable replays SecurityEvent trail ─────
static void ac3_replay_security_events() {
    std::println("\n--- AC3: WAL replay restores SecurityEvent allow/deny ---");
    reset_process();
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "aura-2150-replay";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // Process A: enable + emit effect allow/deny into WAL via Evaluator.
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.enable_mutation_audit_wal(dir.string()), "enable WAL dir");
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(77);
        // Deny un-granted mutate.
        CHECK(!ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac3-deny", 0, 77, 11),
              "deny without grant");
        // Grant + allow.
        ev.grant_effect_capability(77, "m", kEffectMutate, 22);
        CHECK(ev.check_and_record_effect(kEffectMutate, kEffectMutate, "ac3-allow", 0, 77, 22),
              "allow with grant");
        CHECK(g_mutation_audit_wal().is_enabled(), "WAL still on");
        // Flush by disabling (close + fflush).
        g_mutation_audit_wal().disable();
    }

    // Simulate crash: clear in-memory rings.
    reset_security_event_ring_for_test();
    CHECK(g_security_event_ring().total.load() == 0, "ring cleared");

    // Process B: re-enable same dir → replay rebuilds SecurityEvent ring.
    {
        CompilerService cs2;
        auto& ev2 = cs2.evaluator();
        CHECK(ev2.enable_mutation_audit_wal(dir.string()), "re-enable WAL replay");
        const auto total = g_security_event_ring().total.load(std::memory_order_relaxed);
        std::println("  SecurityEvent total after replay={}", total);
        CHECK(total >= 2, "AC3: prior allow+deny present after replay");
        bool saw_deny = false;
        bool saw_allow = false;
        const auto seq = g_security_event_ring().seq.load(std::memory_order_relaxed);
        const std::size_t n = std::min<std::size_t>(seq, g_security_event_ring().ring.size());
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = g_security_event_ring().ring[i];
            if (e.denied)
                saw_deny = true;
            else
                saw_allow = true;
        }
        CHECK(saw_deny, "AC3: deny record replayed");
        CHECK(saw_allow, "AC3: allow record replayed");
    }
    fs::remove_all(dir, ec);
}

// ── AC4: explicit path wins over default ─────────────────────
static void ac4_explicit_path_wins() {
    std::println("\n--- AC4: explicit AURA_MUTATION_AUDIT_WAL wins ---");
    reset_process();
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "aura-2150-explicit";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    set_env("AURA_MULTI_TENANT", "1");
    set_env("AURA_MUTATION_AUDIT_WAL", dir.string().c_str());
    apply_production_security_defaults();
    CHECK(g_mutation_audit_wal().is_enabled(), "WAL enabled");
    const auto used = g_mutation_audit_wal().directory();
    std::println("  dir={}", used);
    CHECK(used.find("aura-2150-explicit") != std::string::npos, "AC4: explicit path used");
    // Forced metric is only for default-dir force (not when explicit path set).
    // enabled still true; using_default_dir should be 0.
    CHECK(snapshot_audit_wal_stats().using_default_dir == 0, "AC4: not default dir");
    clear_env("AURA_MUTATION_AUDIT_WAL");
    clear_env("AURA_MULTI_TENANT");
    reset_audit_wal_for_test();
    fs::remove_all(dir, ec);
}

// ── AC5: flush batching + schema surface ─────────────────────
static void ac5_flush_and_schema() {
    std::println("\n--- AC5: kFlushEvery + schema-2150 ---");
    CHECK(MutationAuditWal::kFlushEvery == 32, "AC5: kFlushEvery unchanged (32)");
    reset_process();
    set_env("AURA_MULTI_TENANT", "1");
    apply_production_security_defaults();
    CompilerService cs;
    // apply_env_sandbox re-applies defaults; also attach evaluator WAL.
    cs.evaluator().apply_env_sandbox();
    CHECK(href_wal(cs, "schema-2150") == kAuditWalForceMultiTenantIssue,
          "schema-2150 on wal-stats");
    CHECK(href_wal(cs, "audit-wal-forced") == 1, "audit-wal-forced");
    CHECK(href_wal(cs, "enabled") == 1, "wal-stats enabled");
    CHECK(href_wal(cs, "flush-every") == 32, "flush-every key");
    CHECK(href_cap(cs, "schema-2150") == kAuditWalForceMultiTenantIssue,
          "schema-2150 on capability-effect-stats");
    CHECK(href_cap(cs, "audit-wal-enabled") == 1, "cap audit-wal-enabled");
    CHECK(href_cap(cs, "audit-wal-force-wired") == 1, "audit-wal-force-wired");
    // Strict alone also forces (AURA_SANDBOX=strict, no multi-tenant).
    reset_process();
    set_env("AURA_SANDBOX", "strict");
    apply_production_security_defaults();
    CHECK(g_mutation_audit_wal().is_enabled(), "Strict alone forces WAL");
    CHECK(snapshot_audit_wal_stats().forced_by_multi_tenant > 0, "forced under Strict");
    clear_env("AURA_SANDBOX");
    clear_env("AURA_MULTI_TENANT");
}

} // namespace

int run_test_audit_wal_force_multi_tenant() {
    std::println("=== Issue #2150: force mutation-audit WAL multi-tenant/Strict ===");
    CHECK(kAuditWalForceMultiTenantIssue == 2150, "issue stamp");
    CHECK(MutationAuditWal::kFlushEvery == 32, "kFlushEvery baseline");

    ac1_multi_tenant_forces_wal();
    ac2_sandbox_off_no_wal();
    ac3_replay_security_events();
    ac4_explicit_path_wins();
    ac5_flush_and_schema();

    reset_process();
    std::println("\n=== #2150 audit WAL force: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_wal_force_multi_tenant();
}
#endif
