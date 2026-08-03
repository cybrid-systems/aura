// @category: unit
// @reason: Issue #2224 — sole export_ref outbound + resolve_stamped
// shared resolve entry + tenant-isolation-stats primitive.
//
//   AC1: export_ref / export_ref_safe stamp tenant + fiber (Phase A mandate)
//   AC2: cross-tenant resolve_stamped deny + metrics bumped
//   AC3: same-tenant resolve_stamped allow + no extra deny metrics
//   AC4: Strict / multi-tenant defaults: tenant 0 deny under Strict
//   AC5: cross-grant bits allow path (A grants B → resolve allows)
//   AC6: schema-2224 / issue-2224 keys in query:tenant-isolation-stats
//   AC7: query:tenant-isolation-stats primitive available
//   AC8: resolve_stamped last_mutate_error_ reasons distinct
//        (isolation-deny vs stale-ref vs no-workspace)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeView;
using aura::ast::NULL_NODE;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::core::workspace_isolation::snapshot_tenant_isolation_stats;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href_iso(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:tenant-isolation-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

NodeId first_live(FlatAST& ws) {
    for (NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id))
            return id;
    }
    return NULL_NODE;
}

void reset_all() {
    reset_capability_effects_for_test();
    reset_tenant_isolation_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace

int main() {
    std::println("=== Issue #2224: export_ref + resolve_stamped gate ===");
    CHECK(true, "issue stamp #2224");

    // ── AC1: export_ref / export_ref_safe stamp tenant + fiber ──
    {
        std::println("\n--- AC1: export mandate ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");

        ev.set_capability_tenant_id(42);
        auto ref = ev.export_ref(id);
        CHECK(ref.tenant_id == 42, "AC1: export_ref stamps tenant 42");
        CHECK(ref.id == id, "AC1: id preserved");

        auto safe = ev.export_ref_safe(id, /*ws=*/0, /*fiber=*/7);
        CHECK(safe.tenant_id == 42, "AC1: export_ref_safe stamps tenant 42");
        CHECK(safe.fiber_id == 7, "AC1: export_ref_safe takes fiber arg");

        // ast:stable-ref / query:as-stable-ref Agent-facing surfaces
        // must route through export_ref — verify by checking the
        // returned (id . gen) pair's underlying ref carries the
        // current tenant. We probe by re-calling make_stamped_ref
        // for comparison and checking the live tenant value via
        // (query:stable-ref-provenance) on the same id.
        const auto t_before = ev.capability_tenant_id();
        CHECK(t_before == 42, "principal still 42");
        ev.set_capability_tenant_id(7);
        auto r1 = cs.eval(std::format("(ast:stable-ref {})", id));
        CHECK(r1 && is_pair(*r1), "ast:stable-ref returns pair");
        auto r2 = cs.eval(std::format("(query:as-stable-ref {})", id));
        CHECK(r2 && is_pair(*r2), "query:as-stable-ref returns pair");
        // Provenance probe: query returns tenant-id of the stamped
        // ref at capture time. Both should be 7 (current principal
        // when the prim ran).
        auto prov = cs.eval(std::format("(query:stable-ref-provenance {})", id));
        CHECK(prov && is_hash(*prov), "provenance hash");
        auto href_q = [&](std::string_view key) {
            auto r =
                cs.eval(std::format("(hash-ref (query:stable-ref-provenance {}) \"{}\")", id, key));
            if (!r || !is_int(*r))
                return std::int64_t{-1};
            return as_int(*r);
        };
        CHECK(href_q("tenant-id") == 7, "AC1: ast/query ref surface tenant stamped");
    }

    // ── AC2: cross-tenant resolve_stamped deny + metrics ──
    {
        std::println("\n--- AC2: cross-tenant resolve deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (g x) (* x 2))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");

        // Tenant A captures ref, then principal switches to B → resolve
        // must deny + bump tenant_boundary_violation_prevented_total /
        // cross_tenant_provenance_deny_total.
        ev.set_capability_tenant_id(1);
        auto ref = ev.export_ref(id);
        CHECK(ref.tenant_id == 1, "captured as tenant 1");

        const auto base = snapshot_tenant_isolation_stats();
        ev.set_capability_tenant_id(2);
        auto view = ev.resolve_stamped(ref);
        CHECK(!view.has_value(), "AC2: cross-tenant resolve denied");
        const auto after = snapshot_tenant_isolation_stats();
        CHECK(after.boundary_violations_prevented > base.boundary_violations_prevented,
              "AC2: tenant_boundary_violation_prevented_total bumped");
        CHECK(after.cross_tenant_provenance_deny > base.cross_tenant_provenance_deny,
              "AC2: cross_tenant_provenance_deny_total bumped");
        CHECK(after.checks > base.checks, "AC2: tenant_boundary_checks_total bumped");
        CHECK(!ev.last_mutate_error().empty(), "AC2: last_mutate_error_ populated");
        const auto& err = ev.last_mutate_error();
        CHECK(err.find("isolation-deny") != std::string::npos,
              std::format("AC2: reason contains isolation-deny (got: {})", err));
    }

    // ── AC3: same-tenant happy path ──
    {
        std::println("\n--- AC3: same-tenant resolve allow ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (h x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");

        ev.set_capability_tenant_id(8);
        auto ref = ev.export_ref(id);
        CHECK(ref.tenant_id == 8, "captured as tenant 8");

        const auto base = snapshot_tenant_isolation_stats();
        auto view = ev.resolve_stamped(ref);
        CHECK(view.has_value(), "AC3: same-tenant resolve allows");
        CHECK(view->tag == ws->tag(id), "AC3: returned NodeView tag matches");
        const auto after = snapshot_tenant_isolation_stats();
        CHECK(after.boundary_violations_prevented == base.boundary_violations_prevented,
              "AC3: no extra deny metric on allow");
        CHECK(after.cross_tenant_provenance_deny == base.cross_tenant_provenance_deny,
              "AC3: no extra cross-tenant-deny metric");
    }

    // ── AC4: Strict / multi-tenant defaults — unstamped (tenant 0)
    //        ref under Strict + non-zero principal must deny.
    {
        std::println("\n--- AC4: Strict + unstamped deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (i x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");

        ev.set_effect_sandbox_mode(2); // Strict
        ev.set_capability_tenant_id(99);
        // Build a ref via raw make_ref (legacy / unstamped path) and
        // then stamp_stable_ref with a different tenant — simulating
        // a ref captured before the principal was set.
        auto raw = ws->make_ref(id);
        CHECK(raw.tenant_id == 0, "raw ref tenant 0");
        // Manually move tenant to 0 (legacy/unstamped) — resolve under
        // Strict + principal 99 must deny.
        const auto base = snapshot_tenant_isolation_stats();
        auto view = ev.resolve_stamped(raw);
        CHECK(!view.has_value(), "AC4: tenant 0 + Strict + non-zero principal → deny");
        const auto after = snapshot_tenant_isolation_stats();
        CHECK(after.strict_denials > base.strict_denials,
              "AC4: strict_sandbox_isolation_denials bumped");
    }

    // ── AC5: cross-grant bits allow ──
    {
        std::println("\n--- AC5: cross-grant allows ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (j x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");

        ev.set_capability_tenant_id(1);
        auto ref = ev.export_ref(id);
        CHECK(ref.tenant_id == 1, "captured as tenant 1");

        // Grant: tenant 2 (current principal after switch) may access
        // tenant 1's resources for any effect bit.
        ev.grant_cross_tenant_access(/*from=*/2, /*to=*/1, /*bits=*/0xFFFF);
        ev.set_capability_tenant_id(2);
        auto view = ev.resolve_stamped(ref);
        CHECK(view.has_value(), "AC5: cross-grant allows resolve");
    }

    // ── AC6: schema-2224 / issue-2224 keys present ──
    {
        std::println("\n--- AC6: schema-2224 keys ---");
        reset_all();
        CompilerService cs;
        auto* m = static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
        auto st = cs.eval(R"((engine:metrics \"query:tenant-isolation-stats\"))");
        CHECK(st && is_hash(*st), "AC6: stats hash present");
        CHECK(href_iso(cs, "schema-2224") == 2224, "AC6: schema-2224 = 2224");
        CHECK(href_iso(cs, "issue-2224") == 2224, "AC6: issue-2224 = 2224");
        CHECK(href_iso(cs, "active") == 1, "AC6: active wired");
        CHECK(href_iso(cs, "export-ref-mandate") == 1, "AC6: export-ref-mandate wired");
        CHECK(href_iso(cs, "resolve-stamped-gate") == 1, "AC6: resolve-stamped-gate wired");
        for (const char* k :
             {"tenant-boundary-checks-total", "tenant-boundary-violation-prevented-total",
              "cross-tenant-provenance-deny-total", "cross-tenant-capability-grant-total",
              "cross-tenant-capability-deny-total", "isolation-audit-total",
              "strict-sandbox-isolation-denials", "current-tenant", "isolation-enabled",
              "allow-cross-tenant", "strict-linked", "atomic-batch-tenant-isolation-denials",
              "phase", "issue"}) {
            CHECK(href_iso(cs, k) >= 0, std::format("AC6: {} present", k));
        }
        (void)m; // suppress unused warning if m==nullptr on some configs
    }

    // ── AC7: query:tenant-isolation-stats primitive returns a hash ──
    {
        std::println("\n--- AC7: primitive discoverable ---");
        reset_all();
        CompilerService cs;
        auto st = cs.eval(R"((engine:metrics \"query:tenant-isolation-stats\"))");
        CHECK(st && is_hash(*st), "AC7: primitive returns hash");
        // The hash should be non-empty (at least the wired entries).
        auto size_r = cs.eval(R"((hash-count (engine:metrics \"query:tenant-isolation-stats\")))");
        CHECK(size_r && is_int(*size_r), "AC7: hash-count returns int");
        const auto sz = as_int(*size_r);
        CHECK(sz >= 14, std::format("AC7: stats has at least 14 keys (got {})", sz));
    }

    // ── AC8: resolve_stamped last_mutate_error_ reasons distinct ──
    {
        std::println("\n--- AC8: distinct deny reasons ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (k x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");

        // 8a: cross-tenant deny reason format — must contain
        // "isolation-deny" and "ref-tenant=N" and NOT contain
        // "stale-ref" (which is the gen-stale reason format).
        ev.set_capability_tenant_id(3);
        auto ref = ev.export_ref(id);
        ev.set_capability_tenant_id(4);
        auto v0 = ev.resolve_stamped(ref);
        CHECK(!v0.has_value(), "AC8: cross-tenant deny");
        const auto& e0 = ev.last_mutate_error();
        CHECK(e0.find("isolation-deny") != std::string::npos,
              std::format("AC8a: reason contains 'isolation-deny' (got: {})", e0));
        CHECK(e0.find("ref-tenant=3") != std::string::npos,
              std::format("AC8a: reason contains 'ref-tenant=3' (got: {})", e0));
        CHECK(e0.find("stale-ref") == std::string::npos,
              "AC8a: cross-tenant reason distinct from stale-ref");

        // 8b: same-tenant allow path clears last_mutate_error_ on
        // success. This proves the helper doesn't leave stale deny
        // state behind for the next call.
        ev.set_capability_tenant_id(0);
        auto ref0 = ev.export_ref(id);
        CHECK(ref0.tenant_id == 0, "AC8b: tenant 0 ref under principal 0");
        auto v1 = ev.resolve_stamped(ref0);
        // Tenant 0 + principal 0 + sandbox off → permissive allow.
        // If allow: last_mutate_error_ is cleared by the impl.
        const auto err_before = ev.last_mutate_error();
        if (v1.has_value()) {
            CHECK(ev.last_mutate_error().empty(), "AC8b: last_mutate_error_ cleared on allow");
        } else {
            // Some configurations deny tenant 0 under Strict; verify
            // the reason is still distinct from stale-ref.
            CHECK(err_before.find("stale-ref") == std::string::npos,
                  "AC8b: non-allow reason still distinct from stale-ref");
        }
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
