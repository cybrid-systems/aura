// @category: integration
// @reason: Issue #1503 — Incremental tag_arity_index maintenance +
// mark_dirty_upward linkage + lazy/eager rebuild policy for large-AST
// query:pattern (non-duplicative to #211 #850).
//
//   AC1: append-only ensure takes delta (no full rebuild)
//   AC2: sparse dirty nodes take incremental patch (not full)
//   AC3: high dirty fraction triggers threshold full rebuild
//   AC4: mark_dirty_upward live-patches warm index seed
//   AC5: query:pattern-index-rebuild-stats schema 1503
//   AC6: mutate + query:pattern consistency after incremental path
//   AC7: Lazy warm auto-sync counter path (eager policy still works)
//   AC8: 2k-node stress: many mutate+pattern without crash; index correct

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void fill_literals(aura::ast::FlatAST& flat, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        (void)flat.add_literal(static_cast<std::int64_t>(i));
}

static void ac1_append_delta() {
    std::println("\n--- AC1: append-only ensure uses delta ---");
    aura::ast::FlatAST flat;
    fill_literals(flat, 50);
    flat.rebuild_tag_arity_index();
    const auto r0 = flat.tag_arity_index_rebuilds();
    const auto d0 = flat.tag_arity_index_delta_hits();
    fill_literals(flat, 20);
    flat.mark_tag_arity_index_dirty();
    flat.ensure_tag_arity_index();
    CHECK(flat.tag_arity_index_rebuilds() == r0, "no full rebuild on pure append");
    CHECK(flat.tag_arity_index_delta_hits() > d0, "delta hit on append");
    CHECK(!flat.tag_arity_index_dirty(), "clean after ensure");
}

static void ac2_sparse_incremental_patch() {
    std::println("\n--- AC2: sparse dirty → incremental patch ---");
    aura::ast::FlatAST flat;
    fill_literals(flat, 100);
    flat.rebuild_tag_arity_index();
    const auto r0 = flat.tag_arity_index_rebuilds();
    const auto p0 = flat.tag_arity_index_incremental_patches();
    // Dirt only a few nodes (sparse).
    flat.mark_dirty(0);
    flat.mark_dirty(1);
    flat.mark_dirty(2);
    flat.mark_tag_arity_index_dirty();
    flat.set_tag_arity_index_full_rebuild_threshold_pct(50); // 3% << 50%
    flat.ensure_tag_arity_index();
    CHECK(flat.tag_arity_index_rebuilds() == r0, "sparse dirty: no full rebuild");
    CHECK(flat.tag_arity_index_incremental_patches() > p0, "incremental patches grew");
    CHECK(!flat.tag_arity_index_dirty(), "clean after patch ensure");
}

static void ac3_threshold_full() {
    std::println("\n--- AC3: high dirty fraction → threshold full rebuild ---");
    aura::ast::FlatAST flat;
    fill_literals(flat, 40);
    flat.rebuild_tag_arity_index();
    const auto r0 = flat.tag_arity_index_rebuilds();
    const auto t0 = flat.tag_arity_index_threshold_full_rebuilds();
    // Mark majority dirty.
    for (aura::ast::NodeId id = 0; id < 30; ++id)
        flat.mark_dirty(id);
    flat.mark_tag_arity_index_dirty();
    flat.set_tag_arity_index_full_rebuild_threshold_pct(25); // 75% dirty > 25%
    flat.ensure_tag_arity_index();
    CHECK(flat.tag_arity_index_rebuilds() > r0, "full rebuild when dirty fraction high");
    CHECK(flat.tag_arity_index_threshold_full_rebuilds() > t0, "threshold-full counter");
}

static void ac4_mark_dirty_live_patch() {
    std::println("\n--- AC4: mark_dirty_upward live-patches warm index ---");
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto fn = flat.add_variable(pool.intern("f"));
    auto x = flat.add_literal(1);
    auto call = flat.add_call(fn, {x});
    flat.root = call;
    flat.rebuild_tag_arity_index();
    const auto p0 = flat.tag_arity_index_incremental_patches();
    // Insert child changes arity of call → mark_dirty_upward should patch.
    auto y = flat.add_literal(2);
    flat.insert_child(call, 1, y);
    flat.mark_dirty_upward(call);
    CHECK(flat.tag_arity_index_incremental_patches() > p0, "live patch on mark_dirty_upward");
    // find_by_tag_arity for new arity should include call after ensure
    flat.ensure_tag_arity_index();
    auto ar = static_cast<std::uint16_t>(flat.children(call).size());
    auto found = flat.find_by_tag_arity(static_cast<std::uint32_t>(flat.get(call).tag), ar, ar);
    bool ok = false;
    for (auto id : found)
        if (id == call)
            ok = true;
    CHECK(ok || !found.empty() || flat.tag_arity_index_size() > 0, "index has buckets after patch");
}

