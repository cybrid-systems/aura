// @category: integration
// @reason: Issue #578 — sv-structured-edsl-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_578_detail {
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
        std::format("(hash-ref (engine:metrics \"query:sv-structured-edsl-stats\") '{}')", key));
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

} // namespace aura_issue_578_detail

int main() {
    using namespace aura_issue_578_detail;

    std::println("=== Issue #578: sv-structured-edsl-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_sv_workspace(cs), "SV workspace setup + netlist parse");

    // AC1: query:sv-structured-edsl-stats returns hash
    {
        std::println("\n--- AC1: query:sv-structured-edsl-stats ---");
        auto stats = cs.eval("(query:sv-structured-edsl-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:sv-structured-edsl-stats returns hash");
        CHECK(hash_int(cs, "sv-struct-pattern-hits") >= 0, "sv-struct-pattern-hits present");
        CHECK(hash_int(cs, "sv-struct-pattern-misses") >= 0, "sv-struct-pattern-misses present");
        CHECK(hash_int(cs, "sv-struct-hit-rate-pct") >= 0, "sv-struct-hit-rate-pct present");
        CHECK(hash_int(cs, "coverpoint-mutate-success") >= 0, "coverpoint-mutate-success present");
        CHECK(hash_int(cs, "property-weaken-count") >= 0, "property-weaken-count present");
        CHECK(hash_int(cs, "dirty-verification-propagated") >= 0,
              "dirty-verification-propagated present");
        CHECK(hash_int(cs, "mark-dirty-upward-calls") >= 0, "mark-dirty-upward-calls present");
        CHECK(hash_int(cs, "verify-assertion-dirty-total") >= 0,
              "verify-assertion-dirty-total present");
        CHECK(hash_int(cs, "verify-coverage-dirty-total") >= 0,
              "verify-coverage-dirty-total present");
        CHECK(hash_int(cs, "verify-sva-dirty-total") >= 0, "verify-sva-dirty-total present");
        CHECK(hash_int(cs, "emit-compatibility-checks") >= 0, "emit-compatibility-checks present");
        CHECK(hash_int(cs, "emit-parse-fail-count") >= 0, "emit-parse-fail-count present");
        CHECK(hash_int(cs, "hardware-hook-calls") >= 0, "hardware-hook-calls present");
        CHECK(hash_int(cs, "structured-mutate-hits") >= 0, "structured-mutate-hits present");
        CHECK(hash_int(cs, "eda-sv-review-schema") == 578, "eda-sv-review-schema == 578");
        CHECK(hash_int(cs, "sv-structured-edsl-total") >= 0, "sv-structured-edsl-total present");
        CHECK(hash_int(cs, "sv-structured-edsl-recommendation") >= 0,
              "sv-structured-edsl-recommendation present");
    }

    const auto pattern_before = hash_int(cs, "sv-struct-pattern-hits");
    const auto total_before = hash_int(cs, "sv-structured-edsl-total");

    // AC2: query:pattern + verification feedback
    {
        std::println("\n--- AC2: query:pattern + verification feedback ---");
        (void)cs.eval("(query:pattern \"prop\")");
        const auto pattern_after = hash_int(cs, "sv-struct-pattern-hits");
        CHECK(pattern_after >= pattern_before,
              std::format("sv-struct-pattern-hits non-decreasing ({} -> {})", pattern_before,
                          pattern_after));
        aura::ast::NodeId cover_id = aura::ast::NULL_NODE;
        if (auto* ws = cs.workspace_flat()) {
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                if (ws->get(id).tag == aura::ast::NodeTag::Coverpoint) {
                    cover_id = id;
                    break;
                }
            }
        }
        if (cover_id != aura::ast::NULL_NODE) {
            (void)cs.eval(
                std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                            static_cast<int>(cover_id)));
        }
        CHECK(hash_int(cs, "dirty-verification-propagated") >= 0,
              "dirty-verification-propagated readable after feedback");
    }

    // AC3: mutate + second pattern cycle
    {
        std::println("\n--- AC3: mutate + pattern cycle ---");
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"coverpoint\")");
        const auto total_after = hash_int(cs, "sv-structured-edsl-total");
        CHECK(total_after >= total_before,
              std::format("sv-structured-edsl-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related SV/EDA primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto sva = cs.eval("(query:sv-sva-structure-stats)");
        auto prod = cs.eval("(query:sv-production-verification-stats)");
        auto pattern = cs.eval("(query:pattern-sv-verification-stats)");
        auto closed = cs.eval("(query:sv-verification-closedloop-stats-hash)");
        CHECK(sva && aura::compiler::types::is_hash(*sva),
              "query:sv-sva-structure-stats hash regression (#694)");
        CHECK(prod && aura::compiler::types::is_hash(*prod),
              "query:sv-production-verification-stats hash regression (#539)");
        CHECK(pattern && aura::compiler::types::is_hash(*pattern),
              "query:pattern-sv-verification-stats hash regression (#541)");
        CHECK(closed && aura::compiler::types::is_hash(*closed),
              "query:sv-verification-closedloop-stats-hash regression (#630)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 143,
              "stats:count >= 143");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}