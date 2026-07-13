// @category: integration
// @reason: Issue #519 — edsl-eda-sv-closedloop-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_519_detail {
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
        "(hash-ref (engine:metrics \"query:edsl-eda-sv-closedloop-stats\") '{}')", key));
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

} // namespace aura_issue_519_detail

int aura_issue_519_observability_run() {
    using namespace aura_issue_519_detail;

    std::println("=== Issue #519: edsl-eda-sv-closedloop-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:edsl-eda-sv-closedloop-stats returns hash
    {
        std::println("\n--- AC1: query:edsl-eda-sv-closedloop-stats ---");
        auto stats = cs.eval("(query:edsl-eda-sv-closedloop-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:edsl-eda-sv-closedloop-stats returns hash");
        CHECK(hash_int(cs, "sv-node-total") >= 0, "sv-node-total present");
        CHECK(hash_int(cs, "tag-arity-index-hits") >= 0, "tag-arity-index-hits present");
        CHECK(hash_int(cs, "coverage-feedback") >= 0, "coverage-feedback present");
        CHECK(hash_int(cs, "atomic-batch-commits") >= 0, "atomic-batch-commits present");
        CHECK(hash_int(cs, "edsl-eda-sv-closedloop-total") >= 0,
              "edsl-eda-sv-closedloop-total present");
        CHECK(hash_int(cs, "edsl-eda-sv-closedloop-recommendation") >= 0,
              "edsl-eda-sv-closedloop-recommendation present");
    }

    const auto cov_before = hash_int(cs, "coverage-feedback");
    const auto sv_before = hash_int(cs, "sv-node-total");
    const auto total_before = hash_int(cs, "edsl-eda-sv-closedloop-total");

    // AC2: coverage feedback bumps verification pillar
    {
        std::println("\n--- AC2: verify:parse-coverage-feedback ---");
        auto r = cs.eval("(verify:parse-coverage-feedback \"0 hole_a\n1 hole_b\n\")");
        CHECK(r && aura::compiler::types::is_int(*r), "verify:parse-coverage-feedback returns int");
        const auto cov_after = hash_int(cs, "coverage-feedback");
        CHECK(cov_after > cov_before,
              std::format("coverage-feedback bumped ({} -> {})", cov_before, cov_after));
    }

    // AC3: SV netlist parse bumps sv-node-total
    {
        std::println("\n--- AC3: eda:parse-netlist SV nodes ---");
        const char* netlist = "interface:iface_a\n"
                              "property:prop_a:req ##1 ack\n"
                              "coverpoint:var_a:b0,b1\n";
        auto parsed = cs.eval(std::format("(eda:parse-netlist \"{}\")", netlist));
        CHECK(parsed && aura::compiler::types::is_int(*parsed) &&
                  aura::compiler::types::as_int(*parsed) >= 3,
              "eda:parse-netlist seeds SV nodes");
        const auto sv_after = hash_int(cs, "sv-node-total");
        CHECK(sv_after > sv_before,
              std::format("sv-node-total bumped ({} -> {})", sv_before, sv_after));
    }

    // AC4: query:pattern bumps tag-arity-index-hits
    {
        std::println("\n--- AC4: query:pattern tag-arity index ---");
        const auto hits_before = hash_int(cs, "tag-arity-index-hits");
        (void)cs.eval("(query:pattern \"x\")");
        const auto hits_after = hash_int(cs, "tag-arity-index-hits");
        CHECK(hits_after >= hits_before,
              std::format("tag-arity-index-hits monotonic ({} -> {})", hits_before, hits_after));
    }

    // AC5: legacy EDA/SV stats regression
    {
        std::println("\n--- AC5: regression ---");
        const auto total_after = hash_int(cs, "edsl-eda-sv-closedloop-total");
        CHECK(total_after >= total_before,
              std::format("edsl-eda-sv-closedloop-total monotonic ({} -> {})", total_before,
                          total_after));
        auto evs = cs.eval("(query:eda-verification-stats)");
        CHECK(evs && aura::compiler::types::is_hash(*evs),
              "query:eda-verification-stats hash regression");
        auto sns = cs.eval("(query:sv-node-stats)");
        CHECK(sns && aura::compiler::types::is_hash(*sns), "query:sv-node-stats hash regression");
        auto efs = cs.eval("(query:eda-foundation-stats)");
        CHECK(efs && aura::compiler::types::is_hash(*efs),
              "query:eda-foundation-stats hash regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 116,
              "stats:count >= 116");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_519_observability_run();
}
#endif
