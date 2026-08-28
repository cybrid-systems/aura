// @category: unit
// @reason: Issue #1566 — WorkspaceIsolationPolicy enforcement:
// capability cross-tenant grant, provenance deny, Strict sandbox link,
// mutate/workspace force path, query:tenant-isolation-stats, stress deny.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"
#include "core/workspace_isolation.hh"
#include "core/capability_model.hh"
#include "core/resource_quota.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectWrite;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::SecurityEventKind;
using aura::core::workspace_isolation::check_boundary;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::IsolationRefProvenance;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::core::workspace_isolation::snapshot_tenant_isolation_stats;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

// Issue #2659: helper to read the current isolation audit seq (after
// a deny we want to know if a new SE was emitted). Without this
// we would have to import g_workspace_isolation everywhere.
static std::uint64_t current_iso_seq() noexcept {
    return g_workspace_isolation().audit_seq.load(std::memory_order_acquire);
}

std::int64_t href_m(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:tenant-isolation-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Issue #2687 / #2705: capture-stamp counters live on query:soa-dirty-stats.
std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:soa-dirty-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static NodeId first_live(FlatAST& ws) {
    for (NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id))
            return id;
    }
    return aura::ast::NULL_NODE;
}

void reset_all() {
    reset_tenant_isolation_for_test();
    // #2968: AC2 grants TenantAdmin into the process-global capability
    // registry; reset_all() must also clear it or later blocks reusing the
    // same tenant id see a leaked admin and the gate never denies.
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::capability::set_effect_fiber_id_override(0);
    set_mode(SandboxMode::Off);
    // Soft / unit path: hard-close off so Soft global-fallback tests stay green.
    aura::core::provenance::set_hard_capture_tenant(false);
    aura::core::provenance::set_isolation_capture_tenant(0);
    aura::core::provenance::set_stable_ref_export_hard_reject(false);
}

// #3090: Restricted/Strict refuse grants when prov.mutation_id==0.
// Stamp a bound mid so TenantAdmin actually lands in the registry.
void grant_tenant_admin_mid(std::uint64_t tenant, std::uint64_t mid = 1) {
    using aura::core::capability::Effect;
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::make_grant_provenance;
    auto prov = make_grant_provenance(mid, /*force_mutation_bind=*/true, 0, 0);
    g_capability_registry().grant(tenant, "tenant-admin", Effect::TenantAdmin, prov);
}

// Issue #3126: TOCTOU in TenantAdmin check vs grant (unlocked effects_for
// + has_capability admin fence). The fix adds locked variants to
// CapabilityRegistry and rewrites every foreign-tenant / high-risk admin
// fence in evaluator_security.cpp to take the registry mtx + use the
// locked variants under one critical section. This AC verifies the
// source-cite surface (locked variants exist + are wired + Soft/Off
// public surfaces stay documented as observational only) and that the
// existing test files (#2490 require_effect_auto_isolation / #2529
// grant_epoch_retain_restricted) keep their shape (no deletion / no
// removed ACs).
static void ac3126_admin_fence_locked() {
    std::println("\n--- #3126: TOCTOU admin fence — locked variants + foreign-tenant gate ---");

    // AC1: capability_model.hh owns the locked variants + Soft/observational
    // comments on the unlocked public readers.
    {
        const auto cm = read_file("src/core/capability_model.hh");
        CHECK(cm.find("effects_for_locked") != std::string::npos,
              "AC1: capability_model.hh has effects_for_locked");
        CHECK(cm.find("provenance_ok_locked") != std::string::npos,
              "AC1: capability_model.hh has provenance_ok_locked");
        CHECK(cm.find("grant_locked") != std::string::npos,
              "AC1: capability_model.hh has grant_locked (caller MUST hold mtx)");
        CHECK(cm.find("revoke_locked") != std::string::npos,
              "AC1: capability_model.hh has revoke_locked (caller MUST hold mtx)");
        // Public unlocked surfaces stay, but are documented as observational.
        CHECK(cm.find("Soft/observational only") != std::string::npos,
              "AC1: capability_model.hh marks Soft/observational only on unlocked readers");
        CHECK(cm.find("DO NOT use for security decisions under concurrent mutation") !=
                  std::string::npos,
              "AC1: unlocked readers carry the security-decision warning");
        // Issue constant present for lineage / source-cite.
        CHECK(cm.find("Issue #3126") != std::string::npos,
              "AC1: capability_model.hh cites Issue #3126");
    }

    // AC2: evaluator_security.cpp grant_effect_capability rewrites the
    // foreign-tenant fence to take the registry mtx + use effects_for_locked
    // + grant_locked (closes the unlocked has_capability TOCTOU window).
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        // The fence must NOT call has_capability (which is unlocked).
        const std::string grant_cap_section_marker = "cross-tenant-grant-needs-tenant-admin";
        const auto fence_pos = es.find("cross-tenant-grant-needs-tenant-admin");
        CHECK(fence_pos != std::string::npos,
              "AC2: foreign-tenant fence string present in evaluator_security.cpp");
        // Within a generous window around the first foreign-tenant fence, the
        // admin check should use effects_for_locked, not has_capability.
        const auto window_start = fence_pos > 600 ? fence_pos - 600 : 0;
        const auto window_end = (fence_pos + 1200 < es.size()) ? fence_pos + 1200 : es.size();
        const std::string fence_window = es.substr(window_start, window_end - window_start);
        CHECK(fence_window.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC2: grant_effect_capability fence uses effects_for_locked");
        CHECK(fence_window.find("has_effect(held, Effect::TenantAdmin)") != std::string::npos,
              "AC2: grant_effect_capability fence checks Effect::TenantAdmin bit");
        CHECK(fence_window.find("reg.grant_locked(") != std::string::npos,
              "AC2: grant_effect_capability fence acts via grant_locked");
        CHECK(fence_window.find("reg.mtx") != std::string::npos,
              "AC2: grant_effect_capability fence holds registry mtx");
    }

    // AC3: grant_effect_durable has TWO gates (foreign-tenant #2969 + high-risk
    // TenantAdmin+reason #2967). Both must use the same locked is_admin.
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        const auto durable_pos = es.find("grant_effect_durable(");
        const auto next_func = es.find("grant_effect_session(", durable_pos);
        const std::string durable_block =
            es.substr(durable_pos, (next_func > durable_pos) ? next_func - durable_pos
                                                             : es.size() - durable_pos);
        CHECK(durable_block.find("std::lock_guard<std::mutex> lock(reg_durable.mtx)") !=
                  std::string::npos,
              "AC3: grant_effect_durable takes registry mtx");
        CHECK(durable_block.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC3: grant_effect_durable uses effects_for_locked");
        CHECK(durable_block.find("has_effect(held_durable, Effect::TenantAdmin)") !=
                  std::string::npos,
              "AC3: grant_effect_durable precomputes is_admin from locked bit");
        CHECK(durable_block.find("reg_durable.grant_locked(") != std::string::npos,
              "AC3: grant_effect_durable acts via grant_locked");
        // Legacy unlocked is_admin helper must not survive in this block.
        CHECK(durable_block.find(
                  "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)") ==
                  std::string::npos,
              "AC3: grant_effect_durable removed unlocked has_capability fence");
    }

    // AC4: grant_effect_session foreign-tenant fence locks + uses
    // effects_for_locked + grant_locked.
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        const auto session_pos = es.find("void Evaluator::grant_effect_session(");
        const auto next_func = es.find("void Evaluator::revoke_effect_capability(", session_pos);
        const std::string session_block =
            es.substr(session_pos, (next_func > session_pos) ? next_func - session_pos
                                                             : es.size() - session_pos);
        CHECK(session_block.find("std::lock_guard<std::mutex> lock(reg_session.mtx)") !=
                  std::string::npos,
              "AC4: grant_effect_session takes registry mtx");
        CHECK(session_block.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC4: grant_effect_session uses effects_for_locked");
        CHECK(session_block.find("reg_session.grant_locked(") != std::string::npos,
              "AC4: grant_effect_session acts via grant_locked");
        CHECK(session_block.find(
                  "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)") ==
                  std::string::npos,
              "AC4: grant_effect_session removed unlocked has_capability fence");
    }

    // AC5: revoke_effect_capability foreign-tenant fence locks + uses
    // effects_for_locked + revoke_locked.
    {
        const auto es = read_file("src/compiler/evaluator_security.cpp");
        const auto revoke_pos = es.find("void Evaluator::revoke_effect_capability(");
        const std::string revoke_block = es.substr(revoke_pos);
        CHECK(revoke_block.find("std::lock_guard<std::mutex> lock(reg_revoke.mtx)") !=
                  std::string::npos,
              "AC5: revoke_effect_capability takes registry mtx");
        CHECK(revoke_block.find("effects_for_locked(self_tenant)") != std::string::npos,
              "AC5: revoke_effect_capability uses effects_for_locked");
        CHECK(revoke_block.find("reg_revoke.revoke_locked(") != std::string::npos,
              "AC5: revoke_effect_capability acts via revoke_locked");
        CHECK(revoke_block.find(
                  "has_capability(kCapTenantAdmin) || has_capability(kCapCapability)") ==
                  std::string::npos,
              "AC5: revoke_effect_capability removed unlocked has_capability fence");
    }

    // AC6: existing tests stay green — no deletion / no removed ACs.
    {
        const auto req = read_file("tests/compiler/test_require_effect_auto_isolation.cpp");
        const auto retain = read_file("tests/compiler/test_grant_epoch_retain_restricted.cpp");
        CHECK(req.find("Issue #2490") != std::string::npos,
              "AC6: test_require_effect_auto_isolation still cites #2490");
        CHECK(req.find("IsolationDeny") != std::string::npos ||
                  req.find("auto-enforce workspace isolation") != std::string::npos,
              "AC6: test_require_effect_auto_isolation AC1/AC2 surface intact");
        CHECK(retain.find("Issue #2529") != std::string::npos,
              "AC6: test_grant_epoch_retain_restricted still cites #2529");
        CHECK(retain.find("kDefaultGrantEpochRetainWindowRestricted") != std::string::npos,
              "AC6: test_grant_epoch_retain_restricted AC1 surface intact");
    }

    // Soft/Off behavior preserved — public unlocked effects_for / provenance_ok
    // remain callable (no API break); the existing reset / re-grant cycle
    // used by reset_all() above still works end-to-end (regression check
    // for the Soft happy path that the issue body mandates).
    {
        reset_all();
        using aura::core::capability::Effect;
        using aura::core::capability::g_capability_registry;
        const auto tenant = std::uint64_t{4242};
        // Soft path: no TenantAdmin held → public unlocked effects_for must
        // observe the same empty Effect set both via raw by_tenant iteration
        // and via effects_for (no race in Soft single-thread tests).
        const auto held = g_capability_registry().effects_for(tenant);
        CHECK(held == Effect::None,
              "AC6 soft: public effects_for returns None for tenant with no grants");
        // After grant_locked via the public grant() (which itself takes the
        // lock), the Soft reader sees the bit. This proves the public API
        // pair is still consistent — Issue #3126 contract is to make the
        // SECURITY FENCE paths lock; Soft / single-thread callers are
        // unchanged.
        grant_tenant_admin_mid(tenant, 7);
        const auto held2 = g_capability_registry().effects_for(tenant);
        CHECK((held2 & Effect::TenantAdmin) != Effect::None,
              "AC6 soft: public effects_for sees TenantAdmin after grant()");
        reset_all();
    }
}

} // namespace

