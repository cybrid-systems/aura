// test_issue_392.cpp — Issue #392: scoped / per-subtree
// generation bumping for precise StableRef invalidation in large
// designs.
//
// Validates the scope-limited close:
//   AC1: bump_generation_subtree(NodeId) bumps the per-subtree
//        counter (subtree_gen_) for the top-level Define ancestor
//        of the argument; does NOT touch unrelated subtrees' gen.
//   AC2: subtree_generation(NodeId) returns the per-subtree
//        counter (0 = never bumped; matches across makes_ref).
//   AC3: subtree_bump_count_ tracks lifetime total of subtree
//        bumps (mirrors C++ accessor + Aura primitive).
//   AC4: is_valid_subtree(ref) returns TRUE for refs in
//        subtrees that were NOT scoped-bumped since capture,
//        even when OTHER subtrees were bumped. This is the
//        over-invalidation fix.
//   AC5: Multi-subtree scenario — 3 top-level Defines; mutating
//        one keeps refs in the other two valid via
//        is_valid_subtree().

#include "test_harness.hpp"

using aura::test::g_passed;
using aura::test::g_failed;

import aura.core.ast;

namespace aura_issue_392_detail {

// Helper: build a 3-Define workspace and return (define_a,
// val_a, define_b, val_b, define_c, val_c). The StringPool is
// owned by the caller and must outlive the FlatAST (Define
// nodes hold SymIds that resolve through the pool).
struct ThreeDefines {
    aura::ast::StringPool* pool;
    aura::ast::NodeId def_a;
    aura::ast::NodeId val_a;
    aura::ast::NodeId def_b;
    aura::ast::NodeId val_b;
    aura::ast::NodeId def_c;
    aura::ast::NodeId val_c;
};

ThreeDefines build_three_defines(aura::ast::FlatAST& flat,
                                  aura::ast::StringPool& pool) {
    ThreeDefines r{};
    r.pool = &pool;
    auto sym_a = pool.intern("a");
    auto sym_b = pool.intern("b");
    auto sym_c = pool.intern("c");
    // Build val_a, val_b, val_c as small literals.
    r.val_a = flat.add_literal(1);
    r.val_b = flat.add_literal(2);
    r.val_c = flat.add_literal(3);
    // Wrap each value in a top-level Define. add_define sets up
    // parent links (val.parent = define) via link_children.
    r.def_a = flat.add_define(sym_a, r.val_a);
    r.def_b = flat.add_define(sym_b, r.val_b);
    r.def_c = flat.add_define(sym_c, r.val_c);
    return r;
}

// ═══════════════════════════════════════════════════════════════
// AC1: bump_generation_subtree bumps the per-subtree counter
// for the top-level Define ancestor of the argument, and ONLY
// for that subtree (not unrelated subtrees).
// ═══════════════════════════════════════════════════════════════

bool test_subtree_bump_is_scoped() {
    std::println("\n--- AC1: bump_generation_subtree is scoped to one Define ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto t = build_three_defines(flat, pool);
    // baseline: all subtrees have subtree_gen == 0
    CHECK(flat.subtree_generation(t.def_a) == 0, "def_a subtree_gen == 0 baseline");
    CHECK(flat.subtree_generation(t.def_b) == 0, "def_b subtree_gen == 0 baseline");
    CHECK(flat.subtree_generation(t.def_c) == 0, "def_c subtree_gen == 0 baseline");
    // bump from inside def_a's subtree (use val_a's id)
    flat.bump_generation_subtree(t.val_a);
    CHECK(flat.subtree_generation(t.def_a) == 1,
          "def_a subtree_gen bumped to 1 (val_a was scoped)");
    CHECK(flat.subtree_generation(t.def_b) == 0,
          "def_b subtree_gen UNCHANGED (unrelated subtree)");
    CHECK(flat.subtree_generation(t.def_c) == 0,
          "def_c subtree_gen UNCHANGED (unrelated subtree)");
    // bump from inside def_b
    flat.bump_generation_subtree(t.val_b);
    CHECK(flat.subtree_generation(t.def_a) == 1, "def_a unchanged after def_b bump");
    CHECK(flat.subtree_generation(t.def_b) == 1,
          "def_b subtree_gen bumped to 1 (val_b was scoped)");
    CHECK(flat.subtree_generation(t.def_c) == 0, "def_c still untouched");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: subtree_generation accessor returns the per-subtree
// counter (0 default; matches across make_ref).
// ═══════════════════════════════════════════════════════════════

bool test_subtree_generation_accessor() {
    std::println("\n--- AC2: subtree_generation accessor matches across ref captures ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto t = build_three_defines(flat, pool);
    auto ref_a = flat.make_ref(t.val_a);
    auto ref_b = flat.make_ref(t.val_b);
    // Captured subtree_gen_at_capture should equal current
    // subtree_generation() of the same node at capture time.
    CHECK(ref_a.subtree_gen_at_capture == 0,
          "ref_a.subtree_gen_at_capture == 0 (default pre-bump)");
    CHECK(ref_b.subtree_gen_at_capture == 0,
          "ref_b.subtree_gen_at_capture == 0 (default pre-bump)");
    CHECK(flat.subtree_generation(t.val_a) == 0,
          "subtree_generation(val_a) == 0 matches captured value");
    // Top-level Define ancestor walk: top_define_of(val_a) == def_a.
    CHECK(flat.top_define_of(t.val_a) == t.def_a,
          "top_define_of(val_a) returns def_a (walk-up works)");
    CHECK(flat.top_define_of(t.val_b) == t.def_b,
          "top_define_of(val_b) returns def_b");
    CHECK(flat.top_define_of(t.def_a) == t.def_a,
          "top_define_of(def_a) is self (Define is its own top)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: subtree_bump_count_ tracks lifetime total of subtree
// bumps (no-ops when subtree_root has no enclosing Define).
// ═══════════════════════════════════════════════════════════════

bool test_subtree_bump_count() {
    std::println("\n--- AC3: subtree_bump_count_ lifetime total ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto t = build_three_defines(flat, pool);
    const auto base = flat.subtree_bump_count();
    flat.bump_generation_subtree(t.val_a);
    CHECK(flat.subtree_bump_count() == base + 1, "first bump increments count");
    flat.bump_generation_subtree(t.val_b);
    CHECK(flat.subtree_bump_count() == base + 2, "second bump increments count");
    flat.bump_generation_subtree(t.val_c);
    CHECK(flat.subtree_bump_count() == base + 3, "third bump increments count");
    // No-op bump on NULL_NODE must NOT increment count.
    flat.bump_generation_subtree(aura::ast::NULL_NODE);
    CHECK(flat.subtree_bump_count() == base + 3,
          "NULL_NODE no-op does NOT increment count");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC4: is_valid_subtree accepts refs in untouched subtrees
// (the over-invalidation fix from #392).
// ═══════════════════════════════════════════════════════════════

bool test_is_valid_subtree_untouched_subtree() {
    std::println("\n--- AC4: is_valid_subtree accepts refs in untouched subtrees ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto t = build_three_defines(flat, pool);
    // Capture refs to nodes in two different subtrees.
    auto ref_a = flat.make_ref(t.val_a);
    auto ref_b = flat.make_ref(t.val_b);
    CHECK(flat.is_valid_subtree(ref_a), "ref_a valid at capture");
    CHECK(flat.is_valid_subtree(ref_b), "ref_b valid at capture");
    // Scoped bump ONLY def_b's subtree.
    flat.bump_generation_subtree(t.val_b);
    // ref_b is in the bumped subtree → invalid.
    CHECK(!flat.is_valid_subtree(ref_b),
          "ref_b INVALID after scoped bump of its own subtree");
    // ref_a is in the UNTOUCHED subtree → STILL valid (the win).
    CHECK(flat.is_valid_subtree(ref_a),
          "ref_a STILL VALID after scoped bump of UNRELATED subtree");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC5: Multi-subtree scenario. 3 top-level Defines, mutate one,
// refs in the other two stay valid via is_valid_subtree().
// ═══════════════════════════════════════════════════════════════

bool test_multi_subtree_isolation() {
    std::println("\n--- AC5: 3-Define workspace — mutating one keeps refs in others valid ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto t = build_three_defines(flat, pool);
    auto ref_a = flat.make_ref(t.val_a);
    auto ref_b = flat.make_ref(t.val_b);
    auto ref_c = flat.make_ref(t.val_c);
    // Sequence: bump def_b twice.
    flat.bump_generation_subtree(t.val_b);
    flat.bump_generation_subtree(t.val_b);
    CHECK(!flat.is_valid_subtree(ref_b),
          "ref_b INVALID after 2 scoped bumps to its subtree");
    CHECK(flat.is_valid_subtree(ref_a),
          "ref_a STILL VALID — def_a untouched by def_b bumps");
    CHECK(flat.is_valid_subtree(ref_c),
          "ref_c STILL VALID — def_c untouched by def_b bumps");
    // Now bump def_a once — ref_a invalidates, ref_c still valid.
    flat.bump_generation_subtree(t.val_a);
    CHECK(!flat.is_valid_subtree(ref_a),
          "ref_a INVALID after its own scoped bump");
    CHECK(flat.is_valid_subtree(ref_c),
          "ref_c STILL VALID — only def_a and def_b bumped");
    // Final: bump def_c — all three refs now invalid.
    flat.bump_generation_subtree(t.val_c);
    CHECK(!flat.is_valid_subtree(ref_a), "ref_a invalid (def_a bumped earlier)");
    CHECK(!flat.is_valid_subtree(ref_b), "ref_b invalid (def_b bumped earlier)");
    CHECK(!flat.is_valid_subtree(ref_c),
          "ref_c invalid after its own scoped bump");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC6: subtree_gen_at_capture round-trips through
// serialize_stable_ref / deserialize_stable_ref (the wire format
// was extended to use the previously-"reserved" bytes 16..17).
// ═══════════════════════════════════════════════════════════════

bool test_subtree_gen_at_capture_serialization_roundtrip() {
    std::println("\n--- AC6: subtree_gen_at_capture serialization roundtrip ---");
    aura::ast::FlatAST flat;
    aura::ast::StringPool pool;
    auto t = build_three_defines(flat, pool);
    // Bump def_a's subtree so its subtree_gen_at_capture is
    // non-zero after make_ref — proves the field actually
    // round-trips through the wire format.
    flat.bump_generation_subtree(t.val_a);
    auto ref = flat.make_ref(t.val_a);
    CHECK(ref.subtree_gen_at_capture == 1,
          "captured subtree_gen_at_capture == 1 (after one bump)");
    std::uint8_t buf[24] = {};
    auto n = flat.serialize_stable_ref(ref, buf);
    CHECK(static_cast<std::size_t>(n) == flat.kStableRefSerializedSize,
          "serialize_stable_ref returns kStableRefSerializedSize (24)");
    CHECK(flat.kStableRefSerializedSize == static_cast<std::size_t>(24),
          "kStableRefSerializedSize == 24 (unchanged wire size)");
    aura::ast::FlatAST::StableNodeRef round{};
    CHECK(flat.deserialize_stable_ref(buf, n, round),
          "deserialize_stable_ref succeeds");
    CHECK(round.subtree_gen_at_capture == ref.subtree_gen_at_capture,
          "subtree_gen_at_capture round-trips through serialize/deserialize");
    CHECK(round.gen == ref.gen, "gen round-trips");
    CHECK(round.id == ref.id, "id round-trips");
    // The round-tripped ref should still pass is_valid_subtree
    // on a sibling node (same FlatAST — nothing has been
    // bumped since the round-trip).
    CHECK(flat.is_valid_subtree(round),
          "round-tripped ref is still valid in same FlatAST");
    return true;
}

} // namespace aura_issue_392_detail

int main() {
    using namespace aura_issue_392_detail;
    std::println("=== test_issue_392: scoped / per-subtree generation bumping ===");
    test_subtree_bump_is_scoped();
    test_subtree_generation_accessor();
    test_subtree_bump_count();
    test_is_valid_subtree_untouched_subtree();
    test_multi_subtree_isolation();
    test_subtree_gen_at_capture_serialization_roundtrip();
    std::println("\n=== Totals: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}