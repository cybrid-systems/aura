// test_sv_verification_edsl_full_structured_closedloop.cpp — Issue #748:
// SV verification EDSL structured class/constraint/covergroup/SVA + emit fidelity
// + dirty re-emit closed-loop (consolidates #724/#725/#726).
//
// Non-duplicative with #496, #662, #694, #693, #640.
//
//   - AC1: query:sv-verification-structure-stats reachable (schema 748)
//   - AC2: constraint mutate bumps structure-mutate + dirty re-emit
//   - AC3: property/coverpoint structured mutate bumps hits
//   - AC4: eda:run-verification-feedback closed-loop
//   - AC5: multi-round mutate matrix monotonic
//   - AC6: query regression (sv-sva, sv-node-stats, hardware-backend-sv)

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace aura_issue_748_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t loc_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:sv-verification-structure-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto mutate = loc_hash(cs, "structure-mutate-hits");
    const auto reemit = loc_hash(cs, "dirty-reemit-triggers");
    const auto pass = loc_hash(cs, "emit-fidelity-pass");
    if (mutate < 0 || reemit < 0 || pass < 0)
        return -1;
    return mutate + reemit + pass;
}

struct SvWorkspace {
    aura::ast::NodeId iface_id = aura::ast::NULL_NODE;
    aura::ast::NodeId constraint_id = aura::ast::NULL_NODE;
    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    aura::ast::NodeId covergroup_id = aura::ast::NULL_NODE;
};

static bool seed_workspace(CompilerService& cs, SvWorkspace& ws) {
    if (!cs.eval("(set-code \"(define seed 1)\")"))
        return false;
    if (!cs.eval("(eval-current)").has_value())
        return false;
    auto* flat = cs.workspace_flat();
    auto* pool = cs.evaluator().workspace_pool();
    if (!flat || !pool)
        return false;
    const auto mp = flat->add_modport(
        pool->intern("master"),
        std::span<const aura::ast::SymId>{pool->intern("clk"), pool->intern("data")});
    const std::vector<aura::ast::NodeId> iface_body{mp};
    ws.iface_id = flat->add_interface(pool->intern("Bus"), iface_body);
    ws.constraint_id = flat->add_constraint(
        pool->intern("c_dist"),
        std::span<const aura::ast::SymId>{pool->intern("dist val inside {[0:255]};")});
    const std::vector<aura::ast::NodeId> cls_body{ws.constraint_id};
    (void)flat->add_class(pool->intern("Packet"), pool->intern("BasePkt"), cls_body);
    ws.property_id = flat->add_property(pool->intern("p_req"), pool->intern("req ##1 ack"));
    const std::vector<aura::ast::SymId> bins{pool->intern("low"), pool->intern("high")};
    ws.coverpoint_id = flat->add_coverpoint(pool->intern("req"), bins);
    const std::vector<aura::ast::NodeId> cps{ws.coverpoint_id};
    ws.covergroup_id = flat->add_covergroup(pool->intern("cg_req"), cps);
    return ws.iface_id != aura::ast::NULL_NODE && ws.constraint_id != aura::ast::NULL_NODE &&
           ws.property_id != aura::ast::NULL_NODE && ws.coverpoint_id != aura::ast::NULL_NODE &&
           ws.covergroup_id != aura::ast::NULL_NODE;
}

