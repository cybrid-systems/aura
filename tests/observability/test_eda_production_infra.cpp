// test_eda_production_infra.cpp — Issue #841:
// EDA production infrastructure observability + co-sim bridge
// (refines #499/#616; non-duplicative with eda-foundation/hw-stats).
//
//   - AC1:  query:eda-infra-stats reachable (schema 841)
//   - AC2:  parse-success-hits bumps on direct path
//   - AC3:  structured-mutate-hits bumps on direct path
//   - AC4:  feedback-ingest-hits bumps on direct path
//   - AC5:  cosim-invoke-hits bumps on direct path
//   - AC6:  EDA primitives exercise production bump paths
//   - AC7:  aggregate counters monotonic after bump matrix
//   - AC8:  query regression (#499 foundation, #616 hw-stats)

#include "test_harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace aura_issue_841_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:eda-infra-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t parse_success(CompilerService& cs) {
    return stat_int(cs, "parse-success-hits");
}
static std::int64_t structured_mutate(CompilerService& cs) {
    return stat_int(cs, "structured-mutate-hits");
}
static std::int64_t feedback_ingest(CompilerService& cs) {
    return stat_int(cs, "feedback-ingest-hits");
}
static std::int64_t cosim_invoke(CompilerService& cs) {
    return stat_int(cs, "cosim-invoke-hits");
}

static std::string repo_relative_fixture(std::string_view rel) {
    if (auto* env = std::getenv("AURA_SRC_ROOT")) {
        if (env[0] != '\0')
            return (std::filesystem::path(env) / rel).string();
    }
    std::filesystem::path cwd = std::filesystem::current_path();
    for (int depth = 0; depth < 6; ++depth) {
        const auto candidate = cwd / rel;
        if (std::filesystem::exists(candidate))
            return candidate.string();
        if (!cwd.has_parent_path() || cwd == cwd.parent_path())
            break;
        cwd = cwd.parent_path();
    }
    return std::string(rel);
}

static aura::ast::NodeId find_interface(CompilerService& cs) {
    auto* ws = cs.workspace_flat();
    if (!ws)
        return aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->get(id).tag == aura::ast::NodeTag::Interface)
            return id;
    }
    return aura::ast::NULL_NODE;
}

