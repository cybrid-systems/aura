// test_issue_303.cpp — Issue #303: SafeStableNodeRef
// wrappers + provenance tracking for fiber & multi-agent
// orchestration (memory-safety P1).
//
// Validates the new StableNodeRef extensions:
//   - fiber_id + last_validated_generation fields
//   - Provenance struct + get_provenance() accessor
//   - validate_with_provenance() side-effect
//   - make_safe_ref + capture_for_fiber factories
//   - Backward-compat: existing make_ref() still works
//   - Cross-fiber detection via fiber_id mismatch
//
// Ship scope (Issue #303 AC #1, #2, #3, #4 partial):
//   - Add fiber_id + last_validated_generation fields
//   - Add Provenance struct + get_provenance()
//   - Add make_safe_ref / capture_for_fiber factories
//   - Add validate_with_provenance() side-effect
//   - Verify backward-compat (existing make_ref unchanged)
//   - Document cross-fiber scenario
//
// AC #2 (auto-capture at fiber yield) and #5 (Agent dev docs)
// are deferred — they're follow-up work that builds on this
// foundation. The ref surface is now ready for fiber loops
// to record provenance; the integration is a follow-up.

#include "test_harness.hpp" // #1960 unified harness

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.core.ast;

namespace aura_303_detail {

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;

// ── Scenario 1: Backward-compat — existing make_ref unchanged ──
bool test_make_ref_unchanged() {
    std::println("\n--- Scenario 1: existing make_ref() unchanged ---");
    FlatAST ast;
    auto n0 = ast.add_raw_node(NodeTag::LiteralInt);
    auto ref = ast.make_ref(n0); // pre-#303 API
    // fiber_id defaults to 0 (no fiber context)
    CHECK(ref.fiber_id == 0, "make_ref() default fiber_id = 0 (no fiber context)");
    CHECK(ref.workspace_id == 0, "make_ref() default workspace_id = 0");
    CHECK(ref.mutation_id_at_capture != 0,
          "make_ref() captures mutation_id_at_capture from current state");
    // Last validated is 0 — pre-#303 behavior preserved.
    CHECK(ref.last_validated_generation == 0,
          "make_ref() default last_validated_generation = 0 (no validation history)");
    // The ref itself is still valid.
    CHECK(ref.is_valid_in(ast), "ref valid immediately after make_ref()");
    return true;
}

// ── Scenario 2: Provenance struct + get_provenance ──
bool test_provenance_accessor() {
    std::println("\n--- Scenario 2: Provenance + get_provenance ---");
    FlatAST ast;
    auto n0 = ast.add_raw_node(NodeTag::LiteralInt);
    auto ref = ast.make_safe_ref(n0, /*workspace_id=*/2,
                                 /*fiber_id=*/7);
    auto prov = ref.get_provenance();
    std::println("  captured_id={} gen={} mutation_id={} ws={} fiber={} last_val={}",
                 prov.captured_id, prov.captured_gen, prov.mutation_id_at_capture,
                 prov.workspace_id, prov.fiber_id, prov.last_validated_generation);
    CHECK(prov.captured_id == n0, "Provenance.captured_id matches");
    CHECK(prov.workspace_id == 2, "Provenance.workspace_id = 2");
    CHECK(prov.fiber_id == 7, "Provenance.fiber_id = 7");
    CHECK(prov.last_validated_generation != 0,
          "make_safe_ref sets last_validated_generation = current gen");
    return true;
}

// ── Scenario 3: validate_with_provenance side-effect ──
bool test_validate_with_provenance() {
    std::println("\n--- Scenario 3: validate_with_provenance ---");
    FlatAST ast;
    auto n0 = ast.add_raw_node(NodeTag::LiteralInt);
    auto ref = ast.capture_for_fiber(n0, /*fiber_id=*/11);
    // First validation: should be valid + update last_validated_generation.
    bool ok1 = ref.validate_with_provenance(ast);
    CHECK(ok1, "validate_with_provenance returns true for valid ref");
    CHECK(ref.last_validated_generation == ast.generation(),
          "last_validated_generation updated to current gen after validation");
    // Bump generation — ref should now be stale.
    ast.bump_generation();
    ast.bump_generation();
    bool ok2 = ref.validate_with_provenance(ast);
    CHECK(!ok2, "validate_with_provenance returns false for stale ref");
    CHECK(ref.last_validated_generation != ast.generation(),
          "last_validated_generation NOT updated when validation fails");
    return true;
}

// ── Scenario 4: Cross-fiber ref detection ──
bool test_cross_fiber_detection() {
    std::println("\n--- Scenario 4: cross-fiber ref detection ---");
    FlatAST ast;
    auto n0 = ast.add_raw_node(NodeTag::LiteralInt);
    // Capture on fiber 3.
    auto ref = ast.capture_for_fiber(n0, /*fiber_id=*/3);
    auto prov = ref.get_provenance();
    CHECK(prov.fiber_id == 3, "captured on fiber 3");
    // Same fiber — safe.
    CHECK(prov.fiber_id == 3, "ref on same fiber 3 (safe)");
    // Simulate cross-fiber steal: caller compares expected
    // fiber (e.g., currently running on fiber 7) vs ref's
    // captured fiber.
    std::uint32_t current_fiber = 7;
    bool is_cross_fiber = (current_fiber != prov.fiber_id);
    CHECK(is_cross_fiber, "cross-fiber detected: ref.fiber_id=3 vs current=7");
    // The agent loop can choose to re-validate or reject.
    // Re-validation here succeeds (ref still valid in FlatAST).
    bool revalid = ref.validate_with_provenance(ast);
    CHECK(revalid, "cross-fiber ref re-validates successfully in FlatAST");
    return true;
}

// ── Scenario 5: capture_for_fiber vs make_safe_ref equivalence ──
bool test_factory_equivalence() {
    std::println("\n--- Scenario 5: factory equivalence ---");
    FlatAST ast;
    auto n0 = ast.add_raw_node(NodeTag::LiteralInt);
    auto ref_a = ast.capture_for_fiber(n0, 5);
    auto ref_b = ast.make_safe_ref(n0, /*workspace_id=*/0, /*fiber_id=*/5);
    CHECK(ref_a.fiber_id == ref_b.fiber_id,
          "capture_for_fiber and make_safe_ref agree on fiber_id");
    CHECK(ref_a.workspace_id == ref_b.workspace_id,
          "capture_for_fiber and make_safe_ref agree on workspace_id");
    CHECK(ref_a.last_validated_generation == ref_b.last_validated_generation,
          "capture_for_fiber and make_safe_ref agree on last_validated_generation");
    return true;
}

} // namespace aura_303_detail

int main() {
    using namespace aura_303_detail;
    test_make_ref_unchanged();
    test_provenance_accessor();
    test_validate_with_provenance();
    test_cross_fiber_detection();
    test_factory_equivalence();
    return run_pilot_tests();
}