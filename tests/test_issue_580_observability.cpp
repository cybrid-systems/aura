// @category: integration
// @reason: Issue #580 — hardware-backend-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_580_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:hardware-backend-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_sv_workspace(aura::compiler::CompilerService& cs, aura::ast::NodeId& property_id,
                               aura::ast::NodeId& coverpoint_id) {
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
    property_id = aura::ast::NULL_NODE;
    coverpoint_id = aura::ast::NULL_NODE;
    if (auto* ws = cs.workspace_flat()) {
        for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
            if (property_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Property)
                property_id = id;
            if (coverpoint_id == aura::ast::NULL_NODE &&
                ws->get(id).tag == aura::ast::NodeTag::Coverpoint)
                coverpoint_id = id;
        }
    }
    return property_id != aura::ast::NULL_NODE && coverpoint_id != aura::ast::NULL_NODE;
}

} // namespace aura_issue_580_detail

int main() {
    using namespace aura_issue_580_detail;

    std::println("=== Issue #580: hardware-backend-stats hash ===");

    aura::compiler::CompilerService cs;
    aura::ast::NodeId property_id = aura::ast::NULL_NODE;
    aura::ast::NodeId coverpoint_id = aura::ast::NULL_NODE;
    CHECK(setup_sv_workspace(cs, property_id, coverpoint_id), "SV workspace setup + netlist parse");

    // AC1: query:hardware-backend-stats returns hash
    {
        std::println("\n--- AC1: query:hardware-backend-stats ---");
        auto stats = cs.eval("(query:hardware-backend-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:hardware-backend-stats returns hash");
        CHECK(hash_int(cs, "emit-compatibility-pass-rate") >= 0,
              "emit-compatibility-pass-rate present");
        CHECK(hash_int(cs, "ppa-refresh-count") >= 0, "ppa-refresh-count present");
        CHECK(hash_int(cs, "incremental-emit-win") >= 0, "incremental-emit-win present");
        CHECK(hash_int(cs, "simulator-parse-success") >= 0, "simulator-parse-success present");
        CHECK(hash_int(cs, "hardware-backend-schema") == 580, "hardware-backend-schema == 580");
        CHECK(hash_int(cs, "hardware-backend-total") >= 0, "hardware-backend-total present");
        CHECK(hash_int(cs, "hardware-backend-recommendation") >= 0,
              "hardware-backend-recommendation present");
    }

    const auto ppa_before = hash_int(cs, "ppa-refresh-count");
    const auto sim_before = hash_int(cs, "simulator-parse-success");
    const auto pass_before = hash_int(cs, "emit-compatibility-pass-rate");
    const auto total_before = hash_int(cs, "hardware-backend-total");

    // AC2: commercial simulator stub + weaken-property bump backend counters
    {
        std::println("\n--- AC2: commercial emit + simulator stub ---");
        auto weaken = cs.eval(
            std::format("(eda:weaken-property {} \"reset\")", static_cast<int>(property_id)));
        CHECK(weaken && aura::compiler::types::is_bool(*weaken) &&
                  aura::compiler::types::as_bool(*weaken),
              "eda:weaken-property succeeds");
        auto stub = cs.eval(std::format("(eda:run-commercial-simulator-stub \"vcs\" {})",
                                        static_cast<int>(property_id)));
        CHECK(stub && aura::compiler::types::is_bool(*stub) &&
                  aura::compiler::types::as_bool(*stub),
              "eda:run-commercial-simulator-stub vcs succeeds");
        const auto ppa_after = hash_int(cs, "ppa-refresh-count");
        const auto sim_after = hash_int(cs, "simulator-parse-success");
        const auto pass_after = hash_int(cs, "emit-compatibility-pass-rate");
        CHECK(ppa_after > ppa_before,
              std::format("ppa-refresh-count grew ({} -> {})", ppa_before, ppa_after));
        CHECK(sim_after > sim_before,
              std::format("simulator-parse-success grew ({} -> {})", sim_before, sim_after));
        CHECK(pass_after >= pass_before,
              std::format("emit-compatibility-pass-rate non-decreasing ({} -> {})", pass_before,
                          pass_after));
    }

    // AC3: verification feedback closed-loop re-emit
    {
        std::println("\n--- AC3: eda:run-verification-feedback ---");
        auto r = cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                                     static_cast<int>(coverpoint_id)));
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "eda:run-verification-feedback coverage succeeds");
        const auto total_after = hash_int(cs, "hardware-backend-total");
        CHECK(
            total_after >= total_before,
            std::format("hardware-backend-total monotonic ({} -> {})", total_before, total_after));
        CHECK(hash_int(cs, "incremental-emit-win") >= 0,
              "incremental-emit-win readable after feedback re-emit");
    }

    // AC4: related hardware backend primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto commercial = cs.eval("(query:hardware-backend-commercial-stats)");
        auto closed = cs.eval("(query:hardware-backend-sv-closedloop-stats)");
        auto feedback = cs.eval("(query:verification-feedback-loop-stats)");
        auto structured = cs.eval("(query:sv-structured-edsl-stats)");
        CHECK(commercial && aura::compiler::types::is_hash(*commercial),
              "query:hardware-backend-commercial-stats hash regression (#698)");
        CHECK(closed && aura::compiler::types::is_hash(*closed),
              "query:hardware-backend-sv-closedloop-stats hash regression (#693)");
        CHECK(feedback && aura::compiler::types::is_hash(*feedback),
              "query:verification-feedback-loop-stats hash regression (#579)");
        CHECK(structured && aura::compiler::types::is_hash(*structured),
              "query:sv-structured-edsl-stats hash regression (#578)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 145,
              "stats:count >= 145");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}