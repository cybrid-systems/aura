// @category: unit
// @reason: Issue #2388 — fold Capability + Isolation private audit rings
// into SecurityEvent ring + optional WAL (durable forensic trail).
//
//   AC1: >128 effect denies under enabled SecurityEvent WAL → after
//        restart/replay, early denies still queryable by mutation_id
//   AC2: Isolation deny produces exactly one IsolationDeny SecurityEvent
//        (no double-count with Evaluator)
//   AC3: Soft / WAL off: persist short-circuits (no extra syscalls)
//   AC4: Additive only — kSecurityAuditFoldIssue=2388 sentinel
//   AC5: Source-cite dual-write + CMake + build.py gate

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/security_event_wal.hh"
#include "core/workspace_epoch.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::kEffectMutate;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityAuditFoldIssue;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEventKind;
using aura::core::security_event_wal::emit_security_event_durable;
using aura::core::security_event_wal::g_security_event_wal;
using aura::core::security_event_wal::reset_security_event_wal_for_test;
using aura::core::security_event_wal::snapshot_security_event_wal_stats;
using aura::core::workspace_isolation::check_boundary;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

namespace fs = std::filesystem;

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

static fs::path fresh_wal_dir(const std::string& tag) {
    const auto base =
        fs::temp_directory_path() / (std::string("aura-sec-fold-2388-") + tag + "-XXXXXX");
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

static void reset_all() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    reset_security_event_wal_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

static std::uint64_t count_kind(SecurityEventKind kind) {
    auto& ring = g_security_event_ring();
    const auto total = ring.total.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;
    const auto n = total < kSecurityEventRingSize ? total : kSecurityEventRingSize;
    std::uint64_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto idx = (total - 1 - i) % kSecurityEventRingSize;
        if (ring.ring[idx].kind == kind)
            ++c;
    }
    return c;
}

static std::uint64_t count_effect_deny_with_mid(std::uint64_t mid) {
    auto& ring = g_security_event_ring();
    const auto total = ring.total.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;
    const auto n = total < kSecurityEventRingSize ? total : kSecurityEventRingSize;
    std::uint64_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const auto idx = (total - 1 - i) % kSecurityEventRingSize;
        const auto& e = ring.ring[idx];
        if (e.kind == SecurityEventKind::EffectDeny && e.mutation_id == mid)
            ++c;
    }
    return c;
}

// AC1: private 128-slot wrap + WAL recovers early denies by mutation_id.
static void ac1_wrap_and_wal_replay() {
    std::println("\n--- #2388 AC1: >128 effect denies + WAL replay ---");
    reset_all();
    bump_mutation_epoch(100);
    const auto epoch = current_mutation_epoch();
    CHECK(epoch >= 100, "AC1: mutation epoch advanced");

    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto dir = fresh_wal_dir("ac1");
    CHECK(ev.enable_security_event_wal(dir.string()), "AC1: enable SecurityEvent WAL");

    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    // 150 denies → wraps private 128-slot capability audit ring; SE ring
    // (1024) + WAL keep the full trail.
    constexpr std::uint64_t kN = 150;
    const std::uint64_t first_mid = 9001;
    for (std::uint64_t i = 0; i < kN; ++i) {
        EffectProvenance prov{};
        prov.mutation_id = first_mid + i;
        prov.epoch = epoch;
        prov.fiber_id = 1;
        const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov,
                                                /*tenant=*/7, "test:ac1-fold-deny",
                                                /*wildcard_ok=*/false, /*sandbox_active=*/true);
        CHECK(!ok, std::format("AC1: deny #{} under Strict no grant", i));
    }

    const auto se_denies = count_kind(SecurityEventKind::EffectDeny);
    std::println("  EffectDeny in SE ring after {} capability denies: {}", kN, se_denies);
    CHECK(se_denies == kN, "AC1: dual-write one SE per capability deny (not 0, not 2x)");
    CHECK(count_effect_deny_with_mid(first_mid) == 1,
          "AC1: first deny (mid=9001) present before wrap of private ring");
    CHECK(count_effect_deny_with_mid(first_mid + kN - 1) == 1, "AC1: last deny present in SE ring");

    const auto snap = snapshot_security_event_wal_stats();
    std::println("  WAL persisted={}", snap.persisted);
    CHECK(snap.persisted >= kN, "AC1: WAL persisted ≥ 150 dual-write records");

    // Simulate process restart: wipe live ring, re-enable WAL → replay.
    ev.disable_security_event_wal();
    reset_security_event_ring_for_test();
    CHECK(g_security_event_ring().seq.load(std::memory_order_relaxed) == 0,
          "AC1: ring wiped for restart sim");
    CHECK(ev.enable_security_event_wal(dir.string()), "AC1: re-enable WAL (replay)");

    const auto after_total = g_security_event_ring().total.load(std::memory_order_relaxed);
    std::println("  after replay ring.total={}", after_total);
    CHECK(after_total >= kN, "AC1: ring.total ≥ 150 after WAL replay");
    CHECK(count_effect_deny_with_mid(first_mid) >= 1,
          "AC1: early deny mid=9001 queryable after replay (wrap recovery)");
    CHECK(count_effect_deny_with_mid(first_mid + 50) >= 1,
          "AC1: mid-range deny still queryable after replay");

    ev.disable_security_event_wal();
    fs::remove_all(dir);
}

