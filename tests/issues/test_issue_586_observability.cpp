// @category: integration
// @reason: Issue #586 — eda-primitives-stats infra registry slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_586_detail {
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
        std::format("(hash-ref (engine:metrics \"query:eda-primitives-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_sv_workspace(aura::compiler::CompilerService& cs, aura::ast::NodeId& interface_id,
                               aura::ast::NodeId& property_id, aura::ast::NodeId& coverpoint_id) {
    if (!cs.eval("(set-code \"(define base 10) (+ base 1)\")")) {
        return false;
    }
    if (!cs.eval("(eval-current)").has_value()) {
        return false;
    }
    const char* netlist = "interface:iface_a\n"
                          "property:prop_a:req ##1 ack\n"
                          "coverpoint:var_a:b0,b1\n";
    auto parsed = cs.eval(std::format("(eda:parse-netlist \"{}\")", netlist));
    if (!parsed || !aura::compiler::types::is_int(*parsed) ||
        aura::compiler::types::as_int(*parsed) < 3) {
        return false;
    }
    interface_id = aura::ast::NULL_NODE;
    property_id = aura::ast::NULL_NODE;
    coverpoint_id = aura::ast::NULL_NODE;
    if (auto* ws = cs.workspace_flat()) {
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (interface_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Interface)
                interface_id = id;
            if (property_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Property)
                property_id = id;
            if (coverpoint_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Coverpoint)
                coverpoint_id = id;
        }
    }
    return interface_id != aura::ast::NULL_NODE && property_id != aura::ast::NULL_NODE &&
           coverpoint_id != aura::ast::NULL_NODE;
}

} // namespace aura_issue_586_detail

int main() {
    using namespace aura_issue_586_detail;

    std::println("=== Issue #586: eda-primitives-stats infra registry ===");

    aura::compiler::CompilerService cs;
    aura::ast::NodeId interface_id = aura::ast::NULL_NODE;
    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    CHECK(setup_sv_workspace(cs, interface_id, property_id, coverpoint_id),
          "SV workspace setup + netlist parse");

    // AC1: query:eda-primitives-stats returns hash with #586 fields
    {
        std::println("\n--- AC1: query:eda-primitives-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:eda-primitives-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:eda-primitives-stats returns hash");
        CHECK(hash_int(cs, "eda-registered-count") > 0, "eda-registered-count > 0");
        CHECK(hash_int(cs, "foundation-parse-total") > 0, "foundation-parse-total > 0 after parse");
        CHECK(hash_int(cs, "foundation-mutate-total") >= 0, "foundation-mutate-total present");
        CHECK(hash_int(cs, "foundation-feedback-total") >= 0, "foundation-feedback-total present");
        CHECK(hash_int(cs, "sv-mutate-attempts") >= 0, "sv-mutate-attempts present");
        CHECK(hash_int(cs, "sv-mutate-success") >= 0, "sv-mutate-success present");
        CHECK(hash_int(cs, "verification-dirty-nodes") >= 0, "verification-dirty-nodes present");
        CHECK(hash_int(cs, "hardware-hook-calls") >= 0, "hardware-hook-calls present");
        CHECK(hash_int(cs, "closed-loop-rate-pct") >= 0, "closed-loop-rate-pct present");
        CHECK(hash_int(cs, "eda-primitives-schema") == 586, "eda-primitives-schema == 586");
        CHECK(hash_int(cs, "eda-primitives-total") > 0, "eda-primitives-total > 0");
        CHECK(hash_int(cs, "eda-primitives-recommendation") >= 0,
              "eda-primitives-recommendation present");
    }

    const auto parse_before = hash_int(cs, "foundation-parse-total");
    const auto sv_success_before = hash_int(cs, "sv-mutate-success");
    const auto feedback_before = hash_int(cs, "foundation-feedback-total");
    const auto hooks_before = hash_int(cs, "hardware-hook-calls");
    const auto total_before = hash_int(cs, "eda-primitives-total");

    // AC2: eda:query-nodes + eda:mutate-add-instance bump infra counters
    {
        std::println("\n--- AC2: query-nodes + mutate-add-instance ---");
        auto nodes = cs.eval("(eda:query-nodes \"property\")");
        CHECK(nodes && aura::compiler::types::is_int(*nodes) &&
                  aura::compiler::types::as_int(*nodes) >= 1,
              "eda:query-nodes property returns >= 1");
        auto add = cs.eval(std::format("(eda:mutate-add-instance {} \"u_dut\" \"ack,req\")",
                                       static_cast<int>(interface_id)));
        CHECK(add && aura::compiler::types::is_bool(*add) && aura::compiler::types::as_bool(*add),
              "eda:mutate-add-instance succeeds");
        const auto mutate_after = hash_int(cs, "foundation-mutate-total");
        CHECK(mutate_after > 0, std::format("foundation-mutate-total > 0 (got {})", mutate_after));
        CHECK(hash_int(cs, "foundation-parse-total") >= parse_before,
              "foundation-parse-total non-decreasing after mutate");
    }

    // AC3: verification feedback + weaken-property closed loop
    {
        std::println("\n--- AC3: feedback + weaken-property closed loop ---");
        auto feedback =
            cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                static_cast<int>(coverpoint_id)));
        CHECK(feedback && aura::compiler::types::is_bool(*feedback) &&
                  aura::compiler::types::as_bool(*feedback),
              "eda:run-verification-feedback succeeds");
        auto weaken = cs.eval(
            std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(property_id)));
        CHECK(weaken && aura::compiler::types::is_bool(*weaken) &&
                  aura::compiler::types::as_bool(*weaken),
              "eda:weaken-property succeeds");
        const auto sv_success_after = hash_int(cs, "sv-mutate-success");
        const auto feedback_after = hash_int(cs, "foundation-feedback-total");
        const auto hooks_after = hash_int(cs, "hardware-hook-calls");
        CHECK(
            sv_success_after > sv_success_before,
            std::format("sv-mutate-success grew ({} -> {})", sv_success_before, sv_success_after));
        CHECK(feedback_after >= feedback_before,
              std::format("foundation-feedback-total non-decreasing ({} -> {})", feedback_before,
                          feedback_after));
        CHECK(hooks_after >= hooks_before,
              std::format("hardware-hook-calls non-decreasing ({} -> {})", hooks_before,
                          hooks_after));
        const auto total_after = hash_int(cs, "eda-primitives-total");
        CHECK(total_after >= total_before,
              std::format("eda-primitives-total monotonic ({} -> {})", total_before, total_after));
        CHECK(hash_int(cs, "verification-dirty-nodes") > 0,
              "verification-dirty-nodes > 0 after closed loop");
    }

    // AC4: related EDA primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto foundation = cs.eval("(engine:metrics \"query:eda-foundation-stats\")");
        auto verification = cs.eval("(engine:metrics \"query:eda-verification-stats\")");
        auto structured = cs.eval("(engine:metrics \"query:sv-structured-edsl-stats\")");
        auto concurrency = cs.eval("(engine:metrics \"query:eda-concurrency-stats\")");
        CHECK(foundation && aura::compiler::types::is_hash(*foundation),
              "query:eda-foundation-stats hash regression (#499)");
        CHECK(verification && aura::compiler::types::is_hash(*verification),
              "query:eda-verification-stats hash regression (#510)");
        CHECK(structured && aura::compiler::types::is_hash(*structured),
              "query:sv-structured-edsl-stats hash regression (#578)");
        CHECK(concurrency && aura::compiler::types::is_hash(*concurrency),
              "query:eda-concurrency-stats hash regression (#582)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 151,
              "stats:count >= 151");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}