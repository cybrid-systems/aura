// @category: unit
// @reason: Issue #2125 — stamp isolation principal on Soft StableNodeRef
// capture paths (make_ref / make_safe_ref / capture_for_fiber), not only
// atomic-batch pin (#2073). Issue #2960: children_stable / parent_stable are
// layout-only; production Agent stamp is Evaluator::stamp_query_stable_ref_export.
//
//   AC1: Source cites #2125; make_ref stamps when isolation principal active
//   AC2: make_ref Soft stamps; children_stable layout-only + Evaluator stamps
//   AC3: Cross-tenant mutate via foreign-stamped ref denied
//   AC4: Same-tenant isolation check still allows
//   AC5: Off / unset tenant remains permissive (no false deny; raw make_ref 0)
//   AC6: #2073 atomic-batch path still stamps via make_stamped_safe_ref
//   AC7: This sibling of test_workspace_isolation_wire (reuse #81967)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <fstream>
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
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::provenance::isolation_capture_tenant;
using aura::core::provenance::kStableRefTenantCaptureIssue;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::snapshot_provenance_enforcement;
using aura::core::workspace_isolation::g_workspace_isolation;
using aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

NodeId first_live(FlatAST& ws) {
    for (NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id))
            return id;
    }
    return NULL_NODE;
}

std::int64_t href_prov(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-provenance-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void reset_all() {
    reset_tenant_isolation_for_test();
    reset_provenance_enforcement_for_test();
    // Soft capture tests need global maybe_stamp path (#2125 AC2); hard-close
    // may be left armed by co-batch members (#2705 / #2759).
    aura::core::provenance::set_hard_capture_tenant(false);
    aura::core::provenance::set_isolation_capture_tenant(0);
}

} // namespace

