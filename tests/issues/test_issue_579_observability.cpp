// @category: integration
// @reason: Issue #579 — verification-feedback-loop-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_579_detail {
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:verification-feedback-loop-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_sv_workspace(aura::compiler::CompilerService& cs) {
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
    return true;
}

} // namespace aura_issue_579_detail

int main() {
    using namespace aura_issue_579_detail;

    std::println("=== Issue #579: verification-feedback-loop-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_sv_workspace(cs), "SV workspace setup + netlist parse");

    // AC1: query:verification-feedback-loop-stats returns hash
    {
        std::println("\n--- AC1: query:verification-feedback-loop-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:verification-feedback-loop-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:verification-feedback-loop-stats returns hash");
        CHECK(hash_int(cs, "feedback-cycles") >= 0, "feedback-cycles present");
        CHECK(hash_int(cs, "mutate-success-on-feedback") >= 0,
              "mutate-success-on-feedback present");
        CHECK(hash_int(cs, "coverage-improvement-delta") >= 0,
              "coverage-improvement-delta present");
        CHECK(hash_int(cs, "assert-fail-resolved") >= 0, "assert-fail-resolved present");
        CHECK(hash_int(cs, "cex-addressed") >= 0, "cex-addressed present");
        CHECK(hash_int(cs, "verification-feedback-schema") == 579,
              "verification-feedback-schema == 579");
        CHECK(hash_int(cs, "verification-feedback-loop-total") >= 0,
              "verification-feedback-loop-total present");
        CHECK(hash_int(cs, "verification-feedback-loop-recommendation") >= 0,
              "verification-feedback-loop-recommendation present");
    }

    const auto cycles_before = hash_int(cs, "feedback-cycles");
    const auto success_before = hash_int(cs, "mutate-success-on-feedback");
    const auto cov_delta_before = hash_int(cs, "coverage-improvement-delta");
    const auto total_before = hash_int(cs, "verification-feedback-loop-total");

    // AC2: eda:run-verification-feedback bumps closed-loop counters
    {
        std::println("\n--- AC2: eda:run-verification-feedback ---");
        aura::ast::NodeId cover_id = aura::ast::NULL_NODE;
        if (auto* ws = cs.workspace_flat()) {
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                if (ws->get(id).tag == aura::ast::NodeTag::Coverpoint) {
                    cover_id = id;
                    break;
                }
            }
        }
        CHECK(cover_id != aura::ast::NULL_NODE, "coverpoint node found for feedback");
        auto r =
            cs.eval(std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole_a\")",
                                static_cast<int>(cover_id)));
        CHECK(r && aura::compiler::types::is_bool(*r) && aura::compiler::types::as_bool(*r),
              "eda:run-verification-feedback coverage path succeeds");
        const auto cycles_after = hash_int(cs, "feedback-cycles");
        const auto success_after = hash_int(cs, "mutate-success-on-feedback");
        const auto cov_delta_after = hash_int(cs, "coverage-improvement-delta");
        CHECK(cycles_after > cycles_before,
              std::format("feedback-cycles grew ({} -> {})", cycles_before, cycles_after));
        CHECK(success_after > success_before,
              std::format("mutate-success-on-feedback grew ({} -> {})", success_before,
                          success_after));
        CHECK(cov_delta_after >= cov_delta_before,
              std::format("coverage-improvement-delta non-decreasing ({} -> {})", cov_delta_before,
                          cov_delta_after));
    }

    // AC3: assert feedback + mutate cycle
    {
        std::println("\n--- AC3: assert feedback + mutate cycle ---");
        aura::ast::NodeId prop_id = aura::ast::NULL_NODE;
        if (auto* ws = cs.workspace_flat()) {
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                if (ws->get(id).tag == aura::ast::NodeTag::Property) {
                    prop_id = id;
                    break;
                }
            }
        }
        if (prop_id != aura::ast::NULL_NODE) {
            (void)cs.eval(
                std::format("(eda:run-verification-feedback \"assert-fail.log\" \"{} fail_prop\")",
                            static_cast<int>(prop_id)));
        }
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"prop\")");
        const auto total_after = hash_int(cs, "verification-feedback-loop-total");
        CHECK(total_after >= total_before,
              std::format("verification-feedback-loop-total monotonic ({} -> {})", total_before,
                          total_after));
        CHECK(hash_int(cs, "assert-fail-resolved") >= 0,
              "assert-fail-resolved readable after assert feedback");
    }

    // AC4: related SV verification primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto prod = cs.eval("(engine:metrics \"query:sv-production-verification-stats\")");
        auto structured = cs.eval("(engine:metrics \"query:sv-structured-edsl-stats\")");
        auto closed = cs.eval("(engine:metrics \"query:sv-verification-closedloop-stats-hash\")");
        auto stress = cs.eval("(engine:metrics \"query:eda-sv-closedloop-stress-stats\")");
        CHECK(prod && aura::compiler::types::is_hash(*prod),
              "query:sv-production-verification-stats hash regression (#539)");
        CHECK(structured && aura::compiler::types::is_hash(*structured),
              "query:sv-structured-edsl-stats hash regression (#578)");
        CHECK(closed && aura::compiler::types::is_hash(*closed),
              "query:sv-verification-closedloop-stats-hash regression (#630)");
        CHECK(stress && aura::compiler::types::is_hash(*stress),
              "query:eda-sv-closedloop-stress-stats hash regression (#695)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 144,
              "stats:count >= 144");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}