// @category: unit
// @reason: Issue #1566 — WorkspaceIsolationPolicy enforcement:
// capability cross-tenant grant, provenance deny, Strict sandbox link,
// mutate/workspace force path, query:tenant-isolation-stats, stress deny.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/workspace_isolation.hh"
#include "core/sandbox.hh"

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
using aura::core::workspace_isolation::check_boundary;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::IsolationRefProvenance;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::core::workspace_isolation::snapshot_tenant_isolation_stats;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

std::int64_t href_m(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:tenant-isolation-stats\") \"{}\")", key));
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
        auto h = cs.eval(R"((engine:metrics "query:tenant-isolation-stats"))");
        CHECK(h && is_hash(*h), "tenant-isolation-stats is hash");
        CHECK(href_m(cs, "schema") == 1566, "schema 1566");
        CHECK(href_m(cs, "active") == 1, "active");
        CHECK(href_m(cs, "phase") == 2, "phase 2");
    }

    // ── AC1: same-tenant / unset allows ──
    {
        reset_all();
        CHECK(check_boundary(0), "unset tenant allows target 0");
        g_workspace_isolation().set_current_tenant(1, "alice");
        CHECK(check_boundary(1), "same tenant allows");
        CHECK(snapshot_tenant_isolation_stats().checks >= 2, "checks counted");
    }

    // ── AC1/2: cross-tenant without grant denies ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        const auto v0 = snapshot_tenant_isolation_stats().boundary_violations_prevented;
        CHECK(!check_boundary(2), "cross-tenant without grant denied");
        CHECK(snapshot_tenant_isolation_stats().boundary_violations_prevented == v0 + 1,
              "boundary violation prevented");
    }

    // ── AC1: capability propagation grant allows ──
    {
        reset_all();
        g_workspace_isolation().set_current_tenant(1, "alice");
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectMutate);
        CHECK(check_boundary(2, nullptr, kEffectMutate), "cross-tenant with Mutate grant allows");
        CHECK(!check_boundary(2, nullptr, kEffectWrite), "Write not covered by Mutate grant");
        g_workspace_isolation().grant_cross_tenant(1, 2, kEffectWrite);
        CHECK(check_boundary(2, nullptr, kEffectWrite), "Write allowed after grant");
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
        CHECK(!check_boundary(1, &ref), "foreign ref tenant denies even on same target");
        CHECK(snapshot_tenant_isolation_stats().cross_tenant_provenance_deny == p0 + 1,
              "provenance deny counted");
        // Grant 1→99 then allow
        g_workspace_isolation().grant_cross_tenant(1, 99, kEffectMutate);
        CHECK(check_boundary(1, &ref, kEffectMutate), "provenance allow after cross grant");
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
                    if (!check_boundary(1, &ref))
                        denies.fetch_add(1, std::memory_order_relaxed);
                    else
                        allows.fetch_add(1, std::memory_order_relaxed);
                    // Cross target without grant denies
                    if (!check_boundary(static_cast<std::uint64_t>(2 + (t % 2))))
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
        CHECK(check_boundary(99), "allow_cross_tenant bypasses boundary");
    }

    reset_all();
    std::println("\n=== test_tenant_isolation_enforcement: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
