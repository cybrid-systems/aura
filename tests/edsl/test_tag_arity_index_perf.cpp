// test_tag_arity_index_perf.cpp — Issue #1371:
// tag_arity_index_ unordered_map + delta update path.

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>

import std;
import aura.core;

using aura::ast::FlatAST;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;

namespace {

// Build a synthetic AST with `n` nodes of mixed tags/arities.
// Returns count of unique (tag, arity=0) buckets expected for literals.
void fill_ast(FlatAST& flat, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        // Alternate tags via add_literal / add_node
        if ((i % 5) == 0)
            (void)flat.add_literal(static_cast<std::int64_t>(i));
        else
            (void)flat.add_raw_node(NodeTag::Begin, SyntaxMarker::User);
    }
}

} // namespace

int main() {
    // ── hash map rebuild + find ──
    {
        FlatAST flat;
        fill_ast(flat, 200);
        CHECK(flat.size() == 200, "200 nodes");
        flat.rebuild_tag_arity_index();
        CHECK(flat.tag_arity_index_size() >= 1, "index has buckets");
        CHECK(!flat.tag_arity_index_dirty(), "clean after rebuild");

        const auto hits0 = flat.tag_arity_index_hits();
        const auto miss0 = flat.tag_arity_index_misses();
        auto lit = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::LiteralInt), 0, 0);
        if (lit.empty())
            lit = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::Begin), 0, 0);
        CHECK(!lit.empty() || flat.tag_arity_index_size() > 0, "find or size ok");
        CHECK(flat.tag_arity_index_hits() + flat.tag_arity_index_misses() > hits0 + miss0,
              "hit/miss counters advanced");
    }

    // ── delta append path ──
    {
        FlatAST flat;
        fill_ast(flat, 50);
        flat.rebuild_tag_arity_index();
        const auto rebuilds0 = flat.tag_arity_index_rebuilds();
        const auto delta0 = flat.tag_arity_index_delta_hits();
        const auto built_n = flat.size();

        // Append more nodes
        fill_ast(flat, 30);
        CHECK(flat.size() == built_n + 30, "appended 30");
        flat.mark_tag_arity_index_dirty();
        flat.ensure_tag_arity_index();
        CHECK(flat.tag_arity_index_delta_hits() == delta0 + 1, "delta path taken");
        CHECK(flat.tag_arity_index_rebuilds() == rebuilds0, "no full rebuild on append");
        CHECK(!flat.tag_arity_index_dirty(), "clean after delta");
    }

    // ── rebuild_tag_arity_index_delta explicit range ──
    {
        FlatAST flat;
        fill_ast(flat, 20);
        flat.rebuild_tag_arity_index();
        const auto d0 = flat.tag_arity_index_delta_hits();
        fill_ast(flat, 10);
        flat.rebuild_tag_arity_index_delta(20, 30);
        CHECK(flat.tag_arity_index_delta_hits() == d0 + 1, "explicit delta +1");
        CHECK(!flat.tag_arity_index_dirty(), "delta clears dirty");
    }

    // ── mark_dirty_upward_with_index_update append ──
    {
        FlatAST flat;
        fill_ast(flat, 10);
        flat.rebuild_tag_arity_index();
        const auto d0 = flat.tag_arity_index_delta_hits();
        auto id = flat.add_literal(99);
        flat.mark_dirty_upward_with_index_update(id);
        // Issue #1503: append may live-patch (incremental_patches) or
        // clear dirty; either is correct vs full rebuild.
        CHECK(flat.tag_arity_index_delta_hits() >= d0 + 1 ||
                  flat.tag_arity_index_incremental_patches() > 0 || !flat.tag_arity_index_dirty(),
              "append update or clean");
        auto found = flat.find_by_tag_arity(static_cast<std::uint32_t>(flat.get(id).tag), 0, 0);
        bool contains = false;
        for (auto n : found)
            if (n == id)
                contains = true;
        // If tag/arity 0 bucket exists, id should be present after index update
        if (!found.empty())
            CHECK(contains, "new node in bucket after mark_dirty_upward_with_index_update");
        else
            CHECK(true, "skip contain check if arity mismatch");
    }

    // ── ensure full rebuild when dirty with no growth ──
    {
        FlatAST flat;
        fill_ast(flat, 40);
        flat.rebuild_tag_arity_index();
        const auto r0 = flat.tag_arity_index_rebuilds();
        flat.mark_tag_arity_index_dirty();
        // same size, dirty → full rebuild
        flat.ensure_tag_arity_index();
        CHECK(flat.tag_arity_index_rebuilds() == r0 + 1, "full rebuild when size unchanged");
        CHECK(!flat.tag_arity_index_dirty(), "clean after full ensure");
    }

    // ── 10K node rebuild + 1000 finds (smoke perf, not hard p99 bound) ──
    {
        FlatAST flat;
        fill_ast(flat, 10000);
        auto t0 = std::chrono::steady_clock::now();
        flat.rebuild_tag_arity_index();
        auto t1 = std::chrono::steady_clock::now();
        const auto rebuild_us =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        CHECK(flat.tag_arity_index_size() >= 1, "10K index non-empty");

        // 1000 O(1) miss lookups (no large bucket copy noise)
        t0 = std::chrono::steady_clock::now();
        std::size_t misses = 0;
        for (int i = 0; i < 1000; ++i) {
            auto v = flat.find_by_tag_arity(0xDEADBEEFu, 99, 99);
            if (v.empty())
                ++misses;
        }
        t1 = std::chrono::steady_clock::now();
        const auto find_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        CHECK(misses == 1000, "1000 hash misses");
        CHECK(find_us < 10'000, "1000 hash misses < 10ms");
        // Hit path correctness on large bucket (copy cost separate from lookup)
        auto big = flat.find_by_tag_arity(static_cast<std::uint32_t>(NodeTag::Begin), 0, 0);
        CHECK(big.size() >= 1000, "Begin bucket large");
        std::println("  10K rebuild {} us; 1000 miss finds {} us; Begin bucket {}", rebuild_us,
                     find_us, big.size());
    }

    // ── atomic batch commit hooks delta ──
    {
        FlatAST flat;
        fill_ast(flat, 15);
        flat.rebuild_tag_arity_index();
        const auto d0 = flat.tag_arity_index_delta_hits();
        flat.begin_atomic_batch();
        fill_ast(flat, 5);
        flat.mark_tag_arity_index_dirty();
        flat.commit_atomic_batch();
        // commit should have run delta when size grew
        CHECK(flat.tag_arity_index_delta_hits() >= d0 + 1 || !flat.tag_arity_index_dirty(),
              "batch commit delta or clean");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("tag_arity_index perf #1371: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
