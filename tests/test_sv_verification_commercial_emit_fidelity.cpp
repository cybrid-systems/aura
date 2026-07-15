// test_sv_verification_commercial_emit_fidelity.cpp — Issue #801:
// SV commercial emit fidelity observability + roundtrip stub + dirty re-emit
// (refines #772/#748/#725; non-duplicative with #748 structure-stats).
//
//   - AC1:  query:sv-commercial-emit-fidelity-stats reachable (schema 801)
//   - AC2:  emit-parse-success-hits bumps on direct path
//   - AC3:  roundtrip-mismatch-prevented bumps on direct path
//   - AC4:  dirty-reemit-hits bumps on direct path
//   - AC5:  commercial-tool-compatible-hits bumps on direct path
//   - AC6:  eda:validate-sv-emit-roundtrip + hardware feedback production path
//   - AC7:  aggregate counters monotonic after bump matrix
//   - AC8:  query regression (#748 structure-stats, #698 hardware-backend)

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace aura_issue_801_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:sv-commercial-emit-fidelity-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t emit_parse_success(CompilerService& cs) {
    return stat_int(cs, "emit-parse-success-hits");
}
static std::int64_t mismatch_prevented(CompilerService& cs) {
    return stat_int(cs, "roundtrip-mismatch-prevented");
}
static std::int64_t dirty_reemit(CompilerService& cs) {
    return stat_int(cs, "dirty-reemit-hits");
}
static std::int64_t tool_compatible(CompilerService& cs) {
    return stat_int(cs, "commercial-tool-compatible-hits");
}

struct SvWorkspace {
    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
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
        std::span<const aura::ast::SymId>{pool->intern("clk"), pool->intern("req")});
    const std::vector<aura::ast::NodeId> iface_body{mp};
    (void)flat->add_interface(pool->intern("Bus"), iface_body);
    ws.property_id = flat->add_property(pool->intern("p_req"), pool->intern("req ##1 ack"));
    const std::vector<aura::ast::SymId> bins{pool->intern("low"), pool->intern("high")};
    const auto cp = flat->add_coverpoint(pool->intern("req"), bins);
    ws.covergroup_id =
        flat->add_covergroup(pool->intern("cg_req"), std::span<const aura::ast::NodeId>(&cp, 1));
    return ws.property_id != aura::ast::NULL_NODE && ws.covergroup_id != aura::ast::NULL_NODE;
}

static void run_production_ac6(CompilerService& cs, const SvWorkspace& ws) {
    std::println("\n--- AC6: roundtrip stub + dirty re-emit production path ---");
    const auto parse6a = emit_parse_success(cs);
    const auto dirty6a = dirty_reemit(cs);
    const auto tool6a = tool_compatible(cs);
    auto roundtrip = cs.eval(std::format("(eda:validate-sv-emit-roundtrip {} \"vcs\")",
                                         static_cast<int>(ws.property_id)));
    CHECK(roundtrip && is_hash(*roundtrip), "eda:validate-sv-emit-roundtrip returns hash");
    CHECK(emit_parse_success(cs) > parse6a, "emit-parse-success grew after roundtrip");
    CHECK(dirty_reemit(cs) > dirty6a, "dirty-reemit-hits grew after roundtrip");
    CHECK(tool_compatible(cs) > tool6a, "commercial-tool-compatible grew after roundtrip");

    const auto dirty6b = dirty_reemit(cs);
    auto fb = cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                  static_cast<int>(ws.property_id)));
    CHECK(fb && is_bool(*fb) && as_bool(*fb), "eda:run-verification-feedback succeeds");
    CHECK(dirty_reemit(cs) > dirty6b, "dirty-reemit-hits grew after verification feedback re-emit");
}

static void run_matrix(CompilerService& cs) {
    SvWorkspace ws{};
    std::println("\n--- AC1: query:sv-commercial-emit-fidelity-stats (schema 801) ---");
    CHECK(seed_workspace(cs, ws), "SV verification workspace seeded");
    auto h = cs.eval("(engine:metrics \"query:sv-commercial-emit-fidelity-stats\")");
    CHECK(h && is_hash(*h), "sv-commercial-emit-fidelity-stats returns hash");
    CHECK(stat_int(cs, "schema") == 801, "schema == 801");
    CHECK(emit_parse_success(cs) >= 0, "emit-parse-success-hits non-negative");
    CHECK(mismatch_prevented(cs) >= 0, "roundtrip-mismatch-prevented non-negative");
    CHECK(dirty_reemit(cs) >= 0, "dirty-reemit-hits non-negative");
    CHECK(tool_compatible(cs) >= 0, "commercial-tool-compatible-hits non-negative");

    std::println("\n--- AC2: emit-parse-success-hits bumps on direct path ---");
    const auto p0 = emit_parse_success(cs);
    cs.evaluator().bump_sv_commercial_emit_parse_success(2);
    CHECK(emit_parse_success(cs) == p0 + 2, "emit-parse-success-hits bumps by 2");

    std::println("\n--- AC3: roundtrip-mismatch-prevented bumps on direct path ---");
    const auto m0 = mismatch_prevented(cs);
    cs.evaluator().bump_sv_commercial_emit_roundtrip_mismatch_prevented();
    CHECK(mismatch_prevented(cs) == m0 + 1, "roundtrip-mismatch-prevented bumps by 1");

    std::println("\n--- AC4: dirty-reemit-hits bumps on direct path ---");
    const auto d0 = dirty_reemit(cs);
    cs.evaluator().bump_sv_commercial_emit_dirty_reemit(2);
    CHECK(dirty_reemit(cs) == d0 + 2, "dirty-reemit-hits bumps by 2");

    std::println("\n--- AC5: commercial-tool-compatible-hits bumps on direct path ---");
    const auto t0 = tool_compatible(cs);
    cs.evaluator().bump_sv_commercial_emit_tool_compatible(3);
    CHECK(tool_compatible(cs) == t0 + 3, "commercial-tool-compatible-hits bumps by 3");

    run_production_ac6(cs, ws);

    std::println("\n--- AC7: aggregate counters monotonic after bump matrix ---");
    const auto agg7a =
        emit_parse_success(cs) + mismatch_prevented(cs) + dirty_reemit(cs) + tool_compatible(cs);
    cs.evaluator().bump_sv_commercial_emit_parse_success();
    cs.evaluator().bump_sv_commercial_emit_roundtrip_mismatch_prevented();
    cs.evaluator().bump_sv_commercial_emit_dirty_reemit();
    cs.evaluator().bump_sv_commercial_emit_tool_compatible();
    const auto agg7b =
        emit_parse_success(cs) + mismatch_prevented(cs) + dirty_reemit(cs) + tool_compatible(cs);
    CHECK(agg7b >= agg7a + 4, "aggregate commercial emit counters monotonic");

    std::println("\n--- AC8: query regression ---");
    auto structure748 = cs.eval("(engine:metrics \"query:sv-verification-structure-stats\")");
    auto hw698 = cs.eval("(engine:metrics \"query:hardware-backend-sv-closedloop-stats\")");
    CHECK(structure748 && is_hash(*structure748),
          "sv-verification-structure-stats regression (#748)");
    CHECK(hw698 && is_hash(*hw698), "hardware-backend-sv-closedloop-stats regression (#698)");
}

} // namespace aura_issue_801_detail

int aura_issue_sv_verification_commercial_emit_fidelity_run() {
    aura::compiler::CompilerService cs;
    aura_issue_801_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_sv_verification_commercial_emit_fidelity_run();
}
#endif
