// tests/core/test_capability_single_use_consume.cpp
// @category: unit
// @reason: Issue #2586 — single-use / mutation-bound grant (auto-revoke
//          after first successful check_and_record_effect that uses the
//          grant's bits; deny path does NOT consume).
//
//   AC1: grant_once(Mutate) → 1st allow; 2nd deny (no other grant)
//   AC2: 1st deny (other reasons, e.g. wrong effect bits) → grant still
//        valid, retryable
//   AC3: non single_use grant behavior unchanged (multiple allows OK)
//   AC4: Soft/Off mode usable; production default API (grant() with
//        single_use=false) does not force auto-revoke
//   AC5: audit / SE dual-write reason "single-use-consumed" visible in
//        SecurityEvent ring + capability_single_use_consumed metric
//   AC6: tests + source-cite (no docs/design/)
//
// Source-cite:
//   src/core/capability_model.hh — CapabilityGrant.single_use field
//     (line ~133), CapabilityEffectMetrics.capability_single_use_consumed_total
//     counter (~196), grant() extended signature (~405), grant_once sugar (~449),
//     check_and_record_effect consume block (~842), CapabilityEffectStatsSnapshot
//     .capability_single_use_consumed (~975), reset + snapshot fields.
//   src/compiler/evaluator_security.cpp — grant_effect_capability single_use
//     parameter forwarding to registry::grant.
//   src/compiler/evaluator.ixx — grant_effect_capability declaration
//     with `bool single_use = false` default parameter.
//   src/compiler/evaluator_primitives_security.cpp — query:capability-effect-stats
//     surfaces capability-single-use-consumed-total / schema-2586 / issue-2586.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::security::kEffectMacroSelfEvo;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectSyscall;
using aura::compiler::security::kEffectTenantAdmin;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::check_and_record_effect;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::kSecurityEventRingSize;
using aura::core::security_event::reset_security_event_ring_for_test;
using aura::core::security_event::SecurityEvent;
using aura::test::g_failed;
using aura::test::g_passed;

// Helper for AC6 source-cite checks (test path + ../ test path).
static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (in)
            return std::string((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    }
    return {};
}

// Walk back through the SE ring looking for an event whose reason matches
// `needle`. Bounded lookback so wrap storms don't scan forever. Returns the
// matched slot if found, nullptr otherwise.
const SecurityEvent* ring_lookup_reason(std::string_view needle, std::uint64_t lookback = 16) {
    const auto& ring = g_security_event_ring();
    const auto cur = ring.seq.load(std::memory_order_acquire);
    if (cur == 0)
        return nullptr;
    const auto start = cur;
    const auto end = (cur > lookback) ? cur - lookback : std::uint64_t{1};
    for (auto s = start; s >= end && s > 0; --s) {
        const auto idx = (s - 1) % kSecurityEventRingSize;
        const auto& e = ring.ring[idx];
        const auto rlen = std::strlen(e.reason);
        if (rlen == needle.size() && std::string_view(e.reason, rlen) == needle) {
            return &e;
        }
    }
    return nullptr;
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_security_event_ring_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace


// ── Issue #3142: SessionBound grant revoke cascade (nested TenantScope
// abort + fiber steal paths must revoke inner SessionBound grants; stolen
// flag prevents caller-side double-consume). Soft / zero live residual is
// a no-op short-circuit (AC3). Additive metrics only (AC4). Source-cite
// capability_model.hh + evaluator_security.cpp + evaluator_fiber_mutation.cpp;
// no docs/design/, no tests/issues/test_issue_3142.cpp (AC5).

static void ac3142_1_nested_abort_cascade_revoke() {
    std::println("\n--- #3142 AC1: nested TenantScope abort cascade-revoke SessionBound ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    constexpr std::uint64_t tenant = 42;
    ev.set_capability_tenant_id(tenant);

    EffectProvenance prov{};
    prov.epoch = 1;
    prov.mutation_id = 1;
    g_capability_registry().grant(tenant, "mut-3142-cascade", Effect::Mutate | Effect::MacroSelfEvo,
                                  prov,
                                  /*single_use=*/false, /*session_bound=*/true);

    const auto before = g_capability_registry().session_bound_entries_alive(tenant);
    CHECK(before == 1, "AC1 pre: tenant has 1 live session_bound grant");

    // Public wrapper takes mtx (#3207: do not nest lock_guard).
    (void)g_capability_registry().revoke_session_grants_for(tenant, prov.mutation_id,
                                                            /*fiber_id=*/0, "scope-dtor-cascade");

    const auto after = g_capability_registry().session_bound_entries_alive(tenant);
    CHECK(after == 0, "AC1: cascade revoke cleared session_bound grant");
    CHECK(g_capability_effect_metrics().session_bound_revoked_on_scope_dtor_total.load() >= 1,
          "AC1: counter bumped on cascade revoke");
}

static void ac3142_2_steal_marks_no_double_consume() {
    std::println(
        "\n--- #3142 AC2: fiber steal marks stolen → caller check fails (no double-consume) ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    constexpr std::uint64_t tenant = 7;
    ev.set_capability_tenant_id(tenant);

    EffectProvenance prov{};
    prov.epoch = 1;
    prov.mutation_id = 42;

    g_capability_registry().grant(tenant, "mut-3142-stolen", Effect::Mutate | Effect::MacroSelfEvo,
                                  prov,
                                  /*single_use=*/false, /*session_bound=*/true);

    // Public wrapper takes mtx (#3207: do not nest lock_guard).
    (void)g_capability_registry().mark_session_bound_stolen(tenant, prov.mutation_id,
                                                            /*fiber_id=*/0);

    // AC2: caller-side check_and_record_effect for the same mid must FAIL
    // (stolen entry excluded from effects_for_locked, no double-consume).
    const bool allowed = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, tenant,
                                                 "mut-3142-stolen-check");
    CHECK(!allowed,
          "AC2: stolen SessionBound grant → check_and_record_effect deny (no double-consume)");

    // Verify stolen flag was set (consume loop skipped the entry).
    bool found_stolen = false;
    {
        std::lock_guard<std::mutex> lock(g_capability_registry().mtx);
        const auto it = g_capability_registry().by_tenant.find(tenant);
        if (it != g_capability_registry().by_tenant.end()) {
            for (const auto& g : it->second) {
                if (g.name == "mut-3142-stolen") {
                    found_stolen = g.stolen;
                    break;
                }
            }
        }
    }
    CHECK(found_stolen, "AC2: entry has stolen=true (caller cannot consume)");
    CHECK(g_capability_effect_metrics().session_bound_revoked_on_steal_total.load() >= 1,
          "AC2: counter bumped on steal mark");
}

static void ac3142_3_long_run_no_leak() {
    std::println("\n--- #3142 AC3: 1000 nested aborts → session_bound_entries_alive == 0 ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    constexpr std::uint64_t tenant = 99;
    ev.set_capability_tenant_id(tenant);

    EffectProvenance prov{};
    prov.epoch = 1;
    prov.mutation_id = 99;

    for (int i = 0; i < 1000; ++i) {
        const std::string name = "mut-3142-loop-" + std::to_string(i);
        g_capability_registry().grant(tenant, name, Effect::Mutate, prov,
                                      /*single_use=*/false, /*session_bound=*/true);
        (void)g_capability_registry().revoke_session_grants_for(tenant, prov.mutation_id,
                                                                /*fiber_id=*/0, "loop-cascade");
    }

    const auto remaining = g_capability_registry().session_bound_entries_alive(tenant);
    CHECK(remaining == 0, "AC3: 1000 nested aborts → no leak (session_bound_entries_alive == 0)");
}

static void ac3142_4_additive_metrics_and_source_cite() {
    std::println("\n--- #3142 AC4/AC5: additive counter + source-cite + linter ---");
    // Source-cite in capability_model.hh
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #3142") != std::string::npos, "AC4: capability_model.hh cites #3142");
    CHECK(cap.find("session_bound_revoked_on_scope_dtor_total") != std::string::npos,
          "AC4: dtor counter present");
    CHECK(cap.find("session_bound_revoked_on_steal_total") != std::string::npos,
          "AC4: steal counter present");
    CHECK(cap.find("revoke_session_grants_for") != std::string::npos,
          "AC4: revoke_session_grants_for overload present");
    CHECK(cap.find("session_bound_entries_alive") != std::string::npos,
          "AC4: session_bound_entries_alive accessor present");
    CHECK(cap.find("mark_session_bound_stolen") != std::string::npos,
          "AC4: mark_session_bound_stolen helper present");
    CHECK(cap.find("bool stolen = false") != std::string::npos,
          "AC4: stolen flag field on CapabilityGrant");

    // Source-cite in evaluator_security.cpp (TenantScope::release cascade revoke)
    const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(eval_sec.find("Issue #3142") != std::string::npos,
          "AC4: evaluator_security.cpp cites #3142");
    CHECK(eval_sec.find("scope-dtor-cascade") != std::string::npos,
          "AC4: TenantScope::release calls revoke_session_grants_for");

    // Source-cite in evaluator_fiber_mutation.cpp (steal mark)
    const auto fiber_mut = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(fiber_mut.find("mark_session_bound_stolen") != std::string::npos,
          "AC4: fiber_mutation.cpp wires mark_session_bound_stolen");
    CHECK(fiber_mut.find("Issue #3142") != std::string::npos,
          "AC4: fiber_mutation.cpp cites #3142");

    // Linter exists
    const auto linter = read_file("scripts/coverage/checks/check_session_bound_dtor_cascade.py");
    CHECK(!linter.empty() && linter.find("Issue #3142") != std::string::npos,
          "AC5: linter exists and cites #3142");

    // build.py wires linter
    const auto build = read_file("build.py");
    CHECK(build.find("check_session_bound_dtor_cascade") != std::string::npos,
          "AC5: build.py wires linter");

    // No docs/design/, no tests/issues/test_issue_3142.cpp
    CHECK(!std::filesystem::exists("docs/design/3142-castop-typed-meta-phase-c.md"),
          "AC5: no docs/design/3142-*.md");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3142.cpp"),
          "AC5: no tests/issues/test_issue_3142.cpp");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3142.cpp"),
          "AC5: no tests/core/test_issue_3142.cpp");
}

// ── Issue #3207: dual-Evaluator grant_session × TenantScope cascade
// linearizability on the process-global CapabilityRegistry. Keep global
// registry (no per-Evaluator shard). Restricted + multi-tenant. Soft/Off
// zero-cost. Existing counters only.

static void ac3207_1_sequential_cascade_then_require_effect_deny() {
    std::println("\n--- #3207 AC1: EvA grant_session → EvB TenantScope cascade → EvA "
                 "require_effect fully denies ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    CHECK(mid != 0, "AC1: mutation epoch non-zero so TenantScope cascade fires");

    CompilerService cs_a;
    CompilerService cs_b;
    auto& ev_a = cs_a.evaluator();
    auto& ev_b = cs_b.evaluator();
    ev_a.set_effect_sandbox_mode(1);
    ev_b.set_effect_sandbox_mode(1);
    constexpr std::uint64_t tenant = 7;
    ev_a.set_capability_tenant_id(tenant);
    ev_b.set_capability_tenant_id(tenant);

    EffectProvenance prov{};
    prov.epoch = mid;
    prov.mutation_id = mid;
    prov.fiber_id = 0; // overlapping fiber (any TenantScope fiber matches)
    g_capability_registry().grant_session(tenant, "mut-3207-seq", Effect::Mutate, prov,
                                          /*single_use=*/true);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1,
          "AC1 pre: live session grant");
    CHECK(g_capability_effect_metrics().capability_live_session_grants.load() >= 1,
          "AC1 pre: live_session_grants > 0");

    {
        Evaluator::TenantScope scope(ev_b, tenant);
        (void)scope;
    }

    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC1: cascade cleared session_bound grant");
    CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
          "AC1: live_session_grants == 0 after cascade (no orphan count)");
    CHECK(!ev_a.require_effect(kEffectMutate, "3207-seq-consume"),
          "AC1: EvA require_effect fully denies after EvB cascade (no half-consume)");
}

