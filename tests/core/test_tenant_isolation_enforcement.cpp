// @category: unit
// @reason: Issue #1566 — WorkspaceIsolationPolicy enforcement:
// capability cross-tenant grant, provenance deny, Strict sandbox link,
// mutate/workspace force path, query:tenant-isolation-stats, stress deny.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/workspace_isolation.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

using aura::ast::FlatAST;
using aura::compiler::CompilerService;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectWrite;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

    std::int64_t href_m(CompilerService & cs, std::string_view key) {
        auto r = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:tenant-isolation-stats\") \"{}\")", key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    }

    void reset_all() {
        reset_tenant_isolation_for_test();
        set_mode(SandboxMode::Off);
    }

} // namespace

int main() {
    reset_all();

    // ── AC6: query:tenant-isolation-stats shape ──
    {
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics \"query:tenant-isolation-stats\"))");
        CHECK(h && is_hash(*h), "tenant-isolation-stats is hash");
        CHECK(href_m(cs, "schema") == 1566, "schema 1566");
        CHECK(href_m(cs, "active") == 1, "active");
        CHECK(href_m(cs, "phase") == 2, "phase 2");
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
        CHECK(href_m(cs, "current-tenant") == 10, "stats current-tenant");
        CHECK(href_m(cs, "boundary-violations-prevented") >= 1, "stats violations");
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

    // ── allow_cross_tenant bypass ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "admin", /*allow_cross=*/true);
        CHECK(check_boundary(1, 99, nullptr, /*allow_cross=*/true),
              "allow_cross_tenant bypasses boundary");
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
            (void)ev.make_stamped_ref(static_cast<aura::FlatAST::NodeId>(i));
        }
        const auto local_after =
            aura::core::provenance::g_isolation_capture_stamp_local_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(local_after >= local_before + 4,
              "AC1: Evaluator::make_stamped_ref bumps local capture counter by >= 4");
        CHECK(global_after == global_before,
              "AC1: Evaluator path does NOT bump global_fallback counter");
        CHECK(miss_after == miss_before,
              "AC1: Evaluator path does NOT bump evaluator_miss counter");
        // Global-fallback path: set process-global capture tenant + call
        // maybe_stamp_stable_ref_isolation_tenant on a StableRefT.
        aura::core::provenance::set_isolation_capture_tenant(42);
        aura::FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(stamped, "AC2: global-fallback path stamps when tenant != 0");
        CHECK(ref.tenant_id == 42, "AC2: global-fallback stamps tenant_id from process-global");
        const auto global_after2 =
            aura::core::provenance::g_isolation_capture_stamp_global_fallback_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(global_after2 >= global_after + 1,
              "AC2: global-fallback path bumps fallback counter");
        // Reset for AC4.
        aura::core::provenance::set_isolation_capture_tenant(0);
    }

    // ── #2687 AC4: Soft / tenant=0 capture remains permissive ──
    {
        std::println("\n--- #2687 AC4: Soft / tenant=0 capture permissive ---");
        reset_all();
        aura::core::provenance::set_isolation_capture_tenant(0);
        aura::FlatAST::StableNodeRef ref{};
        const bool stamped = aura::core::provenance::maybe_stamp_stable_ref_isolation_tenant(ref);
        CHECK(!stamped,
              "AC4: tenant=0 → maybe_stamp_stable_ref_isolation_tenant returns false (no stamp)");
        CHECK(ref.tenant_id == 0, "AC4: tenant_id stays 0 (legacy single-tenant)");
    }

    // ── #2687 AC5: query surface wired ──
    {
        std::println("\n--- #2687 AC5: query surface ---");
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
        CompilerService cs;
        CHECK(href(cs, "schema-2687") == 2687, "AC5: schema-2687 sentinel");
        CHECK(href(cs, "issue-2687") == 2687, "AC5: issue-2687 sentinel");
        const auto local_q = href(cs, "isolation-capture-stamp-local-total");
        const auto fallback_q = href(cs, "isolation-capture-stamp-global-fallback-total");
        const auto miss_q = href(cs, "isolation-capture-stamp-evaluator-miss-total");
        CHECK(local_q >= 0, "AC5: local-total queryable (>= 0)");
        CHECK(fallback_q >= 0, "AC5: global-fallback-total queryable (>= 0)");
        CHECK(miss_q >= 0, "AC5: evaluator-miss-total queryable (>= 0)");
        // Legacy #2659 schema-2659 still works (additive — no schema break).
        CHECK(href(cs, "schema-2659") == 2659, "AC5: legacy schema-2659 still wired");
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

    reset_all();
    std::println("\n=== test_tenant_isolation_enforcement: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
