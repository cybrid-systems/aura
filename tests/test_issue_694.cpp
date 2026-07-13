// @category: integration
// @reason: Issue #694 SVA structured NodeTags + mutate + IR mapping

#include <atomic>
#include <iostream>
#include <mutex>
#include <print>
#include <string>
#include <thread>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.sv_ir;
import aura.core.ast;

namespace aura_issue_694_detail {
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

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:sv-sva-structure-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void seed_sva_workspace(aura::compiler::CompilerService& cs, aura::ast::NodeId& property_id,
                               aura::ast::NodeId& coverpoint_id) {
    cs.eval("(set-code \"(define seed 1)\")");
    cs.eval("(eval-current)");
    auto* ws = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    if (!ws || !pool)
        return;
    const auto expr = pool->intern("req ack");
    property_id = ws->add_property(pool->intern("p_req"), expr);
    const std::vector<aura::ast::SymId> bins{pool->intern("low"), pool->intern("high")};
    coverpoint_id = ws->add_coverpoint(pool->intern("req"), bins);
    const std::vector<aura::ast::NodeId> cps{coverpoint_id};
    (void)ws->add_covergroup(pool->intern("cg_req"), cps);
}

} // namespace aura_issue_694_detail

int aura_issue_694_run() {
    using namespace aura_issue_694_detail;
    std::println("=== Issue #694: SVA structured NodeTags ===");

    aura::compiler::CompilerService cs;

    // AC1: stats hash fields
    {
        std::println("\n--- AC1: query:sv-sva-structure-stats ---");
        auto stats = cs.eval("(query:sv-sva-structure-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:sv-sva-structure-stats returns hash");
        CHECK(stat_int(cs, "property-count") >= 0, "property-count present");
        CHECK(stat_int(cs, "coverpoint-count") >= 0, "coverpoint-count present");
        CHECK(stat_int(cs, "structured-mutate-hits") >= 0, "structured-mutate-hits present");
    }

    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    seed_sva_workspace(cs, property_id, coverpoint_id);

    // AC2: builders + map_property_node_to_ir
    {
        std::println("\n--- AC2: AST builders + IR mapping ---");
        auto* ws = cs.workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(ws != nullptr && pool != nullptr, "workspace available");
        CHECK(property_id < ws->size(), "property node created");
        CHECK(ws->get(property_id).tag == aura::ast::NodeTag::Property, "node tag is Property");
        auto ir = aura::compiler::sv_ir::map_property_node_to_ir(*ws, *pool, property_id);
        CHECK(ir.has_value(), "map_property_node_to_ir succeeds");
        if (ir) {
            const auto emitted = aura::compiler::sv_ir::emit_property(*ir);
            CHECK(emitted.find("property p_req") != std::string::npos,
                  "emit_property contains property name");
            CHECK(emitted.find("req ack") != std::string::npos, "emit_property contains expr");
        }
        const auto prop_count = stat_int(cs, "property-count");
        CHECK(prop_count >= 1, std::format("property-count >= 1 (got {})", prop_count));
    }

    const auto mutate_before = stat_int(cs, "structured-mutate-hits");

    // AC3: eda structured mutates with Guard/StableRef
    {
        std::println("\n--- AC3: eda:weaken-property + eda:add-coverpoint-bin ---");
        auto weaken = cs.eval(
            std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(property_id)));
        CHECK(weaken && aura::compiler::types::is_bool(*weaken) &&
                  aura::compiler::types::as_bool(*weaken),
              "eda:weaken-property succeeds");
        auto add_bin = cs.eval(
            std::format("(eda:add-coverpoint-bin {} \"mid\")", static_cast<int>(coverpoint_id)));
        CHECK(add_bin && aura::compiler::types::is_bool(*add_bin) &&
                  aura::compiler::types::as_bool(*add_bin),
              "eda:add-coverpoint-bin succeeds");
        auto* ws = cs.workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        if (ws && pool) {
            auto ir = aura::compiler::sv_ir::map_coverpoint_node_to_ir(*ws, *pool, coverpoint_id);
            CHECK(ir && ir->bins.size() >= 3,
                  std::format("coverpoint has 3 bins (got {})", ir ? ir->bins.size() : 0));
        }
        const auto mutate_after = stat_int(cs, "structured-mutate-hits");
        CHECK(mutate_after > mutate_before,
              std::format("structured-mutate-hits grew ({} -> {})", mutate_before, mutate_after));
        const auto sva_dirty = stat_int(cs, "sva-dirty-total");
        CHECK(sva_dirty >= 1, std::format("sva-dirty-total >= 1 (got {})", sva_dirty));
    }

    // AC4: stats:count
    {
        std::println("\n--- AC4: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    // AC5: fiber stress
    {
        std::println("\n--- AC5: fiber stress ---");
        std::mutex eval_mtx;
        std::atomic<int> ok_count{0};
        constexpr int k_iters = 20;
        auto worker = [&] {
            for (int i = 0; i < k_iters; ++i) {
                std::lock_guard<std::mutex> lk(eval_mtx);
                (void)cs.eval("(typecheck-current)");
                auto r = cs.eval(std::format("(eda:add-coverpoint-bin {} \"stress\")",
                                             static_cast<int>(coverpoint_id)));
                if (r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r))
                    ok_count.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();
        CHECK(ok_count.load() > 0,
              std::format("fiber stress produced {} successful bin adds", ok_count.load()));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_694_run();
}
#endif