// AC2: IsolationDeny is single-count (private ring dual-write only).
static void ac2_isolation_single_se() {
    std::println("\n--- #2388 AC2: IsolationDeny single SE (no double-count) ---");
    reset_all();
    bump_mutation_epoch(3);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted — unset principal footgun
    const auto before = count_kind(SecurityEventKind::IsolationDeny);
    const bool ok =
        ev.check_workspace_isolation(/*target=*/0, /*ref=*/0, kEffectMutate, "test:ac2-iso-single");
    CHECK(!ok, "AC2: Restricted + unset principal denies Mutate");
    const auto after = count_kind(SecurityEventKind::IsolationDeny);
    std::println("  IsolationDeny count {}→{}", before, after);
    CHECK(after == before + 1, "AC2: exactly one IsolationDeny SE (no Evaluator double-count)");

    // Direct free-function path also single-counts.
    const auto b2 = count_kind(SecurityEventKind::IsolationDeny);
    CHECK(!check_boundary(0, nullptr, kEffectMutate, /*strict=*/true, "test:ac2-boundary",
                          /*restricted=*/false),
          "AC2: Strict free check_boundary denies");
    const auto a2 = count_kind(SecurityEventKind::IsolationDeny);
    CHECK(a2 == b2 + 1, "AC2: free check_boundary also one SE per deny");

    // Reason preserved for Agents.
    auto& ring = g_security_event_ring();
    const auto total = ring.total.load(std::memory_order_relaxed);
    CHECK(total > 0, "AC2: ring non-empty");
    const auto& last = ring.ring[(total - 1) % kSecurityEventRingSize];
    CHECK(last.kind == SecurityEventKind::IsolationDeny, "AC2: last event is IsolationDeny");
    CHECK(std::string_view(last.reason).find("isolation-deny") != std::string_view::npos,
          "AC2: reason carries isolation-deny*");
}

// AC3: WAL off short-circuit.
static void ac3_wal_off_short_circuit() {
    std::println("\n--- #2388 AC3: WAL off persist short-circuit ---");
    reset_all();
    CHECK(!g_security_event_wal().is_enabled(), "AC3: WAL disabled by default");
    const auto snap0 = snapshot_security_event_wal_stats();
    // Dual-write path still appends ring; persist no-ops.
    emit_security_event_durable(SecurityEventKind::EffectAllow, 1, 2, 3, kEffectMutate,
                                "test:ac3-off", "effect-allow", /*denied=*/false, /*fiber=*/0);
    const auto snap1 = snapshot_security_event_wal_stats();
    CHECK(snap1.persisted == snap0.persisted, "AC3: no WAL persist when disabled");
    CHECK(snap1.enabled == 0, "AC3: wal-enabled still 0");
    CHECK(g_security_event_ring().total.load(std::memory_order_relaxed) >= 1,
          "AC3: ring append still works when WAL off");
}

// AC4: additive sentinel only.
static void ac4_sentinel() {
    std::println("\n--- #2388 AC4: kSecurityAuditFoldIssue sentinel ---");
    CHECK(kSecurityAuditFoldIssue == 2388, "AC4: kSecurityAuditFoldIssue == 2388");
    CHECK(kSecurityEventRingSize == 1024, "AC4: SE ring still 1024 (no shrink)");
}

// AC5: source-cite + gate registration.
static void ac5_source_and_gate() {
    std::println("\n--- #2388 AC5: source-cite + CMake/build gate ---");
    const auto cap = read_file("src/core/capability_model.hh");
    const auto iso = read_file("src/core/workspace_isolation.hh");
    const auto wal = read_file("src/core/security_event_wal.hh");
    const auto se = read_file("src/core/security_event.hh");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto linter = read_file("scripts/coverage/checks/check_security_audit_fold_2388.py");

    CHECK(cap.find("emit_security_event_durable") != std::string::npos,
          "AC5: capability record_audit dual-writes via emit_security_event_durable");
    CHECK(iso.find("emit_security_event_durable") != std::string::npos,
          "AC5: isolation record_audit dual-writes via emit_security_event_durable");
    CHECK(wal.find("emit_security_event_durable") != std::string::npos,
          "AC5: emit_security_event_durable helper present");
    CHECK(se.find("kSecurityAuditFoldIssue") != std::string::npos,
          "AC5: kSecurityAuditFoldIssue sentinel in security_event.hh");
    CHECK(se.find("2388") != std::string::npos, "AC5: Issue #2388 cite in security_event.hh");
    // Evaluator must NOT re-append SE on effect/isolation deny (fold path).
    CHECK(sec.find("Issue #2388") != std::string::npos,
          "AC5: evaluator_security cites #2388 single-path fold");
    CHECK(cmake.find("test_security_audit_fold") != std::string::npos,
          "AC5: CMake registers test_security_audit_fold");
    CHECK(build.find("check_security_audit_fold_2388") != std::string::npos,
          "AC5: build.py wires check_security_audit_fold_2388");
    CHECK(build.find("cmd_security_audit_fold_coverage") != std::string::npos,
          "AC5: build.py cmd_security_audit_fold_coverage");
    CHECK(!linter.empty(), "AC5: scripts/coverage/checks/check_security_audit_fold_2388.py exists");
    (void)g_workspace_isolation(); // keep link / header live
}

} // namespace

int run_test_security_audit_fold() {
    std::println("=== Issue #2388: fold Capability/Isolation audit into SecurityEvent WAL ===");
    CHECK(kSecurityAuditFoldIssue == 2388, "issue stamp");

    ac1_wrap_and_wal_replay();
    ac2_isolation_single_se();
    ac3_wal_off_short_circuit();
    ac4_sentinel();
    ac5_source_and_gate();

    std::println("\n=== #2388 results: passed={} failed={} ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_security_audit_fold();
}
#endif
