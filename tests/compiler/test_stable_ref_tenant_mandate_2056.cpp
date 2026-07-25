// @category: unit
// @reason: Issue #2056 — mandate tenant_id + provenance stamp on every
// StableNodeRef creation / rebind path handed to Agent / user code.
//
//   AC1: make_stamped_ref / stamp_stable_ref set tenant_id from principal
//   AC2: refresh_if_stale preserves tenant_id (no silent restamp)
//   AC3: cross-tenant ensure_valid_or_refresh denied under Strict/Restricted
//   AC4: FailOnStale still refuses gen-stale silent restamp
//   AC5: query:stable-ref-provenance-stats schema-2056 keys
//   AC6: query:stable-ref-provenance returns tenant-id
//   AC7: hygiene stamp path still records macro hygiene provenance
//   AC8: zero principal → tenant_id 0 is valid (single-tenant)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
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
using aura::ast::NULL_NODE;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::provenance::kStableRefTenantMandateIssue;
using aura::core::provenance::record_macro_hygiene_provenance;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::snapshot_provenance_enforcement;
using aura::core::provenance::tenant_ids_compatible;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href_prov(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-provenance-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_q(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
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
    reset_provenance_enforcement_for_test();
    set_mode(SandboxMode::Off);
}

} // namespace

int main() {
    std::println("=== Issue #2056: StableNodeRef tenant stamp mandate ===");
    CHECK(kStableRefTenantMandateIssue == 2056, "issue stamp");

    // ── AC1: make_stamped_ref stamps principal tenant ──
    {
        std::println("\n--- AC1: stamp on create ---");
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
        auto ref = ev.make_stamped_ref(id);
        std::println("  stamped tenant_id={} fiber={}", ref.tenant_id, ref.fiber_id);
        CHECK(ref.tenant_id == 42, "AC1: tenant_id = principal 42");
        CHECK(ref.id == id, "id preserved");

        auto safe = ev.make_stamped_safe_ref(id, 0, 7);
        CHECK(safe.tenant_id == 42, "safe ref tenant stamped");
        CHECK(safe.fiber_id == 7, "safe ref fiber from arg");

        // stamp_stable_ref on raw make_ref
        auto raw = ws->make_ref(id);
        CHECK(raw.tenant_id == 0, "raw make_ref unstamped");
        ev.stamp_stable_ref(raw);
        CHECK(raw.tenant_id == 42, "stamp_stable_ref fills tenant");
        CHECK(snapshot_provenance_enforcement().tenant_stamps >= 3, "stamp metric advanced");
    }

    // ── AC2: refresh preserves tenant ──
    {
        std::println("\n--- AC2: refresh preserves tenant ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (g x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");
        ev.set_capability_tenant_id(9);
        auto ref = ev.make_stamped_safe_ref(id);
        CHECK(ref.tenant_id == 9, "stamped 9");
        // Bump generation so ref is gen-stale but slot still live.
        ws->bump_generation();
        CHECK(!ref.is_valid_in(*ws), "gen-stale after bump");
        const auto before = snapshot_provenance_enforcement().tenant_preserved_on_refresh;
        CHECK(ref.refresh_if_stale(*ws), "refresh succeeds");
        CHECK(ref.tenant_id == 9, "tenant preserved across refresh");
        CHECK(ref.is_valid_in(*ws), "valid after refresh");
        CHECK(snapshot_provenance_enforcement().tenant_preserved_on_refresh > before,
              "preserve metric");
    }

    // ── AC3: cross-tenant ensure denied under Strict ──
    {
        std::println("\n--- AC3: cross-tenant ensure deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (h x) (* x 2))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");

        ev.set_effect_sandbox_mode(2); // Strict + FailOnStale policy
        ev.set_capability_tenant_id(1);
        auto foreign = ev.make_stamped_safe_ref(id);
        CHECK(foreign.tenant_id == 1, "captured as tenant 1");

        // Switch principal to tenant 2 — foreign ref must be denied.
        ev.set_capability_tenant_id(2);
        const auto deny0 = snapshot_provenance_enforcement().cross_tenant_denies;
        auto view = ev.ensure_valid_or_refresh(foreign, /*auto_refresh=*/true);
        CHECK(!view.has_value(), "cross-tenant ensure denied under Strict");
        CHECK(snapshot_provenance_enforcement().cross_tenant_denies > deny0,
              "cross-tenant-deny metric");
        CHECK(foreign.tenant_id == 1, "no silent tenant restamp on deny");

        // Own-tenant still works.
        auto own = ev.make_stamped_safe_ref(id);
        CHECK(own.tenant_id == 2, "own stamp 2");
        auto ok = ev.ensure_valid_or_refresh(own, true);
        CHECK(ok.has_value(), "same-tenant ensure allows");
    }

    // ── AC3b: Restricted also denies ──
    {
        std::println("\n--- AC3b: Restricted cross-tenant deny ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (i x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        ev.set_effect_sandbox_mode(1);
        ev.set_capability_tenant_id(5);
        auto ref = ev.make_stamped_ref(id);
        ev.set_capability_tenant_id(6);
        CHECK(!ev.ensure_valid_or_refresh(ref, true).has_value(), "Restricted denies cross-tenant");
    }

    // ── AC4: FailOnStale no silent restamp ──
    {
        std::println("\n--- AC4: FailOnStale ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (j x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        ev.set_effect_sandbox_mode(2); // FailOnStale via set_effect
        ev.set_capability_tenant_id(3);
        auto ref = ev.make_stamped_safe_ref(id);
        ws->bump_generation();
        // FailOnStale: auto_refresh false path inside ensure when policy off
        // set_effect_sandbox_mode(2) sets FailOnStale and stable_ref_auto_refresh false
        auto fail = ev.ensure_valid_or_refresh(ref, /*auto_refresh=*/true);
        // With FailOnStale, auto_refresh policy is off so gen-stale fails.
        CHECK(!fail.has_value() || ref.tenant_id == 3, "no tenant restamp under FailOnStale");
        CHECK(ref.tenant_id == 3, "tenant still 3 after failed ensure");
    }

    // ── AC5: schema-2056 stats ──
    {
        std::println("\n--- AC5: schema-2056 ---");
        reset_all();
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (k x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto st = cs.eval(R"((engine:metrics "query:stable-ref-provenance-stats"))");
        CHECK(st && is_hash(*st), "stats hash");
        CHECK(href_prov(cs, "schema-2056") == 2056, "schema-2056");
        CHECK(href_prov(cs, "issue-2056") == 2056, "issue-2056");
        CHECK(href_prov(cs, "tenant-stamp-wired") == 1, "wired");
        for (const char* k : {"tenant-stamp-total", "cross-tenant-deny-total",
                              "tenant-preserved-on-refresh", "principal-tenant-id"}) {
            CHECK(href_prov(cs, k) >= 0, std::format("{} present", k));
        }
    }

    // ── AC6: query:stable-ref-provenance tenant-id ──
    {
        std::println("\n--- AC6: query provenance tenant-id ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (m x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        ev.set_capability_tenant_id(77);
        auto q = cs.eval(std::format("(query:stable-ref-provenance {})", id));
        CHECK(q && is_hash(*q), "provenance query hash");
        CHECK(href_q(cs, std::format("(query:stable-ref-provenance {})", id), "tenant-id") == 77,
              "query returns tenant-id 77");
        CHECK(href_q(cs, std::format("(query:stable-ref-provenance {})", id), "schema-2056") ==
                  2056,
              "schema-2056 on query");
    }

    // ── AC7: hygiene stamp still dual-records ──
    {
        std::println("\n--- AC7: hygiene provenance ---");
        reset_all();
        const auto before = snapshot_provenance_enforcement().macro_hygiene_provenance_hits;
        record_macro_hygiene_provenance(/*node=*/11, /*tenant=*/8, /*mutation=*/99, /*fiber=*/3);
        const auto after = snapshot_provenance_enforcement().macro_hygiene_provenance_hits;
        CHECK(after > before, "hygiene hit recorded");
        auto& hy = aura::core::provenance::g_last_hygiene_provenance_stamp();
        CHECK(hy.tenant_id == 8, "hygiene tenant stamped");
        CHECK(hy.source_mutation_id == 99, "hygiene mutation stamped");
        CHECK(hy.fiber_id == 3, "hygiene fiber stamped");
    }

    // ── AC8: zero principal ──
    {
        std::println("\n--- AC8: unset principal ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (n x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        ev.set_capability_tenant_id(0);
        auto ref = ev.make_stamped_ref(id);
        CHECK(ref.tenant_id == 0, "unset principal → tenant 0");
        CHECK(tenant_ids_compatible(0, 5), "0 compatible with any");
        CHECK(tenant_ids_compatible(5, 0), "any compatible with 0");
        // Off sandbox: foreign non-zero still allowed when principal 0
        ref.tenant_id = 99;
        ev.set_effect_sandbox_mode(0);
        CHECK(ev.ensure_valid_or_refresh(ref, true).has_value() || !ref.is_valid_in(*ws),
              "Off sandbox does not hard-deny cross-tenant");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
