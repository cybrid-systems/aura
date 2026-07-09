// @category: integration
// @reason: Issue #510 — eda-verification-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_510_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:eda-verification-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_510_detail

int aura_issue_510_observability_run() {
    using namespace aura_issue_510_detail;

    std::println("=== Issue #510: eda-verification-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:eda-verification-stats returns hash
    {
        std::println("\n--- AC1: query:eda-verification-stats ---");
        auto stats = cs.eval("(query:eda-verification-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:eda-verification-stats returns hash");
        CHECK(hash_int(cs, "coverage-delta") >= 0, "coverage-delta present");
        CHECK(hash_int(cs, "assert-fail-count") >= 0, "assert-fail-count present");
        CHECK(hash_int(cs, "auto-mutate-from-feedback") >= 0, "auto-mutate-from-feedback present");
        CHECK(hash_int(cs, "eda-verification-total") >= 0, "eda-verification-total present");
        CHECK(hash_int(cs, "eda-verification-recommendation") >= 0,
              "eda-verification-recommendation present");
    }

    const auto cov_before = hash_int(cs, "coverage-delta");
    const auto assert_before = hash_int(cs, "assert-fail-count");
    const auto auto_before = hash_int(cs, "auto-mutate-from-feedback");
    const auto total_before = hash_int(cs, "eda-verification-total");

    // AC2: coverage feedback bumps coverage-delta
    {
        std::println("\n--- AC2: verify:parse-coverage-feedback ---");
        auto r = cs.eval("(verify:parse-coverage-feedback \"0 hole_a\n2 hole_b\n\")");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 2,
              "verify:parse-coverage-feedback marks 2 nodes");
        const auto cov_after = hash_int(cs, "coverage-delta");
        CHECK(cov_after > cov_before,
              std::format("coverage-delta bumped ({} -> {})", cov_before, cov_after));
    }

    // AC3: assert failure bumps assert-fail-count
    {
        std::println("\n--- AC3: verify:parse-assert-failure ---");
        auto r = cs.eval("(verify:parse-assert-failure \"1 fail_msg\")");
        CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 1,
              "verify:parse-assert-failure marks 1 node");
        const auto assert_after = hash_int(cs, "assert-fail-count");
        CHECK(assert_after > assert_before,
              std::format("assert-fail-count bumped ({} -> {})", assert_before, assert_after));
    }

    // AC4: EDA feedback closed-loop bumps auto-mutate-from-feedback
    {
        std::println("\n--- AC4: eda:run-verification-feedback ---");
        const char* netlist = "interface:iface_a\n"
                              "property:prop_a:req ##1 ack\n"
                              "coverpoint:var_a:b0,b1\n";
        auto parsed = cs.eval(std::format("(eda:parse-netlist \"{}\")", netlist));
        CHECK(parsed && aura::compiler::types::is_int(*parsed) &&
                  aura::compiler::types::as_int(*parsed) >= 3,
              "eda:parse-netlist seeds SV nodes");
        auto* ws = cs.workspace_flat();
        aura::ast::NodeId cover_id = aura::ast::NULL_NODE;
        if (ws) {
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                if (ws->get(id).tag == aura::ast::NodeTag::Coverpoint) {
                    cover_id = id;
                    break;
                }
            }
        }
        CHECK(cover_id != aura::ast::NULL_NODE, "coverpoint node found");
        if (cover_id != aura::ast::NULL_NODE) {
            auto fb = cs.eval(
                std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole_a\")",
                            static_cast<int>(cover_id)));
            CHECK(fb && aura::compiler::types::is_bool(*fb) && aura::compiler::types::as_bool(*fb),
                  "eda:run-verification-feedback returns #t");
            const auto auto_after = hash_int(cs, "auto-mutate-from-feedback");
            CHECK(auto_after > auto_before,
                  std::format("auto-mutate-from-feedback bumped ({} -> {})", auto_before,
                              auto_after));
        }
    }

    // AC5: eda-verification-total monotonic + verification-loop-stats regression
    {
        std::println("\n--- AC5: regression ---");
        const auto total_after = hash_int(cs, "eda-verification-total");
        CHECK(total_after > total_before,
              std::format("eda-verification-total bumped ({} -> {})", total_before, total_after));
        auto vls = cs.eval("(query:verification-loop-stats)");
        CHECK(vls && aura::compiler::types::is_int(*vls),
              "query:verification-loop-stats int regression");
        auto hw = cs.eval("(query:hardware-backend-commercial-stats)");
        CHECK(hw && aura::compiler::types::is_hash(*hw),
              "query:hardware-backend-commercial-stats hash regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 105,
              "stats:count >= 105");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_510_observability_run();
}
#endif