static void run_production_ac6(CompilerService& cs) {
    std::println("\n--- AC6: EDA primitives production bump paths ---");
    const auto parse6a = parse_success(cs);
    CHECK(cs.eval("(eda:parse-netlist "
                  "\"interface:Bus\\nmodport:master:clk,data\\nproperty:p_req:req\")")
              .has_value(),
          "eda:parse-netlist succeeds");
    CHECK(parse_success(cs) > parse6a, "parse-success-hits grew after parse-netlist");

    const auto iface = find_interface(cs);
    CHECK(iface != aura::ast::NULL_NODE, "Interface node present");
    const auto mutate6a = structured_mutate(cs);
    auto upd = cs.eval(std::format("(eda:mutate-add-instance {} \"slave\" \"ready,valid\")",
                                   static_cast<int>(iface)));
    CHECK(upd && is_bool(*upd) && as_bool(*upd), "eda:mutate-add-instance succeeds");
    CHECK(structured_mutate(cs) > mutate6a,
          "structured-mutate-hits grew after mutate-add-instance");

    const auto feedback6a = feedback_ingest(cs);
    auto fb = cs.eval(std::format("(eda:run-hardware-feedback {})", static_cast<int>(iface)));
    CHECK(fb && is_bool(*fb) && as_bool(*fb), "eda:run-hardware-feedback succeeds");
    CHECK(feedback_ingest(cs) > feedback6a,
          "feedback-ingest-hits grew after run-hardware-feedback");

    const auto cosim6a = cosim_invoke(cs);
    const std::string fixture = repo_relative_fixture("tests/fixtures/issue_616/cov.json");
    auto ingest = cs.eval(std::format("(eda:ingest-result \"{}\")", fixture));
    CHECK(ingest && is_hash(*ingest), "eda:ingest-result returns hash");
    CHECK(cosim_invoke(cs) > cosim6a, "cosim-invoke-hits grew after ingest-result");

    const auto cosim6b = cosim_invoke(cs);
    auto invoke = cs.eval(std::format("(eda:invoke-simulator \"{}\")", fixture));
    CHECK(invoke && is_hash(*invoke), "eda:invoke-simulator returns hash");
    CHECK(cosim_invoke(cs) > cosim6b, "cosim-invoke-hits grew after invoke-simulator");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:eda-infra-stats (schema 841) ---");
    auto h = cs.eval("(engine:metrics \"query:eda-infra-stats\")");
    CHECK(h && is_hash(*h), "eda-infra-stats returns hash");
    CHECK(stat_int(cs, "schema") == 841, "schema == 841");
    CHECK(parse_success(cs) >= 0, "parse-success-hits non-negative");
    CHECK(structured_mutate(cs) >= 0, "structured-mutate-hits non-negative");
    CHECK(feedback_ingest(cs) >= 0, "feedback-ingest-hits non-negative");
    CHECK(cosim_invoke(cs) >= 0, "cosim-invoke-hits non-negative");

    std::println("\n--- AC2: parse-success-hits bumps on direct path ---");
    const auto p0 = parse_success(cs);
    cs.evaluator().bump_eda_infra_parse_success(2);
    CHECK(parse_success(cs) == p0 + 2, "parse-success-hits bumps by 2");

    std::println("\n--- AC3: structured-mutate-hits bumps on direct path ---");
    const auto m0 = structured_mutate(cs);
    cs.evaluator().bump_eda_infra_structured_mutate();
    CHECK(structured_mutate(cs) == m0 + 1, "structured-mutate-hits bumps by 1");

    std::println("\n--- AC4: feedback-ingest-hits bumps on direct path ---");
    const auto f0 = feedback_ingest(cs);
    cs.evaluator().bump_eda_infra_feedback_ingest(2);
    CHECK(feedback_ingest(cs) == f0 + 2, "feedback-ingest-hits bumps by 2");

    std::println("\n--- AC5: cosim-invoke-hits bumps on direct path ---");
    const auto c0 = cosim_invoke(cs);
    cs.evaluator().bump_eda_infra_cosim_invoke(3);
    CHECK(cosim_invoke(cs) == c0 + 3, "cosim-invoke-hits bumps by 3");

    CHECK(cs.eval("(set-code \"(define seed 1)\")").has_value(), "workspace setup");
    run_production_ac6(cs);

    std::println("\n--- AC7: aggregate counters monotonic after bump matrix ---");
    const auto agg7a =
        parse_success(cs) + structured_mutate(cs) + feedback_ingest(cs) + cosim_invoke(cs);
    cs.evaluator().bump_eda_infra_parse_success();
    cs.evaluator().bump_eda_infra_structured_mutate();
    cs.evaluator().bump_eda_infra_feedback_ingest();
    cs.evaluator().bump_eda_infra_cosim_invoke();
    const auto agg7b =
        parse_success(cs) + structured_mutate(cs) + feedback_ingest(cs) + cosim_invoke(cs);
    CHECK(agg7b >= agg7a + 4, "aggregate infra counters monotonic");

    std::println("\n--- AC8: query regression ---");
    auto foundation = cs.eval("(engine:metrics \"query:eda-foundation-stats\")");
    auto hw = cs.eval("(engine:metrics \"query:eda-hw-stats\")");
    CHECK(foundation && is_hash(*foundation), "eda-foundation-stats regression (#499)");
    CHECK(hw && is_hash(*hw), "eda-hw-stats regression (#616)");
}

} // namespace aura_issue_841_detail

int aura_issue_eda_production_infra_run() {
    aura::compiler::CompilerService cs;
    aura_issue_841_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_eda_production_infra_run();
}
#endif