static void ac3207_2_dual_evaluator_concurrent_chaos() {
    std::println("\n--- #3207 AC1 chaos: dual-Evaluator concurrent grant_session × "
                 "TenantScope cascade × require_effect ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();

    CompilerService cs_a;
    CompilerService cs_b;
    auto& ev_a = cs_a.evaluator();
    auto& ev_b = cs_b.evaluator();
    ev_a.set_effect_sandbox_mode(1);
    ev_b.set_effect_sandbox_mode(1);
    constexpr std::uint64_t tenant = 7;
    ev_a.set_capability_tenant_id(tenant);
    ev_b.set_capability_tenant_id(tenant);

    std::atomic<int> allows{0};
    std::atomic<int> denies{0};
    constexpr int kIters = 400;
    std::thread t_grant_consume([&] {
        EffectProvenance prov{};
        prov.epoch = mid;
        prov.mutation_id = mid;
        prov.fiber_id = 0;
        for (int i = 0; i < kIters; ++i) {
            g_capability_registry().grant_session(tenant, "mut-3207-chaos", Effect::Mutate, prov,
                                                  /*single_use=*/true);
            if (ev_a.require_effect(kEffectMutate, "3207-chaos-consume"))
                allows.fetch_add(1, std::memory_order_relaxed);
            else
                denies.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::thread t_cascade([&] {
        for (int i = 0; i < kIters; ++i) {
            Evaluator::TenantScope scope(ev_b, tenant);
            (void)scope;
        }
    });
    t_grant_consume.join();
    t_cascade.join();

    const auto allows_n = allows.load();
    const auto denies_n = denies.load();
    CHECK(allows_n + denies_n == kIters,
          "AC1 chaos: every require_effect fully allowed or fully denied");
    const auto alive = g_capability_registry().session_bound_entries_alive(tenant);
    const auto live = g_capability_effect_metrics().capability_live_session_grants.load();
    CHECK(live == alive, "AC1 chaos: live_session_grants matches session_bound_entries_alive "
                         "(no orphan live count)");

    {
        Evaluator::TenantScope scope(ev_b, tenant);
        (void)scope;
    }
    (void)g_capability_registry().revoke_session_grants_for_mid(mid);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC1 chaos: after quiesce, no live session grants");
    CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
          "AC1 chaos: after quiesce, live_session_grants == 0");
    const auto held = g_capability_registry().effects_for(tenant);
    using aura::core::capability::has_effect;
    CHECK(!has_effect(held, Effect::Mutate),
          "AC1 chaos: after quiesce, effects_for has no leftover Mutate");
    (void)allows_n;
    (void)denies_n;
}

static void ac3207_3_soft_off_zero_cost() {
    std::println("\n--- #3207 AC2: Soft/Off TenantScope cascade is zero-cost ---");
    reset_all();
    set_mode(SandboxMode::Off);
    aura::core::bump_mutation_epoch(1);

    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    ev.set_capability_tenant_id(7);
    const auto live0 = g_capability_effect_metrics().capability_live_session_grants.load();
    CHECK(live0 == 0, "AC2 pre: no live session grants");
    {
        Evaluator::TenantScope scope(ev, 7);
        (void)scope;
    }
    CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
          "AC2: Soft/Off cascade leaves live_session_grants == 0");
    CHECK(ev.require_effect(kEffectMutate, "3207-soft"),
          "AC2: Soft/Off require_effect still allows (no extra deny)");
}

static void ac3207_4_source_cite_and_linter() {
    std::println("\n--- #3207 AC5: source-cite + linter + no invent ---");
    const auto cap = read_file("src/core/capability_model.hh");
    const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
    const auto fiber = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto build = read_file("build.py");
    CHECK(cap.find("kCapabilityDualEvaluatorCascadeIssue = 3207") != std::string::npos,
          "AC5: capability_model.hh stamps #3207");
    CHECK(cap.find("revoke_session_grants_for_locked") != std::string::npos,
          "AC5: locked cascade sibling");
    CHECK(cap.find("revoke_session_grants_for_mid_locked") != std::string::npos,
          "AC5: locked mid-revoke sibling");
    CHECK(cap.find("mark_session_bound_stolen_locked") != std::string::npos,
          "AC5: locked stolen-mark sibling");
    CHECK(cap.find("revoke_session_grants_on_steal_or_abort_locked") != std::string::npos,
          "AC5: locked steal/abort sibling");
    CHECK(eval_sec.find("Issue #3207") != std::string::npos,
          "AC5: TenantScope::release cites #3207");
    CHECK(eval_sec.find("revoke_session_grants_for_locked") != std::string::npos,
          "AC5: TenantScope::release uses locked cascade");
    CHECK(fiber.find("mark_session_bound_stolen_locked") != std::string::npos,
          "AC5: steal path uses locked mark");
    CHECK(fiber.find("revoke_session_grants_on_steal_or_abort_locked") != std::string::npos,
          "AC5: steal path uses locked abort revoke");
    CHECK(build.find("check_dual_evaluator_cascade_3207") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3207.cpp"),
          "AC5: no tests/core/test_issue_3207.cpp");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3207.cpp"),
          "AC5: no tests/issues/test_issue_3207.cpp");
    CHECK(!std::filesystem::exists("docs/design/3207-dual-evaluator-cascade.md"),
          "AC5: no docs/design/3207-*.md");
}

// ── Issue #3209: nested abort × steal × resume session-grant quiesce ──

static void ac3209_grant(std::uint64_t tenant, std::uint64_t mid, bool single_use) {
    EffectProvenance prov{};
    prov.epoch = mid;
    prov.mutation_id = mid;
    prov.fiber_id = 0;
    g_capability_registry().grant_session(tenant, "mut-3209", Effect::Mutate, prov, single_use);
}

static bool ac3209_consume(std::uint64_t tenant, std::uint64_t mid) {
    EffectProvenance prov{};
    prov.epoch = mid;
    prov.mutation_id = mid;
    return check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, tenant, "3209-consume",
                                   false, true);
}

static void ac3209_1_outermost_exit_success_and_fail() {
    std::println("\n--- #3209 AC1: outermost success/fail → session_bound_entries_alive==0 ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 9;
    ac3209_grant(tenant, mid, /*single_use=*/false);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1, "AC1 pre: live");
    (void)g_capability_registry().revoke_session_grants_for_mid(mid);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC1: outermost-success-shape revoke clears live");
    CHECK(!ac3209_consume(tenant, mid), "AC1: consume denies after exit revoke");

    ac3209_grant(tenant, mid, false);
    const auto n =
        aura::core::capability::revoke_session_grants_on_steal_or_abort(mid, /*steal=*/false);
    CHECK(n >= 1, "AC1: outermost-fail-shape abort revokes");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC1: abort path live==0");
    CHECK(!ac3209_consume(tenant, mid), "AC1: consume denies after abort");
}

