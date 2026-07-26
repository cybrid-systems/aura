// Issue #2061 — incremental restamp observability for generation wrap.
// Issue #2122 — wrap-time dirty-cone restamp (vs full live walk) + fallback.
//
// #2061:
//   - restamp_nodes_total + restamp_us_total metrics
//   - free-list-skipping full restamp; StableNodeRef wrap invalidation
//   - idempotent restamp_all_node_generations
//
// #2122:
//   AC1: source cites #2122; wrap policy documented
//   AC2: dirty-cone wrap restamps ≪ live_nodes (incremental counter)
//   AC3: pre-wrap StableNodeRef on untouched node invalidated (wrap_epoch)
//   AC4: pinned path still restamps (via full/lazy; Guard restamp_pinned separate)
//   AC5: density threshold forces full fallback + counter
//   AC6: metrics on query:generation-stats (schema-2122 keys when workspace live)
//   AC7: this file (reuse #2061 test per #81967)

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.ast;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::uint64_t force_one_wrap(FlatAST& ast) {
    constexpr std::uint64_t kBumpsPerWrap = 65536;
    for (std::uint64_t i = 0; i < kBumpsPerWrap; ++i)
        ast.bump_generation();
    return kBumpsPerWrap;
}

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href_gen(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:generation-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    std::println("=== Issue #2061 / #2122: restamp observability + wrap cone ===");
    FlatAST ast;

    constexpr int kSeedNodes = 10000;
    for (int i = 0; i < kSeedNodes; ++i)
        ast.add_node(NodeTag::LiteralInt, SyntaxMarker::User);

    // ── AC1 (#2061): force ≥2 wraps, full restamp when no dirty ──
    {
        std::println("\n--- #2061 AC1: forced ≥2 wraps, full restamp (no dirty) ---");
        const auto wrap_before = ast.generation_wrap_count();
        const auto restamp_nodes_before = ast.restamp_nodes_total();
        const auto restamp_us_before = ast.restamp_us_total();
        const auto live_nodes_before = ast.size();
        const auto fb0 = ast.restamp_full_fallback_total();

        force_one_wrap(ast);
        CHECK(ast.generation_wrap_count() == wrap_before + 1, "wrap #1");
        ast.restamp_all_node_generations();
        const auto after_wrap1_nodes = ast.restamp_nodes_total();
        // No dirty → full fallback path restamps all live.
        CHECK(after_wrap1_nodes - restamp_nodes_before == live_nodes_before,
              "wrap#1 full restamp = live_nodes");
        CHECK(ast.restamp_full_fallback_total() >= fb0 + 1, "full fallback on empty touched");

        force_one_wrap(ast);
        CHECK(ast.generation_wrap_count() == wrap_before + 2, "wrap #2");
        ast.restamp_all_node_generations();
        const auto after_wrap2_nodes = ast.restamp_nodes_total();
        CHECK(after_wrap2_nodes - after_wrap1_nodes == live_nodes_before,
              "wrap#2 full restamp = live_nodes");
        CHECK(ast.restamp_us_total() > restamp_us_before, "restamp_us grows");
    }

    // ── AC2 (#2061): StableNodeRef invalid after wrap ──
    {
        std::println("\n--- #2061 AC2: StableNodeRef invalid after wrap ---");
        const auto ref = ast.make_ref(0);
        const auto wrap_epoch_before = ast.wrap_epoch();
        CHECK(ref.is_valid_in(ast), "ref valid before wrap");
        force_one_wrap(ast);
        ast.restamp_all_node_generations();
        CHECK(ast.wrap_epoch() > wrap_epoch_before, "wrap_epoch bumped");
        CHECK(!ref.is_valid_in(ast), "ref invalid after wrap (wrap_epoch fence)");
    }

    // ── AC3 (#2061): restamp idempotent ──
    {
        std::println("\n--- #2061 AC3: restamp idempotency ---");
        const auto restamp_nodes_before = ast.restamp_nodes_total();
        const auto live_nodes = ast.size();
        ast.restamp_all_node_generations();
        ast.restamp_all_node_generations();
        ast.restamp_all_node_generations();
        CHECK(ast.restamp_nodes_total() - restamp_nodes_before == 3 * live_nodes,
              "3 restamps = 3 * live");
    }

    // ── AC4 (#2061): metrics move ──
    {
        std::println("\n--- #2061 AC4: metrics present ---");
        CHECK(ast.generation_wrap_count() >= 3, "wrap_count >= 3");
        CHECK(ast.restamp_nodes_total() > 0, "restamp_nodes > 0");
        CHECK(ast.restamp_us_total() > 0, "restamp_us > 0");
    }

    // ── #2122 AC1: source docs ──
    {
        std::println("\n--- #2122 AC1: source cites policy ---");
        auto src = read_file("src/core/ast.ixx");
        CHECK(!src.empty(), "read ast.ixx");
        CHECK(src.find("#2122") != std::string::npos, "cites #2122");
        CHECK(src.find("restamp_incremental") != std::string::npos ||
                  src.find("dirty-cone") != std::string::npos ||
                  src.find("use_incremental") != std::string::npos,
              "incremental policy present");
        CHECK(src.find("restamp_full_fallback_total") != std::string::npos, "fallback counter");
    }

    // ── #2122 AC2: dirty cone ≪ live on wrap ──
    {
        std::println("\n--- #2122 AC2: incremental dirty-cone wrap ---");
        FlatAST cone;
        constexpr int kN = 8000;
        for (int i = 0; i < kN; ++i)
            cone.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
        // Dirty a small cone (few nodes).
        for (int i = 0; i < 5; ++i)
            cone.mark_dirty(static_cast<aura::ast::NodeId>(i));
        const auto inc0 = cone.restamp_incremental_nodes_total();
        const auto rn0 = cone.restamp_nodes_total();
        force_one_wrap(cone);
        cone.restamp_all_node_generations();
        const auto inc_delta = cone.restamp_incremental_nodes_total() - inc0;
        const auto rn_delta = cone.restamp_nodes_total() - rn0;
        std::println("  live={} restamped={} incremental_delta={}", cone.size(), rn_delta,
                     inc_delta);
        CHECK(inc_delta > 0, "incremental path restamped some nodes");
        CHECK(inc_delta <= 32, "incremental restamped small cone (≪ live)");
        CHECK(rn_delta == inc_delta, "restamp_nodes_total matches incremental count");
        CHECK(rn_delta * 10 < cone.size(), "restamped ≪ live_nodes");
        CHECK(cone.restamp_lazy_align_enabled(), "lazy align enabled after incremental");
    }

    // ── #2122 AC3: untouched pre-wrap ref not silently valid ──
    {
        std::println("\n--- #2122 AC3: untouched ref after incremental wrap ---");
        FlatAST cone;
        for (int i = 0; i < 1000; ++i)
            cone.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
        // Capture ref on node that will NOT be dirtied.
        const auto untouched = cone.make_ref(500);
        CHECK(untouched.is_valid_in(cone), "untouched valid pre-wrap");
        cone.mark_dirty(0);
        force_one_wrap(cone);
        cone.restamp_all_node_generations();
        // wrap_epoch fence: must not be silently valid with wrong gen.
        CHECK(!untouched.is_valid_in(cone), "pre-wrap ref invalid after wrap (epoch fence)");
        // Live node still accessible via is_valid(NodeId) through lazy align.
        CHECK(cone.is_valid(static_cast<aura::ast::NodeId>(500)),
              "untouched live NodeId valid via lazy align");
        // Fresh ref works after lazy align.
        const auto fresh = cone.make_ref(500);
        CHECK(fresh.is_valid_in(cone), "fresh ref on untouched node valid after wrap");
    }

    // ── #2122 AC5: density threshold → full fallback ──
    {
        std::println("\n--- #2122 AC5: density threshold full fallback ---");
        FlatAST dense;
        constexpr int kN = 100;
        for (int i = 0; i < kN; ++i)
            dense.add_node(NodeTag::LiteralInt, SyntaxMarker::User);
        // Dirty >30% of nodes.
        for (int i = 0; i < 50; ++i)
            dense.mark_dirty(static_cast<aura::ast::NodeId>(i));
        const auto fb0 = dense.restamp_full_fallback_total();
        const auto inc0 = dense.restamp_incremental_nodes_total();
        force_one_wrap(dense);
        dense.restamp_all_node_generations();
        CHECK(dense.restamp_full_fallback_total() >= fb0 + 1, "full fallback bumped");
        CHECK(dense.restamp_incremental_nodes_total() == inc0,
              "incremental counter unchanged on fallback");
        CHECK(dense.restamp_nodes_total() >= static_cast<std::uint64_t>(kN),
              "full path restamped all live");
        CHECK(!dense.restamp_lazy_align_enabled(), "lazy align off after full");
    }

    // ── #2122 AC6: query surface ──
    {
        std::println("\n--- #2122 AC6: query:generation-stats schema-2122 ---");
        CompilerService cs;
        // Need a workspace for live FlatAST counters — load minimal source.
        auto set = cs.eval("(set-code \"(define x 1)\")");
        (void)set;
        CHECK(href_gen(cs, "schema-2122") == 2122 || href_gen(cs, "schema") == 1282,
              "generation-stats reachable");
        // schema-2122 only when workspace present
        if (cs.evaluator().workspace_flat()) {
            CHECK(href_gen(cs, "schema-2122") == 2122, "schema-2122");
            CHECK(href_gen(cs, "issue-2122") == 2122, "issue-2122");
            CHECK(href_gen(cs, "restamp-nodes-total") >= 0, "restamp-nodes-total");
            CHECK(href_gen(cs, "restamp-incremental-nodes-total") >= 0, "incremental key");
            CHECK(href_gen(cs, "restamp-full-fallback-total") >= 0, "fallback key");
        }
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