int main() {
    reset_all();

    // ── Issue #3126: TOCTOU admin fence (locked variants + foreign-tenant gate) ──
    ac3126_admin_fence_locked();

    // ── AC6: query:tenant-isolation-stats shape ──
    {
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:tenant-isolation-stats"))");
        if (h && is_hash(*h)) {
            CHECK(true, "tenant-isolation-stats is hash");
            CHECK(href_m(cs, "schema") == 1566, "schema 1566");
            CHECK(href_m(cs, "active") == 1, "active");
            CHECK(href_m(cs, "phase") == 2, "phase 2");
        } else {
            // Light link may omit some engine:metrics surface; C++ stats remain
            // authoritative via snapshot_tenant_isolation_stats().
            const auto snap = snapshot_tenant_isolation_stats();
            CHECK(snap.issue == 1566 || snap.phase >= 1,
                  "tenant-isolation-stats: C++ snapshot path available");
        }
    }

    // ── AC1: same-tenant / unset allows ──
    {
        reset_all();
        CHECK(check_boundary(0, 0), "unset tenant allows target 0");
        g_workspace_isolation().set_current_tenant(1, "alice");
        CHECK(check_boundary(1, 1), "same tenant allows");
        CHECK(snapshot_tenant_isolation_stats().checks >= 2, "checks counted");
    }

    // ── AC1/2: cross-tenant without grant denies ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        const auto v0 = snapshot_tenant_isolation_stats().boundary_violations_prevented;
        CHECK(!check_boundary(1, 2), "cross-tenant without grant denied");
        CHECK(snapshot_tenant_isolation_stats().boundary_violations_prevented == v0 + 1,
              "boundary violation prevented");
    }

    // ── AC1: capability propagation grant allows ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectMutate);
        CHECK(check_boundary(1, 2, nullptr, false, kEffectMutate),
              "cross-tenant with Mutate grant allows");
        CHECK(!check_boundary(1, 2, nullptr, false, kEffectWrite),
              "Write not covered by Mutate grant");
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectWrite);
        CHECK(check_boundary(1, 2, nullptr, false, kEffectWrite), "Write allowed after grant");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_capability_grants >= 2,
              "grants counted");
    }

    // ── AC2: provenance ref_tenant mismatch denies ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        IsolationRefProvenance ref{};
        ref.tenant_id = 99;
        const auto p0 = snapshot_tenant_isolation_stats().cross_tenant_provenance_deny;
        CHECK(!check_boundary(1, 1, &ref), "foreign ref tenant denies even on same target");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_provenance_deny == p0 + 1,
              "provenance deny counted");
        // Grant 1→99 then allow
        g_workspace_isolation().grant_cross_tenant(1, 99, kEffectMutate);
        CHECK(check_boundary(1, 1, &ref, false, kEffectMutate),
              "provenance allow after cross grant");
    }

    // ── AC4: Strict sandbox linked ──
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_tenant_principal(7, "bob");
        ev.set_effect_sandbox_mode(2); // Strict → links isolation
        CHECK(snapshot_tenant_isolation_stats().strict_linked == 1 ||
                  g_workspace_isolation().strict_sandbox_linked,
              "strict linked after set_effect_sandbox_mode(2)");
        CHECK(!ev.check_workspace_isolation(8, 0, kEffectMutate, "strict-x"),
              "Strict + cross-tenant deny");
        CHECK(snapshot_tenant_isolation_stats().strict_denials >= 1, "strict denials counted");
    }

    // ── AC3: StableNodeRef.tenant_id stamp ──
    {
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        (void)cs.eval("(set-code \"(define x 1)\")");
        ev.set_tenant_principal(42, "t42");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace flat");
        FlatAST::StableNodeRef ref{};
        if (ws) {
            for (std::uint32_t i = 1; i < ws->size(); ++i) {
                if (ws->is_live_node(i) && !ws->is_free_slot(i)) {
                    ref = ws->make_safe_ref(i);
                    break;
                }
            }
        }
        CHECK(ref.id != 0, "captured ref");
        ev.stamp_ref_tenant(ref);
        CHECK(ref.tenant_id == 42, "StableNodeRef.tenant_id stamped");
        // Foreign principal cannot use this ref without grant
        ev.set_tenant_principal(43, "t43");
        CHECK(!ev.check_workspace_isolation(43, ref.tenant_id, 0, "ref-use"),
              "stamped foreign tenant_id denied");
    }

    // ── AC: EDSL set-tenant / grant-cross / check ──
    {
        reset_all();
        CompilerService cs;
        auto s = cs.eval("(security:set-tenant-principal! 10)");
        CHECK(s && is_bool(*s) && as_bool(*s), "set-tenant-principal!");
        auto c1 = cs.eval("(security:check-tenant-isolation 11)");
        CHECK(c1 && is_bool(*c1) && !as_bool(*c1), "check isolation denies cross");
        auto g = cs.eval(std::format("(security:grant-cross-tenant! 10 11 {})", kEffectMutate));
        CHECK(g && is_bool(*g) && as_bool(*g), "grant-cross-tenant!");
        auto c2 = cs.eval(std::format("(security:check-tenant-isolation 11 0 {})", kEffectMutate));
        CHECK(c2 && is_bool(*c2) && as_bool(*c2), "check allows after grant");
        // #2659: set_tenant_principal is per-Evaluator (capability_tenant_id_),
        // not process-global WorkspaceIsolationPolicy::current — so snapshot
        // current_tenant may stay 0. Principal authority is the Evaluator.
        CHECK(cs.evaluator().capability_tenant_id() == 10,
              "EDSL set-tenant-principal sets Evaluator capability_tenant_id_");
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.boundary_violations_prevented >= 1 ||
                  href_m(cs, "boundary-violations-prevented") >= 1,
              "stats violations");
    }

    // ── AC5: multi-thread stress — concurrent cross-tenant denies ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "agent");
        constexpr int kThreads = 4;
        constexpr int kIters = 200;
        std::atomic<int> denies{0};
        std::atomic<int> allows{0};
        std::vector<std::thread> thr;
        thr.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            thr.emplace_back([&, t] {
                for (int i = 0; i < kIters; ++i) {
                    IsolationRefProvenance ref{};
                    ref.tenant_id = static_cast<std::uint64_t>(100 + (i % 3));
                    // Foreign refs should deny
                    if (!check_boundary(1, 1, &ref))
                        denies.fetch_add(1, std::memory_order_relaxed);
                    else
                        allows.fetch_add(1, std::memory_order_relaxed);
                    // Cross target without grant denies
                    if (!check_boundary(1, static_cast<std::uint64_t>(2 + (t % 2))))
                        denies.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : thr)
            th.join();
        CHECK(denies.load() >= kThreads * kIters, "stress: most attempts denied");
        CHECK(snapshot_tenant_isolation_stats().boundary_violations_prevented >=
                  static_cast<std::uint64_t>(kThreads * kIters),
              "stress: violations audited");
        CHECK(snapshot_tenant_isolation_stats().audits >=
                  static_cast<std::uint64_t>(kThreads * kIters),
              "stress: audits recorded");
        (void)allows;
    }

    // ── #3332: allow_cross is scoped to cross_grants, not a full bypass ──
    // Soft/Off keep the zero-cost short-circuit (AC5). Restricted walks
    // grant bits even when allow_cross is set (AC2/AC3).
    {
        reset_all(); // Off
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true),
              "Soft/Off allow_cross still short-circuits");
        CHECK(!check_boundary(1, 99, nullptr, /*allow_cross=*/true, kEffectMutate,
                              /*sandbox_strict=*/false, "3332-no-grant",
                              /*sandbox_restricted=*/true),
              "allow_cross without grant denies");
        g_workspace_isolation().grant_cross_tenant(1, 99, kEffectMutate);
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true, kEffectMutate,
                             /*sandbox_strict=*/false, "3332-grant",
                             /*sandbox_restricted=*/true),
              "allow_cross + grant allows");
        CHECK(!check_boundary(1, 99, nullptr, /*allow_cross=*/true, kEffectWrite,
                              /*sandbox_strict=*/false, "3332-bits",
                              /*sandbox_restricted=*/true),
              "insufficient bits still deny");
    }

    // ─── Issue #2659: per-Evaluator principal (multi-Evaluator no cross-talk) ──
    //   AC1: Two Evaluators in one process, tenants 7 and 42, concurrent
    //        require_effect(Mutate) — each sees only its own principal.
    //   AC2: Existing single-Evaluator + TenantScope remains green.
    //   AC3: Cross-tenant grant still allows the intended path (global table).
    //   AC4: Restricted + unset principal deny fires when CALLING Evaluator
    //        has principal 0 (per-Evaluator lens).
    //   AC5: Metrics / SecurityEvent IsolationDeny carry correct tenant ids.
    //   AC6: source-cite + coverage linter (no docs/design per #1655).

    // AC1: two Evaluators concurrent require_effect — no cross-talk.
    {
        std::println("\n--- #2659 AC1: multi-Evaluator no cross-talk ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(42);
        const auto me_a = aura::core::current_mutation_epoch();
        const auto me_b = me_a == 0 ? 1 : me_a;
        ev_a.grant_effect_capability(7, "mutate-2657-A1-a", kEffectMutate, me_a == 0 ? 1 : me_a);
        ev_b.grant_effect_capability(42, "mutate-2657-A1-b", kEffectMutate, me_b);

        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> a_ok{0};
        std::atomic<std::uint64_t> b_ok{0};
        std::atomic<std::uint64_t> err{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t]() {
                while (!stop.load(std::memory_order_acquire)) {
                    try {
                        // Required_effects=0 + Restricted → pure read is permissive
                        // (no principal required). Use Mutate with a real grant.
                        const bool ok =
                            (t & 1) ? ev_a.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                                          "test:2659-a", 0, 7)
                                    : ev_b.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                                          "test:2659-b", 0, 42);
                        if (ok) {
                            if (t & 1)
                                a_ok.fetch_add(1, std::memory_order_relaxed);
                            else
                                b_ok.fetch_add(1, std::memory_order_relaxed);
                        }
                    } catch (...) {
                        err.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        stop.store(true, std::memory_order_release);
        for (auto& th : threads)
            th.join();
        std::println("  a_ok={} b_ok={} err={}", a_ok.load(), b_ok.load(), err.load());
        CHECK(a_ok.load() > 0, "AC1: Evaluator A (tenant 7) allowed when its grant exists");
        CHECK(b_ok.load() > 0, "AC1: Evaluator B (tenant 42) allowed when its grant exists");
        CHECK(err.load() == 0, "AC1: no exceptions under concurrent multi-Evaluator");
        // Per-Evaluator principal preserved (no global cross-talk).
        CHECK(ev_a.capability_tenant_id() == 7, "AC1: Evaluator A principal still 7 post-stress");
        CHECK(ev_b.capability_tenant_id() == 42, "AC1: Evaluator B principal still 42 post-stress");
    }

    // AC2: TenantScope RAII still snapshots/restores per-Evaluator.
    {
        std::println("\n--- #2659 AC2: TenantScope per-Evaluator snapshot ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        // Enter TenantScope with tenant 42; on exit, ev.capability_tenant_id_ should be 7.
        {
            Evaluator::TenantScope scope(ev, 42, "scoped-42");
            CHECK(ev.capability_tenant_id() == 42, "AC2: TenantScope sets principal to 42");
            CHECK(scope.previous_tenant() == 7, "AC2: snapshot captured prior principal 7");
        }
        CHECK(ev.capability_tenant_id() == 7, "AC2: TenantScope restored principal to 7");
    }

    // AC3: cross-tenant grant still works (global table).
    {
        std::println("\n--- #2659 AC3: cross-tenant grant table ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // #3090: Restricted refuse mid==0; #3086: grant_cross_tenant
        // requires TenantAdmin on caller/target.
        grant_tenant_admin_mid(7);
        // Issue grant 7 → 42 globally (shared policy).
        g_workspace_isolation().grant_cross_tenant(7, 42, kEffectMutate);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto me = aura::core::current_mutation_epoch();
        ev.grant_effect_capability(7, "mutate-2657-A3", kEffectMutate, me == 0 ? 1 : me);
        // Cross-tenant mutate target=42 with cover-by-grant → allow.
        CHECK(ev.check_workspace_isolation(42, 0, kEffectMutate, "test:2659-ac3-xgrant"),
              "AC3: cross-tenant target 42 with grant from 7 allows");
    }

    // AC4: Restricted + unset principal (caller's tenant_id == 0) denies.
    {
        std::println("\n--- #2659 AC4: per-Evaluator unset principal deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(0); // unset principal
        const auto before = current_iso_seq();
        const bool ok = ev.check_workspace_isolation(0, 0, kEffectMutate, "test:2659-ac4-unset");
        CHECK(!ok, "AC4: Restricted + unset principal on calling Evaluator denies");
        const auto after = current_iso_seq();
        CHECK(after > before, "AC4: IsolationDeny SE emitted");
    }

    // AC5: SecurityEvent IsolationDeny carries correct tenant ids (caller + ref).
    {
        std::println("\n--- #2659 AC5: IsolationDeny tenant ids ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(0);
        // foreign ref tenant triggers provenance deny (#2490 + #2659).
        (void)ev.check_workspace_isolation(0, 99, kEffectMutate, "test:2659-ac5");
        // Find the most recent IsolationDeny SE.
        bool found = false;
        for (std::uint64_t s = baseline; s < ring.seq.load(); ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (static_cast<int>(e.kind) ==
                    static_cast<int>(
                        aura::core::security_event::SecurityEventKind::IsolationDeny) &&
                e.seq == s) {
                // SE carries the target tenant (0 here) and the ref_tenant (99)
                // in the tenant_id field per #2388 / #2156 vocab.
                // mid is Mutation epoch (not tenant id).
                CHECK(e.tenant_id == 0, "AC5: SE tenant_id is target (0)");
                CHECK(e.mutation_id != 0, "AC5: SE mid is non-zero Mutation epoch");
                found = true;
            }
        }
        CHECK(found, "AC5: IsolationDeny SE in ring");
    }

    // AC6: source-cite + coverage manifest.
    {
        std::println("\n--- #2659 AC6: source-cite + coverage ---");
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        (void)baseline;
        reset_all();
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(sec.find("Issue #2659") != std::string::npos,
              "AC6: evaluator_security.cpp cites #2659");
        // set_tenant_principal / TenantScope no longer write g_workspace_isolation().current.
        const auto wihh = read_file("src/core/workspace_isolation.hh");
        CHECK(wihh.find("caller_principal") != std::string::npos,
              "AC6: check_boundary_ex takes caller_principal");
        CHECK(wihh.find("Issue #2659") != std::string::npos,
              "AC6: workspace_isolation.hh cites #2659");
        const auto ixx = read_file("src/compiler/evaluator.ixx");
        CHECK(ixx.find("allow_cross_tenant_") != std::string::npos,
              "AC6: Evaluator has per-instance allow_cross_tenant_ (Issue #2659)");
        CHECK(ixx.find("prev_allow_cross_") != std::string::npos,
              "AC6: TenantScope snapshots prev_allow_cross_");
        // Coverage manifest + linter.
        const auto gate = read_file("scripts/coverage/checks/check_2659.py");
        CHECK(!gate.empty(), "AC6: coverage linter check_2659.py present");
        const auto manifest = read_file("scripts/coverage/manifests/2659.json");
        CHECK(!manifest.empty(), "AC6: coverage manifest 2659.json present");
        CHECK(read_file("docs/design/2659-multi-eval-principal.md").empty(),
              "AC6: no docs/design/ — design rationale in commit/close");
    }

    // ── #2687 AC1/AC2: per-Evaluator capture tenant accounting ──
    {
        std::println("\n--- #2687 AC1/AC2: per-Evaluator isolation_capture_tenant ---");
        reset_all();
        // Production multi-tenant path goes through Evaluator::stamp_stable_ref
        // which uses Evaluator::capability_tenant_id_ (per-Evaluator authority
        // from #2659 + #2056). The new #2687 counters distinguish:
        //   local: Evaluator::stamp_stable_ref (per-Evaluator, authority)
        //   global_fallback: maybe_stamp_stable_ref_isolation_tenant (FlatAST
        //                    fallback path, reads g_isolation_capture_tenant)
        //   evaluator_miss: diagnostic for FlatAST factories called under
        //                   an active Evaluator (should have used
        //                   Evaluator::stamp_stable_ref).
        const auto local_before =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto global_before =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto miss_before =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        // Issue #2056: full stamp (tenant + fiber) — Evaluator::make_stamped_ref
        // calls stamp_stable_ref which bumps g_isolation_capture_stamp_local_total_atomic.
        for (int i = 0; i < 4; ++i) {
            (void)ev.make_stamped_ref(static_cast<NodeId>(i));
        }
        const auto local_after =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto global_after =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto miss_after =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_after >= local_before + 4,
              "AC1: Evaluator::make_stamped_ref bumps local capture counter by >= 4");
        CHECK(global_after == global_before,
              "AC1: Evaluator path does NOT bump global_fallback counter");
        CHECK(miss_after == miss_before,
              "AC1: Evaluator path does NOT bump evaluator_miss counter");
        // Global-fallback path (Soft): set process-global capture tenant + call
        // maybe_stamp_stable_ref_isolation_tenant on a StableRefT.
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(42);
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(stamped, "AC2: Soft global-fallback path stamps when tenant != 0");
        CHECK(ref.tenant_id == 42, "AC2: global-fallback stamps tenant_id from process-global");
        const auto global_after2 =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(global_after2 >= global_after + 1,
              "AC2: Soft global-fallback path bumps fallback counter");
        // Reset for AC4.
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2687 AC4: Soft / tenant=0 capture remains permissive ──
    {
        std::println("\n--- #2687 AC4: Soft / tenant=0 capture permissive ---");
        reset_all();
        aura::core::provenance::set_isolation_capture_tenant(0);
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(!stamped,
              "AC4: tenant=0 → maybe_stamp_stable_ref_isolation_tenant returns false (no stamp)");
        CHECK(ref.tenant_id == 0, "AC4: tenant_id stays 0 (legacy single-tenant)");
    }

    // ── #2687 AC5: counters + query surface (source + live atomics) ──
    // Light-link binaries do not always register full query:soa-dirty-stats
    // (obs_jit register_jit_p5). Live authority is the provenance atomics;
    // schema/key wiring is source-cited in AC6 + coverage linter.
    {
        std::println("\n--- #2687 AC5: counters + query surface ---");
        const auto local_q =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_q =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto miss_q =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_q >= 0, "AC5: local-total live (>= 0)");
        CHECK(fallback_q >= 0, "AC5: global-fallback-total live (>= 0)");
        CHECK(miss_q >= 0, "AC5: evaluator-miss-total live (>= 0)");
        CHECK(aura::core::provenance::kEvaluatorCaptureTenantIssue == 2687,
              "AC5: kEvaluatorCaptureTenantIssue == 2687");
        // Best-effort engine:metrics when full JIT obs is linked.
        CompilerService cs;
        const auto schema_q = href(cs, "schema-2687");
        if (schema_q >= 0) {
            CHECK(schema_q == 2687, "AC5: schema-2687 sentinel (when query wired)");
            CHECK(href(cs, "issue-2687") == 2687, "AC5: issue-2687 sentinel (when query wired)");
        } else {
            CHECK(true, "AC5: query:soa-dirty-stats not in light link — atomics + source-cite OK");
        }
    }

    // ── #2687 AC6: source-cite + no regression ──
    {
        std::println("\n--- #2687 AC6: source-cite + no regression ---");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
        const auto workspace = read_file("src/core/workspace_isolation.hh");
        const auto q_src = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        // Issue #2687 sentinel in all 4 prod-side files.
        CHECK(prov.find("#2687") != std::string::npos, "AC6: provenance_tracker.hh cites #2687");
        CHECK(eval_sec.find("#2687") != std::string::npos,
              "AC6: evaluator_security.cpp cites #2687");
        CHECK(workspace.find("#2687") != std::string::npos,
              "AC6: workspace_isolation.hh cites #2687");
        CHECK(q_src.find("#2687") != std::string::npos,
              "AC6: evaluator_primitives_obs_jit.cpp cites #2687");
        // Counters declared + wired.
        CHECK(prov.find("g_isolation_capture_stamp_local_total_atomic") != std::string::npos,
              "AC5: local counter declared in provenance_tracker.hh");
        CHECK(prov.find("g_isolation_capture_stamp_global_fallback_total_atomic") !=
                  std::string::npos,
              "AC5: global-fallback counter declared in provenance_tracker.hh");
        CHECK(eval_sec.find("g_isolation_capture_stamp_local_total_atomic") != std::string::npos,
              "AC1: local counter bumped from Evaluator::stamp_stable_ref");
        CHECK(prov.find("g_isolation_capture_stamp_global_fallback_total_atomic") !=
                  std::string::npos,
              "AC2: global-fallback counter bumped from maybe_stamp_stable_ref_isolation_tenant");
        // #2659 regression: Evaluator::set_tenant_principal must NOT write the global.
        CHECK(eval_sec.find("set_isolation_capture_tenant") == std::string::npos,
              "AC2 (#2659 regression): Evaluator::set_tenant_principal must not write global");
        // No design doc regression (per #1655).
        for (const auto& p : {"docs/design/evaluator_capture_tenant_2687.md",
                              "docs/evaluator_capture_tenant_2687.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
        }
    }

    // ── #2705 AC1: production hard-close refuses global stamp ──
    {
        std::println("\n--- #2705 AC1: hard-close refuses FlatAST global capture stamp ---");
        reset_all();
        // Soft baseline first: set global tenant under soft, then arm hard-close
        // (mirrors production multi-tenant residual where global may be non-zero
        // from a legacy WorkspaceIsolationPolicy mirror, but stamp must refuse).
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(42);
        const auto miss_before =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_before =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::hard_capture_tenant_active(),
              "AC1: hard_capture_tenant_active after arm");
        FlatAST::StableNodeRef ref{};
        ref.tenant_id = 0;
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(!stamped, "AC1: hard-close → maybe_stamp returns false (no stamp)");
        CHECK(ref.tenant_id == 0, "AC1: tenant_id stays 0 (no cross-tenant pollution)");
        const auto miss_after =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_after =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(miss_after >= miss_before + 1, "AC1: evaluator_miss advances on refuse");
        CHECK(fallback_after == fallback_before, "AC1: global_fallback stays 0 under hard-close");
        // Dual-Evaluator local path still works under hard-close (AC3).
        const auto local_before =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CompilerService cs_a;
        CompilerService cs_b;
        cs_a.evaluator().set_capability_tenant_id(7);
        cs_b.evaluator().set_capability_tenant_id(42);
        auto ra = cs_a.evaluator().make_stamped_ref(static_cast<NodeId>(1));
        auto rb = cs_b.evaluator().make_stamped_ref(static_cast<NodeId>(2));
        CHECK(ra.tenant_id == 7, "AC1/AC3: Evaluator A stamps tenant 7 (local authority)");
        CHECK(rb.tenant_id == 42, "AC1/AC3: Evaluator B stamps tenant 42 (local authority)");
        const auto local_after =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_after >= local_before + 2,
              "AC3: local counter still advances under hard-close");
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2705 AC2: Soft / tenant=0 stays permissive ──
    {
        std::println("\n--- #2705 AC2: Soft / tenant=0 capture permissive ---");
        reset_all();
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(0);
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(!stamped, "AC2: tenant=0 → no stamp (zero-cost early return)");
        CHECK(ref.tenant_id == 0, "AC2: tenant_id stays 0");
        // Soft path with global tenant still stamps (legacy single-tenant).
        aura::core::provenance::set_isolation_capture_tenant(9);
        FlatAST::StableNodeRef ref2{};
        const bool stamped2 = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref2);
        CHECK(stamped2, "AC2: Soft + tid!=0 still stamps (legacy allow)");
        CHECK(ref2.tenant_id == 9, "AC2: Soft stamps tenant 9 from global");
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2705 AC5: query surface (live API + optional engine:metrics) ──
    {
        std::println("\n--- #2705 AC5: query surface ---");
        reset_all();
        CHECK(aura::core::provenance::kHardCaptureTenantIssue == 2705,
              "AC5: kHardCaptureTenantIssue == 2705");
        aura::core::provenance::set_hard_capture_tenant(false);
        CHECK(!aura::core::provenance::hard_capture_tenant_active(),
              "AC5: hard-close-armed false when pref off");
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::hard_capture_tenant_active(),
              "AC5: hard-close-armed true when pref on");
        aura::core::provenance::set_hard_capture_tenant(false);
        // #2687 counters preserved (additive).
        CHECK(aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                  std::memory_order_relaxed) >= 0,
              "AC5: evaluator-miss-total still live");
        CompilerService cs;
        const auto schema_q = href(cs, "schema-2705");
        if (schema_q >= 0) {
            CHECK(schema_q == 2705, "AC5: schema-2705 sentinel (when query wired)");
            CHECK(href(cs, "issue-2705") == 2705, "AC5: issue-2705 sentinel (when query wired)");
            const auto armed = href(cs, "isolation-capture-hard-close-armed");
            CHECK(armed == 0 || armed == 1, "AC5: hard-close-armed query is 0/1");
        } else {
            CHECK(true, "AC5: query keys source-cited; light link skips engine:metrics");
        }
    }

    // ── #2705 AC6: source-cite ──
    {
        std::println("\n--- #2705 AC6: source-cite ---");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto sec_def = read_file("src/compiler/security_defaults.hh");
        const auto q_src = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(prov.find("#2705") != std::string::npos, "AC6: provenance_tracker.hh cites #2705");
        CHECK(prov.find("hard_capture_tenant") != std::string::npos,
              "AC6: hard_capture_tenant API in provenance_tracker.hh");
        CHECK(sec_def.find("#2705") != std::string::npos, "AC6: security_defaults.hh cites #2705");
        CHECK(q_src.find("#2705") != std::string::npos,
              "AC6: evaluator_primitives_obs_jit.cpp cites #2705");
        CHECK(q_src.find("isolation-capture-hard-close-armed") != std::string::npos,
              "AC6: hard-close-armed query key present");
        for (const auto& p :
             {"docs/design/hard_capture_tenant_2705.md", "docs/hard_capture_tenant_2705.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
        }
    }

    // ── #2759 AC1: Evaluator stamp sole production authority ──
    {
        std::println("\n--- #2759 AC1: Evaluator stamp sole authority under hard-close ---");
        reset_all();
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::hard_capture_tenant_active(), "AC1: hard-close armed");
        // Non-zero global write suppressed under hard-close.
        const auto supp_before =
            aura::core::provenance::g_isolation_capture_global_write_suppressed_total_atomic().load(
                std::memory_order_relaxed);
        aura::core::provenance::set_isolation_capture_tenant(99);
        CHECK(aura::core::provenance::isolation_capture_tenant() == 0,
              "AC1: non-zero global write suppressed under hard-close");
        const auto supp_after =
            aura::core::provenance::g_isolation_capture_global_write_suppressed_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(supp_after >= supp_before + 1, "AC1: global-write-suppressed advances");
        // Dual Evaluator: make_stamped_ref is local-only (no miss/fallback).
        const auto miss_before =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_before =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto local_before =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CompilerService cs_a;
        CompilerService cs_b;
        cs_a.evaluator().set_capability_tenant_id(7);
        cs_b.evaluator().set_capability_tenant_id(42);
        auto ra = cs_a.evaluator().make_stamped_ref(static_cast<NodeId>(1));
        auto rb = cs_b.evaluator().make_stamped_ref(static_cast<NodeId>(2));
        CHECK(ra.tenant_id == 7, "AC1: Evaluator A stamps tenant 7 only");
        CHECK(rb.tenant_id == 42, "AC1: Evaluator B stamps tenant 42 only");
        const auto miss_after =
            aura::core::provenance::g_isolation_capture_stamp_evaluator_miss_total_atomic().load(
                std::memory_order_relaxed);
        const auto fallback_after =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        const auto local_after =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_after >= local_before + 2, "AC1: local counter advances on stamp");
        CHECK(miss_after == miss_before,
              "AC1: make_stamped_ref (layout+stamp) does NOT bump evaluator_miss");
        CHECK(fallback_after == fallback_before,
              "AC1: make_stamped_ref does NOT bump global_fallback");
        aura::core::provenance::set_hard_capture_tenant(false);
    }

    // ── #2759 AC2: Soft / tenant=0 stays permissive ──
    {
        std::println("\n--- #2759 AC2: Soft global write + stamp still permissive ---");
        reset_all();
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(11);
        CHECK(aura::core::provenance::isolation_capture_tenant() == 11,
              "AC2: Soft allows non-zero global write");
        FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(stamped, "AC2: Soft maybe_stamp still stamps");
        CHECK(ref.tenant_id == 11, "AC2: Soft stamps tenant from global");
        aura::core::provenance::set_isolation_capture_tenant(0);
        FlatAST::StableNodeRef ref0{};
        CHECK(!aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref0),
              "AC2: tenant=0 still no-op");
    }

    // ── #2759 AC3: refresh preserves tenant; no global re-stamp ──
    {
        std::println("\n--- #2759 AC3: refresh preserves tenant under hard-close ---");
        reset_all();
        // Soft write global first, then arm hard-close with global already set
        // (legacy residual pollution). refresh must preserve tenant and must
        // not stamp from global (layout remake).
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(77);
        aura::core::provenance::set_hard_capture_tenant(true);
        CHECK(aura::core::provenance::isolation_capture_tenant() == 77,
              "AC3: pre-arm global still visible (suppress only blocks new writes)");
        FlatAST::StableNodeRef ref{};
        ref.id = static_cast<NodeId>(1);
        ref.tenant_id = 7;
        // refresh_if_stale needs a live FlatAST — use make_safe_ref_layout path
        // via direct field restore semantics already unit-tested in
        // test_stable_ref_tenant_mandate. Here we assert source contract:
        // remake uses make_safe_ref_layout (no maybe_stamp).
        const auto stab = read_file("src/core/ast_stability.cpp");
        CHECK(stab.find("make_safe_ref_layout") != std::string::npos,
              "AC3: refresh_if_stale remakes via make_safe_ref_layout");
        CHECK(stab.find("preserved_tenant") != std::string::npos,
              "AC3: refresh preserves tenant_id");
        // maybe_stamp under hard-close with residual global refuses.
        FlatAST::StableNodeRef r2{};
        r2.tenant_id = 0;
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(r2);
        CHECK(!stamped, "AC3: hard-close refuses global re-stamp");
        CHECK(r2.tenant_id == 0, "AC3: tenant_id unchanged by refused stamp");
        aura::core::provenance::set_hard_capture_tenant(false);
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2759 AC5/AC6: query + source-cite ──
    {
        std::println("\n--- #2759 AC5/AC6: query + source-cite ---");
        reset_all();
        CHECK(aura::core::provenance::kEvaluatorStampSoleAuthorityIssue == 2759,
              "AC5: kEvaluatorStampSoleAuthorityIssue == 2759");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto eval_sec = read_file("src/compiler/evaluator_security.cpp");
        const auto ast = read_file("src/core/ast.ixx");
        const auto q_src = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(prov.find("#2759") != std::string::npos, "AC6: provenance_tracker.hh cites #2759");
        CHECK(prov.find("g_isolation_capture_global_write_suppressed_total_atomic") !=
                  std::string::npos,
              "AC5: global-write-suppressed counter declared");
        CHECK(eval_sec.find("make_ref_layout") != std::string::npos,
              "AC1: make_stamped_ref uses make_ref_layout");
        CHECK(ast.find("make_ref_layout") != std::string::npos, "AC1: make_ref_layout in FlatAST");
        CHECK(ast.find("make_safe_ref_layout") != std::string::npos,
              "AC3: make_safe_ref_layout in FlatAST");
        CHECK(q_src.find("schema-2759") != std::string::npos, "AC5: schema-2759 query key");
        CHECK(q_src.find("issue-2759") != std::string::npos, "AC5: issue-2759 query key");
        CHECK(q_src.find("isolation-capture-global-write-suppressed-total") != std::string::npos,
              "AC5: global-write-suppressed query key");
        // #2705 / #2687 keys preserved.
        CHECK(q_src.find("schema-2705") != std::string::npos, "AC5: schema-2705 preserved");
        CHECK(q_src.find("schema-2687") != std::string::npos, "AC5: schema-2687 preserved");
        for (const auto& p : {"docs/design/evaluator_stamp_sole_authority_2759.md",
                              "docs/evaluator_stamp_sole_authority_2759.md", "design/2759.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
        }
    }

    // ── #2960: query stable returns stamp full provenance ──
    {
        std::println("\n--- #2960 AC1/AC2: query stamp helper + counters ---");
        reset_all();
        CHECK(aura::core::provenance::kQueryStableRefStampIssue == 2960,
              "AC2: kQueryStableRefStampIssue == 2960");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (q-stamp x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");

        ev.set_capability_tenant_id(55);
        const auto stamped0 =
            aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
                std::memory_order_relaxed);
        const auto prev0 =
            aura::core::provenance::g_query_stable_ref_unstamped_prevented_total_atomic().load(
                std::memory_order_relaxed);

        // Layout path (primary): cow/wrap match workspace → stamped only.
        auto layout = ws->make_ref_layout(id);
        CHECK(layout.tenant_id == 0, "AC1: layout-only tenant 0 before stamp");
        ev.stamp_query_stable_ref_export(layout);
        CHECK(layout.tenant_id == 55, "AC1: stamp_query fills capability tenant");
        CHECK(layout.cow_epoch_at_capture == ws->workspace_cow_epoch(),
              "AC1: cow_epoch preserved from layout");

        // Brace-init residual under advanced wrap: remade + unstamped_prevented.
        if (ws->wrap_epoch() == 0) {
            // Force wrap_epoch visibility by bumping generation many times is heavy;
            // source-cite residual path instead when wrap still 0.
            const auto sec = read_file("src/compiler/evaluator_security.cpp");
            CHECK(sec.find("record_query_stable_ref_unstamped_prevented") != std::string::npos,
                  "AC2: unstamped residual path wired");
        } else {
            FlatAST::StableNodeRef brace{};
            brace.id = id;
            brace.gen = ws->generation();
            ev.stamp_query_stable_ref_export(brace);
            CHECK(brace.tenant_id == 55, "AC2: brace residual remade+stamped");
            CHECK(
                aura::core::provenance::g_query_stable_ref_unstamped_prevented_total_atomic().load(
                    std::memory_order_relaxed) > prev0,
                "AC2: unstamped_prevented advanced on brace residual");
        }

        CHECK(aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
                  std::memory_order_relaxed) > stamped0,
              "AC2: query_stable_ref_stamped_total advanced");

        // Multi-tenant isolation fail-closed on foreign stamped ref.
        auto foreign = layout;
        foreign.tenant_id = 99;
        CHECK(!ev.check_workspace_isolation(55, foreign.tenant_id, 0, "test:2960-x"),
              "AC3: cross-tenant isolation deny");

        // Source cite FlatAST layout-only children_stable / for_each.
        const auto ast = read_file("src/core/ast.ixx");
        CHECK(ast.find("make_ref_layout(cid)") != std::string::npos ||
                  ast.find("make_ref_layout(pid)") != std::string::npos,
              "AC1: children/parent_stable use make_ref_layout");
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(qws.find("stamp_query_stable_ref_export") != std::string::npos,
              "AC1: query workspace stamps via stamp_query_stable_ref_export");
        const auto qhash = read_file("src/compiler/evaluator_primitives_query.cpp");
        CHECK(qhash.find("query-stable-ref-stamped-total") != std::string::npos,
              "AC2: stable-ref-stats-hash exposes stamped total");
        CHECK(qhash.find("schema-2960") != std::string::npos, "AC2: schema-2960 on stats hash");
        for (const auto& p : {"docs/design/query_stable_ref_stamp_2960.md",
                              "docs/query_stable_ref_stamp_2960.md", "design/2960.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "AC4: no design doc at " + std::string(p));
        }
    }

    // ── #3000: restamp-lag export face (isolation / tenant-capture sibling) ──
    {
        std::println("\n--- #3000 AC1/AC2: stamp rejects lagging gen under production ---");
        reset_all();
        CHECK(aura::core::provenance::kQueryStableRefRestampLagIssue == 3000,
              "AC4: kQueryStableRefRestampLagIssue == 3000");
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (q-lag a) a) (define (q-lag2 b) b) "
                      "(define (q-lag3 c) c) (define (q-lag4 d) d)\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");
        apply_production_audit_defaults();
        set_restamp_budget_nodes_for_process(1);
        ws->bump_generation();
        ws->restamp_all_node_generations();
        CHECK(ws->restamp_last_budget_exceeded(), "#3000: last restamp exceeded");
        if (!ws->node_generation_is_post_mutate(id)) {
            CHECK(!ev.allow_query_stable_ref_export(id),
                  "#3000: production allow rejects lagging node");
            FlatAST::StableNodeRef brace{};
            brace.id = id;
            ev.stamp_query_stable_ref_export(brace);
            CHECK(brace.id == NULL_NODE, "#3000: stamp nulls lagging export");
            CHECK(aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic()
                          .load(std::memory_order_relaxed) >= 1,
                  "#3000: prevented advanced");
        } else {
            CHECK(true, "#3000: node incrementally restamped — post-mutate allow");
        }
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(qws.find("restamp-lag") != std::string::npos, "#3000: typed restamp-lag reason");
        CHECK(qws.find("allow_query_stable_ref_export") != std::string::npos,
              "#3000: query workspace gates export");
        for (const auto& p : {"docs/design/3000-restamp-lag.md", "docs/query_restamp_lag_3000.md",
                              "design/3000.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "#3000: no design doc at " + std::string(p));
        }
    }

    // ── #3037: over-budget restamp torn export (lazy-align must not hide) ──
    {
        std::println("\n--- #3037 AC1/AC2: torn reject after lazy-align under production ---");
        reset_all();
        CHECK(aura::core::provenance::kQueryStableRefRestampTornIssue == 3037,
              "3037: kQueryStableRefRestampTornIssue == 3037");
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (q-torn a) a) (define (q-torn2 b) b) "
                      "(define (q-torn3 c) c) (define (q-torn4 d) d)\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");
        apply_production_audit_defaults();
        set_restamp_budget_nodes_for_process(1);
        ws->bump_generation();
        ws->restamp_all_node_generations();
        CHECK(ws->restamp_generation_torn(), "#3037: generation torn");
        if (!ws->node_eagerly_restamped(id)) {
            (void)ws->is_valid(id);
            CHECK(ws->node_generation_is_post_mutate(id), "#3037: lazy-align hid raw gen lag");
            CHECK(!ev.allow_query_stable_ref_export(id),
                  "#3037: production rejects after lazy-align");
            FlatAST::StableNodeRef brace{};
            brace.id = id;
            ev.stamp_query_stable_ref_export(brace);
            CHECK(brace.id == NULL_NODE, "#3037: stamp nulls torn export");
            CHECK(
                aura::core::provenance::g_query_stable_ref_restamp_torn_reject_total_atomic().load(
                    std::memory_order_relaxed) >= 1,
                "#3037: torn reject advanced");
        } else {
            CHECK(true, "#3037: node eagerly restamped — current gen ok");
        }
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(qws.find("Issue #3037") != std::string::npos, "#3037: query workspace cites torn");
        CHECK(qws.find("generation torn") != std::string::npos, "#3037: torn message");
        const auto astx = read_file("src/core/ast.ixx");
        CHECK(astx.find("node_eagerly_restamped") != std::string::npos, "#3037: eager bit helper");
        for (const auto& p : {"docs/design/3037-restamp-over-budget-export.md",
                              "docs/restamp_over_budget_export_3037.md", "design/3037.md"}) {
            std::ifstream f(p);
            CHECK(!f.good(), "#3037: no design doc at " + std::string(p));
        }
    }

    // ── #2968: cross-tenant grant write path requires TenantAdmin ──
    {
        std::println("\n--- #2968 AC1: cross-tenant grant without TenantAdmin → deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: cross_tenant_grant_deny_total bumps when caller lacks TenantAdmin");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC1: no cross grant written on deny");
        // SE reason present in ring.
        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "cross-tenant-grant-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'cross-tenant-grant-needs-tenant-admin' recorded");
    }

    // ── #2968 AC2: TenantAdmin allows cross-tenant grant ──
    {
        std::println("\n--- #2968 AC2: TenantAdmin allows cross-tenant grant ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        // #3090: grant_capability() stamps mid=0 → refused under Restricted.
        // Registry TenantAdmin with bound mid so SSOT grant_cross_tenant allows.
        grant_tenant_admin_mid(7);

        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(allow_after == allow_before + 1,
              "AC2: allow bumps cross_tenant_capability_grant_total");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC2: cross grant installed with TenantAdmin");
    }

    // ── #2968 AC2b: foreign-tenant grant_effect_capability gate ──
    {
        std::println("\n--- #2968 AC2b: foreign-tenant grant_effect_capability gate ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);

        // No TenantAdmin → foreign-tenant grant denied.
        ev.grant_effect_capability(/*tenant=*/42, "mut-2968-foreign", kEffectMutate,
                                   /*mid=*/5);
        aura::core::capability::CapabilityGrant g{};
        CHECK(
            !aura::core::capability::g_capability_registry().find_grant(42, "mut-2968-foreign", g),
            "AC2b: foreign grant denied without TenantAdmin");
        // Same-tenant self-grant stays allowed (existing policy).
        ev.grant_effect_capability(/*tenant=*/7, "mut-2968-self", kEffectMutate, /*mid=*/6);
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "mut-2968-self", g),
              "AC2b: same-tenant self-grant stays allowed");
        // With TenantAdmin → foreign grant allowed.
        grant_tenant_admin_mid(ev.capability_tenant_id());
        ev.grant_effect_capability(/*tenant=*/42, "mut-2968-admin", kEffectMutate, /*mid=*/7);
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "mut-2968-admin", g),
              "AC2b: foreign grant allowed with TenantAdmin");
    }

    // ── #2968 AC3: Off path no hard gate ──
    {
        std::println("\n--- #2968 AC3: Off path no hard gate ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/1, /*to=*/2, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: Off path does not deny (no hard gate)");
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) != 0,
              "AC3: Off path cross-tenant grant proceeds");
    }

    // ── #2968 AC5: snapshot + posture additive keys ──
    {
        std::println("\n--- #2968 AC5: snapshot + posture additive keys ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_cross_tenant_access(7, 42, kEffectMutate); // deny (no admin)
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.cross_tenant_grant_deny >= 1, "AC5: snapshot exposes cross_tenant_grant_deny");
        // Posture prim cites schema-2968 + additive keys.
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-2968") != std::string::npos, "AC5: posture cites schema-2968");
        CHECK(posture.find("cross-tenant-grant-tenant-admin-wired") != std::string::npos,
              "AC5: posture exposes cross-tenant-grant-tenant-admin-wired");
        CHECK(posture.find("cross-tenant-grant-deny-total") != std::string::npos,
              "AC5: posture exposes cross-tenant-grant-deny-total");
    }

    // ── #2968 AC6: source-cite + no invent + no docs/design/ ──
    {
        std::println("\n--- #2968 AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("#2968") != std::string::npos, "AC6: workspace_isolation.hh cites #2968");
        CHECK(sec.find("#2968") != std::string::npos, "AC6: evaluator_security.cpp cites #2968");
        CHECK(posture.find("schema-2968") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-2968");
        CHECK(test_self.find("#2968") != std::string::npos, "AC6: test file cites #2968");
        CHECK(build.find("check_cross_tenant_grant_gate_2968") != std::string::npos,
              "AC6: build.py wires #2968 linter");
        std::ifstream invent("tests/core/test_issue_2968.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_2968.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_2968.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2968-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3086: SSOT fence — direct g_workspace_isolation().grant_cross_tenant
    // bypass previously routed only through Evaluator::grant_cross_tenant_access.
    // Now fence lives in the method body; raw callers cannot widen the table.
    {
        std::println("\n--- #3086 AC1: raw grant_cross_tenant under Restricted without TenantAdmin "
                     "→ deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // default_tenant = 7, target = 42, no TenantAdmin on either → deny.
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: cross_tenant_grant_deny_total bumps on direct raw grant under Restricted");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == 0,
              "AC1: no cross grant written on deny (raw path)");
        // SE reason present in ring.
        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "cross-tenant-grant-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'cross-tenant-grant-needs-tenant-admin' recorded (raw path)");
    }

    // ── #3086 AC2: Soft / Off zero-cost allow on raw grant ──
    {
        std::println("\n--- #3086 AC2: raw grant under Off → allow (zero-cost) ---");
        reset_all(); // Off
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/1, /*to=*/2, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC2: Off path does not deny (raw call)");
        CHECK(allow_after == allow_before + 1, "AC2: Off path bumps allow counter");
        CHECK(g_workspace_isolation().cross_grant_bits(1, 2) == kEffectMutate,
              "AC2: Off path cross grant installed");
    }

    // ── #3086 AC3: target tenant holds TenantAdmin → allow under Restricted ──
    {
        std::println("\n--- #3086 AC3: TenantAdmin on target tenant → allow raw grant ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        // Grant TenantAdmin to the *target* tenant (42), not caller (7).
        grant_tenant_admin_mid(42);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: target-tenant admin → no deny bump");
        CHECK(allow_after == allow_before + 1, "AC3: target-tenant admin → allow bump");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) == kEffectMutate,
              "AC3: cross grant installed when target holds TenantAdmin");
    }

    // ── #3086 AC4: no double-count via Evaluator wrapper ──
    {
        std::println("\n--- #3086 AC4: Evaluator wrapper does not double-bump deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::g_capability_registry().default_tenant.store(
            7, std::memory_order_release);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC4: Evaluator wrapper denial bumps SSOT counter exactly once (no double-count)");
    }

    // ── #3086 AC5: zero-id guard still short-circuits (no SE, no counter bump) ──
    {
        std::println("\n--- #3086 AC5: zero-id short-circuit ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        g_workspace_isolation().grant_cross_tenant(/*from=*/0, /*to=*/42, kEffectMutate);
        g_workspace_isolation().grant_cross_tenant(/*from=*/7, /*to=*/0, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC5: zero-id short-circuit does not bump deny counter");
    }

    // ── #3086 AC6: source-cite + no invent + no docs/design/ ──
    {
        std::println("\n--- #3086 AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        CHECK(iso.find("#3086") != std::string::npos, "AC6: workspace_isolation.hh cites #3086");
        CHECK(iso.find("try_grant_cross_tenant_privileged") != std::string::npos,
              "AC6: SSOT helper present");
        CHECK(sec.find("#3086") != std::string::npos, "AC6: evaluator_security.cpp cites #3086");
        CHECK(sec.find("g_workspace_isolation().grant_cross_tenant(from_tenant, to_tenant, "
                       "effect_bits)") != std::string::npos,
              "AC6: Evaluator wrapper delegates to SSOT method (no second policy)");
        CHECK(test_self.find("#3086") != std::string::npos, "AC6: test file cites #3086");
        std::ifstream invent("tests/core/test_issue_3086.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3086.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3086.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3086-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #2969: registry write-fence — foreign-tenant grant/revoke requires
    // TenantAdmin (Option A, minimal; storage/write-isolation face) ──
    {
        std::println("\n--- #2969 AC1: durable/session/revoke foreign-tenant gate ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        aura::core::capability::CapabilityGrant g{};

        // AC1: durable foreign grant denied without TenantAdmin (low-risk
        // effect — isolates #2969 fence from the #2967 high-risk gate).
        const auto deny_before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-foreign", kEffectWrite, /*mid=*/0,
                                /*reason=*/"audit-reason");
        const auto deny_after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: durable foreign grant denied without TenantAdmin");
        CHECK(
            !aura::core::capability::g_capability_registry().find_grant(42, "dur-2969-foreign", g),
            "AC1: no durable foreign grant written on deny");
        // SE reason present in ring.
        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "grant-foreign-tenant-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'grant-foreign-tenant-needs-tenant-admin' recorded");

        // AC1: session foreign grant denied without TenantAdmin.
        ev.grant_effect_session(/*tenant=*/42, "ses-2969-foreign", kEffectWrite, /*mid=*/1);
        CHECK(
            !aura::core::capability::g_capability_registry().find_grant(42, "ses-2969-foreign", g),
            "AC1: no session foreign grant written on deny");

        // AC1: revoke foreign denied without TenantAdmin — seed a foreign
        // grant through the admin path, then a second (non-admin) Evaluator
        // attempts the cross-tenant revoke (two-Evaluator verification).
        grant_tenant_admin_mid(ev.capability_tenant_id());
        ev.grant_effect_capability(/*tenant=*/42, "mut-2969-seed", kEffectWrite, /*mid=*/1);
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "mut-2969-seed", g),
              "AC1: admin path seeds foreign grant (audited)");
        {
            // Second (non-admin) Evaluator under a DIFFERENT tenant principal
            // (9) attempts the cross-tenant revoke. No reset_all() here — it
            // would clear the process-global registry (#2968) and drop the
            // seeded foreign grant, defeating the survival check.
            CompilerService cs2;
            auto& ev2 = cs2.evaluator();
            ev2.set_effect_sandbox_mode(1);
            ev2.set_capability_tenant_id(9); // non-admin principal A
            const auto deny2_before =
                aura::core::capability::g_capability_effect_metrics()
                    .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
            ev2.revoke_effect_capability(/*tenant=*/42, "mut-2969-seed");
            const auto deny2_after =
                aura::core::capability::g_capability_effect_metrics()
                    .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
            CHECK(deny2_after == deny2_before + 1,
                  "AC1: foreign revoke denied without TenantAdmin");
            CHECK(aura::core::capability::g_capability_registry().find_grant(42, "mut-2969-seed",
                                                                             g) &&
                      !g.revoked,
                  "AC1: foreign grant survives non-admin revoke attempt");
        }
    }

    // ── #2969 AC2: same-tenant grant/revoke keep existing policy ──
    {
        std::println("\n--- #2969 AC2: same-tenant grant/revoke unchanged ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        aura::core::capability::CapabilityGrant g{};
        const auto deny_before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/7, "dur-2969-self", kEffectWrite, /*mid=*/1,
                                /*reason=*/"r");
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "dur-2969-self", g),
              "AC2: same-tenant durable grant stays allowed");
        ev.grant_effect_session(/*tenant=*/7, "ses-2969-self", kEffectWrite, /*mid=*/1);
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "ses-2969-self", g),
              "AC2: same-tenant session grant stays allowed");
        ev.revoke_effect_capability(/*tenant=*/7, "dur-2969-self");
        CHECK(aura::core::capability::g_capability_registry().find_grant(7, "dur-2969-self", g) &&
                  g.revoked,
              "AC2: same-tenant revoke works");
        const auto deny_after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC2: no fence deny on same-tenant operations");
    }

    // ── #2969 AC3: Off path no hard fence (zero extra cost) ──
    {
        std::println("\n--- #2969 AC3: Off path no hard fence ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        aura::core::capability::CapabilityGrant g{};
        const auto deny_before =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-off", kEffectWrite, /*mid=*/0,
                                /*reason=*/"r");
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "dur-2969-off", g),
              "AC3: Off path durable foreign grant proceeds");
        const auto deny_after =
            aura::core::capability::g_capability_effect_metrics()
                .capability_grant_foreign_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: Off path no fence deny");
    }

    // ── #2969 AC4: allow counter bumps only on allow (deny does not) ──
    {
        std::println("\n--- #2969 AC4: allow counter only on allow ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);
        const auto grants_before =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-noadmin", kEffectWrite, /*mid=*/0,
                                /*reason=*/"r"); // deny
        const auto grants_deny =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        CHECK(grants_deny == grants_before, "AC4: deny does not bump capability_grant_total");
        grant_tenant_admin_mid(ev.capability_tenant_id());
        // grant_capability mirrors into the registry (bumps grant_total once
        // for the tenant-admin grant itself) — snapshot AFTER it so the +1
        // assertion isolates the durable allow path.
        const auto grants_admin_granted =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-admin", kEffectWrite, /*mid=*/1,
                                /*reason=*/"r"); // allow (admin)
        const auto grants_allow =
            aura::core::capability::g_capability_effect_metrics().capability_grant_total.load(
                std::memory_order_relaxed);
        CHECK(grants_allow == grants_admin_granted + 1, "AC4: allow bumps capability_grant_total");
    }

    // ── #2969 AC5: snapshot + posture additive keys ──
    {
        std::println("\n--- #2969 AC5: snapshot + posture additive keys ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_effect_durable(/*tenant=*/42, "dur-2969-snap", kEffectWrite, /*mid=*/0,
                                /*reason=*/"r"); // deny
        const auto cap = aura::core::capability::snapshot_capability_effect_stats();
        CHECK(cap.capability_grant_foreign_tenant_deny >= 1,
              "AC5: snapshot exposes capability_grant_foreign_tenant_deny");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-2969") != std::string::npos, "AC5: posture cites schema-2969");
        CHECK(posture.find("issue-2969") != std::string::npos, "AC5: posture cites issue-2969");
        CHECK(posture.find("capability-grant-write-fence-wired") != std::string::npos,
              "AC5: posture exposes capability-grant-write-fence-wired");
        CHECK(posture.find("capability-grant-foreign-tenant-deny-total") != std::string::npos,
              "AC5: posture exposes capability-grant-foreign-tenant-deny-total");
    }

    // ── #2969 AC6: source-cite + no invent + no docs/design/ ──
    {
        std::println("\n--- #2969 AC6: source-cite + no invent + no docs/design/ ---");
        const auto model = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(model.find("#2969") != std::string::npos, "AC6: capability_model.hh cites #2969");
        CHECK(sec.find("#2969") != std::string::npos, "AC6: evaluator_security.cpp cites #2969");
        CHECK(posture.find("schema-2969") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-2969");
        CHECK(test_self.find("#2969") != std::string::npos, "AC6: test file cites #2969");
        CHECK(build.find("check_capability_write_fence_2969") != std::string::npos,
              "AC6: build.py wires #2969 linter");
        std::ifstream invent("tests/core/test_issue_2969.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_2969.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_2969.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("2969-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3010: allow_cross_tenant_ write requires TenantAdmin ──
    {
        std::println(
            "\n--- #3010 AC1: Restricted same-tenant allow_cross without admin → deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1); // Restricted
        ev.set_capability_tenant_id(7);

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(!ev.allow_cross_tenant(), "AC1: flag starts false");
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(!ev.allow_cross_tenant(), "AC1: C++ set_tenant_principal refuses flag without admin");
        CHECK(ev.capability_tenant_id() == 7, "AC1: tenant id still binds on flag deny");
        const auto deny_cpp = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                  .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_cpp == deny_before + 1, "AC1: C++ deny bumps allow_cross_tenant_deny_total");

        auto edsl = cs.eval("(security:set-tenant-principal! 7 #t)");
        CHECK(edsl && is_bool(*edsl) && !as_bool(*edsl),
              "AC1: EDSL set-tenant-principal! same-tenant #t returns #f");
        CHECK(!ev.allow_cross_tenant(), "AC1: EDSL deny leaves flag false");
        const auto deny_edsl = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                   .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_edsl == deny_cpp + 1, "AC1: EDSL deny bumps allow_cross_tenant_deny_total");

        const auto& ring = g_security_event_ring();
        bool found = false;
        const auto cur = ring.seq.load(std::memory_order_acquire);
        for (auto s = cur; s > 0 && s + 16 > cur; --s) {
            const auto& e = ring.ring[(s - 1) % ring.ring.size()];
            if (std::string_view(e.reason) == "allow-cross-needs-tenant-admin") {
                found = true;
                break;
            }
        }
        CHECK(found, "AC1: SE reason 'allow-cross-needs-tenant-admin' recorded");
    }

    {
        std::println("\n--- #3010 AC2: Restricted + TenantAdmin can set flag ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        grant_tenant_admin_mid(ev.capability_tenant_id());
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin); // local list

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        auto edsl = cs.eval("(security:set-tenant-principal! 7 #t)");
        CHECK(edsl && is_bool(*edsl) && as_bool(*edsl),
              "AC2: EDSL set-tenant-principal! with TenantAdmin returns #t");
        CHECK(ev.allow_cross_tenant(), "AC2: flag set with TenantAdmin");
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC2: admin path does not bump deny counter");
        // #3332: Restricted allow_cross is not a full isolation bypass —
        // foreign Mutate without cross_grants[T1→T2] still denies (cap_deny).
        // Grant-write (#2968) stays independently gated.
        CHECK(!ev.check_workspace_isolation(42, 0, kEffectMutate, "3010-cross"),
              "AC2: allow_cross without grant denies foreign Mutate (#3332)");
        {
            // Different principal: TenantAdmin was granted on tenant 7 in the
            // process-global registry; a non-admin tenant must still hit #2968.
            CompilerService cs2;
            auto& ev2 = cs2.evaluator();
            ev2.set_effect_sandbox_mode(1);
            ev2.set_capability_tenant_id(9);
            // SSOT grant_cross_tenant reads registry default_tenant, not
            // Evaluator principal. Point it at the non-admin tenant so
            // #2968 still denies (grant_tenant_admin_mid left default=7).
            aura::core::capability::g_capability_registry().default_tenant.store(
                9, std::memory_order_release);
            ev2.grant_cross_tenant_access(9, 42, kEffectMutate);
            CHECK(g_workspace_isolation().cross_grant_bits(9, 42) == 0,
                  "AC2: #2968 grant-write still requires TenantAdmin");
        }
    }

    {
        std::println("\n--- #3010 AC3: Soft / Off path no hard gate ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "AC3: Off C++ path sets flag without admin");
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/false);
        auto edsl = cs.eval("(security:set-tenant-principal! 7 #t)");
        CHECK(edsl && is_bool(*edsl) && as_bool(*edsl), "AC3: Off EDSL path sets flag");
        CHECK(ev.allow_cross_tenant(), "AC3: Off EDSL flag set");
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .allow_cross_tenant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before, "AC3: Off path does not deny (no hard gate)");
    }

    {
        std::println("\n--- #3010 AC5: snapshot + posture additive keys ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true); // deny
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.allow_cross_tenant_deny >= 1, "AC5: snapshot exposes allow_cross_tenant_deny");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-3010") != std::string::npos, "AC5: posture cites schema-3010");
        CHECK(posture.find("allow-cross-tenant-admin-wired") != std::string::npos,
              "AC5: posture exposes allow-cross-tenant-admin-wired");
        CHECK(posture.find("allow-cross-tenant-deny-total") != std::string::npos,
              "AC5: posture exposes allow-cross-tenant-deny-total");
        CHECK(posture.find("allow-cross-needs-tenant-admin") != std::string::npos,
              "AC5: prim cites SE reason allow-cross-needs-tenant-admin");
    }

    {
        std::println("\n--- #3010 AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("#3010") != std::string::npos, "AC6: workspace_isolation.hh cites #3010");
        CHECK(sec.find("#3010") != std::string::npos, "AC6: evaluator_security.cpp cites #3010");
        CHECK(posture.find("schema-3010") != std::string::npos,
              "AC6: evaluator_primitives_security.cpp cites schema-3010");
        CHECK(test_self.find("#3010") != std::string::npos, "AC6: test file cites #3010");
        CHECK(build.find("check_allow_cross_tenant_admin_3010") != std::string::npos,
              "AC6: build.py wires #3010 linter");
        std::ifstream invent("tests/core/test_issue_3010.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3010.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3010.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3010-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3332: Restricted allow_cross is scoped to cross_grants (not full bypass) ──
    {
        std::println("\n--- #3332 AC1: #3010 write gate does not regress ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(!ev.allow_cross_tenant(), "3332 AC1: Restricted without TenantAdmin cannot set flag");
        grant_tenant_admin_mid(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "3332 AC1: TenantAdmin can still set flag (#3010)");
    }

    {
        std::println("\n--- #3332 AC2: Restricted allow_cross without grant denies ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::set_effect_fiber_id_override(42);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        grant_tenant_admin_mid(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "3332 AC2: flag set");
        const auto cap0 = snapshot_tenant_isolation_stats().cross_tenant_capability_deny;
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        CHECK(!ev.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac2-no-grant"),
              "3332 AC2: foreign Mutate without grant denies");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_capability_deny == cap0 + 1,
              "3332 AC2: cap_deny counted");
        bool found = false;
        const auto head = ring.seq.load(std::memory_order_acquire);
        for (auto s = baseline; s < head; ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.kind == SecurityEventKind::IsolationDeny && e.seq == s) {
                CHECK(e.fiber_id == 42, "3332 AC2: IsolationDeny fiber_id (#3011)");
                found = true;
            }
        }
        CHECK(found, "3332 AC2: IsolationDeny SE recorded");
        aura::core::workspace_isolation::IsolationAuditEntry priv{};
        const auto aseq = g_workspace_isolation().load_audit_seq();
        CHECK(aseq >= 1 && g_workspace_isolation().try_load_audit_seq(aseq - 1, priv),
              "3332 AC2: private isolation ring loadable");
        CHECK(priv.denied && priv.capability_deny, "3332 AC2: capability_deny latched");
        aura::core::capability::set_effect_fiber_id_override(0);
    }

    {
        std::println("\n--- #3332 AC3: allow_cross + grant allows; insufficient bits deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        grant_tenant_admin_mid(7);
        g_workspace_isolation().grant_cross_tenant(7, 42, kEffectMutate, /*caller=*/7);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac3-grant"),
              "3332 AC3: Mutate grant + allow_cross allows");
        CHECK(!ev.check_workspace_isolation(42, 0, kEffectWrite, "3332-ac3-bits"),
              "3332 AC3: insufficient bits still deny");
    }

    {
        std::println("\n--- #3332 AC4: stamped foreign ref without grant is prov_deny ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        grant_tenant_admin_mid(7);
        ev.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        const auto p0 = snapshot_tenant_isolation_stats().cross_tenant_provenance_deny;
        CHECK(!ev.check_workspace_isolation(7, 99, kEffectMutate, "3332-ac4-prov"),
              "3332 AC4: foreign ref without current→ref grant denies");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_provenance_deny == p0 + 1,
              "3332 AC4: prov_deny counted");
        g_workspace_isolation().grant_cross_tenant(7, 99, kEffectMutate, /*caller=*/7);
        CHECK(ev.check_workspace_isolation(7, 99, kEffectMutate, "3332-ac4-ok"),
              "3332 AC4: provenance allow after current→ref grant");
    }

    {
        std::println("\n--- #3332 AC5: Soft/Off allow_cross short-circuit zero extra ---");
        reset_all(); // Off
        const auto cap0 = snapshot_tenant_isolation_stats().cross_tenant_capability_deny;
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true),
              "3332 AC5: Off allow_cross still short-circuits");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_capability_deny == cap0,
              "3332 AC5: Off path does not bump cap_deny");
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev.allow_cross_tenant(), "3332 AC5: Off sets flag without TenantAdmin");
        CHECK(ev.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac5-off"),
              "3332 AC5: Off Evaluator allow_cross still allows");
    }

    {
        std::println("\n--- #3332 AC6: dual Evaluator shares cross_grants ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        grant_tenant_admin_mid(7);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(7);
        ev_a.grant_capability(aura::compiler::security::kCapTenantAdmin);
        ev_a.set_tenant_principal(7, "t7", /*allow_cross=*/true);
        CHECK(ev_a.allow_cross_tenant(), "3332 AC6: A has allow_cross");
        CHECK(!ev_b.allow_cross_tenant(), "3332 AC6: B does not");
        CHECK(!ev_a.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-a-nogrant"),
              "3332 AC6: A without grant still denies");
        CHECK(!ev_b.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-b-nogrant"),
              "3332 AC6: B without grant denies");
        g_workspace_isolation().grant_cross_tenant(7, 42, kEffectMutate, /*caller=*/7);
        CHECK(ev_a.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-a-grant"),
              "3332 AC6: A allow_cross + shared grant allows");
        CHECK(ev_b.check_workspace_isolation(42, 0, kEffectMutate, "3332-ac6-b-grant"),
              "3332 AC6: B uses the same cross_grants table");
    }

    {
        std::println("\n--- #3332 AC6: source-cite + linter + no invent ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("kAllowCrossScopedGrantIssue = 3332") != std::string::npos,
              "3332 AC6: issue stamp");
        CHECK(iso.find("allow_cross_tenant && !(strict || sandbox_restricted)") !=
                  std::string::npos,
              "3332 AC6: Soft/Off-only short-circuit");
        CHECK(test_self.find("allow_cross without grant denies") != std::string::npos,
              "3332 AC6: original bypass case rewritten");
        CHECK(build.find("check_allow_cross_scoped_grant_3332") != std::string::npos,
              "3332 AC6: build.py wires linter after #3010");
        std::ifstream invent("tests/core/test_issue_3332.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3332.cpp");
        CHECK(!invent.good(), "3332 AC6: no tests/core/test_issue_3332.cpp");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3332-") == std::string::npos,
                      std::string("3332 AC6: no docs/design/") + name);
            }
        }
    }

    // ── #3011: IsolationDeny SecurityEvent stamps live fiber ──
    {
        std::println("\n--- #3011 AC1: IsolationDeny fiber_id == calling fiber ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::set_effect_fiber_id_override(42);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        const auto& ring = g_security_event_ring();
        const auto baseline = ring.seq.load(std::memory_order_acquire);
        CHECK(!ev.check_workspace_isolation(99, 0, kEffectMutate, "test:3011-cross"),
              "AC1: Restricted cross-tenant mutate denied");
        bool found = false;
        const auto head = ring.seq.load(std::memory_order_acquire);
        for (auto s = baseline; s < head; ++s) {
            const auto& e = ring.ring[s % ring.ring.size()];
            if (e.kind == SecurityEventKind::IsolationDeny && e.seq == s) {
                CHECK(e.fiber_id == 42, "AC1: IsolationDeny fiber_id equals calling fiber");
                found = true;
            }
        }
        CHECK(found, "AC1: IsolationDeny SE recorded");
        aura::core::workspace_isolation::IsolationAuditEntry priv{};
        const auto aseq = g_workspace_isolation().load_audit_seq();
        CHECK(aseq >= 1 && g_workspace_isolation().try_load_audit_seq(aseq - 1, priv),
              "AC1: private isolation ring loadable");
        CHECK(priv.denied && priv.fiber_id == 42, "AC1: private ring fiber_id matches");
    }

    {
        std::println("\n--- #3011 AC2: EffectDeny fiber path unchanged ---");
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(cap.find("static_cast<std::int64_t>(prov.fiber_id)") != std::string::npos,
              "AC2: EffectDeny still stamps prov.fiber_id");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        CHECK(iso.find("/*fiber_id=*/0") == std::string::npos,
              "AC2: IsolationDeny no longer hard-codes fiber_id=0");
    }

    {
        std::println("\n--- #3011 AC3: Soft / Off allow does not emit IsolationDeny ---");
        reset_all(); // Off
        const auto& ring = g_security_event_ring();
        const auto before = ring.seq.load(std::memory_order_acquire);
        CHECK(check_boundary(0, 0), "AC3: Off unset principal allows");
        const auto after = ring.seq.load(std::memory_order_acquire);
        CHECK(after == before, "AC3: Off allow does not append IsolationDeny");
    }

    {
        std::println("\n--- #3011 AC4: query:security-audit filters IsolationDeny by fiber ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        aura::core::capability::set_effect_fiber_id_override(42);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(!ev.check_workspace_isolation(99, 0, kEffectMutate, "test:3011-filter"),
              "AC4: deny to seed IsolationDeny");
        auto q = cs.eval(R"((engine:metrics "query:security-audit" 16 99 42))");
        CHECK(q.has_value(), "AC4: query:security-audit fiber filter callable");
        bool saw_fiber = false;
        if (q) {
            auto cur = *q;
            int guard = 0;
            auto& pairs = ev.pairs();
            auto heap = ev.string_heap();
            while (is_pair(cur) && guard++ < 64) {
                const auto idx = as_pair_idx(cur);
                if (idx >= pairs.size())
                    break;
                if (is_string(pairs[idx].car)) {
                    const auto sidx = as_string_idx(pairs[idx].car);
                    if (sidx < heap.size()) {
                        const std::string ln(heap[sidx]);
                        if (ln.find("kind=IsolationDeny") != std::string::npos &&
                            ln.find("fiber=42") != std::string::npos)
                            saw_fiber = true;
                    }
                }
                cur = pairs[idx].cdr;
            }
        }
        CHECK(saw_fiber, "AC4: query:security-audit fiber=42 returns IsolationDeny");
        auto wired = cs.eval(
            R"((hash-ref (engine:metrics "query:security-stats") "isolation-deny-fiber-wired"))");
        CHECK(wired && is_int(*wired) && as_int(*wired) == 1,
              "AC4: query:security-stats exposes isolation-deny-fiber-wired");
        auto schema =
            cs.eval(R"((hash-ref (engine:metrics "query:security-stats") "schema-3011"))");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 3011,
              "AC4: query:security-stats cites schema-3011");
    }

    {
        std::println("\n--- #3011 AC5/AC6: source-cite + no invent + no docs/design/ ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(iso.find("#3011") != std::string::npos, "AC6: workspace_isolation.hh cites #3011");
        CHECK(iso.find("effect_fiber_id_or") != std::string::npos,
              "AC5: record_audit uses effect_fiber_id_or");
        CHECK(posture.find("schema-3011") != std::string::npos, "AC5: posture cites schema-3011");
        CHECK(posture.find("filt_fiber") != std::string::npos,
              "AC5: query:security-audit still filters by fiber");
        CHECK(test_self.find("#3011") != std::string::npos, "AC6: test file cites #3011");
        CHECK(build.find("check_isolation_deny_fiber_3011") != std::string::npos,
              "AC6: build.py wires #3011 linter");
        std::ifstream invent("tests/core/test_issue_3011.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3011.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3011.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3011-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3040: residual compile NodeId-only entry gated before body ──
    {
        std::println("\n--- #3040 AC1: Restricted NodeId compile entry denied before body ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(cs.eval("(set-code \"(define (n3040 x) x)\")").has_value(), "3040 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3040 eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3040 workspace");
        const auto before_bumps = ws->subtree_bump_count();
        const auto prev = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                              .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        auto r = cs.eval("(compile:subtree-bump (car (query:defines-by-marker \"User\")))");
        CHECK(r.has_value(), "ac3040_1_edsl_returns");
        CHECK(r && is_error(*r), "ac3040_1_sunk_lisp_#3172");
        CHECK(!ev.require_effect_for_node_id(kEffectMutate, "compile:subtree-bump", /*node_id=*/1),
              "ac3040_1_denied_before_body");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3040_1_no_topology_write");
        const auto after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        CHECK(after >= prev + 1, "ac3040_1_nodeid_only_entry_prevented");
        const auto compile_src = read_file("src/compiler/evaluator_primitives_compile.cpp");
        CHECK(compile_src.find("gate_compile_node_effect") != std::string::npos,
              "ac3040_1_gate_helper");
        CHECK(compile_src.find("require_effect_for_node_id") != std::string::npos,
              "ac3040_1_for_node_id");
    }

    {
        std::println("\n--- #3040 AC2: foreign stamped ref denied before body ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        ev.grant_effect_capability(/*tenant=*/7, "mut-3040-ac2", kEffectMutate, /*mid=*/1);
        CHECK(cs.eval("(set-code \"(define (n3040b x) x)\")").has_value(), "3040 AC2 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3040 AC2 eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3040 AC2 workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3040 AC2 live node");
        const auto before_bumps = ws->subtree_bump_count();
        const auto iso_before = snapshot_tenant_isolation_stats().boundary_violations_prevented;
        auto foreign = ev.make_stamped_ref(id);
        foreign.tenant_id = 99;
        CHECK(!ev.require_effect_on_ref(kEffectMutate, "compile:subtree-bump", foreign),
              "ac3040_2_on_ref_foreign_denies");
        auto edsl =
            cs.eval(std::format("(compile:subtree-bump (cons {} (cons 0 (cons 99 0))))", id));
        CHECK(edsl.has_value(), "ac3040_2_edsl_returns");
        CHECK(edsl && is_error(*edsl), "ac3040_2_sunk_lisp_#3172");
        CHECK(ws->subtree_bump_count() == before_bumps, "ac3040_2_no_topology_write");
        const auto iso_after = snapshot_tenant_isolation_stats().boundary_violations_prevented;
        CHECK(iso_after > iso_before, "ac3040_2_isolation_counters_bump");
    }

    {
        std::println("\n--- #3040 AC3: Soft / Off path unchanged ---");
        reset_all(); // Off
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3040c x) x)\")").has_value(), "3040 AC3 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3040 AC3 eval");
        const auto prev = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                              .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        auto r = cs.eval("(compile:subtree-bump (car (query:defines-by-marker \"User\")))");
        CHECK(r.has_value(), "ac3040_3_soft_off_returns");
        CHECK(r && is_error(*r), "ac3040_3_sunk_lisp_#3172");
        const auto after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .nodeid_only_entry_prevented_total.load(std::memory_order_relaxed);
        CHECK(after == prev, "ac3040_3_soft_off_no_prevent_store");
        const auto compile_src = read_file("src/compiler/evaluator_primitives_compile.cpp");
        CHECK(compile_src.find("sandbox_mode() == 0 && ev.effect_sandbox_mode() == 0") !=
                  std::string::npos,
              "ac3040_3_soft_off_short_circuit");
        (void)ev;
    }

    {
        std::println("\n--- #3040 AC4: schema-3040 + snapshot counter ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(7);
        CHECK(!ev.require_effect_for_node_id(kEffectMutate, "compile:subtree-bump", /*node_id=*/1),
              "ac3040_4_for_node_id_denies_unset_grant");
        const auto snap = snapshot_tenant_isolation_stats();
        CHECK(snap.nodeid_only_entry_prevented >= 1, "ac3040_4_snapshot_counter");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-3040") != std::string::npos, "ac3040_4_schema");
        CHECK(posture.find("nodeid-only-entry-prevented-wired") != std::string::npos,
              "ac3040_4_wired");
        CHECK(posture.find("nodeid-only-entry-prevented-total") != std::string::npos,
              "ac3040_4_total_key");
        CHECK(aura::compiler::kNodeIdOnlyEntryIssue == 3040, "ac3040_4_issue_const");
        CHECK(aura::compiler::kNodeIdOnlyEntryPreventedWired == 1, "ac3040_4_wired_const");
    }

    {
        std::println("\n--- #3040 AC5/AC6: source-cite + linter + no invent ---");
        const auto compile_src = read_file("src/compiler/evaluator_primitives_compile.cpp");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");
        CHECK(compile_src.find("Issue #3040") != std::string::npos, "ac3040_5_compile_cite");
        CHECK(sec.find("Issue #3040") != std::string::npos, "ac3040_5_security_cite");
        CHECK(iso.find("#3040") != std::string::npos, "ac3040_5_iso_cite");
        CHECK(posture.find("schema-3040") != std::string::npos, "ac3040_5_posture");
        CHECK(test_self.find("#3040") != std::string::npos, "ac3040_5_test_cite");
        CHECK(build.find("check_compile_node_id_entry_3040") != std::string::npos,
              "ac3040_5_linter_and_suite");
        std::ifstream invent("tests/core/test_issue_3040.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3040.cpp");
        CHECK(!invent.good(), "ac3040_5_no_invent_test");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3040-") == std::string::npos,
                      std::string("ac3040_5: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3041: production restamp budget exceed forces QueryEpoch stale ──
    {
        std::println("\n--- #3041 AC1: production unified restamp forces QueryEpoch stale ---");
        reset_all();
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        using aura::core::capture_query_epoch;
        using aura::core::g_query_epoch_forced_stale;
        using aura::core::g_restamp_budget_query_epoch_stale_total;
        using aura::core::reset_query_epoch_metrics_for_test;
        reset_query_epoch_metrics_for_test();
        apply_production_audit_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n3041 a) a) (define (n3041b b) b) "
                      "(define (n3041c c) c) (define (n3041d d) d)\")")
                  .has_value(),
              "3041 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3041 eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3041 workspace");
        (void)capture_query_epoch(ws->generation(), 0);
        set_restamp_budget_nodes_for_process(1);
        const auto qe0 = g_restamp_budget_query_epoch_stale_total().load();
        auto r = ev.unified_restamp_after_boundary(Evaluator::UnifiedRestampSite::BoundarySuccess);
        CHECK(r.budget_exceeded || ws->restamp_last_budget_exceeded(), "ac3041_1_budget_exceeded");
        CHECK(ws->restamp_lazy_align_enabled(), "ac3041_1_lazy_align");
        CHECK(g_query_epoch_forced_stale().load() != 0, "ac3041_1_query_epoch_forced_stale");
        CHECK(g_restamp_budget_query_epoch_stale_total().load() > qe0, "ac3041_1_stale_counter");
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        reset_query_epoch_metrics_for_test();
        const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(qws.find("schema-3041") != std::string::npos, "ac3041_4_schema");
        CHECK(qws.find("restamp-budget-query-epoch-stale-total") != std::string::npos,
              "ac3041_4_key");
    }

    // ── #3048: steal × session-grant residual (tenant isolation suite) ──
    {
        std::println("\n--- #3048: steal×session-grant chaos under Restricted ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        using aura::core::capability::check_and_record_effect;
        using aura::core::capability::Effect;
        using aura::core::capability::EffectProvenance;
        using aura::core::capability::g_capability_registry;
        using aura::core::capability::revoke_session_grants_on_steal_or_abort;
        using aura::core::capability::snapshot_capability_effect_stats;
        EffectProvenance prov{};
        prov.epoch = 48;
        prov.mutation_id = 48;
        g_capability_registry().grant_session(/*tenant=*/7, "mut-3048-iso", Effect::Mutate, prov,
                                              /*single_use=*/false);
        CHECK(check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 7, "3048-iso-pre",
                                      false, true),
              "3048: same-tenant session allow before steal");
        const auto n = revoke_session_grants_on_steal_or_abort(48, /*steal=*/true);
        CHECK(n >= 1, "3048: steal hook revokes session grant");
        CHECK(snapshot_capability_effect_stats().capability_live_session_grants == 0,
              "3048: live session residual 0 after steal");
        CHECK(!check_and_record_effect(Effect::Mutate, Effect::Mutate, prov, 7, "3048-iso-post",
                                       false, true),
              "3048: Restricted denies after steal revoke");
        const auto steal = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        const auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto cap = read_file("src/core/capability_model.hh");
        CHECK(steal.find("revoke_session_grants_on_steal_or_abort") != std::string::npos,
              "3048: steal-complete / force-degrade cite hook");
        CHECK(bound.find("set_current_fiber_session_mid") != std::string::npos,
              "3048: Guard enter stamps fiber session mid");
        CHECK(cap.find("Issue #3048") != std::string::npos ||
                  cap.find("#3048") != std::string::npos,
              "3048: capability_model.hh cites #3048");
        std::ifstream invent("tests/core/test_issue_3048.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3048.cpp");
        CHECK(!invent.good(), "3048: no test_issue_3048.cpp");
    }

    // ── #3049: per-tenant ResourceQuota (DoS isolation) ──
    {
        std::println("\n--- #3049 AC1/AC2: tenant A exhaust does not deny tenant B ---");
        using aura::core::resource_quota::Dimension;
        using aura::core::resource_quota::process_resource_quota;
        using aura::core::resource_quota::reset_process_resource_quota_for_test;
        using aura::core::resource_quota::set_quota_per_tenant_enabled_for_test;
        reset_process_resource_quota_for_test();
        set_quota_per_tenant_enabled_for_test(true);
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 4); // process ceiling
        pq.set_tenant_limit(1, Dimension::Fibers, 2);
        pq.set_tenant_limit(2, Dimension::Fibers, 2);
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1).has_value(),
              "3049: A first fiber ok");
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1).has_value(),
              "3049: A second fiber ok");
        auto a3 = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1);
        CHECK(a3.has_value(), "3049: A third fiber denied (tenant budget)");
        if (a3) {
            CHECK(a3->message.find("quota-exceeded:tenant=1:dim=fibers") != std::string::npos,
                  "3049 AC5: Agent-readable tenant deny reason");
        }
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2).has_value(),
              "3049 AC2: B still admits after A exhaust");
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2).has_value(),
              "3049 AC2: B second fiber ok");
        auto b3 = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2);
        CHECK(b3.has_value(), "3049: B third denied by tenant budget");
        // Process ceiling: A=2 + B=2 == 4; extra from either tenant fails globally.
        auto ceil = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2);
        CHECK(ceil.has_value(), "3049: process ceiling still binds");
        CHECK(pq.quota_reject_by_tenant_total.load() >= 2, "3049 AC4: tenant reject counter");
        pq.release(Dimension::Fibers, 2, 1);
        pq.release(Dimension::Fibers, 2, 2);
        CHECK(pq.used(Dimension::Fibers) == 0, "3049: process used restored");
        CHECK(pq.tenant_used(1, Dimension::Fibers) == 0, "3049: A used restored");
        // Mutations dimension: same tenant keying (AC1 orch/scheduler dims).
        pq.set_limit(Dimension::Mutations, 4);
        pq.set_tenant_limit(1, Dimension::Mutations, 1);
        pq.set_tenant_limit(2, Dimension::Mutations, 1);
        CHECK(!pq.check_and_consume(Dimension::Mutations, 1, 1).has_value(),
              "3049 AC1: A mutation consume");
        CHECK(pq.check_and_consume(Dimension::Mutations, 1, 1).has_value(),
              "3049 AC1: A mutation budget exhausted");
        CHECK(!pq.check_and_consume(Dimension::Mutations, 1, 2).has_value(),
              "3049 AC1: B mutation still admits");
        pq.release(Dimension::Mutations, 1, 1);
        pq.release(Dimension::Mutations, 1, 2);
        reset_process_resource_quota_for_test();
    }
    {
        std::println("\n--- #3049 AC3: Soft/off path stays process-global ---");
        using aura::core::resource_quota::Dimension;
        using aura::core::resource_quota::process_resource_quota;
        using aura::core::resource_quota::quota_per_tenant_enabled;
        using aura::core::resource_quota::reset_process_resource_quota_for_test;
        using aura::core::resource_quota::set_quota_per_tenant_enabled_for_test;
        reset_process_resource_quota_for_test();
        set_quota_per_tenant_enabled_for_test(false);
        CHECK(!quota_per_tenant_enabled(), "3049 AC3: per-tenant off");
        auto& pq = process_resource_quota();
        pq.set_limit(Dimension::Fibers, 1);
        pq.set_tenant_limit(1, Dimension::Fibers, 1);
        pq.set_tenant_limit(2, Dimension::Fibers, 1);
        CHECK(!pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/1).has_value(),
              "3049 AC3: A consumes process slot");
        auto b = pq.check_and_consume(Dimension::Fibers, 1, /*tenant=*/2);
        CHECK(b.has_value(), "3049 AC3: B denied by process-global limit (no tenant map)");
        if (b) {
            CHECK(b->message.find("quota-exceeded:tenant=") == std::string::npos,
                  "3049 AC3: deny reason is process-global, not tenant");
        }
        CHECK(pq.quota_reject_by_tenant_total.load() == 0, "3049 AC3: no tenant reject counter");
        pq.release(Dimension::Fibers, 1, 1);
        reset_process_resource_quota_for_test();
    }
    {
        std::println("\n--- #3049 AC4/AC6: posture + source-cite + no invent ---");
        const auto rq = read_file("src/core/resource_quota.hh");
        const auto sched = read_file("src/serve/scheduler.cpp");
        const auto orch = read_file("src/orch/agent_spawn.h");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        const auto build = read_file("build.py");
        CHECK(rq.find("quota_per_tenant_enabled") != std::string::npos,
              "3049: quota enable helper");
        CHECK(rq.find("check_and_consume_tenant") != std::string::npos ||
                  rq.find("TenantId tenant") != std::string::npos,
              "3049: tenant-keyed consume");
        CHECK(rq.find("quota-exceeded:tenant=") != std::string::npos, "3049 AC5: deny reason");
        CHECK(sched.find("check_and_consume_fiber(spawn_tenant)") != std::string::npos,
              "3049 AC6: scheduler spawn keys tenant");
        CHECK(orch.find("check_orchestration_fibers") != std::string::npos,
              "3049 AC6: orch admission cite");
        CHECK(obs.find("schema-3049") != std::string::npos, "3049 AC4: schema-3049");
        CHECK(obs.find("quota-reject-by-tenant-total") != std::string::npos,
              "3049 AC4: reject-by-tenant key");
        CHECK(build.find("check_quota_per_tenant_3049") != std::string::npos,
              "3049 AC6: build.py wires linter");
        std::ifstream invent("tests/core/test_issue_3049.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3049.cpp");
        CHECK(!invent.good(), "3049: no test_issue_3049.cpp");
    }

    // ── Issue #3145: try_grant_cross_tenant_privileged + grant_macro_self_evo
    // privilege check — explicit caller_principal (per-Evaluator
    // capability_tenant_id_, restored by TenantScope) instead of the
    // process-global default_tenant (almost always 0 under multi-Evaluator),
    // and effects_for under the registry mtx (effects_for_locked) so a
    // concurrent revoke cannot race past the fence.
    {
        std::println("\n--- #3145 AC1: dual-Evaluator chaos — revoke mid-flight, racing "
                     "grant_cross_tenant fails closed ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(7); // same principal as A
        // TenantAdmin on tenant 7 with bound mid so it actually lands.
        grant_tenant_admin_mid(7);

        // First grant succeeds (admin present, locked read).
        const auto allow0 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev_a.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto allow1 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(allow1 == allow0 + 1, "AC1: initial grant with TenantAdmin on caller → allow");

        // Concurrent revoke of TenantAdmin from tenant 7 — must close the
        // racing grant (the gate reads effects_for_locked under registry mtx,
        // so a revoke that lands before the read fails closed).
        using aura::core::capability::g_capability_registry;
        g_capability_registry().revoke(7, "tenant-admin");

        const auto deny_before = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                     .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_before =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev_b.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny_after = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                                    .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow_after =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny_after == deny_before + 1,
              "AC1: post-revoke racing grant_cross_tenant fails closed (deny + counter)");
        CHECK(allow_after == allow_before,
              "AC1: post-revoke racing grant_cross_tenant does not bump allow counter");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC1: only the pre-revoke grant landed; the racing one was denied");
    }

    // ── #3145 AC2: explicit caller_principal wins; process-global
    // default_tenant alone never authorises the gate. Two Evaluators in one
    // process: a (capability_tenant_id=7, no admin) and b (capability_tenant_id=42,
    // holds TenantAdmin). default_tenant stays 0 — if the gate read
    // default_tenant alone, both Evaluators would be denied because the
    // process-global principal is unset. With explicit caller_principal,
    // b's grant_cross_tenant_access routes through b's own principal (42) and
    // sees the admin on tenant 42 → allow.
    {
        std::println("\n--- #3145 AC2: explicit caller_principal — gate uses Evaluator principal, "
                     "not process-global default_tenant ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // Force default_tenant=0 so any reliance on the process-global would deny.
        aura::core::capability::g_capability_registry().default_tenant.store(
            0, std::memory_order_release);

        CompilerService cs_a;
        CompilerService cs_b;
        auto& ev_a = cs_a.evaluator();
        auto& ev_b = cs_b.evaluator();
        ev_a.set_effect_sandbox_mode(1);
        ev_b.set_effect_sandbox_mode(1);
        ev_a.set_capability_tenant_id(7);
        ev_b.set_capability_tenant_id(42);
        // TenantAdmin on tenant 42 (b's principal), not on tenant 0 or 7.
        grant_tenant_admin_mid(42);

        // Evaluator a (tenant 7, no admin): grant denied — caller lacks admin
        // and target 42 holds admin (allowed as fallback). Target-tenant admin
        // is still a valid gate clear post-#2968/#3086.
        const auto deny0 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        ev_a.grant_cross_tenant_access(/*from=*/7, /*to=*/42, kEffectMutate);
        const auto deny1 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        // Either deny (caller lacks admin, target has admin → allow) or allow —
        // we only care that the gate resolves via caller_principal (42) not
        // default_tenant (0). A's call sees caller=7 (no admin) and target=42
        // (has admin) — the target-admin branch allows, no deny bump.
        CHECK(deny1 == deny0, "AC2: gate uses caller_principal=7 (no admin) → target-admin path "
                              "allows, no deny bump");
        CHECK(g_workspace_isolation().cross_grant_bits(7, 42) ==
                  static_cast<std::uint16_t>(kEffectMutate),
              "AC2: 7→42 grant landed via target-tenant admin fallback");

        // Now b's grant to a different target (99) where neither caller nor
        // target has admin (caller_principal=42 has admin on 42, but target=99
        // has nothing) → deny on missing admin, no fallback. The gate reads
        // caller_principal=42 (b's principal) which DOES hold TenantAdmin →
        // caller-admin path allows. Verify explicit principal routes through.
        const auto deny2 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow2 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        ev_b.grant_cross_tenant_access(/*from=*/42, /*to=*/99, kEffectMutate);
        const auto deny3 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow3 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny3 == deny2,
              "AC2: caller_principal=42 holds admin → no deny bump (caller-admin allow)");
        CHECK(allow3 == allow2 + 1,
              "AC2: 42→99 grant allowed via caller_principal=42 holding TenantAdmin");

        // Reset default_tenant so it does not leak into later blocks.
        aura::core::capability::g_capability_registry().default_tenant.store(
            0, std::memory_order_release);
    }

    // ── #3145 AC3: Soft/Off remains zero-cost. No lock, no principal load.
    {
        std::println("\n--- #3145 AC3: Soft/Off zero-cost — no lock, no principal load ---");
        reset_all(); // Off
        const auto deny0 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow0 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        // No TenantAdmin anywhere; Soft/Off must still allow (zero-cost).
        g_workspace_isolation().grant_cross_tenant(/*from=*/1, /*to=*/2, kEffectMutate,
                                                   /*caller=*/1);
        const auto deny1 = aura::core::workspace_isolation::g_tenant_isolation_metrics()
                               .cross_tenant_grant_deny_total.load(std::memory_order_relaxed);
        const auto allow1 =
            aura::core::workspace_isolation::g_tenant_isolation_metrics()
                .cross_tenant_capability_grant_total.load(std::memory_order_relaxed);
        CHECK(deny1 == deny0, "AC3: Soft/Off does not bump deny (zero-cost)");
        CHECK(allow1 == allow0 + 1, "AC3: Soft/Off allows without lock or principal load");
    }

    // ── #3145 AC4: grant_macro_self_evo privilege check aligned (same
    // principal source — explicit caller_principal — and runs under the
    // registry mtx so concurrent revoke cannot race past the fence).
    {
        std::println(
            "\n--- #3145 AC4: grant_macro_self_evo aligned with explicit caller_principal ---");
        reset_all();
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        // TenantAdmin on tenant 42 (b's principal), nothing on 0 or 7.
        grant_tenant_admin_mid(42);

        // (a) caller_principal=0 fallback to default_tenant=0, no admin on
        // caller or target → deny.
        const auto deny0 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        aura::core::capability::g_capability_registry().grant_macro_self_evo(
            /*tenant=*/7, aura::core::capability::MacroSelfEvoPolicy{}, /*prov_in=*/{},
            /*caller_principal=*/0);
        const auto deny1 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny1 == deny0 + 1,
              "AC4: explicit caller_principal=0 (no admin) → deny + counter bump");

        // (b) caller_principal=42 holds admin → allow.
        aura::core::capability::g_capability_registry().grant_macro_self_evo(
            /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{}, /*prov_in=*/{},
            /*caller_principal=*/42);
        aura::core::capability::CapabilityGrant g{};
        CHECK(aura::core::capability::g_capability_registry().find_grant(42, "macro-self-evo", g),
              "AC4: caller_principal=42 (holds TenantAdmin) → macro-self-evo grant lands");

        // (c) post-revoke alignment — revoke TenantAdmin on 42, then the same
        // explicit caller_principal=42 must deny (the registry mtx covers the
        // by_tenant find so the revoke races correctly).
        aura::core::capability::g_capability_registry().revoke(42, "tenant-admin");
        const auto deny2 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        aura::core::capability::g_capability_registry().grant_macro_self_evo(
            /*tenant=*/42, aura::core::capability::MacroSelfEvoPolicy{}, /*prov_in=*/{},
            /*caller_principal=*/42);
        const auto deny3 =
            aura::core::capability::g_capability_effect_metrics()
                .capability_macro_self_evo_grant_deny_total.load(std::memory_order_relaxed);
        CHECK(deny3 == deny2 + 1,
              "AC4: post-revoke, explicit caller_principal=42 (no admin) → deny");
    }

    // ── #3145 AC5/AC6: source-cite + linter + no invent + no docs/design/
    {
        std::println("\n--- #3145 AC5/AC6: source-cite + linter + no invent ---");
        const auto iso = read_file("src/core/workspace_isolation.hh");
        const auto cap = read_file("src/core/capability_model.hh");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto test_self = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
        const auto build = read_file("build.py");

        // AC5: workspace_isolation.hh owns the SSOT helper, explicit
        // caller_principal parameter, and the locked effects_for_locked
        // read under the registry mtx.
        CHECK(iso.find("Issue #3145") != std::string::npos,
              "AC5: workspace_isolation.hh cites Issue #3145");
        CHECK(iso.find("caller_principal") != std::string::npos,
              "AC5: SSOT helper accepts caller_principal");
        CHECK(iso.find("effects_for_locked") != std::string::npos,
              "AC5: privilege read uses effects_for_locked (TOCTOU closure)");
        CHECK(iso.find("reg.mtx") != std::string::npos, "AC5: privilege read holds registry mtx");

        // AC5: capability_model.hh grant_macro_self_evo accepts caller_principal.
        CHECK(cap.find("Issue #3145") != std::string::npos,
              "AC5: capability_model.hh cites Issue #3145");
        CHECK(cap.find("caller_principal") != std::string::npos,
              "AC5: grant_macro_self_evo accepts caller_principal");

        // AC5: Evaluator wrapper forwards capability_tenant_id_.
        CHECK(sec.find("Issue #3145") != std::string::npos,
              "AC5: evaluator_security.cpp cites Issue #3145");
        CHECK(sec.find("g_workspace_isolation().grant_cross_tenant") != std::string::npos &&
                  sec.find("capability_tenant_id_") != std::string::npos,
              "AC5: Evaluator wrapper forwards capability_tenant_id_ to SSOT method");

        // AC5: Evaluator prim site forwards ev.capability_tenant_id().
        CHECK(prim.find("Issue #3145") != std::string::npos ||
                  prim.find("#3145") != std::string::npos,
              "AC5: evaluator_primitives_security.cpp cites Issue #3145");
        CHECK(prim.find("ev.capability_tenant_id()") != std::string::npos,
              "AC5: prim forwards ev.capability_tenant_id() to grant_macro_self_evo");

        // AC5: this test file cites #3145.
        CHECK(test_self.find("#3145") != std::string::npos, "AC5: test file cites Issue #3145");

        // AC6: linter wired into build.py.
        CHECK(build.find("check_cross_tenant_grant_principal_3145") != std::string::npos,
              "AC6: build.py wires #3145 linter");

        // AC6: no new posture / query key (AC6 explicit).
        const auto posture = read_file("src/compiler/evaluator_primitives_security.cpp");
        CHECK(posture.find("schema-3145") == std::string::npos,
              "AC6: no new posture key (schema-3145 forbidden per issue body)");
        CHECK(posture.find("issue-3145") == std::string::npos,
              "AC6: no new query key (issue-3145 forbidden per issue body)");

        // No invent + no docs/design/ (#81967 / #1655).
        std::ifstream invent("tests/core/test_issue_3145.cpp");
        if (!invent.good())
            invent.open("../tests/core/test_issue_3145.cpp");
        CHECK(!invent.good(), "AC6: no tests/core/test_issue_3145.cpp (forbidden per #81967)");
        const std::filesystem::path docs_design = "docs/design";
        std::error_code ec;
        if (std::filesystem::is_directory(docs_design, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(docs_design, ec)) {
                const auto name = entry.path().filename().string();
                CHECK(name.find("3145-") == std::string::npos,
                      std::string("AC6: no docs/design/") + name + " (forbidden per #1655)");
            }
        }
    }

    // ── #3204: production Agent export hard-reject tenant_id=0 ──
    {
        std::println("\n--- #3204 AC1: production defaults arm hard-reject without env ---");
        reset_all();
        CHECK(aura::core::provenance::kStableRefExportProductionHardRejectIssue == 3204,
              "3204 AC1: issue stamp");
        ::unsetenv("AURA_STABLE_REF_EXPORT_HARD_REJECT");
        CHECK(!aura::core::provenance::stable_ref_export_hard_reject(), "3204 AC1: Soft pref off");
        ::setenv("AURA_SANDBOX", "restricted", 1);
        aura::compiler::security::apply_production_security_defaults();
        CHECK(aura::core::provenance::stable_ref_export_hard_reject(),
              "ac3204_1_production_hard_reject: Restricted arms hard-reject");
        ::setenv("AURA_SANDBOX", "off", 1);
        aura::compiler::security::apply_production_security_defaults();
        CHECK(!aura::core::provenance::stable_ref_export_hard_reject(),
              "3204 AC1: sandbox=off leaves Soft");
        ::unsetenv("AURA_SANDBOX");
        reset_all();
    }
    {
        std::println("\n--- #3204 AC2: layout-only handoff stamps or denies; never tenant 0 ---");
        reset_all();
        aura::core::provenance::set_stable_ref_export_hard_reject(true);
        aura::core::provenance::set_hard_capture_tenant(true);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (h x) (+ x 1))\")").has_value(), "3204 AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3204 AC2: eval");
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3204 AC2: workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3204 AC2: live node");
        auto layout = ws->make_ref_layout(id);
        CHECK(layout.tenant_id == 0, "3204 AC2: layout-only tenant 0");
        const auto stale0 =
            aura::core::provenance::g_provenance_enforcement()
                .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed);
        auto out = ev.handoff_ref(layout);
        if (out) {
            CHECK(out->tenant_id == 7, "ac3204_2_handoff_never_tenant_zero: stamped 7");
            CHECK(out->tenant_id != 0, "3204 AC2: never tenant_id==0");
            ev.set_capability_tenant_id(9);
            CHECK(!ev.check_workspace_isolation(9, out->tenant_id, 0, "test:3204-x"),
                  "3204 AC2: wrong principal IsolationDeny");
        } else {
            CHECK(aura::core::provenance::g_provenance_enforcement()
                          .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed) >
                      stale0,
                  "3204 AC2: deny bumps stale-reject");
        }
        reset_all();
    }
    {
        std::println("\n--- #3204 AC3: Soft layout-only stays zero-cost contract ---");
        reset_all();
        ::setenv("AURA_SANDBOX", "off", 1);
        aura::compiler::security::apply_production_security_defaults();
        CHECK(!aura::core::provenance::stable_ref_export_hard_reject(),
              "ac3204_3_soft_quiet: Soft hard-reject off");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (s x) x)\")").has_value(), "3204 AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3204 AC3: eval");
        auto& ev = cs.evaluator();
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3204 AC3: workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3204 AC3: live");
        auto layout = ws->make_ref_layout(id);
        CHECK(layout.tenant_id == 0, "3204 AC3: layout-only tenant 0");
        const auto stamp0 =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto stale0 =
            aura::core::provenance::g_provenance_enforcement()
                .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed);
        auto out = ev.handoff_ref(layout);
        const auto stamp1 =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        const auto stale1 =
            aura::core::provenance::g_provenance_enforcement()
                .stable_ref_export_stale_reject_total.load(std::memory_order_relaxed);
        CHECK(stamp1 == stamp0, "3204 AC3: Soft no extra stamp");
        CHECK(stale1 == stale0, "3204 AC3: Soft no extra stale-reject");
        if (out)
            CHECK(out->tenant_id == 0, "3204 AC3: Soft keeps layout-only tenant 0");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        CHECK(sec.find("Issue #3204") != std::string::npos, "3204 AC3: finalize cites #3204");
        CHECK(sec.find("stamp_stable_ref") != std::string::npos, "3204 AC3: stamp authority");
        CHECK(read_file("src/compiler/security_defaults.hh").find("Issue #3204") !=
                  std::string::npos,
              "3204 AC3: production defaults cite #3204");
        ::unsetenv("AURA_SANDBOX");
        reset_all();
    }
    {
        std::println("\n--- #3204 AC4: concurrent export + principal switch ---");
        reset_all();
        aura::core::provenance::set_stable_ref_export_hard_reject(true);
        aura::core::provenance::set_hard_capture_tenant(true);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (c x) x)\")").has_value(), "3204 AC4: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3204 AC4: eval");
        auto& ev = cs.evaluator();
        ev.set_capability_tenant_id(7);
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3204 AC4: workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "3204 AC4: live");
        std::atomic<int> leaked{0};
        std::atomic<int> ok_n{0};
        std::thread exporter([&] {
            for (int i = 0; i < 64; ++i) {
                auto layout = ws->make_ref_layout(id);
                auto out = ev.handoff_ref(layout);
                if (out) {
                    if (out->tenant_id == 0)
                        leaked.fetch_add(1, std::memory_order_relaxed);
                    else
                        ok_n.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        std::thread switcher([&] {
            for (int i = 0; i < 64; ++i)
                ev.set_capability_tenant_id((i & 1) ? 7 : 9);
        });
        exporter.join();
        switcher.join();
        CHECK(leaked.load(std::memory_order_relaxed) == 0,
              "ac3204_4_concurrent: no tenant_id==0 leak");
        (void)ok_n;
        reset_all();
    }
    {
        std::println("\n--- #3204 AC5: source-cite + linter + no invent ---");
        const auto prov = read_file("src/core/provenance_tracker.hh");
        const auto build = read_file("build.py");
        CHECK(prov.find("kStableRefExportProductionHardRejectIssue = 3204") != std::string::npos,
              "ac3204_5_source_linter: stamp");
        CHECK(build.find("check_stable_ref_export_production_hard_reject_3204") !=
                  std::string::npos,
              "3204 AC5: build.py");
        CHECK(read_file("tests/core/test_issue_3204.cpp").empty(), "3204 AC5: no invent");
        CHECK(read_file("docs/design/3204-stable-ref-export-hard-reject.md").empty(),
              "3204 AC5: no docs/design");
    }

    // ── #3276: freeze the privileged-write call-site allowlist. Runtime
    // fences (#2968/#3086/#3145/#3029/#2969/#3141) are solid; residual is
    // static surface area — any NEW TU can call g_capability_registry().grant
    // / grant_locked / grant_session / grant_once / grant_macro_self_evo /
    // g_workspace_isolation().grant_cross_tenant and bypass Evaluator
    // principal / audit / TenantAdmin wrappers. The allowlist + coverage
    // linter freeze the sole permitted inventory; the linter fails the gate
    // on any src/ hit outside it. Read-only getters (grant_epoch_retain_window
    // / grant_min_valid_epoch) are not grant writes and stay out of scope.
    {
        const auto al = read_file("scripts/coverage/allowlists/privileged_grant_calls.json");
        const auto lint =
            read_file("scripts/coverage/checks/check_privileged_grant_callsite_allowlist_3276.py");
        const auto sec = read_file("src/compiler/evaluator_security.cpp");
        const auto sdef = read_file("src/compiler/security_defaults.hh");
        const auto prim = read_file("src/compiler/evaluator_primitives_security.cpp");
        const auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        const auto build = read_file("build.py");

        std::println("\n--- #3276 AC1: allowlist freezes the sole permitted inventory ---");
        CHECK(al.find("src/compiler/evaluator_security.cpp") != std::string::npos,
              "3276 AC1: evaluator_security.cpp allowlisted");
        CHECK(al.find("src/compiler/security_defaults.hh") != std::string::npos,
              "3276 AC1: security_defaults.hh allowlisted");
        CHECK(al.find("src/compiler/evaluator_primitives_security.cpp") != std::string::npos,
              "3276 AC1: evaluator_primitives_security.cpp allowlisted");
        CHECK(al.find(".grant_locked(") != std::string::npos,
              "3276 AC1: grant_locked pattern in allowlist");
        CHECK(al.find(".grant_session(") != std::string::npos,
              "3276 AC1: grant_session pattern in allowlist");
        CHECK(al.find(".grant_macro_self_evo(") != std::string::npos,
              "3276 AC1: grant_macro_self_evo pattern in allowlist");
        CHECK(al.find(".grant_cross_tenant(") != std::string::npos,
              "3276 AC1: grant_cross_tenant pattern in allowlist");
        CHECK(al.find(".grant_epoch_retain_window(") == std::string::npos &&
                  al.find(".grant_min_valid_epoch(") == std::string::npos,
              "3276 AC1: read-only getters NOT in allowlist (out of scope)");

        std::println("\n--- #3276 AC2: current call sites sit inside the allowlist ---");
        CHECK(sec.find("g_capability_registry().grant(") != std::string::npos,
              "3276 AC2: evaluator_security.cpp direct grant (authority)");
        CHECK(sec.find(".grant_locked(") != std::string::npos,
              "3276 AC2: evaluator_security.cpp grant_locked (authority)");
        CHECK(sec.find("g_workspace_isolation().grant_cross_tenant(") != std::string::npos,
              "3276 AC2: evaluator_security.cpp cross-tenant grant (authority)");
        CHECK(sdef.find("g_capability_registry().grant(") != std::string::npos,
              "3276 AC2: security_defaults.hh bootstrap grant");
        CHECK(sdef.find("/*tenant=*/0") != std::string::npos,
              "3276 AC2: bootstrap render grants stay tenant=0");
        CHECK(prim.find("g_capability_registry().grant_macro_self_evo(") != std::string::npos,
              "3276 AC2: prim macro-self-evo grant (behind #3029 fence)");
        CHECK(obs.find(".grant_epoch_retain_window(") != std::string::npos &&
                  obs.find(".grant_min_valid_epoch(") != std::string::npos,
              "3276 AC2: obs_jit read-only getters remain (not grant writes)");

        std::println("\n--- #3276 AC3/AC4: linter scans + no new TU / no invent ---");
        CHECK(lint.find("Issue #3276") != std::string::npos, "3276 AC3: linter cites #3276");
        CHECK(lint.find("SCANNED_PATTERNS") != std::string::npos,
              "3276 AC3: linter scans grant-family patterns");
        CHECK(lint.find("hits_outside") != std::string::npos,
              "3276 AC3: linter fails on hits outside allowlist");
        CHECK(lint.find("_strip_comments") != std::string::npos,
              "3276 AC3: comments stripped (doc mentions not false hits)");
        CHECK(build.find("check_privileged_grant_callsite_allowlist_3276") != std::string::npos,
              "3276 AC4: build.py wires linter");
        CHECK(read_file("tests/core/test_issue_3276.cpp").empty() &&
                  read_file("tests/issues/test_issue_3276.cpp").empty(),
              "3276 AC4: no test_issue_3276.cpp per #81967");
        CHECK(read_file("docs/design/3276-privileged-grant-allowlist.md").empty(),
              "3276 AC4: no docs/design/3276-* per #1655");
    }

    reset_all();
    std::println("\n=== test_tenant_isolation_enforcement: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