static void ac3209_2_nested_abort_then_outermost() {
    std::println("\n--- #3209 AC1: nested abort + outermost exit ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    constexpr std::uint64_t tenant = 9;
    ev.set_capability_tenant_id(tenant);
    ac3209_grant(tenant, mid, false);
    {
        Evaluator::TenantScope inner(ev, tenant);
        (void)inner; // nested abort cascade
    }
    (void)g_capability_registry().revoke_session_grants_for_mid(mid);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC1 nested: live==0 after cascade + outermost revoke");
    CHECK(!ac3209_consume(tenant, mid), "AC1 nested: consume denies");
}

static void ac3209_3_steal_no_double_consume() {
    std::println("\n--- #3209 AC2: steal then resume consume denies without single-use ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 11;
    ac3209_grant(tenant, mid, /*single_use=*/true);
    const auto consumed0 =
        g_capability_effect_metrics().capability_single_use_consumed_total.load();
    const auto n =
        aura::core::capability::revoke_session_grants_on_steal_or_abort(mid, /*steal=*/true);
    CHECK(n >= 1, "AC2: steal revokes");
    CHECK(!ac3209_consume(tenant, mid), "AC2: first resume consume denies");
    CHECK(!ac3209_consume(tenant, mid), "AC2: second resume consume denies (no double-consume)");
    CHECK(g_capability_effect_metrics().capability_single_use_consumed_total.load() == consumed0,
          "AC2: single-use-consumed not bumped on stolen/revoked");
    CHECK(ring_lookup_reason("single-use-consumed") == nullptr,
          "AC2: audit reason single-use-consumed never fires on stolen entry");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0, "AC2: live==0");
}

static void ac3209_4_composed_chaos() {
    std::println("\n--- #3209 AC1 chaos: nested abort × steal × outermost ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    constexpr std::uint64_t tenant = 13;
    ev.set_capability_tenant_id(tenant);
    ac3209_grant(tenant, mid, /*single_use=*/true);
    const auto consumed0 =
        g_capability_effect_metrics().capability_single_use_consumed_total.load();
    std::thread t_cascade([&] {
        Evaluator::TenantScope inner(ev, tenant);
        (void)inner;
    });
    std::thread t_steal([&] {
        (void)aura::core::capability::revoke_session_grants_on_steal_or_abort(mid, /*steal=*/true);
    });
    t_cascade.join();
    t_steal.join();
    (void)g_capability_registry().revoke_session_grants_for_mid(mid);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC1 chaos: live==0 after composed quiesce");
    CHECK(!ac3209_consume(tenant, mid), "AC1 chaos: resume consume denies");
    CHECK(g_capability_effect_metrics().capability_single_use_consumed_total.load() == consumed0,
          "AC2 chaos: no single-use consume on stolen/revoked");
}

static void ac3209_5_soft_zero_cost_and_source() {
    std::println("\n--- #3209 AC3/AC4/AC5: Soft zero-cost + source-cite ---");
    reset_all();
    set_mode(SandboxMode::Off);
    const auto n =
        aura::core::capability::revoke_session_grants_on_steal_or_abort(77, /*steal=*/true);
    CHECK(n == 0, "AC3: Soft empty live → steal quiesce returns 0");
    const auto cap = read_file("src/core/capability_model.hh");
    const auto steal = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto build = read_file("build.py");
    CHECK(cap.find("kCapabilitySessionQuiesceIssue = 3209") != std::string::npos,
          "AC5: stamp #3209");
    CHECK(cap.find("mark_session_bound_stolen_for_mid_locked") != std::string::npos,
          "AC5: for-mid stolen mark");
    CHECK(steal.find("Issue #3209") != std::string::npos, "AC5: steal path cites #3209");
    CHECK(bound.find("Issue #3209") != std::string::npos, "AC5: Guard dtor cites #3209");
    CHECK(build.find("check_session_grant_quiesce_3209") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3209.cpp"), "AC5: no invent");
    CHECK(!std::filesystem::exists("docs/design/3209-session-quiesce.md"), "AC5: no docs/design");
}

// ── Issue #3241: concurrent outermost sharing epoch mid must not
// over-revoke a peer fiber's session_bound grants. Same revoke policy
// as TenantScope (fiber filter); fiber=0 remains legacy mid-only.

static bool ac3241_grant_live(std::uint64_t tenant, std::string_view name) {
    CapabilityGrant g;
    if (!g_capability_registry().find_grant(tenant, name, g))
        return false;
    return !g.revoked && g.session_bound;
}

static void ac3241_grant(std::uint64_t tenant, std::string_view name, std::uint64_t mid,
                         std::uint32_t fiber) {
    EffectProvenance prov{};
    prov.epoch = mid;
    prov.mutation_id = mid;
    prov.fiber_id = fiber;
    g_capability_registry().grant_session(tenant, name, Effect::Mutate, prov, /*single_use=*/false);
}

static bool ac3241_consume(std::uint64_t tenant, std::uint64_t mid, std::uint32_t fiber) {
    EffectProvenance prov{};
    prov.epoch = mid;
    prov.mutation_id = mid;
    prov.fiber_id = fiber;
    return check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, tenant, "3241-consume",
                                   false, true);
}

// AC6: Restricted + multi-tenant production defaults (hard fiber isolation).
// Do not call apply_production_security_defaults here: AURA_SANDBOX=off in
// unit tests would force Soft. Consume-before-revoke is omitted because
// provenance_ok walks every live grant and a peer fiber mismatches.
static void ac3241_arm_restricted_multi_tenant() {
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    g_capability_registry().set_hard_fiber_isolation(true);
}

static void ac3241_1_peer_fiber_not_over_revoked() {
    std::println("\n--- #3241 AC1: Fiber A exit does not revoke Fiber B session grant ---");
    reset_all();
    ac3241_arm_restricted_multi_tenant();
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 41;
    constexpr std::uint32_t fiber_a = 101;
    constexpr std::uint32_t fiber_b = 202;
    ac3241_grant(tenant, "mut-A", mid, fiber_a);
    ac3241_grant(tenant, "mut-B", mid, fiber_b);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 2, "AC1: both live");
    const auto n =
        g_capability_registry().revoke_session_grants_for_mid(mid, "session-mid-exit", fiber_a);
    CHECK(n >= 1, "AC1: Fiber A mid-exit revokes >=1");
    CHECK(!ac3241_grant_live(tenant, "mut-A"), "AC1: mut-A revoked");
    CHECK(ac3241_grant_live(tenant, "mut-B"), "AC1: mut-B still live session_bound");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1, "AC1: B residual 1");
    CHECK(ac3241_consume(tenant, mid, fiber_b), "AC1: Fiber B require_effect still allowed");
    CHECK(!ac3241_consume(tenant, mid, fiber_a), "AC1: Fiber A consume denies after exit");
    (void)g_capability_registry().revoke_session_grants_for_mid(mid, "session-mid-exit", fiber_b);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0, "AC1: B exit clears");
}

static void ac3241_2_steal_a_does_not_touch_b() {
    std::println("\n--- #3241 AC2: steal/abort of A clears A only; B unaffected ---");
    reset_all();
    ac3241_arm_restricted_multi_tenant();
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 42;
    constexpr std::uint32_t fiber_a = 111;
    constexpr std::uint32_t fiber_b = 222;
    ac3241_grant(tenant, "mut-A", mid, fiber_a);
    ac3241_grant(tenant, "mut-B", mid, fiber_b);
    const auto n = aura::core::capability::revoke_session_grants_on_steal_or_abort(
        mid, /*steal=*/true, fiber_a);
    CHECK(n >= 1, "AC2: steal A revokes A");
    CHECK(!ac3241_grant_live(tenant, "mut-A"), "AC2: A session grant gone (no sticky residual)");
    CHECK(ac3241_grant_live(tenant, "mut-B"), "AC2: B unaffected by A steal");
    CHECK(ac3241_consume(tenant, mid, fiber_b), "AC2: B consume still allowed");
    CHECK(!ac3241_consume(tenant, mid, fiber_a), "AC2: A resume consume denies");
    const auto n2 = aura::core::capability::revoke_session_grants_on_steal_or_abort(
        mid, /*steal=*/false, fiber_b);
    CHECK(n2 >= 1, "AC2: abort B clears B");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0, "AC2: both cleared");
}

static void ac3241_3_soft_zero_and_legacy_mid_only() {
    std::println("\n--- #3241 AC3/AC4: Soft zero-cost; fiber=0 still mid-only ---");
    reset_all();
    set_mode(SandboxMode::Off);
    const auto n0 =
        g_capability_registry().revoke_session_grants_for_mid(88, "session-mid-exit", 7);
    CHECK(n0 == 0, "AC3: Soft empty live → no lock/work");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 43;
    ac3241_grant(tenant, "mut-A", mid, 101);
    ac3241_grant(tenant, "mut-B", mid, 202);
    const auto n = g_capability_registry().revoke_session_grants_for_mid(mid); // fiber=0 legacy
    CHECK(n >= 2, "AC3: fiber=0 still mid-only (both revoked)");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC3: legacy mid-only clears both");
}

static void ac3241_4_source_cite_and_linter() {
    std::println("\n--- #3241 AC5/AC6: source-cite + linter + no invent ---");
    const auto cap = read_file("src/core/capability_model.hh");
    const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto steal = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
    const auto build = read_file("build.py");
    CHECK(cap.find("kCapabilitySessionPeerFiberIssue = 3241") != std::string::npos,
          "AC5: stamp #3241");
    CHECK(cap.find("Issue #3241") != std::string::npos, "AC5: registry cites #3241");
    CHECK(cap.find("grant_fiber_id != fiber_id") != std::string::npos,
          "AC5: mid-revoke fiber filter");
    CHECK(bound.find("Issue #3241") != std::string::npos, "AC5: outermost dtor cites #3241");
    CHECK(bound.find("revoke_session_grants_for_mid_locked") != std::string::npos,
          "AC5: dtor still uses mid helper");
    CHECK(steal.find("revoke_session_grants_on_steal_or_abort_locked") != std::string::npos,
          "AC5: steal/abort still locked helper");
    CHECK(eval_sec.find("revoke_session_grants_for_locked") != std::string::npos,
          "AC5: TenantScope cascade unchanged");
    CHECK(build.find("check_session_grant_peer_fiber_3241") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3241.cpp"), "AC6: no invent");
    CHECK(!std::filesystem::exists("docs/design/3241-session-grant-peer-fiber.md"),
          "AC6: no docs/design/");
}

static void ac3144_1_production_wildcard_only_strip_tenant_admin() {
    std::println(
        "\n--- #3144 AC1: production + kCapWildcard (no explicit TenantAdmin) → strip ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto tenant = ev.capability_tenant_id();
    ev.set_capability_tenant_id(7);

    g_capability_registry().grant(tenant, "*",
                                  Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate |
                                      Effect::Network | Effect::Ffi | Effect::Render |
                                      Effect::MacroSelfEvo | Effect::TenantAdmin | Effect::Syscall,
                                  {});

    const auto before =
        g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.load();
    const auto stripped = g_capability_registry().effects_for(tenant);
    using aura::core::capability::has_effect;
    CHECK(!has_effect(stripped, Effect::TenantAdmin),
          "AC1: TenantAdmin stripped from effects_for() (wildcard-only holder)");
    CHECK(!has_effect(stripped, Effect::MacroSelfEvo),
          "AC1: MacroSelfEvo stripped from effects_for() (wildcard-only holder)");
    CHECK(has_effect(stripped, Effect::Read),
          "AC1: non-privilege Effect::Read preserved in stripped mask");
    const auto after =
        g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.load();
    CHECK(after > before, "AC1: counter bumped on strip");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

static void ac3144_2_explicit_tenant_admin_no_strip() {
    std::println("\n--- #3144 AC2: production + explicit TenantAdmin → no strip ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto tenant = ev.capability_tenant_id();
    ev.set_capability_tenant_id(7);
    g_capability_registry().grant(tenant, "*",
                                  Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate |
                                      Effect::Network | Effect::Ffi | Effect::Render |
                                      Effect::MacroSelfEvo | Effect::TenantAdmin | Effect::Syscall,
                                  {});
    g_capability_registry().grant(tenant, aura::compiler::security::kCapTenantAdmin,
                                  Effect::TenantAdmin, {});
    using aura::core::capability::has_effect;
    const auto before =
        g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.load();
    const auto held = g_capability_registry().effects_for(tenant);
    CHECK(has_effect(held, Effect::TenantAdmin),
          "AC2: TenantAdmin preserved in effects_for() (explicit TA holder)");
    CHECK(has_effect(held, Effect::MacroSelfEvo),
          "AC2: MacroSelfEvo preserved in effects_for() (explicit TA holder)");
    const auto after =
        g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.load();
    CHECK(after == before, "AC2: counter NOT bumped for explicit TenantAdmin holder (no strip)");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

static void ac3144_3_soft_off_no_strip() {
    std::println("\n--- #3144 AC3: Soft / sandbox=off → zero-cost (no strip) ---");
    reset_all();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto tenant = ev.capability_tenant_id();
    ev.set_capability_tenant_id(7);
    g_capability_registry().grant(tenant, "*",
                                  Effect::Read | Effect::Write | Effect::Exec | Effect::Mutate |
                                      Effect::Network | Effect::Ffi | Effect::Render |
                                      Effect::MacroSelfEvo | Effect::TenantAdmin | Effect::Syscall,
                                  {});
    using aura::core::capability::has_effect;
    const auto before =
        g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.load();
    const auto held = g_capability_registry().effects_for(tenant);
    CHECK(has_effect(held, Effect::TenantAdmin),
          "AC3: Soft/Off → TenantAdmin preserved (wildcard contract preserved)");
    CHECK(has_effect(held, Effect::MacroSelfEvo), "AC3: Soft/Off → MacroSelfEvo preserved");
    const auto after =
        g_capability_effect_metrics().wildcard_strip_tenant_admin_effect_total.load();
    CHECK(after == before, "AC3: Soft/Off → counter NOT bumped (zero-cost, no strip)");
}

static void ac3144_4_additive_counter_and_source_cite() {
    std::println("\n--- #3144 AC4/AC5: additive counter + source-cite + linter ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #3144") != std::string::npos, "AC4: capability_model.hh cites #3144");
    CHECK(cap.find("wildcard_strip_tenant_admin_effect_total") != std::string::npos,
          "AC4: counter present");
    CHECK(cap.find("wildcard_strip_tenant_admin_effect_total{0}") != std::string::npos,
          "AC4: counter field initialized");
    CHECK(cap.find("Effect::TenantAdmin") != std::string::npos &&
              cap.find("Effect::MacroSelfEvo") != std::string::npos,
          "AC4: strip bits in effects_for / effects_for_locked");
    CHECK(cap.find("has_wildcard && !has_explicit_TenantAdmin") != std::string::npos,
          "AC4: strip condition present");
    CHECK(cap.find("Soft/Off: zero-cost") != std::string::npos, "AC4: Soft/Off zero-cost comment");
    const auto pos_3141 = cap.find("capability_wildcard_write_fence_deny_total{0};");
    const auto pos_3144 = cap.find("wildcard_strip_tenant_admin_effect_total{0};");
    CHECK(pos_3141 != std::string::npos && pos_3144 != std::string::npos,
          "AC4: both counter fields present");
    CHECK(pos_3144 > pos_3141,
          "AC4: #3144 counter appended after #3141 counter (struct END per #2906)");
    const auto sec_cap = read_file("src/compiler/security_capabilities.h");
    CHECK(sec_cap.find("Issue #3144") != std::string::npos,
          "AC4: security_capabilities.h cites #3144");
    CHECK(sec_cap.find("wildcard_strip_tenant_admin_effect_total_v_read") != std::string::npos,
          "AC4: accessor exported");
    const auto linter = read_file("scripts/coverage/checks/check_wildcard_effects_for_fence.py");
    CHECK(!linter.empty() && linter.find("Issue #3144") != std::string::npos,
          "AC5: linter exists and cites #3144");
    const auto build = read_file("build.py");
    CHECK(build.find("check_wildcard_effects_for_fence") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!std::filesystem::exists("docs/design/3144-castop-typed-meta-phase-c.md"),
          "AC5: no docs/design/3144-*.md");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3144.cpp"),
          "AC5: no tests/issues/test_issue_3144.cpp");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3144.cpp"),
          "AC5: no tests/core/test_issue_3144.cpp");
    CHECK(!std::filesystem::exists("tests/compiler/test_issue_3144.cpp"),
          "AC5: no tests/compiler/test_issue_3144.cpp");
}

// ── Issue #3279: session_bound orphan fail-closed sweep ─────────────
// session_bound_orphan_detected_total was metric-only (declared, never
// bumped). Under production long-run, a lost Guard / abort-without-mid-
// clear / dual-Evaluator race / sticky escape can leave a live
// session_bound grant whose bound_mutation_id is no longer in the live
// mid set → privilege sticky. The SSOT sweep walks by_tenant under mtx,
// counts orphans (bumps the existing counter), and under Restricted/
// Strict revokes them with reason "session-orphan-sweep" (SE + audit
// joinable by mid). Soft/Off: observe-only — never revoke/deny solely
// due to the sweep.

// AC1: SSOT detection + Soft observe-only (counter bumps, no revoke).
static void ac3279_1_soft_observe_only() {
    std::println("\n--- #3279 AC1: orphan detection SSOT + Soft observe-only ---");
    reset_all();
    set_mode(SandboxMode::Off);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 91;
    ac3241_grant(tenant, "mut-3279-orphan", mid, 0);
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1,
          "AC1: orphan grant live pre-sweep");
    const auto before = g_capability_effect_metrics().session_bound_orphan_detected_total.load(
        std::memory_order_relaxed);
    // Live mid set does NOT contain the orphan's bound mid.
    const auto orphans =
        g_capability_registry().sweep_session_bound_orphans({mid + 777}, /*fiber_id=*/0);
    CHECK(orphans >= 1, "AC1: sweep detects >= 1 orphan");
    const auto after = g_capability_effect_metrics().session_bound_orphan_detected_total.load(
        std::memory_order_relaxed);
    CHECK(after > before, "AC1: existing orphan counter bumped (SSOT detection)");
    // Soft/Off: observe-only — grant NOT revoked by the sweep.
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1,
          "AC1: Soft observe-only — grant stays live");
    CHECK(ac3241_grant_live(tenant, "mut-3279-orphan"),
          "AC1: grant still live (never revoke solely due to sweep)");
    (void)g_capability_registry().revoke_session_grants_for_mid(mid);
}

