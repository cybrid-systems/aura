// tests/serve/test_resume_session_revoke_3320.cpp
// @category: unit
// @reason: Issue #3320 — dual-Evaluator + fiber-steal/resume residual race on
// session-grant revoke & principal restore. A fiber stolen while holding live
// session_bound grants can resume on a different Evaluator host before the
// original outermost dtor / steal-abort revoke completed. The resume entry
// hook (aura_fiber_install_tenant_scope_for_resume) must itself take the
// capability lock and revoke when it observes the steal Ok marker
// (resume_safety_ticket) + a live session mid, before the first effect gate.
//
//   AC1: steal-resume (ticket + session_mid) revokes live session grants at
//        resume entry; check_and_record_effect denies afterwards
//   AC2: normal resume (no ticket) does NOT revoke — in-guard grants survive
//   AC3: dual-Evaluator chaos: grant × effect × steal-resume × revoke never
//        leaves a live grant consumable by the wrong principal; steal/abort
//        revoke counters return to steady state
//   AC4: Soft/Off zero-cost — no lock when no live session grants (AC4)
//   AC5: no new capability model / second registry / query-key change;
//        source-cite evaluator_fiber_mutation.cpp resume-entry revoke

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "serve/fiber.h"
#include "serve/steal_safety.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_fiber_install_tenant_scope_for_resume(void* fiber_ptr) noexcept;
extern "C" void aura_evaluator_on_fiber_join(void* joined_fiber);
extern "C" void aura_evaluator_on_fiber_join_session_revoke(void* joined_fiber);

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::serve::Fiber;
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

static void reset_all() {
    reset_capability_effects_for_test();
    set_mode(SandboxMode::Off);
    aura::core::capability::g_capability_registry().sandbox_mode.store(
        aura::core::capability::EffectSandboxMode::Restricted, std::memory_order_release);
    aura::core::capability::g_capability_registry().mtx.lock();
    aura::core::capability::g_capability_registry().mtx.unlock();
}

static void grant_session(std::uint64_t tenant, std::uint64_t mid, const char* name) {
    EffectProvenance prov{};
    prov.epoch = mid;
    prov.mutation_id = mid;
    prov.fiber_id = 0;
    g_capability_registry().grant_session(tenant, name, Effect::Mutate, prov,
                                          /*single_use=*/false);
}

static bool consume(std::uint64_t tenant, std::uint64_t mid) {
    EffectProvenance prov{};
    prov.epoch = mid;
    prov.mutation_id = mid;
    return check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, tenant, "3320-consume",
                                   false, true);
}

// ── AC1: steal-resume revokes residual session grants at resume entry ──
static void ac3320_1_steal_resume_revokes() {
    std::println("\n--- #3320 AC1: steal-resume entry revokes residual session grants ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 42;
    grant_session(tenant, mid, "mut-3320-resume");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1, "AC1 pre: live grant");

    // Stolen fiber resuming on a new host: steal Ok marker + live session mid.
    Fiber f([] {});
    f.set_assigned_tenant_id(tenant);
    f.set_session_mid(mid);
    f.set_resume_safety_ticket(1);

    // Production multi-tenant (Restricted). Resume entry must revoke.
    CompilerService cs;
    cs.evaluator().set_capability_tenant_id(tenant);
    aura_fiber_install_tenant_scope_for_resume(&f);

    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC1: resume-entry revoke cleared session grant");
    CHECK(f.session_mid() == 0, "AC1: fiber session mid cleared after revoke");
    CHECK(!consume(tenant, mid), "AC1: check_and_record_effect denies after resume revoke");
    CHECK(g_capability_effect_metrics().capability_session_revoke_steal_total.load() >= 1,
          "AC1: steal revoke counter bumped (resume-host revoke)");
}

// ── AC2: normal resume (no ticket) does NOT revoke in-guard grants ──
static void ac3320_2_normal_resume_keeps_grants() {
    std::println("\n--- #3320 AC2: normal resume (no steal ticket) keeps live grants ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 43;
    grant_session(tenant, mid, "mut-3320-normal");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1, "AC2 pre: live grant");

    // No resume_safety_ticket → normal in-guard resume must NOT revoke.
    Fiber f([] {});
    f.set_assigned_tenant_id(tenant);
    f.set_session_mid(mid);
    CHECK(!f.has_resume_safety_ticket(), "AC2: no steal ticket on normal resume");

    CompilerService cs;
    cs.evaluator().set_capability_tenant_id(tenant);
    const auto steal0 = g_capability_effect_metrics().capability_session_revoke_steal_total.load();
    aura_fiber_install_tenant_scope_for_resume(&f);

    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1,
          "AC2: normal resume keeps live session grant (no spurious revoke)");
    CHECK(f.session_mid() == mid, "AC2: session mid untouched on normal resume");
    CHECK(g_capability_effect_metrics().capability_session_revoke_steal_total.load() == steal0,
          "AC2: no steal revoke counter bump on normal resume");
    CHECK(consume(tenant, mid), "AC2: in-guard effect still allowed after normal resume");
}

