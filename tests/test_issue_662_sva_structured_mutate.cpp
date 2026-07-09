// @category: integration
// @reason: Issue #662 — SVA structured mutate feedback closed loop
//  (no dedicated NodeTag/IR for SVA, no dirty targeting, no feedback-
//  driven mutate primitive). The work was done in #310 (NodeTag
//  Interface/Modport foundation) + #694 (SVA structured NodeTags +
//  map_ + dirty bits + (eda:weaken-property) + (eda:add-coverpoint-bin)
//  + query:sv-sva-structure-stats) + #640 (verification feedback
//  closed loop). This test ships the dedicated regression net that
//  codifies the AI self-evo "coverage hole → add coverpoint" /
//  "assert fail → weaken property" closed loop.
//
//   - AC1:  query:sv-sva-structure-stats reachable (schema 694),
//           property/coverpoint counts baseline
//   - AC2:  (eda:weaken-property pid clause) succeeds + bumps
//           sva_structured_mutate_hits_total
//   - AC3:  (eda:add-coverpoint-bin cid bin-name) succeeds + bumps
//           coverpoint-count observable
//   - AC4:  structured-mutate-hits grew after both mutations
//   - AC5:  sva-dirty-total reflects the kSvaDirty propagation
//   - AC6:  regression — query:sv-interface-structure-stats (schema 661) +
//           query:sv-node-stats still reachable (no Foundry break)
//   - AC7:  regression — (eda:parse-netlist) interface/modport path
//           from #661 still works (cross-feature integration)

#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_662_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:sv-sva-structure-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t iface_stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:sv-interface-structure-stats) '{}')", key));
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
}

static void run_ac1_baseline(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:sv-sva-structure-stats baseline ---");
    auto h = cs.eval("(query:sv-sva-structure-stats)");
    CHECK(h && aura::compiler::types::is_hash(*h), "sv-sva-structure-stats returns hash");
    aura::ast::NodeId pid, cid;
    seed_sva_workspace(cs, pid, cid);
    CHECK(pid != aura::ast::NULL_NODE, "property seeded");
    CHECK(cid != aura::ast::NULL_NODE, "coverpoint seeded");
    const auto pc = stat_int(cs, "property-count");
    const auto cc = stat_int(cs, "coverpoint-count");
    std::println("  baseline property-count={}, coverpoint-count={}", pc, cc);
    CHECK(pc >= 1, "property-count observable (>= 1 after seed)");
    CHECK(cc >= 1, "coverpoint-count observable (>= 1 after seed)");
}

static void run_ac2_weaken_property(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: eda:weaken-property ---");
    aura::ast::NodeId pid, cid;
    seed_sva_workspace(cs, pid, cid);
    CHECK(pid != aura::ast::NULL_NODE, "property seeded");
    const auto mh_before = stat_int(cs, "structured-mutate-hits");
    const auto r =
        cs.eval(std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(pid)));
    CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
          "eda:weaken-property returned #t");
    const auto mh_after = stat_int(cs, "structured-mutate-hits");
    std::println("  structured-mutate-hits: {} -> {}", mh_before, mh_after);
    CHECK(mh_after > mh_before, "structured-mutate-hits bumped after eda:weaken-property");
}

static void run_ac3_add_coverpoint_bin(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: eda:add-coverpoint-bin ---");
    aura::ast::NodeId pid, cid;
    seed_sva_workspace(cs, pid, cid);
    CHECK(cid != aura::ast::NULL_NODE, "coverpoint seeded");
    const auto mh_before = stat_int(cs, "structured-mutate-hits");
    const auto r =
        cs.eval(std::format("(eda:add-coverpoint-bin {} \"mid\")", static_cast<int>(cid)));
    CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
          "eda:add-coverpoint-bin returned #t");
    const auto mh_after = stat_int(cs, "structured-mutate-hits");
    std::println("  structured-mutate-hits: {} -> {}", mh_before, mh_after);
    CHECK(mh_after > mh_before, "structured-mutate-hits bumped after eda:add-coverpoint-bin");
}

