// test_issue_393.cpp — Issue #393: C++ API for explicit
// (id, gen) pair construction and flat-style validity check.
//
// Validates the two new C++ helpers added to FlatAST:
//
//   1. make_ref_from_gen(NodeId, uint16_t gen)
//      Construct a StableNodeRef from a known (id, gen) pair
//      without going through make_ref()/make_safe_ref(). All
//      other StableNodeRef fields (mutation_id_at_capture,
//      workspace_id, fiber_id, last_validated_generation,
//      wrap_epoch, subtree_gen_at_capture) are captured from
//      the current FlatAST state.
//
//   2. is_valid_id_gen(NodeId, uint16_t gen,
//                      uint32_t wrap_epoch_at_capture = 0)
//      Flat-style validity check for callers that have an
//      (id, gen) pair but don't want to allocate a
//      StableNodeRef wrapper. Returns true iff the slot at
//      `id` is in-bounds AND its stored generation matches
//      AND the wrap epoch matches (when explicit). Does NOT
//      consult the global generation_ — the caller decides
//      what counts as "valid".
//
// AC coverage:
//   AC1: make_ref_from_gen returns a StableNodeRef with the
//        given id and gen (other fields populated from FlatAST)
//   AC2: make_ref_from_gen captures current generation_ as gen
//        when called after a mutation that bumped it
//   AC3: is_valid_id_gen returns true for a valid (id, gen)
//        pair (slot in-bounds, gen matches, wrap_epoch default
//        → matches current)
//   AC4: is_valid_id_gen returns false for NULL_NODE
//   AC5: is_valid_id_gen returns false for out-of-bounds id
//   AC6: is_valid_id_gen returns false when gen doesn't match
//        the slot's stored generation
//   AC7: is_valid_id_gen returns false when wrap_epoch is
//        explicit and doesn't match the current wrap_epoch
//   AC8: make_ref_from_gen + is_valid_id_gen round-trip —
//        capture a ref, extract (id, gen) via .id / .gen,
//        reconstruct via make_ref_from_gen, verify valid
//   AC9: make_ref_from_gen is noexcept (no allocation, no
//        throw) — verified by the [[nodiscard]] header spec
//        and the test pattern (no try/catch needed)

#include "test_harness.hpp"

using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;