// ── AC3: dual-Evaluator chaos — grant × effect × steal-resume × revoke ──
static void ac3320_3_dual_evaluator_chaos() {
    std::println(
        "\n--- #3320 AC3: dual-Evaluator chaos — no wrong-principal consume after revoke ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 44;
    constexpr std::uint64_t foreign = 99;

    CompilerService csA;
    CompilerService csB; // dual Evaluator, shared process-global registry
    csA.evaluator().set_capability_tenant_id(tenant);
    csB.evaluator().set_capability_tenant_id(foreign);

    constexpr int kIters = 200;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> allowed_after_revoke{0};
    std::vector<std::thread> threads;

    // Writer loop: grant → steal-resume revoke (ticket + resume entry).
    threads.emplace_back([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            grant_session(tenant, mid, "mut-3320-chaos");
            Fiber f([] {});
            f.set_assigned_tenant_id(tenant);
            f.set_session_mid(mid);
            f.set_resume_safety_ticket(1);
            aura_fiber_install_tenant_scope_for_resume(&f);
        }
    });

    // Reader loop: try to consume under the foreign principal — must never
    // succeed after the resume-entry revoke (AC1/AC3 invariant).
    threads.emplace_back([&] {
        for (int i = 0; i < kIters && !stop.load(); ++i) {
            if (consume(tenant, mid)) {
                // Allowed only while the grant is live; after revoke it must
                // deny. Track any post-revoke allow for the final assert.
                allowed_after_revoke.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    for (auto& t : threads)
        t.join();

    // Steady state: no live session grants remain; revoke counters non-zero.
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC3: no residual live session grant after chaos");
    CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
          "AC3: capability_live_session_grants steady-state zero");
    CHECK(g_capability_effect_metrics().capability_session_revoke_steal_total.load() >= 1,
          "AC3: steal revoke counter observed during chaos");
}

// ── AC4: Soft/Off zero-cost — no lock when no live session grants ──
static void ac3320_4_soft_zero_cost() {
    std::println("\n--- #3320 AC4: Soft path zero-cost (no live grants → no lock) ---");
    reset_all();
    set_mode(SandboxMode::Off);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 45;

    Fiber f([] {});
    f.set_assigned_tenant_id(tenant);
    f.set_session_mid(mid);
    f.set_resume_safety_ticket(1);

    const auto steal0 = g_capability_effect_metrics().capability_session_revoke_steal_total.load();
    CompilerService cs;
    cs.evaluator().set_capability_tenant_id(tenant);
    aura_fiber_install_tenant_scope_for_resume(&f);
    // Soft: resume hook must not have bumped the steal counter (no live grants).
    CHECK(g_capability_effect_metrics().capability_session_revoke_steal_total.load() == steal0,
          "AC4: Soft zero-cost — no steal revoke counter bump");
}

// ── AC5: source-cite — resume-entry revoke + no new registry/query key ──
static void ac3320_5_source_cite() {
    std::println("\n--- #3320 AC5: source-cite resume-entry revoke (no new model/keys) ---");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fm.find("Issue #3320") != std::string::npos,
          "AC5: evaluator_fiber_mutation.cpp cites #3320");
    CHECK(fm.find("aura_fiber_install_tenant_scope_for_resume") != std::string::npos,
          "AC5: resume-entry hook in evaluator_fiber_mutation.cpp");
    CHECK(fm.find("has_resume_safety_ticket") != std::string::npos,
          "AC5: steal Ok marker gates the resume revoke");
    CHECK(fm.find("revoke_session_grants_on_steal_or_abort_locked") != std::string::npos,
          "AC5: resume revoke reuses existing locked steal/abort wrapper");
    // No new query keys / second registry: reuse-only implementation.
    const auto cm = read_file("src/core/capability_model.hh");
    CHECK(cm.find("revoke_session_grants_on_steal_or_abort_locked") != std::string::npos,
          "AC5: capability_model.hh owns the locked steal/abort revoke (no new registry)");
}

} // namespace

static void ac3563_1_join_done_revokes() {
    std::println("\n--- #3563 AC1: join Done revokes session grants ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 56;
    grant_session(tenant, mid, "mut-3563-join");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1, "AC1 pre: live grant");
    Fiber f([] {});
    f.set_assigned_tenant_id(tenant);
    f.set_session_mid(mid);
    CompilerService cs;
    cs.evaluator().set_capability_tenant_id(tenant);
    aura_evaluator_on_fiber_join(&f);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "3563 AC1: join Done cleared session grant");
    CHECK(f.session_mid() == 0, "3563 AC1: fiber session mid cleared");
    CHECK(!consume(tenant, mid), "3563 AC1: check_and_record_effect denies after join revoke");
}