static void run_ac4_combined_hits(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: combined weaken + add-coverpoint-bin hits ---");
    aura::ast::NodeId pid, cid;
    seed_sva_workspace(cs, pid, cid);
    const auto mh_before = stat_int(cs, "structured-mutate-hits");
    CHECK(
        cs.eval(std::format("(eda:weaken-property {} \"rst\")", static_cast<int>(pid))).has_value(),
        "weaken-property applied");
    CHECK(cs.eval(std::format("(eda:add-coverpoint-bin {} \"stress\")", static_cast<int>(cid)))
              .has_value(),
          "add-coverpoint-bin applied");
    const auto mh_after = stat_int(cs, "structured-mutate-hits");
    std::println("  structured-mutate-hits: {} -> {} (expect +2)", mh_before, mh_after);
    CHECK(mh_after >= mh_before + 2,
          "structured-mutate-hits grew by at least 2 after both mutations");
}

static void run_ac5_sva_dirty(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: sva-dirty-total reflects kSvaDirty propagation ---");
    aura::ast::NodeId pid, cid;
    seed_sva_workspace(cs, pid, cid);
    CHECK(
        cs.eval(std::format("(eda:weaken-property {} \"rst\")", static_cast<int>(pid))).has_value(),
        "weaken applied (sets kSvaDirty on the property)");
    const auto dt = stat_int(cs, "sva-dirty-total");
    std::println("  sva-dirty-total = {}", dt);
    CHECK(dt >= 1, "sva-dirty-total >= 1 (kSvaDirty propagated)");
}

static void run_ac6_sv_primitives_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: regression — SV primitives still reachable ---");
    auto sv_sva = cs.eval("(query:sv-sva-structure-stats)");
    auto sv_node = cs.eval("(query:sv-node-stats)");
    auto sv_iface = cs.eval("(query:sv-interface-structure-stats)");
    CHECK(sv_sva && aura::compiler::types::is_hash(*sv_sva),
          "sv-sva-structure-stats regression [hash, schema 694]");
    CHECK(sv_node &&
              (aura::compiler::types::is_int(*sv_node) || aura::compiler::types::is_hash(*sv_node)),
          "sv-node-stats regression [schema 496]");
    CHECK(sv_iface && aura::compiler::types::is_hash(*sv_iface),
          "sv-interface-structure-stats regression [hash, schema 661]");
}

static void run_ac7_eda_parse_netlist_integration(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC7: regression — eda:parse-netlist (interface+modport) coexists ---");
    // eda:parse-netlist requires workspace_flat_ to be non-null (parser
    // mutates workspace state). Bootstrap via (set-code) + (eval-current),
    // same pattern as test_issue_586 / test_issue_661 AC6.
    CHECK(cs.eval("(set-code \"(define base 10) (+ base 1)\")").has_value(), "set-code bootstrap");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current bootstrap");
    auto r = cs.eval(R"((eda:parse-netlist "interface:my_iface\nmodport:master:clk,rst,data\n"))");
    CHECK(r && aura::compiler::types::is_int(*r),
          "eda:parse-netlist still works alongside SVA primitives");
    const auto mp = iface_stat_int(cs, "modport-views");
    const auto pc = iface_stat_int(cs, "ports-count");
    std::println("  sv-interface-structure-stats: modport-views={}, ports-count={}", mp, pc);
    CHECK(mp >= 1, "modport-views bumped by eda:parse-netlist");
    CHECK(pc >= 3, "ports-count bumped by 3 (3 ports in the modport)");
}

} // namespace aura_issue_662_detail

int aura_issue_662_sva_structured_mutate_run() {
    using namespace aura_issue_662_detail;

    {
        aura::compiler::CompilerService cs;
        run_ac1_baseline(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_weaken_property(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_add_coverpoint_bin(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_combined_hits(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_sva_dirty(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_sv_primitives_regression(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac7_eda_parse_netlist_integration(cs);
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_662_sva_structured_mutate_run();
}
#endif