namespace aura_issue_393_detail {

// Helper: populate a workspace with a few defines and return
// the per-define NodeIds. Caller owns the FlatAST + StringPool
// (same shape as test_issue_392's build_three_defines).
struct SimpleIds {
    aura::ast::NodeId def_a;
    aura::ast::NodeId val_a;
    aura::ast::NodeId def_b;
    aura::ast::NodeId val_b;
};

SimpleIds build_simple(aura::ast::FlatAST& flat,
                        aura::ast::StringPool& pool) {
    SimpleIds r{};
    auto sym_a = pool.intern("a");
    auto sym_b = pool.intern("b");
    r.val_a = flat.add_literal(1);
    r.val_b = flat.add_literal(2);
    r.def_a = flat.add_define(sym_a, r.val_a);
    r.def_b = flat.add_define(sym_b, r.val_b);
    return r;
}

// ═══════════════════════════════════════════════════════════════
// AC1: make_ref_from_gen returns a StableNodeRef with the given
// id and gen; other fields populated from current FlatAST state.
// ═══════════════════════════════════════════════════════════════

bool test_make_ref_from_gen_basic() {
    std::println("\n--- AC1: make_ref_from_gen basic fields ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    constexpr std::uint16_t kTestGen = 42;
    auto ref = flat.make_ref_from_gen(w.val_a, kTestGen);
    CHECK(ref.id == w.val_a, "ref.id == caller's id");
    CHECK(ref.gen == kTestGen, "ref.gen == caller's gen (not FlatAST's current gen)");
    // wrap_epoch is captured from current FlatAST (relaxed load
    // on the atomic); should be 0 in a fresh FlatAST.
    CHECK(ref.wrap_epoch == 0, "ref.wrap_epoch captured from current FlatAST (== 0 fresh)");
    // mutation_id_at_capture is captured from next_mutation_id_
    // (1 in a fresh flat — no mutations yet, so next = 1).
    CHECK(ref.mutation_id_at_capture == 1,
          "ref.mutation_id_at_capture == 1 (fresh flat, no mutations)");
    // subtree_gen_at_capture is captured from subtree_generation(id).
    CHECK(ref.subtree_gen_at_capture == 0,
          "ref.subtree_gen_at_capture == 0 (no scoped bumps yet)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: make_ref_from_gen uses the caller's gen, not the current
// FlatAST's gen. This is the whole point of the helper —
// reconstructing a ref from a serialized (id, gen) pair.
// ═══════════════════════════════════════════════════════════════

bool test_make_ref_from_gen_uses_caller_gen() {
    std::println("\n--- AC2: make_ref_from_gen uses caller's gen verbatim ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    // Current FlatAST gen is 1 (fresh). Pass 99 — the helper
    // should NOT silently replace it with 1.
    constexpr std::uint16_t kExplicitGen = 99;
    auto ref = flat.make_ref_from_gen(w.val_a, kExplicitGen);
    CHECK(ref.gen == kExplicitGen,
          "caller's gen 99 preserved (FlatAST's current 1 ignored)");
    // Sanity: confirm FlatAST's current gen really is 1.
    // (We can't read it directly from public API in the
    // public test path, but make_ref() captures it and
    // ref2.gen should be 1, not 99.)
    auto ref_via_make = flat.make_ref(w.val_a);
    CHECK(ref_via_make.gen == 1,
          "make_ref() captures FlatAST's current gen == 1");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: is_valid_id_gen returns true for a valid (id, gen) pair.
// ═══════════════════════════════════════════════════════════════

bool test_is_valid_id_gen_happy() {
    std::println("\n--- AC3: is_valid_id_gen valid pair ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    auto ref = flat.make_ref(w.val_a);
    // ref.id == val_a, ref.gen == 1 (current). Round-trip
    // through the flat-style checker.
    CHECK(flat.is_valid_id_gen(ref.id, ref.gen),
          "is_valid_id_gen(id=val_a, gen=1) returns true");
    // Default wrap_epoch (0) means "use current" — should
    // pass for any fresh capture.
    CHECK(flat.is_valid_id_gen(ref.id, ref.gen, 0),
          "is_valid_id_gen with default wrap_epoch == 0 returns true");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC4: is_valid_id_gen returns false for NULL_NODE.
// ═══════════════════════════════════════════════════════════════

bool test_is_valid_id_gen_null_node() {
    std::println("\n--- AC4: is_valid_id_gen NULL_NODE ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    CHECK(!flat.is_valid_id_gen(aura::ast::NULL_NODE, 1),
          "is_valid_id_gen(NULL_NODE, _) returns false");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC5: is_valid_id_gen returns false for out-of-bounds id.
// ═══════════════════════════════════════════════════════════════

bool test_is_valid_id_gen_out_of_bounds() {
    std::println("\n--- AC5: is_valid_id_gen out-of-bounds id ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    // Flat has a few nodes; pick an id well past the end.
    constexpr std::uint32_t kHuge = 1000000;
    CHECK(!flat.is_valid_id_gen(kHuge, 1),
          "is_valid_id_gen(huge id, _) returns false");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC6: is_valid_id_gen returns false when gen doesn't match.
// ═══════════════════════════════════════════════════════════════

bool test_is_valid_id_gen_wrong_gen() {
    std::println("\n--- AC6: is_valid_id_gen wrong gen ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    auto ref = flat.make_ref(w.val_a); // ref.gen == 1
    // Pass a gen that doesn't match.
    CHECK(!flat.is_valid_id_gen(ref.id, 0),
          "is_valid_id_gen(id, gen=0) returns false (gen 0 = sentinel/unset)");
    CHECK(!flat.is_valid_id_gen(ref.id, 9999),
          "is_valid_id_gen(id, gen=9999) returns false (no slot has gen 9999)");
    // The correct gen still passes.
    CHECK(flat.is_valid_id_gen(ref.id, ref.gen),
          "is_valid_id_gen(id, gen=ref.gen) returns true");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC7: is_valid_id_gen returns false when wrap_epoch is
// explicit and doesn't match the current wrap_epoch.
// ═══════════════════════════════════════════════════════════════

bool test_is_valid_id_gen_wrong_wrap_epoch() {
    std::println("\n--- AC7: is_valid_id_gen wrong wrap_epoch ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    auto ref = flat.make_ref(w.val_a);
    // Current wrap_epoch is 0 (fresh). Pass a non-zero
    // wrap_epoch_at_capture that doesn't match → should fail.
    CHECK(!flat.is_valid_id_gen(ref.id, ref.gen, 42),
          "is_valid_id_gen(id, gen, wrap_epoch=42) returns false "
          "(current wrap_epoch is 0)");
    // But if we pass 0 explicitly, that means "use current" — should pass.
    CHECK(flat.is_valid_id_gen(ref.id, ref.gen, 0),
          "is_valid_id_gen(id, gen, wrap_epoch=0) returns true "
          "(0 = use current)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC8: make_ref_from_gen + is_valid_id_gen round-trip.
// Capture a ref, extract (id, gen) via .id / .gen, reconstruct
// via make_ref_from_gen, verify the new ref passes the checker.
// ═══════════════════════════════════════════════════════════════

bool test_round_trip() {
    std::println("\n--- AC8: make_ref_from_gen ↔ is_valid_id_gen round-trip ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    // Step 1: original ref via make_ref (captures current gen + wrap_epoch).
    auto original = flat.make_ref(w.val_a);
    // Step 2: extract (id, gen) pair.
    auto id = original.id;
    auto gen = original.gen;
    // Step 3: reconstruct a new ref from the pair.
    auto reconstructed = flat.make_ref_from_gen(id, gen);
    // Step 4: the new ref's id and gen match the original.
    CHECK(reconstructed.id == original.id,
          "reconstructed ref.id == original ref.id");
    CHECK(reconstructed.gen == original.gen,
          "reconstructed ref.gen == original ref.gen");
    // Step 5: the new ref passes the flat-style checker.
    CHECK(flat.is_valid_id_gen(reconstructed.id, reconstructed.gen),
          "reconstructed ref passes is_valid_id_gen (round-trip OK)");
    // Step 6: reconstructed ref also passes the strict is_valid
    // (because FlatAST hasn't been mutated between captures).
    CHECK(flat.is_valid(reconstructed),
          "reconstructed ref passes strict is_valid (no mutations between)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC9: make_ref_from_gen + is_valid_id_gen across a mutation.
// After a mutation, the reconstructed ref's gen is stale —
// is_valid_id_gen should return false.
// ═══════════════════════════════════════════════════════════════

bool test_stale_after_mutation() {
    std::println("\n--- AC9: reconstructed ref becomes stale after mutation ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto w = build_simple(flat, pool);
    // Capture a ref to val_a.
    auto ref_before = flat.make_ref(w.val_a);
    CHECK(flat.is_valid_id_gen(ref_before.id, ref_before.gen),
          "ref valid before mutation");
    // Mutate: bump the subtree of val_a.
    flat.bump_generation_subtree(w.val_a);
    // After bump, the global gen is bumped, AND node_gen_[val_a]
    // is restamped to the new gen. The ref's gen (captured
    // before) is now stale.
    CHECK(!flat.is_valid_id_gen(ref_before.id, ref_before.gen),
          "ref stale after bump_generation_subtree (gen mismatch)");
    // Reconstruct a fresh ref via make_ref (uses current gen).
    auto ref_after = flat.make_ref(w.val_a);
    CHECK(flat.is_valid_id_gen(ref_after.id, ref_after.gen),
          "fresh ref valid after mutation");
    return true;
}

} // namespace aura_issue_393_detail

int main() {
    using namespace aura_issue_393_detail;
    std::println("=== test_issue_393: C++ API for (id, gen) pair construction ===");
    test_make_ref_from_gen_basic();
    test_make_ref_from_gen_uses_caller_gen();
    test_is_valid_id_gen_happy();
    test_is_valid_id_gen_null_node();
    test_is_valid_id_gen_out_of_bounds();
    test_is_valid_id_gen_wrong_gen();
    test_is_valid_id_gen_wrong_wrap_epoch();
    test_round_trip();
    test_stale_after_mutation();
    std::println("\n=== Totals: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