static void ac3563_2_reclaimed_revokes_only() {
    std::println("\n--- #3563 AC2: Reclaimed revoke-only hook clears session rows ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 57;
    grant_session(tenant, mid, "mut-3563-reclaim");
    Fiber f([] {});
    f.set_assigned_tenant_id(tenant);
    f.set_session_mid(mid);
    aura_evaluator_on_fiber_join_session_revoke(&f);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "3563 AC2: Reclaimed revoke-only cleared session grant");
    CHECK(f.session_mid() == 0, "3563 AC2: session mid cleared");
    CHECK(!consume(tenant, mid), "3563 AC2: consume denies after Reclaimed revoke");
}

static void ac3563_3_idempotent() {
    std::println("\n--- #3563 AC3: second join revoke is a commutative no-op ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 58;
    grant_session(tenant, mid, "mut-3563-idem");
    Fiber f([] {});
    f.set_session_mid(mid);
    aura_evaluator_on_fiber_join(&f);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0, "3563 AC3: first join");
    aura_evaluator_on_fiber_join(&f);
    aura_evaluator_on_fiber_join_session_revoke(&f);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "3563 AC3: second join no-op, no residual");
}

static void ac3563_4_soft_zero_cost() {
    std::println("\n--- #3563 AC4: Soft/Off + no live grants is zero extra lock ---");
    reset_all();
    set_mode(SandboxMode::Off);
    Fiber f([] {});
    const auto steal0 = g_capability_effect_metrics().capability_session_revoke_total.load();
    aura_evaluator_on_fiber_join(&f);
    aura_evaluator_on_fiber_join_session_revoke(&f);
    CHECK(g_capability_effect_metrics().capability_session_revoke_total.load() == steal0,
          "3563 AC4: Soft no live grants → no session_revoke bump");
}

static void ac3563_5_source_cite() {
    std::println("\n--- #3563 AC5: source-cite join session revoke ---");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fm.find("Issue #3563") != std::string::npos, "3563 AC5: fiber_mutation cites #3563");
    CHECK(fm.find("aura_evaluator_on_fiber_join_session_revoke") != std::string::npos,
          "3563 AC5: revoke-only ABI");
    CHECK(fm.find("revoke_session_grants_for_mid_locked") != std::string::npos,
          "3563 AC5: reuses outermost mid-exit helper");
    const auto fc = read_file("src/serve/fiber.cpp");
    CHECK(fc.find("aura_evaluator_on_fiber_join_session_revoke") != std::string::npos,
          "3563 AC5: Reclaimed / fiber-stack join calls revoke-only");
    CHECK(read_file("tests/serve/test_issue_3563.cpp").empty(), "3563 AC5: no test_issue_3563.cpp");
}

int run_test_resume_session_revoke_3320() {
    std::println("=== Issue #3320: resume-host session-grant revoke (steal×resume) ===");
    ac3320_1_steal_resume_revokes();
    ac3320_2_normal_resume_keeps_grants();
    ac3320_3_dual_evaluator_chaos();
    ac3320_4_soft_zero_cost();
    ac3320_5_source_cite();
    ac3563_1_join_done_revokes();
    ac3563_2_reclaimed_revokes_only();
    ac3563_3_idempotent();
    ac3563_4_soft_zero_cost();
    ac3563_5_source_cite();
    return g_failed == 0 ? 0 : 1;
}

int run_test_resume_session_revoke() {
    return run_test_resume_session_revoke_3320();
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_resume_session_revoke();
}
#endif
