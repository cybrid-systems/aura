// test_sv_verification_self_evolution_closed_loop_reliability.cpp — Issue #802:
// SV verification feedback-driven self-evolution closed-loop reliability
// (refines #774/#726/#748; non-duplicative with closed-loop-reliability-stats).
//
//   - AC1:  query:sv-verification-self-evolution-stats reachable (schema 802)
//   - AC2:  feedback-parse-hits bumps on direct path
//   - AC3:  structured-mutate-hits bumps on direct path
//   - AC4:  closed-loop-rounds bumps on direct path
//   - AC5:  convergence-hits bumps on direct path
//   - AC6:  feedback parse + mutate:from-verification-feedback production path
//   - AC7:  aggregate counters monotonic after bump matrix
//   - AC8:  query regression (#726 closed-loop, #748 structure-stats)

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace aura_issue_802_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (query:sv-verification-self-evolution-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t feedback_parse(CompilerService& cs) {
    return stat_int(cs, "feedback-parse-hits");
}
static std::int64_t structured_mutate(CompilerService& cs) {
    return stat_int(cs, "structured-mutate-hits");
}
static std::int64_t closed_loop_rounds(CompilerService& cs) {
    return stat_int(cs, "closed-loop-rounds");
}
static std::int64_t convergence(CompilerService& cs) {
    return stat_int(cs, "convergence-hits");
}

struct SvWorkspace {
    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId constraint_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
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
        std::span<const aura::ast::SymId>{pool->intern("clk"), pool->intern("req")});
    const std::vector<aura::ast::NodeId> iface_body{mp};
    (void)flat->add_interface(pool->intern("Bus"), iface_body);
    ws.constraint_id = flat->add_constraint(
        pool->intern("c_dist"),
        std::span<const aura::ast::SymId>{pool->intern("dist val inside {[0:255]};")});
    ws.property_id = flat->add_property(pool->intern("p_req"), pool->intern("req ##1 ack"));
    const std::vector<aura::ast::SymId> bins{pool->intern("low"), pool->intern("high")};
    ws.coverpoint_id = flat->add_coverpoint(pool->intern("req"), bins);
    return ws.property_id != aura::ast::NULL_NODE && ws.constraint_id != aura::ast::NULL_NODE &&
           ws.coverpoint_id != aura::ast::NULL_NODE;
}

static void run_production_ac6(CompilerService& cs, const SvWorkspace& ws) {
    std::println("\n--- AC6: feedback parse + structured mutate production path ---");
    const auto parse6a = feedback_parse(cs);
    const auto cov_text = std::format("{} hit_rate=0.45", static_cast<int>(ws.coverpoint_id));
    auto parsed = cs.eval(std::format("(verify:parse-coverage-feedback \"{}\")", cov_text));
    CHECK(parsed && is_int(*parsed) && as_int(*parsed) >= 1,
          "verify:parse-coverage-feedback marks coverage hole");
    CHECK(feedback_parse(cs) > parse6a, "feedback-parse-hits grew after coverage parse");

    const auto mutate6a = structured_mutate(cs);
    const auto rounds6a = closed_loop_rounds(cs);
    auto mutated = cs.eval(std::format("(mutate:from-verification-feedback \"weaken-property\" "
                                       "{} \"reset\")",
                                       static_cast<int>(ws.property_id)));
    CHECK(mutated && is_bool(*mutated) && as_bool(*mutated),
          "mutate:from-verification-feedback weaken-property succeeds");
    CHECK(structured_mutate(cs) > mutate6a, "structured-mutate-hits grew after feedback mutate");
    CHECK(closed_loop_rounds(cs) > rounds6a, "closed-loop-rounds grew after feedback mutate");

    const auto conv6a = convergence(cs);
    auto fb = cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                  static_cast<int>(ws.coverpoint_id)));
    CHECK(fb && is_bool(*fb) && as_bool(*fb), "eda:run-verification-feedback succeeds");
    CHECK(convergence(cs) > conv6a, "convergence-hits grew after verification feedback loop");
}

static void run_matrix(CompilerService& cs) {
    SvWorkspace ws{};
    std::println("\n--- AC1: query:sv-verification-self-evolution-stats (schema 802) ---");
    CHECK(seed_workspace(cs, ws), "SV verification workspace seeded");
    auto h = cs.eval("(query:sv-verification-self-evolution-stats)");
    CHECK(h && is_hash(*h), "sv-verification-self-evolution-stats returns hash");
    CHECK(stat_int(cs, "schema") == 802, "schema == 802");
    CHECK(feedback_parse(cs) >= 0, "feedback-parse-hits non-negative");
    CHECK(structured_mutate(cs) >= 0, "structured-mutate-hits non-negative");
    CHECK(closed_loop_rounds(cs) >= 0, "closed-loop-rounds non-negative");
    CHECK(convergence(cs) >= 0, "convergence-hits non-negative");

    std::println("\n--- AC2: feedback-parse-hits bumps on direct path ---");
    const auto p0 = feedback_parse(cs);
    cs.evaluator().bump_sv_self_evo_feedback_parse(2);
    CHECK(feedback_parse(cs) == p0 + 2, "feedback-parse-hits bumps by 2");

    std::println("\n--- AC3: structured-mutate-hits bumps on direct path ---");
    const auto m0 = structured_mutate(cs);
    cs.evaluator().bump_sv_self_evo_structured_mutate();
    CHECK(structured_mutate(cs) == m0 + 1, "structured-mutate-hits bumps by 1");

    std::println("\n--- AC4: closed-loop-rounds bumps on direct path ---");
    const auto r0 = closed_loop_rounds(cs);
    cs.evaluator().bump_sv_self_evo_closed_loop_rounds(2);
    CHECK(closed_loop_rounds(cs) == r0 + 2, "closed-loop-rounds bumps by 2");

    std::println("\n--- AC5: convergence-hits bumps on direct path ---");
    const auto c0 = convergence(cs);
    cs.evaluator().bump_sv_self_evo_convergence_hits(3);
    CHECK(convergence(cs) == c0 + 3, "convergence-hits bumps by 3");

    run_production_ac6(cs, ws);

    std::println("\n--- AC7: aggregate counters monotonic after bump matrix ---");
    const auto agg7a =
        feedback_parse(cs) + structured_mutate(cs) + closed_loop_rounds(cs) + convergence(cs);
    cs.evaluator().bump_sv_self_evo_feedback_parse();
    cs.evaluator().bump_sv_self_evo_structured_mutate();
    cs.evaluator().bump_sv_self_evo_closed_loop_rounds();
    cs.evaluator().bump_sv_self_evo_convergence_hits();
    const auto agg7b =
        feedback_parse(cs) + structured_mutate(cs) + closed_loop_rounds(cs) + convergence(cs);
    CHECK(agg7b >= agg7a + 4, "aggregate self-evolution counters monotonic");

    std::println("\n--- AC8: query regression ---");
    auto closed726 = cs.eval("(query:closed-loop-reliability-stats)");
    auto structure748 = cs.eval("(query:sv-verification-structure-stats)");
    CHECK(closed726 && is_hash(*closed726), "closed-loop-reliability-stats regression (#726)");
    CHECK(structure748 && is_hash(*structure748),
          "sv-verification-structure-stats regression (#748)");
}

} // namespace aura_issue_802_detail

int aura_issue_sv_verification_self_evolution_closed_loop_reliability_run() {
    aura::compiler::CompilerService cs;
    aura_issue_802_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_sv_verification_self_evolution_closed_loop_reliability_run();
}
#endif