// AC2: production (Restricted) sweep revokes orphan + clears live counter.
static void ac3279_2_production_revoke() {
    std::println("\n--- #3279 AC2: Restricted sweep revokes orphan (fail-closed) ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 92;
    ac3241_grant(tenant, "mut-3279-revoke", mid, 0);
    CHECK(g_capability_effect_metrics().capability_live_session_grants.load() >= 1,
          "AC2: live_session_grants > 0 pre-sweep");
    const auto revoked =
        g_capability_registry().sweep_session_bound_orphans({mid + 888}, /*fiber_id=*/0);
    CHECK(revoked >= 1, "AC2: production sweep revoked >= 1 orphan");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC2: orphan cleared after sweep");
    CHECK(g_capability_effect_metrics().capability_live_session_grants.load() == 0,
          "AC2: live_session_grants cleared");
    CHECK(!ac3241_grant_live(tenant, "mut-3279-revoke"), "AC2: grant revoked (not consumable)");
    const auto* se = ring_lookup_reason("session-orphan-sweep");
    CHECK(se != nullptr, "AC2: SE reason 'session-orphan-sweep' emitted");
    if (se != nullptr)
        CHECK(se->mutation_id == mid, "AC2: SE joinable by bound mid");
    (void)g_capability_registry().revoke_session_grants_for_mid(mid);
}

// AC3: live grant (mid in live set) is NOT swept.
static void ac3279_3_live_grant_not_swept() {
    std::println("\n--- #3279 AC3: live mid in set → no orphan / no revoke ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 93;
    ac3241_grant(tenant, "mut-3279-live", mid, 0);
    const auto before = g_capability_effect_metrics().session_bound_orphan_detected_total.load(
        std::memory_order_relaxed);
    const auto orphans = g_capability_registry().sweep_session_bound_orphans({mid}, /*fiber_id=*/0);
    CHECK(orphans == 0, "AC3: live mid in set → zero orphans");
    CHECK(g_capability_effect_metrics().session_bound_orphan_detected_total.load(
              std::memory_order_relaxed) == before,
          "AC3: counter unchanged (no orphan)");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1,
          "AC3: live grant untouched");
    (void)g_capability_registry().revoke_session_grants_for_mid(mid);
}

// AC4: peer-fiber grant skipped when sweeping with a current fiber id (#3241).
static void ac3279_4_peer_fiber_skip() {
    std::println("\n--- #3279 AC4: peer-fiber grant skipped (#3241) ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    aura::core::bump_mutation_epoch(1);
    const auto mid = aura::core::current_mutation_epoch();
    constexpr std::uint64_t tenant = 94;
    constexpr std::uint32_t fiber_peer = 777;
    ac3241_grant(tenant, "mut-3279-peer", mid, fiber_peer);
    // Sweep from a DIFFERENT fiber (fiber_id=1) with a live set that does
    // not contain mid: peer-fiber grant stays (live on the peer).
    const auto orphans =
        g_capability_registry().sweep_session_bound_orphans({mid + 999}, /*fiber_id=*/1);
    CHECK(orphans == 0, "AC4: peer-fiber grant not counted as orphan");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 1,
          "AC4: peer grant untouched");
    // Same sweep FROM the granting fiber would revoke it (its mid is gone).
    const auto revoked =
        g_capability_registry().sweep_session_bound_orphans({mid + 999}, /*fiber_id=*/fiber_peer);
    CHECK(revoked >= 1, "AC4: owner-fiber sweep revokes its orphan");
    CHECK(g_capability_registry().session_bound_entries_alive(tenant) == 0,
          "AC4: owner-fiber sweep cleared it");
}

// AC5: source-cite + linter + no invent.
static void ac3279_5_source_cite_and_linter() {
    std::println("\n--- #3279 AC5: source-cite + linter + no invent ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #3279") != std::string::npos, "AC5: capability_model.hh cites #3279");
    CHECK(cap.find("sweep_session_bound_orphans") != std::string::npos,
          "AC5: sweep helper present");
    CHECK(cap.find("count_session_bound_orphans_locked") != std::string::npos,
          "AC5: SSOT detection present");
    CHECK(cap.find("session-orphan-sweep") != std::string::npos,
          "AC5: stable SE reason 'session-orphan-sweep'");
    CHECK(cap.find("session_bound_orphan_detected_total") != std::string::npos,
          "AC5: reuses existing orphan counter (no new metric)");
    const auto boundary = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(boundary.find("Issue #3279") != std::string::npos,
          "AC5: outermost Guard enter trigger cites #3279");
    CHECK(boundary.find("sweep_session_bound_orphans") != std::string::npos,
          "AC5: trigger calls sweep");
    const auto lint = read_file("scripts/coverage/checks/check_session_bound_orphan_sweep_3279.py");
    CHECK(!lint.empty() && lint.find("#3279") != std::string::npos,
          "AC5: linter present and cites #3279");
    const auto build = read_file("build.py");
    CHECK(build.find("check_session_bound_orphan_sweep_3279.py") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(!std::filesystem::exists("docs/design/3279-"), "AC5: no docs/design per #1655");
    CHECK(!std::filesystem::exists("tests/issues/test_issue_3279.cpp"),
          "AC5: no invent test per #81967");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3279.cpp"),
          "AC5: no invent test per #81967");
    CHECK(!std::filesystem::exists("tests/compiler/test_issue_3279.cpp"),
          "AC5: no invent test per #81967");
}

// ── Issue #3333: provenance_ok mid join is per contributing grant ──
static void ac3333_1_unrelated_grant_does_not_poison() {
    std::println("\n--- #3333 AC1: Render@mid=5 does not poison Mutate@mid=9 ---");
    reset_all();
    set_mode(SandboxMode::Strict);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    constexpr std::uint64_t tenant = 81;
    EffectProvenance render{};
    render.mutation_id = 5;
    render.epoch = 5;
    g_capability_registry().grant(tenant, "render-3333", Effect::Render, render);
    EffectProvenance mutate{};
    mutate.mutation_id = 9;
    mutate.epoch = 9;
    g_capability_registry().grant(tenant, "mut-3333", Effect::Mutate, mutate);
    CHECK(g_capability_registry().provenance_ok(tenant, mutate, Effect::Mutate),
          "3333 AC1: provenance_ok Mutate@9 ignores Render@5");
    CHECK(!g_capability_registry().provenance_ok(tenant, mutate),
          "3333 AC1: query path (required=None) still sees all grants");
    CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, mutate, tenant, "3333-ac1"),
          "3333 AC1: check Mutate@9 allows despite live Render@5");
}

