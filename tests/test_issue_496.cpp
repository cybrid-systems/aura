// @category: integration
// @reason: Issue #496 — Native SV Constraint/Class NodeTags + stats + mutate

#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.sv_ir;
import aura.core.ast;

namespace aura_issue_496_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:sv-node-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void seed_sv_workspace(aura::compiler::CompilerService& cs, aura::ast::NodeId& iface_id,
                              aura::ast::NodeId& constraint_id) {
    cs.eval("(set-code \"(define seed 1)\")");
    cs.eval("(eval-current)");
    auto* ws = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    if (!ws || !pool)
        return;
    const auto mp = ws->add_modport(pool->intern("master"),
                                    std::span<const aura::ast::SymId>{
                                        pool->intern("clk"), pool->intern("data")});
    const std::vector<aura::ast::NodeId> body{mp};
    iface_id = ws->add_interface(pool->intern("Bus"), body);
    constraint_id = ws->add_constraint(
        pool->intern("c_dist"),
        std::span<const aura::ast::SymId>{pool->intern("dist val inside {[0:255]};")});
    const std::vector<aura::ast::NodeId> cls_body{constraint_id};
    (void)ws->add_class(pool->intern("Packet"), pool->intern("BasePkt"), cls_body);
}

} // namespace aura_issue_496_detail

int main() {
    using namespace aura_issue_496_detail;

    std::println("=== Issue #496: Native SV NodeTag + SoA support ===");

    aura::compiler::CompilerService cs;

    // AC1: query:sv-node-stats fields
    {
        std::println("\n--- AC1: query:sv-node-stats ---");
        auto stats = cs.eval("(query:sv-node-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats), "query:sv-node-stats returns hash");
        CHECK(snap_stat(cs, "interface-count") >= 0, "interface-count present");
        CHECK(snap_stat(cs, "modport-count") >= 0, "modport-count present");
        CHECK(snap_stat(cs, "constraint-count") >= 0, "constraint-count present");
        CHECK(snap_stat(cs, "class-count") >= 0, "class-count present");
        CHECK(snap_stat(cs, "sv-node-total") >= 0, "sv-node-total present");
        CHECK(snap_stat(cs, "sv-mutate-attempts") >= 0, "sv-mutate-attempts present");
    }

    aura::ast::NodeId iface_id = aura::ast::NULL_NODE;
    aura::ast::NodeId constraint_id = aura::ast::NULL_NODE;
    seed_sv_workspace(cs, iface_id, constraint_id);

    // AC2: builders + IR mapping + re-emit
    {
        std::println("\n--- AC2: AST builders + IR mapping ---");
        auto* ws = cs.workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        CHECK(ws != nullptr && pool != nullptr, "workspace available");
        CHECK(iface_id < ws->size(), "interface node created");
        CHECK(constraint_id < ws->size(), "constraint node created");
        CHECK(ws->get(iface_id).tag == aura::ast::NodeTag::Interface, "Interface tag");
        CHECK(ws->get(constraint_id).tag == aura::ast::NodeTag::Constraint, "Constraint tag");
        auto if_ir = aura::compiler::sv_ir::map_interface_node_to_ir(*ws, *pool, iface_id);
        CHECK(if_ir.has_value() && if_ir->modports.size() == 1,
              "map_interface_node_to_ir sees modport");
        auto c_ir = aura::compiler::sv_ir::map_constraint_node_to_ir(*ws, *pool, constraint_id);
        CHECK(c_ir.has_value() && c_ir->expressions.size() == 1,
              "map_constraint_node_to_ir sees expr");
        const auto emitted = aura::compiler::sv_ir::emit_constraint(*c_ir);
        CHECK(emitted.find("constraint c_dist") != std::string::npos,
              "emit_constraint contains name");
        const auto reemit = aura::compiler::sv_ir::reemit_sv_node(*ws, *pool, iface_id);
        CHECK(!reemit.sv_text.empty(), "reemit_sv_node for interface");
        CHECK(snap_stat(cs, "interface-count") >= 1,
              std::format("interface-count >= 1 (got {})", snap_stat(cs, "interface-count")));
        CHECK(snap_stat(cs, "constraint-count") >= 1,
              std::format("constraint-count >= 1 (got {})", snap_stat(cs, "constraint-count")));
        CHECK(snap_stat(cs, "class-count") >= 1,
              std::format("class-count >= 1 (got {})", snap_stat(cs, "class-count")));
    }

    const auto total_before = snap_stat(cs, "sv-node-total");
    const auto mutate_before = snap_stat(cs, "structured-mutate-hits");

    // AC3: eda:update-constraint structured mutate
    {
        std::println("\n--- AC3: eda:update-constraint ---");
        auto upd = cs.eval(std::format("(eda:update-constraint {} \"val < 128;\")",
                                       static_cast<int>(constraint_id)));
        CHECK(upd && aura::compiler::types::is_bool(*upd) &&
                  aura::compiler::types::as_bool(*upd),
              "eda:update-constraint succeeds");
        auto* ws = cs.workspace_flat();
        auto* pool = cs.evaluator().workspace_pool();
        if (ws && pool) {
            auto ir = aura::compiler::sv_ir::map_constraint_node_to_ir(*ws, *pool, constraint_id);
            CHECK(ir && ir->expressions.size() == 2,
                  std::format("constraint has 2 exprs (got {})", ir ? ir->expressions.size() : 0));
        }
        const auto mutate_after = snap_stat(cs, "structured-mutate-hits");
        CHECK(mutate_after > mutate_before,
              std::format("structured-mutate-hits grew ({} -> {})", mutate_before, mutate_after));
        CHECK(snap_stat(cs, "sv-mutate-success") >= 1, "sv-mutate-success >= 1");
    }

    // AC4: query:sv-sva-structure-stats regression
    {
        std::println("\n--- AC4: sv-sva-structure-stats regression ---");
        auto sva = cs.eval("(query:sv-sva-structure-stats)");
        CHECK(sva && aura::compiler::types::is_hash(*sva), "sv-sva-structure-stats regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) == 93,
              "stats:count == 93");
    }

    CHECK(snap_stat(cs, "sv-node-total") >= total_before, "sv-node-total monotonic");

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}