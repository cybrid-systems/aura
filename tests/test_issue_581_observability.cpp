// @category: integration
// @reason: Issue #581 — stable-ref-sv-scale-stats hash slice

#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_581_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:stable-ref-sv-scale-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void build_soc_hierarchy(aura::ast::FlatAST& flat, aura::ast::StringPool& pool) {
    std::size_t batch = 0;
    while (flat.size() < 5000) {
        for (int clk = 0; clk < 10 && flat.size() < 5000; ++clk) {
            std::vector<aura::ast::NodeId> body;
            for (int mp = 0; mp < 4; ++mp) {
                const std::vector<aura::ast::SymId> ports{pool.intern("sig_a"),
                                                          pool.intern("sig_b")};
                body.push_back(
                    flat.add_modport(pool.intern(std::format("mp_{}_{}", batch, mp)), ports));
            }
            (void)flat.add_interface(pool.intern(std::format("iface_{}_{}", batch, clk)), body);
            const auto prop = flat.add_property(pool.intern(std::format("p_{}_{}", batch, clk)),
                                                pool.intern("req ##1 ack"));
            const std::vector<aura::ast::SymId> bins{pool.intern("b0"), pool.intern("b1")};
            const auto cp =
                flat.add_coverpoint(pool.intern(std::format("v_{}_{}", batch, clk)), bins);
            (void)flat.add_covergroup(pool.intern(std::format("cg_{}_{}", batch, clk)),
                                      std::span<const aura::ast::NodeId>(&cp, 1));
            (void)flat.add_assert(pool.intern(std::format("a_{}_{}", batch, clk)), prop);
        }
        ++batch;
    }
}

static bool setup_large_sv_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define soc 1)\")")) {
        return false;
    }
    if (!cs.eval("(eval-current)").has_value()) {
        return false;
    }
    auto* ws = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    if (!ws || !pool) {
        return false;
    }
    build_soc_hierarchy(*ws, *pool);
    return ws->size() >= 5000;
}

} // namespace aura_issue_581_detail

int main() {
    using namespace aura_issue_581_detail;

    std::println("=== Issue #581: stable-ref-sv-scale-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_large_sv_workspace(cs), "large SV SoC workspace (5k+ nodes)");

    // AC1: query:stable-ref-sv-scale-stats returns hash
    {
        std::println("\n--- AC1: query:stable-ref-sv-scale-stats ---");
        auto stats = cs.eval("(query:stable-ref-sv-scale-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:stable-ref-sv-scale-stats returns hash");
        CHECK(hash_int(cs, "wrap-events") >= 0, "wrap-events present");
        CHECK(hash_int(cs, "avg-dirty-walk-depth-on-sv") >= 0,
              "avg-dirty-walk-depth-on-sv present");
        CHECK(hash_int(cs, "compact-trigger-count") >= 0, "compact-trigger-count present");
        CHECK(hash_int(cs, "stale-ref-on-large-ast") >= 0, "stale-ref-on-large-ast present");
        CHECK(hash_int(cs, "stable-ref-sv-scale-schema") == 581,
              "stable-ref-sv-scale-schema == 581");
        CHECK(hash_int(cs, "sv-node-count") > 0, "sv-node-count > 0 on large SoC");
        CHECK(hash_int(cs, "stable-ref-sv-scale-total") >= 0, "stable-ref-sv-scale-total present");
        CHECK(hash_int(cs, "stable-ref-sv-scale-recommendation") >= 0,
              "stable-ref-sv-scale-recommendation present");
    }

    const auto sv_before = hash_int(cs, "sv-node-count");
    const auto depth_before = hash_int(cs, "avg-dirty-walk-depth-on-sv");
    const auto total_before = hash_int(cs, "stable-ref-sv-scale-total");

    // AC2: SV structural mutates + dirty propagation on large AST
    {
        std::println("\n--- AC2: SV mutate + dirty propagation ---");
        aura::ast::NodeId property_id = aura::ast::NULL_NODE;
        aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
        if (auto* ws = cs.workspace_flat()) {
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                if (property_id == aura::ast::NULL_NODE &&
                    ws->get(id).tag == aura::ast::NodeTag::Property)
                    property_id = id;
                if (coverpoint_id == aura::ast::NULL_NODE &&
                    ws->get(id).tag == aura::ast::NodeTag::Coverpoint)
                    coverpoint_id = id;
                if (property_id != aura::ast::NULL_NODE && coverpoint_id != aura::ast::NULL_NODE)
                    break;
            }
        }
        CHECK(property_id != aura::ast::NULL_NODE, "property node found in large SoC");
        CHECK(coverpoint_id != aura::ast::NULL_NODE, "coverpoint node found in large SoC");
        auto weaken = cs.eval(
            std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(property_id)));
        CHECK(weaken && aura::compiler::types::is_bool(*weaken) &&
                  aura::compiler::types::as_bool(*weaken),
              "eda:weaken-property on large SoC succeeds");
        auto feedback =
            cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                static_cast<int>(coverpoint_id)));
        CHECK(feedback && aura::compiler::types::is_bool(*feedback) &&
                  aura::compiler::types::as_bool(*feedback),
              "eda:run-verification-feedback on large SoC succeeds");
        const auto depth_after = hash_int(cs, "avg-dirty-walk-depth-on-sv");
        const auto total_after = hash_int(cs, "stable-ref-sv-scale-total");
        CHECK(depth_after >= depth_before,
              std::format("avg-dirty-walk-depth-on-sv non-decreasing ({} -> {})", depth_before,
                          depth_after));
        CHECK(total_after >= total_before,
              std::format("stable-ref-sv-scale-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC3: stable-ref query cycle on large AST
    {
        std::println("\n--- AC3: stable-ref query cycle ---");
        (void)cs.eval("(query:stable-ref 1)");
        (void)cs.eval("(query:pattern \"prop\")");
        CHECK(hash_int(cs, "sv-node-count") >= sv_before,
              std::format("sv-node-count stable ({} -> {})", sv_before,
                          hash_int(cs, "sv-node-count")));
        CHECK(hash_int(cs, "stale-ref-on-large-ast") >= 0,
              "stale-ref-on-large-ast readable after query cycle");
    }

    // AC4: related stability primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto eda_stab = cs.eval("(query:eda-stability-stats)");
        auto lifecycle = cs.eval("(query:stable-ref-lifecycle-stats)");
        auto legacy = cs.eval("(query:stable-ref-stats-hash)");
        auto workspace = cs.eval("(query:stable-ref-workspace-tree-stats)");
        CHECK(eda_stab && aura::compiler::types::is_hash(*eda_stab),
              "query:eda-stability-stats hash regression (#540)");
        CHECK(lifecycle && aura::compiler::types::is_hash(*lifecycle),
              "query:stable-ref-lifecycle-stats hash regression (#497)");
        CHECK(legacy && aura::compiler::types::is_hash(*legacy),
              "query:stable-ref-stats-hash regression (#470)");
        CHECK(workspace && aura::compiler::types::is_int(*workspace),
              "query:stable-ref-workspace-tree-stats int regression (#424)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 146,
              "stats:count >= 146");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}