static void run_matrix(CompilerService& cs) {
    SvWorkspace ws{};
    std::println("\n--- AC1: query:sv-verification-structure-stats (schema 748) ---");
    CHECK(seed_workspace(cs, ws), "full SV verification workspace seeded");
    auto h = cs.eval("(query:sv-verification-structure-stats)");
    CHECK(h && is_hash(*h), "sv-verification-structure-stats returns hash");
    CHECK(loc_hash(cs, "schema") == 748, "schema == 748");
    CHECK(loc_hash(cs, "structure-mutate-hits") >= 0, "structure-mutate-hits present");
    CHECK(loc_hash(cs, "dirty-reemit-triggers") >= 0, "dirty-reemit-triggers present");
    CHECK(loc_hash(cs, "emit-fidelity-pass") >= 0, "emit-fidelity-pass present");
    CHECK(loc_hash(cs, "emit-fidelity-fail") >= 0, "emit-fidelity-fail present");

    std::println("\n--- AC2: structured mutate + dirty re-emit closed-loop ---");
    const auto mutate0 = loc_hash(cs, "structure-mutate-hits");
    const auto reemit0 = loc_hash(cs, "dirty-reemit-triggers");
    const auto pass0 = loc_hash(cs, "emit-fidelity-pass");
    auto upd = cs.eval(std::format("(eda:update-constraint {} \"val < 128;\")",
                                   static_cast<int>(ws.constraint_id)));
    CHECK(upd && is_bool(*upd) && as_bool(*upd), "eda:update-constraint succeeds");
    const auto mutate1 = loc_hash(cs, "structure-mutate-hits");
    const auto reemit1 = loc_hash(cs, "dirty-reemit-triggers");
    const auto pass1 = loc_hash(cs, "emit-fidelity-pass");
    std::println("  structure-mutate-hits: {} -> {}", mutate0, mutate1);
    std::println("  dirty-reemit-triggers: {} -> {}", reemit0, reemit1);
    std::println("  emit-fidelity-pass: {} -> {}", pass0, pass1);
    CHECK(mutate1 > mutate0, "structure-mutate-hits grew after constraint mutate");
    CHECK(reemit1 > reemit0 || pass1 > pass0,
          "dirty-reemit or emit-fidelity-pass grew after constraint mutate");

    std::println("\n--- AC3: property + coverpoint structured mutate ---");
    const auto mutate2a = loc_hash(cs, "structure-mutate-hits");
    auto weak = cs.eval(
        std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(ws.property_id)));
    CHECK(weak && is_bool(*weak) && as_bool(*weak), "eda:weaken-property succeeds");
    auto bin = cs.eval(
        std::format("(eda:add-coverpoint-bin {} \"mid\")", static_cast<int>(ws.coverpoint_id)));
    CHECK(bin && is_bool(*bin) && as_bool(*bin), "eda:add-coverpoint-bin succeeds");
    const auto mutate2b = loc_hash(cs, "structure-mutate-hits");
    std::println("  structure-mutate-hits: {} -> {}", mutate2a, mutate2b);
    CHECK(mutate2b >= mutate2a + 2,
          "structure-mutate-hits grew by >= 2 after property/coverpoint mutate");

    std::println("\n--- AC4: eda:run-verification-feedback closed-loop ---");
    const auto mutate4a = loc_hash(cs, "structure-mutate-hits");
    const auto reemit4a = loc_hash(cs, "dirty-reemit-triggers");
    auto fb_cov =
        cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                            static_cast<int>(ws.covergroup_id)));
    CHECK(fb_cov && is_bool(*fb_cov) && as_bool(*fb_cov),
          "eda:run-verification-feedback coverage path succeeds");
    auto fb_assert =
        cs.eval(std::format("(eda:run-verification-feedback \"assert-fail.log\" \"{} fail\")",
                            static_cast<int>(ws.property_id)));
    CHECK(fb_assert && is_bool(*fb_assert) && as_bool(*fb_assert),
          "eda:run-verification-feedback assert path succeeds");
    const auto mutate4b = loc_hash(cs, "structure-mutate-hits");
    const auto reemit4b = loc_hash(cs, "dirty-reemit-triggers");
    std::println("  structure-mutate-hits: {} -> {}", mutate4a, mutate4b);
    std::println("  dirty-reemit-triggers: {} -> {}", reemit4a, reemit4b);
    CHECK(mutate4b > mutate4a || reemit4b > reemit4a,
          "feedback loop bumped structure-mutate or dirty-reemit");

    std::println("\n--- AC5: multi-round mutate matrix ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval(std::format("(eda:update-constraint {} \"val < {};\")",
                                  static_cast<int>(ws.constraint_id), 64 + round));
        (void)cs.eval(std::format("(eda:add-coverpoint-bin {} \"r{}\")",
                                  static_cast<int>(ws.coverpoint_id), round));
    }
    const auto stats5b = stats_sum(cs);
    std::println("  closed-loop sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "stats monotonic over mutate matrix");

    std::println("\n--- AC6: query regression ---");
    auto sva = cs.eval("(query:sv-sva-structure-stats)");
    auto sv_node = cs.eval("(query:sv-node-stats)");
    auto hw = cs.eval("(query:hardware-backend-sv-closedloop-stats)");
    auto verify = cs.eval("(query:sv-verification-closedloop-stats)");
    CHECK(sva && is_hash(*sva), "sv-sva-structure-stats regression");
    CHECK(sv_node && (is_int(*sv_node) || is_hash(*sv_node)), "sv-node-stats regression");
    CHECK(hw && is_hash(*hw), "hardware-backend-sv-closedloop-stats regression");
    CHECK(verify && is_hash(*verify), "sv-verification-closedloop-stats regression");
}

} // namespace aura_issue_748_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_issue_748_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}