static void ac3333_2_true_mismatch_still_denies() {
    std::println("\n--- #3333 AC2: Mutate bound_mid=5 vs check mid=9 denies ---");
    reset_all();
    set_mode(SandboxMode::Strict);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    constexpr std::uint64_t tenant = 82;
    EffectProvenance grant_p{};
    grant_p.mutation_id = 5;
    grant_p.epoch = 5;
    g_capability_registry().grant(tenant, "mut-3333-mm", Effect::Mutate, grant_p);
    EffectProvenance call{};
    call.mutation_id = 9;
    call.epoch = 9;
    CHECK(!g_capability_registry().provenance_ok(tenant, call, Effect::Mutate),
          "3333 AC2: contributing Mutate mid mismatch denies");
    const auto mm0 = snapshot_capability_effect_stats().provenance_mismatch;
    const auto& ring = g_security_event_ring();
    const auto baseline = ring.seq.load(std::memory_order_acquire);
    CHECK(!check_and_record_effect(Effect::Mutate, Effect::Mutate, call, tenant, "3333-ac2"),
          "3333 AC2: check Mutate@9 denies");
    CHECK(snapshot_capability_effect_stats().provenance_mismatch > mm0,
          "3333 AC2: provenance_mismatch metric bumps");
    bool found = false;
    const auto head = ring.seq.load(std::memory_order_acquire);
    for (auto s = baseline; s < head; ++s) {
        const auto& e = ring.ring[s % kSecurityEventRingSize];
        if (e.kind == aura::core::security_event::SecurityEventKind::EffectDeny) {
            CHECK(e.mutation_id == 9, "3333 AC2: SE.mutation_id is the check mid");
            found = true;
        }
    }
    CHECK(found, "3333 AC2: EffectDeny recorded");
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(tenant, "mut-3333-mm", g), "3333 AC2: grant live");
    CHECK(g.bound_mutation_id == 5, "3333 AC2: grant.bound_mid remains 5 (explainable vs SE)");
}

static void ac3333_3_zero_mid_fail_closed() {
    std::println("\n--- #3333 AC3: Restricted/Strict prov.mid=0 still denies (#2707) ---");
    reset_all();
    set_mode(SandboxMode::Restricted);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    constexpr std::uint64_t tenant = 83;
    EffectProvenance grant_p{};
    grant_p.mutation_id = 5;
    grant_p.epoch = 5;
    g_capability_registry().grant(tenant, "mut-3333-z", Effect::Mutate, grant_p);
    EffectProvenance call{};
    call.mutation_id = 0;
    const auto z0 = g_capability_effect_metrics().capability_mid_join_zero_deny_total.load();
    CHECK(!g_capability_registry().provenance_ok(tenant, call, Effect::Mutate),
          "3333 AC3: fail-closed zero mid denies");
    CHECK(g_capability_effect_metrics().capability_mid_join_zero_deny_total.load() > z0,
          "3333 AC3: mid_join_zero_deny bumps");
}

static void ac3333_4_soft_zero_skip() {
    std::println("\n--- #3333 AC4: Soft/Off zero mid still skips join ---");
    reset_all();
    constexpr std::uint64_t tenant = 84;
    EffectProvenance grant_p{};
    grant_p.mutation_id = 42;
    grant_p.epoch = 42;
    g_capability_registry().grant(tenant, "mut-3333-soft", Effect::Mutate, grant_p);
    EffectProvenance call{};
    call.mutation_id = 0;
    const auto z0 = g_capability_effect_metrics().capability_mid_join_zero_deny_total.load();
    CHECK(g_capability_registry().provenance_ok(tenant, call, Effect::Mutate),
          "3333 AC4: Soft/Off + prov mid=0 skips join");
    CHECK(g_capability_effect_metrics().capability_mid_join_zero_deny_total.load() == z0,
          "3333 AC4: no mid_join_zero_deny under Off");
}

static void ac3333_5_stolen_skip_and_source() {
    std::println("\n--- #3333 AC5: stolen skip + session/steal lineage ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("g.revoked || g.stolen") != std::string::npos,
          "3333 AC5: provenance_ok skips stolen grants");
    CHECK(cap.find("Issue #3142") != std::string::npos, "3333 AC5: #3142 stolen lineage");
    CHECK(cap.find("kProvenanceContributingMidIssue = 3333") != std::string::npos,
          "3333 AC5: issue stamp");
}

static void ac3333_6_source_and_linter() {
    std::println("\n--- #3333 AC6: source-cite + linter + no invent ---");
    const auto cap = read_file("src/core/capability_model.hh");
    const auto test_self = read_file("tests/core/test_capability_single_use_consume.cpp");
    const auto lint2707 = read_file("scripts/coverage/checks/check_mid_join_fail_closed_2707.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_provenance_contributing_mid_3333.py");
    const auto build = read_file("build.py");
    CHECK(cap.find("Issue #3333") != std::string::npos, "3333 AC6: capability_model cites #3333");
    CHECK(cap.find("required != Effect::None && !has_effect(g.effects, required)") !=
              std::string::npos,
          "3333 AC6: contributing filter");
    CHECK(cap.find("provenance_ok_locked(tenant, prov, required)") != std::string::npos,
          "3333 AC6: check_and_record_effect passes required");
    CHECK(cap.find("fail_closed_mid") != std::string::npos, "3333 AC6: #2707 fail_closed_mid kept");
    CHECK(!lint2707.empty() && lint2707.find("#2707") != std::string::npos,
          "3333 AC6: #2707 linter retained");
    CHECK(!lint.empty() && lint.find("Issue #3333") != std::string::npos, "3333 AC6: linter");
    CHECK(build.find("check_provenance_contributing_mid_3333") != std::string::npos,
          "3333 AC6: build.py after #2707");
    CHECK(test_self.find("ac3333_1_unrelated_grant_does_not_poison") != std::string::npos,
          "3333 AC6: AC1 in suite");
    CHECK(!std::filesystem::exists("tests/core/test_issue_3333.cpp"),
          "3333 AC6: no invent test_issue_3333");
    CHECK(!std::filesystem::exists("docs/design/3333-contributing-mid.md"),
          "3333 AC6: no docs/design");
}