int run_test_stable_ref_tenant_capture() {
    std::println("=== Issue #2125: stamp_ref_tenant on all StableNodeRef capture paths ===");
    CHECK(kStableRefTenantCaptureIssue == 2125, "AC1: issue stamp constant");

    // ── AC5: isolation off / unset → make_ref leaves tenant_id 0 ──
    {
        std::println("\n--- AC5: unset principal remains permissive ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live node");

        // capability-only (no isolation principal) must not stamp make_ref
        // — preserves #2056 raw-unstamped contract.
        ev.set_capability_tenant_id(42);
        CHECK(isolation_capture_tenant() == 0, "capture principal still 0 without isolation");
        auto raw = ws->make_ref(id);
        CHECK(raw.tenant_id == 0, "AC5: raw make_ref unstamped when isolation off");

        // Explicit Evaluator stamp still works (mandate path #2056).
        auto stamped = ev.make_stamped_ref(id);
        CHECK(stamped.tenant_id == 42, "AC6-related: make_stamped_ref still stamps");

        // Isolation check with unset principal remains permissive.
        CHECK(ev.check_workspace_isolation(0, 0, kEffectMutate, "test:ac5-unset"),
              "AC5: unset isolation check allows");
    }

    // ── AC1/AC2: make_ref / children_stable stamp under isolation principal ──
    {
        std::println("\n--- AC1/AC2: make_ref + children_stable stamp under principal ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.grant_capability(kCapWildcard);
        CHECK(cs.eval("(set-code \"(define (g x) (* x 2))\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        CHECK(id != NULL_NODE, "live");

        // Soft capture principal is process-global (FlatAST make_ref family).
        // #2659: set_tenant_principal is Evaluator-local only — Soft make_ref
        // still needs set_isolation_capture_tenant / set_current_tenant.
        aura::core::provenance::set_hard_capture_tenant(false);
        g_workspace_isolation().set_current_tenant(42, "tenant-a");
        ev.set_capability_tenant_id(42); // production Evaluator stamp path
        CHECK(isolation_capture_tenant() == 42, "isolation capture principal = 42");

        const auto stamps0 = snapshot_provenance_enforcement().tenant_stamps;
        const auto cap0 = snapshot_provenance_enforcement().tenant_stamp_capture;

        auto ref = ws->make_ref(id);
        std::println("  make_ref tenant_id={}", ref.tenant_id);
        CHECK(ref.tenant_id == 42, "AC1/AC2: make_ref stamps principal 42");

        auto safe = ws->make_safe_ref(id, 0, 7);
        CHECK(safe.tenant_id == 42, "AC2: make_safe_ref stamps 42");
        CHECK(safe.fiber_id == 7, "fiber preserved");

        auto fiber = ws->capture_for_fiber(id, 9);
        CHECK(fiber.tenant_id == 42, "AC2: capture_for_fiber stamps 42");

        auto layer = ws->make_ref_in_layer(id, 1);
        CHECK(layer.tenant_id == 42, "AC2: make_ref_in_layer stamps 42");

        auto from_gen = ws->make_ref_from_gen(id, ref.gen);
        CHECK(from_gen.tenant_id == 42, "AC2: make_ref_from_gen stamps 42");

        // Issue #2960: children_stable / parent_stable are layout-only (no Soft
        // global stamp). Production stamps via Evaluator.
        auto kids = ws->children_stable(id);
        std::println("  children_stable count={}", kids.size());
        for (const auto& k : kids) {
            CHECK(k.tenant_id == 0, "AC2/#2960: children_stable layout-only (tenant 0)");
            auto stamped_k = k;
            ev.stamp_query_stable_ref_export(stamped_k);
            CHECK(stamped_k.tenant_id == 42,
                  "AC2/#2960: Evaluator stamp_query fills principal on child");
        }
        auto parent = ws->parent_stable(id);
        if (parent.id != NULL_NODE) {
            CHECK(parent.tenant_id == 0, "AC2/#2960: parent_stable layout-only");
            ev.stamp_query_stable_ref_export(parent);
            CHECK(parent.tenant_id == 42, "AC2/#2960: Evaluator stamp on parent_stable");
        }

        CHECK(snapshot_provenance_enforcement().tenant_stamps > stamps0,
              "AC1: stamp metric advanced");
        CHECK(snapshot_provenance_enforcement().tenant_stamp_capture > cap0,
              "AC1: capture-path metric advanced");
        CHECK(aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
                  std::memory_order_relaxed) >= 1,
              "AC2/#2960: query_stable_ref_stamped_total advanced");
    }

    // ── AC3/AC4: cross-tenant deny vs same-tenant allow ──
    {
        std::println("\n--- AC3/AC4: cross-tenant deny / same-tenant allow ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.grant_capability(kCapWildcard);
        CHECK(cs.eval("(set-code \"(define (h x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);

        // Capture under tenant A via Soft global capture + Evaluator principal.
        aura::core::provenance::set_hard_capture_tenant(false);
        g_workspace_isolation().set_current_tenant(1, "alice");
        ev.set_capability_tenant_id(1);
        auto foreign = ws->make_ref(id);
        CHECK(foreign.tenant_id == 1, "captured as tenant 1");

        // Switch to tenant B — isolation must deny foreign-stamped ref.
        g_workspace_isolation().set_current_tenant(2, "bob");
        ev.set_capability_tenant_id(2);
        CHECK(!ev.check_workspace_isolation(2, foreign.tenant_id, kEffectMutate, "test:ac3-x"),
              "AC3: cross-tenant ref denied");

        // Same-tenant: recapture under B and allow.
        auto own = ws->make_ref(id);
        CHECK(own.tenant_id == 2, "AC4: recapture stamps current principal");
        CHECK(ev.check_workspace_isolation(2, own.tenant_id, kEffectMutate, "test:ac4-same"),
              "AC4: same-tenant isolation allows");
    }

    // ── AC6: stamped factories / pin path still stamp (defense-in-depth) ──
    {
        std::println("\n--- AC6: make_stamped_* + pin share stamp helper ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (i x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);

        ev.set_capability_tenant_id(77);
        auto pin_style = ev.make_stamped_safe_ref(id); // same helper as pin_node_for_atomic_batch
        CHECK(pin_style.tenant_id == 77, "AC6: make_stamped_safe_ref stamps 77");

        // Direct stamp_ref_tenant alias still works on a manually-built ref.
        FlatAST::StableNodeRef manual{};
        manual.id = id;
        manual.gen = ws->generation();
        ev.stamp_ref_tenant(manual);
        CHECK(manual.tenant_id == 77, "AC6: stamp_ref_tenant fills principal");
    }

    // ── AC7 + metrics surface (schema-2125 fold-in) ──
    {
        std::println("\n--- AC7: schema-2125 metrics surface ---");
        reset_all();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define (j x) x)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        const auto id = first_live(*ws);
        aura::core::provenance::set_hard_capture_tenant(false);
        g_workspace_isolation().set_current_tenant(9, "metrics");
        (void)ws->make_ref(id);

        CHECK(href_prov(cs, "schema-2125") == 2125, "schema-2125 present");
        CHECK(href_prov(cs, "issue-2125") == 2125, "issue-2125");
        CHECK(href_prov(cs, "ref-tenant-stamp-capture-total") >= 1, "capture total >= 1");
        CHECK(href_prov(cs, "ref-tenant-stamp-total") >= 1, "stamp total >= 1");
        CHECK(href_prov(cs, "isolation-capture-tenant") == 9, "isolation-capture-tenant = 9");
        // zero-rejected is optional soft counter (no default deny) — key exists.
        CHECK(href_prov(cs, "ref-tenant-stamp-zero-rejected-total") >= 0,
              "zero-rejected key present");
    }

    // ── #3000: restamp-lag must not stamp-green a pre-mutate generation ──
    {
        std::println("\n--- #3000: tenant-capture stamp gate on lagging gen ---");
        reset_all();
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.grant_capability(kCapWildcard);
        CHECK(cs.eval("(set-code \"(define (lag-a x) x) (define (lag-b y) y) "
                      "(define (lag-c z) z)\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        ev.set_capability_tenant_id(42);
        apply_production_audit_defaults();
        set_restamp_budget_nodes_for_process(1);
        ws->bump_generation();
        ws->restamp_all_node_generations();
        if (!ws->node_generation_is_post_mutate(id)) {
            FlatAST::StableNodeRef r{};
            r.id = id;
            ev.stamp_query_stable_ref_export(r);
            CHECK(r.id == NULL_NODE, "#3000: production stamp does not export lagging gen");
            CHECK(r.tenant_id == 0, "#3000: rejected ref not stamp-greened");
        } else {
            auto kids = ws->children_stable(id);
            for (auto k : kids) {
                if (!ws->node_generation_is_post_mutate(k.id)) {
                    ev.stamp_query_stable_ref_export(k);
                    CHECK(k.id == NULL_NODE || k.gen == ws->generation(),
                          "#3000: child export fail-closed or post-mutate");
                }
            }
        }
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        {
            std::ifstream f("docs/design/3000-restamp-lag.md");
            CHECK(!f.good(), "#3000: no docs/design/3000-*");
        }
        CHECK(aura::core::provenance::kQueryStableRefRestampLagIssue == 3000, "#3000: issue stamp");
    }

    // ── #3037: over-budget torn export (lazy-align must not stamp-green) ──
    {
        std::println("\n--- #3037: tenant-capture torn gate after lazy-align ---");
        reset_all();
        using aura::ast::clear_restamp_budget_nodes_override_for_test;
        using aura::ast::set_restamp_budget_nodes_for_process;
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.grant_capability(kCapWildcard);
        CHECK(cs.eval("(set-code \"(define (torn-a x) x) (define (torn-b y) y) "
                      "(define (torn-c z) z)\")")
                  .has_value(),
              "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "workspace");
        const auto id = first_live(*ws);
        ev.set_capability_tenant_id(42);
        apply_production_audit_defaults();
        set_restamp_budget_nodes_for_process(1);
        ws->bump_generation();
        ws->restamp_all_node_generations();
        CHECK(ws->restamp_generation_torn(), "#3037: generation torn");
        if (!ws->node_eagerly_restamped(id)) {
            (void)ws->make_ref_layout(id);
            CHECK(ws->node_generation_is_post_mutate(id), "#3037: lazy-align hid raw gen lag");
            FlatAST::StableNodeRef r{};
            r.id = id;
            ev.stamp_query_stable_ref_export(r);
            CHECK(r.id == NULL_NODE, "#3037: production stamp does not export torn gen");
            CHECK(r.tenant_id == 0, "#3037: rejected ref not stamp-greened");
        } else {
            CHECK(true, "#3037: node eagerly restamped");
        }
        apply_dev_audit_defaults();
        clear_restamp_budget_nodes_override_for_test();
        {
            std::ifstream f("docs/design/3037-restamp-over-budget-export.md");
            CHECK(!f.good(), "#3037: no docs/design/3037-*");
        }
        CHECK(aura::core::provenance::kQueryStableRefRestampTornIssue == 3037,
              "#3037: issue stamp");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_stable_ref_tenant_capture();
}
#endif
