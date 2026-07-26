// @category: unit
// @reason: Issue #2125 — stamp isolation principal on all StableNodeRef
// capture paths (make_ref / make_safe_ref / capture_for_fiber /
// children_stable), not only atomic-batch pin (#2073).
//
//   AC1: Source cites #2125; make_ref stamps when isolation principal active
//   AC2: make_ref / children_stable / non-batch capture have non-zero tenant
//   AC3: Cross-tenant mutate via foreign-stamped ref denied
//   AC4: Same-tenant isolation check still allows
//   AC5: Off / unset tenant remains permissive (no false deny; raw make_ref 0)
//   AC6: #2073 atomic-batch path still stamps via make_stamped_safe_ref
//   AC7: This sibling of test_workspace_isolation_wire_2073 (reuse #81967)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/provenance_tracker.hh"
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
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectMutate;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::provenance::isolation_capture_tenant;
using aura::core::provenance::kStableRefTenantCaptureIssue;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::snapshot_provenance_enforcement;
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
}

} // namespace

int main() {
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

        ev.set_tenant_principal(42, "tenant-a");
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

        // children_stable goes through make_ref — non-batch query path.
        auto kids = ws->children_stable(id);
        std::println("  children_stable count={}", kids.size());
        for (const auto& k : kids) {
            CHECK(k.tenant_id == 42, "AC2: children_stable child stamped 42");
        }
        // Even empty children list is fine; parent_stable also uses make_ref.
        auto parent = ws->parent_stable(id);
        if (parent.id != NULL_NODE)
            CHECK(parent.tenant_id == 42, "AC2: parent_stable stamped when live");

        CHECK(snapshot_provenance_enforcement().tenant_stamps > stamps0,
              "AC1: stamp metric advanced");
        CHECK(snapshot_provenance_enforcement().tenant_stamp_capture > cap0,
              "AC1: capture-path metric advanced");
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

        // Capture under tenant A via non-batch make_ref.
        ev.set_tenant_principal(1, "alice");
        auto foreign = ws->make_ref(id);
        CHECK(foreign.tenant_id == 1, "captured as tenant 1");

        // Switch to tenant B — isolation must deny foreign-stamped ref.
        ev.set_tenant_principal(2, "bob");
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

        ev.set_tenant_principal(77, "batch");
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
        ev.set_tenant_principal(9, "metrics");
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

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