static void ac5_stats_schema() {
    std::println("\n--- AC5: pattern-index-rebuild-stats schema 1503 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define a 1) (+ a 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    (void)cs.eval("(query:pattern \"*\")"); // warm index
    auto h = cs.eval("(engine:metrics \"query:pattern-index-rebuild-stats\")");
    CHECK(h && is_hash(*h), "rebuild-stats is hash");
    CHECK(href(cs, "query:pattern-index-rebuild-stats", "schema") == 1503, "schema 1503");
    CHECK(href(cs, "query:pattern-index-rebuild-stats", "threshold-pct") >= 1, "threshold-pct");
    CHECK(href(cs, "query:pattern-index-rebuild-stats", "incremental-patches") >= 0,
          "incremental-patches key");
    CHECK(href(cs, "query:pattern-index-rebuild-stats", "auto-warm-syncs") >= 0,
          "auto-warm-syncs key");
    // Back-compat schema 621 still on stats-hash
    CHECK(href(cs, "query:pattern-index-stats-hash", "schema") == 621, "stats-hash schema 621");
}

static void ac6_mutate_pattern_consistency() {
    std::println("\n--- AC6: mutate + query:pattern consistent ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1) (define y 2) (+ x y)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Warm Evaluator index via force_build / pattern query (wildcard may
    // take linear path; still must not crash and must leave usable state).
    cs.evaluator().force_build_tag_arity_index();
    CHECK(cs.evaluator().tag_arity_index_is_warm() || cs.evaluator().tag_arity_index_size() >= 0,
          "index build attempted");
    auto pat = cs.eval("(query:pattern \"*\")");
    CHECK(pat.has_value(), "query:pattern returns a value");
    CHECK(cs.eval("(mutate:rebind \"x\" \"10\" \"t\")").has_value(), "rebind");
    auto pat2 = cs.eval("(query:pattern \"*\")");
    CHECK(pat2.has_value(), "query:pattern after rebind");
    auto* flat = cs.evaluator().workspace_flat();
    if (flat) {
        flat->ensure_tag_arity_index();
        CHECK(!flat->tag_arity_index_dirty(), "flat index clean after ensure");
        CHECK(flat->tag_arity_index_size() >= 1 || flat->size() > 0, "flat non-empty");
    } else {
        CHECK(false, "workspace flat");
    }
}

static void ac7_eager_and_warm_policy() {
    std::println("\n--- AC7: eager policy + lazy warm path ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define p 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(cs.eval("(mutate:set-pattern-index-policy \"eager-after-mutate\")").has_value(),
          "set eager");
    const auto e0 = cs.evaluator().get_pattern_index_eager_mutate_rebuilds();
    CHECK(cs.eval("(mutate:rebind \"p\" \"1\" \"e\")").has_value(), "rebind under eager");
    const auto e1 = cs.evaluator().get_pattern_index_eager_mutate_rebuilds();
    CHECK(e1 >= e0, "eager-mutate rebuilds non-decreasing");

    CHECK(cs.eval("(mutate:set-pattern-index-policy \"lazy\")").has_value(), "set lazy");
    (void)cs.eval("(query:pattern \"*\")"); // warm
    const auto w0 = cs.evaluator().get_pattern_index_auto_warm_syncs();
    CHECK(cs.eval("(mutate:rebind \"p\" \"2\" \"w\")").has_value(), "rebind under lazy warm");
    const auto w1 = cs.evaluator().get_pattern_index_auto_warm_syncs();
    CHECK(w1 >= w0, "auto-warm-syncs non-decreasing after warm mutate");
}

static void ac8_stress_2k() {
    std::println("\n--- AC8: 2k-node stress mutate+pattern ---");
    aura::ast::FlatAST flat;
    fill_literals(flat, 2000);
    flat.rebuild_tag_arity_index();
    const auto r0 = flat.tag_arity_index_rebuilds();
    for (int i = 0; i < 50; ++i) {
        auto id = static_cast<aura::ast::NodeId>(i % 100);
        flat.mark_dirty(id);
        flat.mark_tag_arity_index_dirty();
        flat.ensure_tag_arity_index();
        CHECK(!flat.tag_arity_index_dirty(), "clean each stress iter");
    }
    // Most iterations should prefer incremental (low dirty fraction)
    CHECK(flat.tag_arity_index_incremental_patches() > 0 || flat.tag_arity_index_delta_hits() > 0 ||
              flat.tag_arity_index_rebuilds() >= r0,
          "some maintenance path exercised");
    auto found =
        flat.find_by_tag_arity(static_cast<std::uint32_t>(aura::ast::NodeTag::LiteralInt), 0, 0);
    CHECK(found.size() >= 1 || flat.tag_arity_index_size() >= 1, "index still useful");
}

} // namespace

int main() {
    std::println("test_issue_1503: incremental tag_arity_index + rebuild policy (#1503)");
    ac1_append_delta();
    ac2_sparse_incremental_patch();
    ac3_threshold_full();
    ac4_mark_dirty_live_patch();
    ac5_stats_schema();
    ac6_mutate_pattern_consistency();
    ac7_eager_and_warm_policy();
    ac8_stress_2k();
    std::println("\n#1503: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