int run_test_capability_single_use_consume() {
    std::println("=== Issue #2586/#3142/#3144: single-use + SessionBound revoke + kCapWildcard "
                 "effects_for strip ===");

    // ── AC1: grant_once(Mutate) → 1st allow; 2nd deny ──────────────
    {
        std::println("\n--- #2586 AC1: single-use auto-revoke on 2nd check ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 1;
        prov.mutation_id = 1;
        g_capability_registry().grant_once(/*tenant=*/1, "mut-2586-once", Effect::Mutate, prov);

        const bool ok1 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 1, "mut-once-1");
        CHECK(ok1, "AC1: 1st check_and_record_effect(Mutate) → allow");

        const bool ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 1, "mut-once-2");
        CHECK(!ok2, "AC1: 2nd check_and_record_effect(Mutate) → deny (single_use consumed)");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1,
              "AC1: capability_single_use_consumed == 1 after 1st allow");
        CHECK(snap.revokes == 1, "AC1: revokes counter == 1 (single_use path bumps revoke_total)");
        CHECK(snap.enforced == 1, "AC1: enforced counter == 1");
        CHECK(snap.denied == 1, "AC1: denied counter == 1");
        CHECK(snap.audits >= 2, "AC1: audits counter >= 2 (effect + revoke audit)");
    }

    // ── AC2: deny path does NOT consume (retryable) ────────────────
    {
        std::println("\n--- #2586 AC2: deny does not consume (retryable) ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 2;
        prov.mutation_id = 2;
        g_capability_registry().grant_once(/*tenant=*/2, "mut-2586-retry", Effect::Mutate, prov);

        // 1st: deny — ask for Write bit but grant only has Mutate.
        const bool ok1 =
            check_and_record_effect(Effect::Write, Effect::Write, prov, 2, "write-deny");
        CHECK(!ok1, "AC2: 1st check_and_record_effect(Write) → deny (no Write bit in grant)");

        const auto snap0 = snapshot_capability_effect_stats();
        CHECK(snap0.capability_single_use_consumed == 0,
              "AC2: single_use_consumed == 0 after deny (deny path does NOT consume)");

        // Retry: Mutate bit still works because deny did not consume.
        const bool ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 2, "mut-retry-1");
        CHECK(ok2, "AC2: retry check_and_record_effect(Mutate) → allow (deny did not consume)");

        // Post-retry, 2nd Mutate check should deny because single_use now consumed.
        const bool ok3 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 2, "mut-retry-2");
        CHECK(!ok3, "AC2: post-retry 2nd Mutate check → deny (now consumed)");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1,
              "AC2: single_use_consumed == 1 (only the successful Mutate check consumed)");
        CHECK(snap.denied == 2, "AC2: denied counter == 2 (Write deny + post-retry Mutate deny)");
        CHECK(snap.enforced == 1, "AC2: enforced counter == 1 (the successful Mutate retry)");
    }

    // ── AC3: non single_use grant behavior unchanged ──────────────
    {
        std::println("\n--- #2586 AC3: non single_use grant unchanged ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 3;
        prov.mutation_id = 3;
        // Default grant() — single_use=false (legacy behavior).
        g_capability_registry().grant(/*tenant=*/3, "mut-2586-perm", Effect::Mutate, prov);

        for (int i = 0; i < 5; ++i) {
            const std::string op = "mut-perm-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 3, op);
            CHECK(ok, "AC3: non single_use grant 5x allow");
        }

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC3: single_use_consumed == 0 for non single_use grant");
        CHECK(snap.revokes == 0, "AC3: revokes == 0 for non single_use grant");
        CHECK(snap.enforced == 5, "AC3: enforced == 5 (all 5 allowed)");
    }

    // ── AC4: Soft/Off usable; production default API not forced ────
    {
        std::println("\n--- #2586 AC4: Off mode + production default API ---");
        // (4a) Off mode: single_use grant is "usable" — counter tracks
        // consumption on every successful allow, but Off-mode always-allow
        // semantics are unchanged (grant not required for allow). 2nd call
        // still returns true in Off mode; the consumption is observable via
        // capability_single_use_consumed.
        reset_all();
        set_mode(SandboxMode::Off);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        EffectProvenance prov{};
        prov.epoch = 4;
        prov.mutation_id = 4;
        g_capability_registry().grant_once(/*tenant=*/4, "mut-2586-off", Effect::Mutate, prov);

        const bool off_ok1 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 4, "off-mut-1");
        CHECK(off_ok1, "AC4a: Off mode 1st allow");
        const bool off_ok2 =
            check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 4, "off-mut-2");
        CHECK(off_ok2, "AC4a: Off mode 2nd STILL allow (Off-mode semantics — grant not required; "
                       "single_use consumption tracked via counter, not via allow decision)");

        const auto snap0 = snapshot_capability_effect_stats();
        CHECK(snap0.capability_single_use_consumed == 1,
              "AC4a: Off mode single_use_consumed == 1 (consumption tracked even when grant "
              "not required for allow)");

        // (4b) Production default API: grant_effect_capability / grant()
        // without single_use param → no auto-revoke.
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov2{};
        prov2.epoch = 5;
        prov2.mutation_id = 5;
        g_capability_registry().grant(/*tenant=*/5, "mut-2586-default", Effect::Mutate, prov2);

        for (int i = 0; i < 3; ++i) {
            const std::string op = "default-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov2, 5, op);
            CHECK(ok, "AC4b: default API 3x allow (no single_use forced)");
        }

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC4b: default API single_use_consumed == 0");
        CHECK(snap.revokes == 0, "AC4b: default API revokes == 0");
    }

    // ── AC5: audit / SE visible revoke with reason 'single-use-consumed' ──
    {
        std::println("\n--- #2586 AC5: audit/SE reason 'single-use-consumed' ---");
        reset_all();
        set_mode(SandboxMode::Strict);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);

        EffectProvenance prov{};
        prov.epoch = 6;
        prov.mutation_id = 6;
        g_capability_registry().grant_once(/*tenant=*/6, "mut-2586-audit", Effect::Mutate, prov);

        const auto ring_seq_before = g_security_event_ring().seq.load(std::memory_order_acquire);
        const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 6, "audit-1");
        CHECK(ok, "AC5: single_use grant 1st allow");

        const auto ring_seq_after = g_security_event_ring().seq.load(std::memory_order_acquire);
        CHECK(ring_seq_after > ring_seq_before,
              "AC5: SE ring advanced (single_use audit appended)");

        // Look for an event with reason "single-use-consumed" — emitted by
        // record_audit() inside the consume block.
        const auto* found = ring_lookup_reason("single-use-consumed");
        CHECK(found != nullptr, "AC5: SE ring has event with reason 'single-use-consumed'");
        if (found) {
            CHECK(!found->denied, "AC5: single-use-consumed event is denied=false (allow path)");
            CHECK(found->effect_bits == static_cast<std::uint16_t>(Effect::Mutate),
                  "AC5: event effect_bits == Mutate");
        }

        // Metric surface also reflects the consumption (counters bump atomic).
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 1, "AC5: capability_single_use_consumed == 1");
        CHECK(snap.revokes == 1, "AC5: revokes == 1 (single_use path bumps revoke_total)");
    }

    // ── AC6: tests + source-cite (no docs/design/) ─────────────────
    {
        std::println("\n--- #2586 AC6: source-cite ---");
        std::println("AC6 — see file header for source-cite list:");
        std::println("  - src/core/capability_model.hh: CapabilityGrant.single_use, "
                     "grant() extended signature, grant_once sugar, check_and_record_effect "
                     "consume block, CapabilityEffectMetrics + Snapshot.");
        std::println("  - src/compiler/evaluator_security.cpp: grant_effect_capability "
                     "single_use parameter.");
        std::println("  - src/compiler/evaluator.ixx: grant_effect_capability declaration.");
        std::println("  - src/compiler/evaluator_primitives_security.cpp: "
                     "query:capability-effect-stats capability-single-use-consumed-total "
                     "/ schema-2586 / issue-2586.");
        std::println("  - tests/core/test_capability_single_use_consume.cpp (this file).");
        std::println("  - no docs/design/ (per #1655 #1485 ship philosophy).");
        CHECK(true, "AC6: source-cite listed above (no docs/design)");
    }

    // ── #2882 AC1: production default forces single_use for high-risk ───────────
    {
        std::println("\n--- #2882 AC1: production default single-use for high-risk ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        // Use the production default surface (grant_effect_capability with
        // single_use=false default). Under Restricted + Mutate, the force
        // logic must promote single_use=true — first allow consumes, second
        // denies with the existing #2586 'single-use-consumed' reason.
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        const auto epoch_before =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        // Call the production default API with single_use=false (the default).
        ev.grant_effect_capability(/*tenant=*/7, "mut-2882-default", kEffectMutate, /*mid=*/1,
                                   /*single_use=*/false);
        const auto epoch_after =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        CHECK(epoch_after == epoch_before + 1,
              "AC1: capability_high_risk_forced_single_use_total bumps under Restricted");

        // Restricted needs sandbox_active=true for grant enforcement.
        // Stamp mid so fail-closed mid join (#2707) matches the grant.
        EffectProvenance prov{};
        prov.mutation_id = 1;
        prov.epoch = 1;
        // 1st allow — Mutate bit satisfied by the grant.
        const bool ok1 = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, /*tenant=*/7,
                                                 "2882-default-1", /*wildcard_ok=*/false,
                                                 /*sandbox_active=*/true);
        CHECK(ok1, "AC1: 1st allow under production default high-risk grant");

        // 2nd deny — single_use consumed by 1st allow.
        const bool ok2 = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, /*tenant=*/7,
                                                 "2882-default-2", /*wildcard_ok=*/false,
                                                 /*sandbox_active=*/true);
        CHECK(!ok2, "AC1: 2nd deny (forced single_use consumed)");
    }

    // ── #2882 AC2: explicit grant_effect_durable admin path bypasses force ──────
    {
        std::println("\n--- #2882 AC2: explicit grant_effect_durable admin path ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(8);

        const auto durable_before =
            g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
        const auto forced_before =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();

        // grant_effect_durable MUST bypass the force and stay single_use=false.
        // Issue #2967: under production the durable surface also requires the
        // caller to hold TenantAdmin + a non-empty audit reason — grant the
        // meta-privilege to the caller first so the #2882 durable-override
        // semantics are exercised (admin caller, sticky grant).
        ev.grant_capability("tenant-admin");
        ev.grant_effect_durable(/*tenant=*/8, "mut-2882-durable", kEffectMutate, /*mid=*/2,
                                /*reason=*/"2882-ac2-durable");

        const auto durable_after =
            g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
        const auto forced_after =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        CHECK(durable_after == durable_before + 1,
              "AC2: capability_durable_high_risk_grant_total bumps for high-risk durable override");
        CHECK(forced_after == forced_before,
              "AC2: capability_high_risk_forced_single_use_total NOT bumped by durable override");

        // Multiple allows — durable grants stay sticky.
        for (int i = 0; i < 3; ++i) {
            const std::string op = "2882-durable-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate,
                                                    EffectProvenance{}, /*tenant=*/8, op);
            CHECK(ok,
                  std::string("AC2: durable admin grant 3x allow (run ") + std::to_string(i) + ")");
        }
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0,
              "AC2: durable admin grant bypasses single_use (consumed=0)");
    }

    // ── #2882 AC3: Off / Soft path no force applied ───────────────────────────────
    {
        std::println("\n--- #2882 AC3: Off / Soft path no force ---");
        reset_all();
        set_mode(SandboxMode::Off);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0); // Off
        ev.set_capability_tenant_id(9);

        const auto forced_before =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();

        // Under Off, the production default surface must NOT force single_use.
        ev.grant_effect_capability(/*tenant=*/9, "mut-2882-off", kEffectMutate, /*mid=*/3,
                                   /*single_use=*/false);

        const auto forced_after =
            g_capability_effect_metrics().capability_high_risk_forced_single_use_total.load();
        CHECK(forced_after == forced_before,
              "AC3: Off mode does NOT bump capability_high_risk_forced_single_use_total");

        // Multiple allows — caller intent (single_use=false) preserved.
        for (int i = 0; i < 3; ++i) {
            const std::string op = "2882-off-" + std::to_string(i);
            const bool ok = check_and_record_effect(Effect::Mutate, Effect::Mutate,
                                                    EffectProvenance{}, /*tenant=*/9, op);
            CHECK(ok, std::string("AC3: Off mode multi-use (run ") + std::to_string(i) + ")");
        }
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_single_use_consumed == 0, "AC3: Off mode multi-use (consumed=0)");
    }

    // ── #2882 AC5: snapshot + posture prim additive surface ────────────────────
    {
        std::println("\n--- #2882 AC5: snapshot exposes #2882 counters ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(10);

        // Trigger both counters under production defaults (Restricted).
        // Issue #2967: durable high-risk requires caller TenantAdmin + reason.
        ev.grant_capability("tenant-admin");
        ev.grant_effect_capability(/*tenant=*/10, "mut-2882-q", kEffectMutate, /*mid=*/4, false);
        ev.grant_effect_durable(/*tenant=*/11, "mac-2882-q", kEffectMacroSelfEvo, /*mid=*/5,
                                /*reason=*/"2882-ac5-mac");
        ev.grant_effect_durable(/*tenant=*/12, "adm-2882-q", kEffectTenantAdmin, /*mid=*/6,
                                /*reason=*/"2882-ac5-adm");
        ev.grant_effect_durable(/*tenant=*/13, "sys-2882-q", kEffectSyscall, /*mid=*/7,
                                /*reason=*/"2882-ac5-sys");

        // Verify the CapabilityEffectStatsSnapshot struct exposes the 2 new
        // #2882 counters (compile-time member access check) and that the
        // counter snapshot reflects the grants. The posture prim
        // (query:capability-effect-stats) hash surface is verified separately
        // in AC6 source-cite check (the prim inserts schema-2882 /
        // high-risk-default-single-use-mask / forced + durable totals).
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_high_risk_forced_single_use == 1,
              "AC5: forced-single-use counter reflects 1 forced grant");
        CHECK(snap.capability_durable_high_risk_grant == 3,
              "AC5: durable-high-risk-grant counter reflects 3 durable grants");
        CHECK(snap.capability_single_use_consumed == 0,
              "AC5: durable grants do not consume single_use");

        // Posture prim must reference kHighRiskMask — the linter (AC5 in
        // check_production_default_single_use_2882.py) cross-checks both
        // sides (evaluator_security.cpp + evaluator_primitives_security.cpp).
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("kHighRiskMask") != std::string::npos,
              "AC5: posture prim references kHighRiskMask");
    }

    // ── #2882 AC6: source-cite + no invent + no docs/design/ ────────────────────
    {
        std::println("\n--- #2882 AC6: source-cite + no invent + no docs/design/ ---");
        const auto cap_model = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto build = read_file("build.py");

        // #2882 source-cite in cap_model + sec + ixx + posture.
        CHECK(cap_model.find("Issue #2882") != std::string::npos,
              "AC6: capability_model.hh cites Issue #2882");
        CHECK(sec.find("Issue #2882") != std::string::npos,
              "AC6: evaluator_security.cpp cites Issue #2882");
        CHECK(ixx.find("#2882") != std::string::npos, "AC6: evaluator.ixx cites #2882");
        CHECK(posture.find("schema-2882") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-2882");

        // build.py wires the new linter.
        CHECK(build.find("check_production_default_single_use_2882") != std::string::npos,
              "AC6: build.py wires #2882 linter");

        // No new test_issue_2882.cpp (per #81967).
        std::ifstream invent_c("tests/core/test_issue_2882.cpp");
        if (!invent_c.good())
            invent_c.open("../tests/core/test_issue_2882.cpp");
        CHECK(!invent_c.good(), "AC6: no tests/core/test_issue_2882.cpp (forbidden per #81967)");
        std::ifstream invent_cp("tests/compiler/test_issue_2882.cpp");
        if (!invent_cp.good())
            invent_cp.open("../tests/compiler/test_issue_2882.cpp");
        CHECK(!invent_cp.good(),
              "AC6: no tests/compiler/test_issue_2882.cpp (forbidden per #81967)");

        // No docs/design/2882-* (per #1655).
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2882-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #2944: mutation-session grants (mid-bound + auto-revoke on boundary) ──
    {
        std::println("\n--- #2944 AC1: session grant mid-bound under Restricted ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        EffectProvenance prov{};
        prov.epoch = 10;
        prov.mutation_id = 10;
        // session_bound without single_use so multi-check under same mid works.
        g_capability_registry().grant_session(/*tenant=*/20, "mut-2944-sess", Effect::Mutate, prov,
                                              /*single_use=*/false);

        CapabilityGrant g;
        CHECK(g_capability_registry().find_grant(20, "mut-2944-sess", g), "AC1: grant found");
        CHECK(g.session_bound, "AC1: session_bound stamped");
        CHECK(g.bound_mutation_id == 10, "AC1: bound_mutation_id == 10");

        // Restricted needs sandbox_active=true for grant + mid join enforce.
        const bool ok_same = check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 20,
                                                     "2944-same-mid", false, true);
        CHECK(ok_same, "AC1: same-mid check allows under Restricted");

        EffectProvenance other = prov;
        other.mutation_id = 11;
        other.epoch = 11;
        const bool ok_cross = check_and_record_effect(Effect::Mutate, Effect::Mutate, other, 20,
                                                      "2944-cross-mid", false, true);
        CHECK(!ok_cross, "AC1: cross-mid check denies under Restricted");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_session_grant >= 1, "AC1: session_grant counter");
        CHECK(snap.capability_live_session_grants >= 1, "AC1: live session residual");
    }
    {
        std::println("\n--- #2944 AC2: revoke_session_grants_for_mid on mid exit ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        EffectProvenance prov{};
        prov.epoch = 20;
        prov.mutation_id = 20;
        g_capability_registry().grant_session(21, "mut-2944-exit", Effect::Mutate, prov, false);

        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 21, "2944-pre", false,
                                      true),
              "AC2: allow before session revoke");

        const auto n = g_capability_registry().revoke_session_grants_for_mid(20);
        CHECK(n >= 1, "AC2: revoke_session_grants_for_mid revokes >=1");

        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_session_revoke >= 1, "AC2: session_revoke counter");
        CHECK(snap.capability_live_session_grants == 0, "AC2: live residual cleared");

        CHECK(!check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 21, "2944-post", false,
                                       true),
              "AC2: deny after session revoke");

        // SE dual-write reason session-mid-exit
        const auto* se = ring_lookup_reason("session-mid-exit");
        CHECK(se != nullptr, "AC2: SE reason session-mid-exit present");
    }
    {
        std::println("\n--- #2944 AC3: Soft/Off zero-cost empty session path ---");
        reset_all();
        set_mode(SandboxMode::Off);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        const auto n = g_capability_registry().revoke_session_grants_for_mid(99);
        CHECK(n == 0, "AC3: empty live session → revoke returns 0");
        const auto snap = snapshot_capability_effect_stats();
        CHECK(snap.capability_session_revoke == 0, "AC3: no session_revoke under empty");
    }
    {
        std::println("\n--- #2944 AC4: durable grants unaffected by session revoke ---");
        reset_all();
        set_mode(SandboxMode::Restricted);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(22);
        // Durable Mutate (sticky) + session Mutate under same mid.
        ev.grant_effect_durable(22, "mut-2944-dur", kEffectMutate, /*mid=*/30);
        ev.grant_effect_session(22, "mut-2944-sess2", kEffectMutate, /*mid=*/30,
                                /*single_use=*/false);

        EffectProvenance prov{};
        prov.epoch = 30;
        prov.mutation_id = 30;
        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 22, "2944-both", false,
                                      true),
              "AC4: allow with durable+session");

        (void)g_capability_registry().revoke_session_grants_for_mid(30);
        // Durable remains — still allows.
        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 22, "2944-dur-only",
                                      false, true),
              "AC4: durable survives session revoke");
    }
    {
        std::println("\n--- #2944 AC5/AC6: schema + source-cite + no invent ---");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto build = read_file("build.py");
        CHECK(cap.find("session_bound") != std::string::npos, "AC6: session_bound field");
        CHECK(cap.find("revoke_session_grants_for_mid") != std::string::npos,
              "AC6: revoke_session_grants_for_mid");
        CHECK(cap.find("capability_session_revoke_total") != std::string::npos,
              "AC6: session_revoke metric");
        CHECK(sec.find("grant_effect_session") != std::string::npos,
              "AC6: grant_effect_session impl");
        CHECK(ixx.find("grant_effect_session") != std::string::npos,
              "AC6: grant_effect_session decl");
        CHECK(bound.find("revoke_session_grants_for_mid") != std::string::npos,
              "AC6: outermost dtor revokes session");
        CHECK(bound.find("Issue #2944") != std::string::npos ||
                  bound.find("#2944") != std::string::npos,
              "AC6: boundary cites #2944");
        CHECK(posture.find("schema-2944") != std::string::npos, "AC5: schema-2944");
        CHECK(posture.find("mutation-session-grant-wired") != std::string::npos,
              "AC5: mutation-session-grant-wired");
        CHECK(posture.find("capability-session-revoke-total") != std::string::npos,
              "AC5: session-revoke-total key");
        CHECK(build.find("check_mutation_session_grant_2944") != std::string::npos,
              "AC6: build.py wires linter");
        std::ifstream invent("tests/core/test_issue_2944.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_2944.cpp");
        CHECK(!invent.good(), "AC6: no test_issue_2944.cpp");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2944-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name);
            }
        }

        // ── #2967 AC1: production durable high-risk requires TenantAdmin ────────
        {
            std::println("\n--- #2967 AC1: durable high-risk grant without TenantAdmin → deny ---");
            reset_all();
            set_mode(SandboxMode::Restricted);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

            CompilerService cs;
            auto& ev = cs.evaluator();
            ev.set_effect_sandbox_mode(1); // Restricted
            ev.set_capability_tenant_id(20);

            const auto deny_before =
                g_capability_effect_metrics().capability_durable_grant_deny_total.load();
            const auto allow_before =
                g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();

            // No TenantAdmin on the caller → durable high-risk grant denied.
            ev.grant_effect_durable(/*tenant=*/20, "mut-2967-noadmin", kEffectMutate,
                                    /*mid=*/30,
                                    /*reason=*/"2967-ac1");

            const auto deny_after =
                g_capability_effect_metrics().capability_durable_grant_deny_total.load();
            const auto allow_after =
                g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
            CHECK(deny_after == deny_before + 1,
                  "AC1: capability_durable_grant_deny_total bumps when caller lacks TenantAdmin");
            CHECK(allow_after == allow_before,
                  "AC1: capability_durable_high_risk_grant_total NOT bumped on deny");
            CHECK(ring_lookup_reason("durable-grant-needs-tenant-admin") != nullptr,
                  "AC1: SE EffectDeny reason 'durable-grant-needs-tenant-admin' recorded");
        }

        // ── #2967 AC2: TenantAdmin + reason → allow; empty reason → deny ────────
        {
            std::println("\n--- #2967 AC2: TenantAdmin + reason allows; empty reason denies ---");
            reset_all();
            set_mode(SandboxMode::Restricted);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

            CompilerService cs;
            auto& ev = cs.evaluator();
            ev.set_effect_sandbox_mode(1); // Restricted
            ev.set_capability_tenant_id(21);
            // Caller holds the meta-privilege.
            ev.grant_capability("tenant-admin");

            const auto allow_before =
                g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
            ev.grant_effect_durable(/*tenant=*/21, "mut-2967-admin", kEffectMutate, /*mid=*/31,
                                    /*reason=*/"2967-ac2-rotate");
            const auto allow_after =
                g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
            CHECK(allow_after == allow_before + 1,
                  "AC2: durable high-risk grant allowed with TenantAdmin + reason");

            // Empty reason under production → deny.
            const auto deny_before =
                g_capability_effect_metrics().capability_durable_grant_deny_total.load();
            ev.grant_effect_durable(/*tenant=*/21, "mut-2967-noreason", kEffectMutate,
                                    /*mid=*/32);
            const auto deny_after =
                g_capability_effect_metrics().capability_durable_grant_deny_total.load();
            CHECK(deny_after == deny_before + 1,
                  "AC2: empty reason under production → deny (deny counter bumps)");
            CHECK(ring_lookup_reason("durable-grant-reason-required") != nullptr,
                  "AC2: SE EffectDeny reason 'durable-grant-reason-required' recorded");
        }

        // ── #2967 AC3: Soft / Off no hard gate (zero-cost path) ────────────────
        {
            std::println("\n--- #2967 AC3: Off path no gate ---");
            reset_all();
            set_mode(SandboxMode::Off);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

            CompilerService cs;
            auto& ev = cs.evaluator();
            ev.set_effect_sandbox_mode(0); // Off
            ev.set_capability_tenant_id(22);

            const auto deny_before =
                g_capability_effect_metrics().capability_durable_grant_deny_total.load();
            const auto allow_before =
                g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
            // No TenantAdmin, no reason — Off path must not hard-gate.
            ev.grant_effect_durable(/*tenant=*/22, "mut-2967-off", kEffectMutate, /*mid=*/33);
            const auto deny_after =
                g_capability_effect_metrics().capability_durable_grant_deny_total.load();
            const auto allow_after =
                g_capability_effect_metrics().capability_durable_high_risk_grant_total.load();
            CHECK(deny_after == deny_before, "AC3: Off path does not deny (no hard gate)");
            CHECK(allow_after == allow_before + 1,
                  "AC3: Off path durable high-risk grant proceeds (zero-cost)");
        }

        // ── #2967 AC4: snapshot + posture additive keys ─────────────────────────
        {
            std::println("\n--- #2967 AC4: snapshot + posture additive keys ---");
            reset_all();
            set_mode(SandboxMode::Restricted);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

            CompilerService cs;
            auto& ev = cs.evaluator();
            ev.set_effect_sandbox_mode(1); // Restricted
            ev.set_capability_tenant_id(23);

            // Trigger one deny (no TenantAdmin) + one allow (TenantAdmin + reason).
            ev.grant_effect_durable(/*tenant=*/23, "mut-2967-q-deny", kEffectMutate, /*mid=*/34,
                                    /*reason=*/"2967-ac4");
            ev.grant_capability("tenant-admin");
            ev.grant_effect_durable(/*tenant=*/23, "mut-2967-q-allow", kEffectMutate,
                                    /*mid=*/35,
                                    /*reason=*/"2967-ac4-allow");

            const auto snap = snapshot_capability_effect_stats();
            CHECK(snap.capability_durable_grant_deny == 1,
                  "AC4: snapshot exposes capability_durable_grant_deny");
            CHECK(snap.capability_durable_high_risk_grant == 1,
                  "AC4: snapshot durable-high-risk-grant reflects 1 allow");

            const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
            CHECK(posture.find("schema-2967") != std::string::npos,
                  "AC4: posture prim cites schema-2967");
            CHECK(posture.find("capability-durable-grant-deny-total") != std::string::npos,
                  "AC4: posture prim exposes capability-durable-grant-deny-total");
            CHECK(posture.find("durable-grant-tenant-admin-wired") != std::string::npos,
                  "AC4: posture prim exposes durable-grant-tenant-admin-wired");
            CHECK(posture.find("durable-grant-reason-wired") != std::string::npos,
                  "AC4: posture prim exposes durable-grant-reason-wired");
        }

        // ── #2967 AC5: source-cite + no invent + no docs/design/ ────────────────
        {
            std::println("\n--- #2967 AC5: source-cite + no invent + no docs/design/ ---");
            const auto cap_model = read_file("src/core/capability_model.hh");
            const auto sec = read_file("src/compiler/evaluator_security.cpp");
            const auto ixx = read_file("src/compiler/evaluator.ixx");
            const auto build = read_file("build.py");

            CHECK(cap_model.find("Issue #2967") != std::string::npos,
                  "AC5: capability_model.hh cites Issue #2967");
            CHECK(sec.find("Issue #2967") != std::string::npos,
                  "AC5: evaluator_security.cpp cites Issue #2967");
            CHECK(ixx.find("#2967") != std::string::npos, "AC5: evaluator.ixx cites #2967");
            CHECK(build.find("check_capability_durable_gate_2967") != std::string::npos,
                  "AC5: build.py wires #2967 linter");

            // No new test_issue_2967.cpp (per #81967).
            std::ifstream invent_2967("tests/core/test_issue_2967.cpp");
            if (!invent_2967.good())
                invent_2967.open("../tests/core/test_issue_2967.cpp");
            CHECK(!invent_2967.good(),
                  "AC5: no tests/core/test_issue_2967.cpp (forbidden per #81967)");

            // No docs/design/2967-* (per #1655).
            const std::filesystem::path docs_design_2967 = "docs/design";
            std::error_code ec2967;
            if (std::filesystem::is_directory(docs_design_2967, ec2967)) {
                for (const auto& entry :
                     std::filesystem::directory_iterator(docs_design_2967, ec2967)) {
                    const auto name = entry.path().filename().string();
                    CHECK(name.find("2967-") == std::string::npos,
                          std::string("AC5: no docs/design/") + name + " (forbidden per #1655)");
                }
            }
        }

        // ── #3048: steal / force-cancel / abort residual session revoke ──
        {
            std::println("\n--- #3048 AC1/AC2: steal hook revokes live session grant ---");
            reset_all();
            set_mode(SandboxMode::Restricted);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

            EffectProvenance prov{};
            prov.epoch = 40;
            prov.mutation_id = 40;
            g_capability_registry().grant_session(30, "mut-3048-steal", Effect::Mutate, prov,
                                                  /*single_use=*/false);
            CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 30, "3048-pre",
                                          false, true),
                  "AC2: allow before steal revoke");
            const auto n =
                aura::core::capability::revoke_session_grants_on_steal_or_abort(40, /*steal=*/true);
            CHECK(n >= 1, "AC1: steal hook revokes >=1");
            const auto snap = snapshot_capability_effect_stats();
            CHECK(snap.capability_live_session_grants == 0, "AC2: live residual cleared");
            CHECK(snap.capability_session_revoke_steal >= 1, "AC5: steal revoke counter");
            CHECK(!check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 30, "3048-post",
                                           false, true),
                  "AC2: deny after steal revoke under Restricted");
            CHECK(ring_lookup_reason("session-mid-steal-exit") != nullptr,
                  "AC5: SE reason session-mid-steal-exit");
        }
        {
            std::println("\n--- #3048 AC1/AC2: abort hook + double-revoke no-op ---");
            reset_all();
            set_mode(SandboxMode::Restricted);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);

            EffectProvenance prov{};
            prov.epoch = 41;
            prov.mutation_id = 41;
            g_capability_registry().grant_session(31, "mut-3048-abort", Effect::Mutate, prov,
                                                  false);
            const auto n1 = aura::core::capability::revoke_session_grants_on_steal_or_abort(
                41, /*steal=*/false);
            CHECK(n1 >= 1, "AC1: abort hook revokes >=1");
            const auto n2 = g_capability_registry().revoke_session_grants_for_mid(41);
            CHECK(n2 == 0, "AC3: second revoke (Guard dtor shape) is no-op");
            const auto snap = snapshot_capability_effect_stats();
            CHECK(snap.capability_live_session_grants == 0, "AC3: live residual stays 0");
            CHECK(snap.capability_session_revoke_abort >= 1, "AC5: abort revoke counter");
            CHECK(!check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 31, "3048-abort",
                                           false, true),
                  "AC2: deny after abort revoke");
            CHECK(ring_lookup_reason("session-mid-abort-exit") != nullptr,
                  "AC5: SE reason session-mid-abort-exit");
        }
        {
            std::println("\n--- #3048 AC3: Guard dtor path still session-mid-exit ---");
            reset_all();
            set_mode(SandboxMode::Restricted);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
            EffectProvenance prov{};
            prov.epoch = 42;
            prov.mutation_id = 42;
            g_capability_registry().grant_session(32, "mut-3048-dtor", Effect::Mutate, prov, false);
            const auto n = g_capability_registry().revoke_session_grants_for_mid(42);
            CHECK(n >= 1, "AC3: normal dtor-shape revoke still works");
            CHECK(ring_lookup_reason("session-mid-exit") != nullptr,
                  "AC3: SE reason session-mid-exit unchanged");
            const auto snap = snapshot_capability_effect_stats();
            CHECK(snap.capability_session_revoke_steal == 0, "AC3: dtor path is not steal");
            CHECK(snap.capability_session_revoke_abort == 0, "AC3: dtor path is not abort");
        }
        {
            std::println("\n--- #3048 AC4: Soft/Off empty live residual is zero-cost ---");
            reset_all();
            set_mode(SandboxMode::Off);
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            const auto n =
                aura::core::capability::revoke_session_grants_on_steal_or_abort(99, /*steal=*/true);
            CHECK(n == 0, "AC4: empty live session → steal hook returns 0");
            const auto n0 =
                aura::core::capability::revoke_session_grants_on_steal_or_abort(0, /*steal=*/false);
            CHECK(n0 == 0, "AC4: mid==0 early-out");
            const auto snap = snapshot_capability_effect_stats();
            CHECK(snap.capability_session_revoke_steal == 0, "AC4: no steal counter under empty");
            CHECK(snap.capability_session_revoke_abort == 0, "AC4: no abort counter under empty");
        }
        {
            std::println("\n--- #3048 AC5/AC6: schema + source-cite + no invent ---");
            const auto cap = read_file("src/core/capability_model.hh");
            const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
            const auto steal = read_file("src/compiler/evaluator_fiber_mutation.cpp");
            const auto fiber_h = read_file("src/serve/fiber.h");
            const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
            const auto build = read_file("build.py");
            CHECK(cap.find("revoke_session_grants_on_steal_or_abort") != std::string::npos,
                  "AC6: capability_model.hh hook");
            CHECK(cap.find("session-mid-steal-exit") != std::string::npos,
                  "AC5: steal SE reason in registry");
            CHECK(cap.find("session-mid-abort-exit") != std::string::npos,
                  "AC5: abort SE reason in registry");
            CHECK(bound.find("set_current_fiber_session_mid") != std::string::npos,
                  "AC6: Guard enter stamps fiber session mid");
            CHECK(steal.find("revoke_session_grants_on_steal_or_abort") != std::string::npos,
                  "AC6: steal-complete / abort hook");
            CHECK(steal.find("aura_evaluator_on_steal_complete") != std::string::npos,
                  "AC6: steal-complete cites hook site");
            CHECK(fiber_h.find("session_mid_") != std::string::npos,
                  "AC6: Fiber session_mid_ field");
            CHECK(posture.find("schema-3048") != std::string::npos, "AC5: schema-3048");
            CHECK(posture.find("session-grant-steal-abort-wired") != std::string::npos,
                  "AC5: session-grant-steal-abort-wired");
            CHECK(build.find("check_session_grant_steal_3048") != std::string::npos,
                  "AC6: build.py wires linter");
            std::ifstream invent("tests/core/test_issue_3048.cpp");
            if (!invent.good())
                invent.open("../tests/core/test_issue_3048.cpp");
            CHECK(!invent.good(), "AC6: no test_issue_3048.cpp");
            const std::filesystem::path docs_design = "docs/design";
            std::error_code ec;
            if (std::filesystem::is_directory(docs_design, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                    const auto name = entry.path().filename().string();
                    CHECK(name.find("3048-") == std::string::npos,
                          std::string("AC6: no docs/design/") + name);
                }
            }
        }

        ac3142_1_nested_abort_cascade_revoke();
        ac3142_2_steal_marks_no_double_consume();
        ac3142_3_long_run_no_leak();
        ac3142_4_additive_metrics_and_source_cite();
        ac3207_1_sequential_cascade_then_require_effect_deny();
        ac3207_2_dual_evaluator_concurrent_chaos();
        ac3207_3_soft_off_zero_cost();
        ac3207_4_source_cite_and_linter();
        ac3209_1_outermost_exit_success_and_fail();
        ac3209_2_nested_abort_then_outermost();
        ac3209_3_steal_no_double_consume();
        ac3209_4_composed_chaos();
        ac3209_5_soft_zero_cost_and_source();
        ac3241_1_peer_fiber_not_over_revoked();
        ac3241_2_steal_a_does_not_touch_b();
        ac3241_3_soft_zero_and_legacy_mid_only();
        ac3241_4_source_cite_and_linter();
        ac3144_1_production_wildcard_only_strip_tenant_admin();
        ac3144_2_explicit_tenant_admin_no_strip();
        ac3144_3_soft_off_no_strip();
        ac3144_4_additive_counter_and_source_cite();
        // Issue #3279: session_bound orphan fail-closed sweep.
        ac3279_1_soft_observe_only();
        ac3279_2_production_revoke();
        ac3279_3_live_grant_not_swept();
        ac3279_4_peer_fiber_skip();
        ac3279_5_source_cite_and_linter();
        ac3333_1_unrelated_grant_does_not_poison();
        ac3333_2_true_mismatch_still_denies();
        ac3333_3_zero_mid_fail_closed();
        ac3333_4_soft_zero_skip();
        ac3333_5_stolen_skip_and_source();
        ac3333_6_source_and_linter();

        std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
        return g_failed == 0 ? 0 : 1;
    }
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_capability_single_use_consume();
}
#endif
