// @category: integration
// @reason: Issue #539 — sv-production-verification-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_539_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:sv-production-verification-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_sv_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1)\")").has_value()) {
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

} // namespace aura_issue_539_detail

int aura_issue_539_observability_run() {
    using namespace aura_issue_539_detail;

    std::println("=== Issue #539: sv-production-verification-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_sv_workspace(cs), "SV netlist workspace setup");

    // AC1: query:sv-production-verification-stats returns hash
    {
        std::println("\n--- AC1: query:sv-production-verification-stats ---");
        auto stats = cs.eval("(query:sv-production-verification-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:sv-production-verification-stats returns hash");
        CHECK(hash_int(cs, "feedback-mapped-count") >= 0, "feedback-mapped-count present");
        CHECK(hash_int(cs, "feedback-mutate-success") >= 0, "feedback-mutate-success present");
        CHECK(hash_int(cs, "structured-mutate-hits") >= 0, "structured-mutate-hits present");
        CHECK(hash_int(cs, "sv-mutate-attempts") >= 0, "sv-mutate-attempts present");
        CHECK(hash_int(cs, "sv-mutate-success") >= 0, "sv-mutate-success present");
        CHECK(hash_int(cs, "stable-ref-captures-in-sv") >= 0, "stable-ref-captures-in-sv present");
        CHECK(hash_int(cs, "dirty-propagated-nodes") >= 0, "dirty-propagated-nodes present");
        CHECK(hash_int(cs, "coverage-feedback-total") >= 0, "coverage-feedback-total present");
        CHECK(hash_int(cs, "assert-failure-total") >= 0, "assert-failure-total present");
        CHECK(hash_int(cs, "reverify-success") >= 0, "reverify-success present");
        CHECK(hash_int(cs, "verification-convergence") >= 0, "verification-convergence present");
        CHECK(hash_int(cs, "hardware-hook-calls") >= 0, "hardware-hook-calls present");
        CHECK(hash_int(cs, "commercial-reemits") >= 0, "commercial-reemits present");
        CHECK(hash_int(cs, "rollback-on-partial") >= 0, "rollback-on-partial present");
        CHECK(hash_int(cs, "feedback-success-rate-pct") >= 0, "feedback-success-rate-pct present");
        CHECK(hash_int(cs, "sv-production-verification-total") >= 0,
              "sv-production-verification-total present");
        CHECK(hash_int(cs, "sv-production-verification-recommendation") >= 0,
              "sv-production-verification-recommendation present");
    }

    const auto feedback_before = hash_int(cs, "feedback-mapped-count");
    const auto cov_before = hash_int(cs, "coverage-feedback-total");
    const auto total_before = hash_int(cs, "sv-production-verification-total");

    // AC2: verification feedback closed-loop bumps counters
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
        const auto feedback_after = hash_int(cs, "feedback-mapped-count");
        const auto cov_after = hash_int(cs, "coverage-feedback-total");
        CHECK(feedback_after > feedback_before, std::format("feedback-mapped-count grew ({} -> {})",
                                                            feedback_before, feedback_after));
        CHECK(cov_after >= cov_before,
              std::format("coverage-feedback-total non-decreasing ({} -> {})", cov_before,
                          cov_after));
    }

    // AC3: assert feedback + query cycle
    {
        std::println("\n--- AC3: assert feedback batch ---");
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
        (void)cs.eval("(query:pattern \"prop\")");
        const auto total_after = hash_int(cs, "sv-production-verification-total");
        CHECK(total_after >= total_before,
              std::format("sv-production-verification-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related SV verification primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto closedloop = cs.eval("(query:edsl-eda-sv-closedloop-stats)");
        auto sv_hash = cs.eval("(query:sv-verification-closedloop-stats-hash)");
        auto eda_ver = cs.eval("(query:eda-verification-stats)");
        CHECK(closedloop && aura::compiler::types::is_hash(*closedloop),
              "query:edsl-eda-sv-closedloop-stats hash regression");
        CHECK(sv_hash && aura::compiler::types::is_hash(*sv_hash),
              "query:sv-verification-closedloop-stats-hash hash regression");
        CHECK(eda_ver && aura::compiler::types::is_hash(*eda_ver),
              "query:eda-verification-stats hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 132,
              "stats:count >= 132");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_539_observability_run();
}
#endif
