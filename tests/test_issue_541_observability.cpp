// @category: integration
// @reason: Issue #541 — pattern-sv-verification-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_541_detail {
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
        "(hash-ref (engine:metrics \"query:pattern-sv-verification-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t result_count(aura::compiler::CompilerService& cs, const std::string& expr) {
    auto r = cs.eval("(length " + expr + ")");
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_sv_hygiene_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (define base 10) (+ base 1)\")")) {
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

} // namespace aura_issue_541_detail

int aura_issue_541_observability_run() {
    using namespace aura_issue_541_detail;

    std::println("=== Issue #541: pattern-sv-verification-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_sv_hygiene_workspace(cs), "SV + hygienic macro workspace setup");

    // AC1: query:pattern-sv-verification-stats returns hash
    {
        std::println("\n--- AC1: query:pattern-sv-verification-stats ---");
        auto stats = cs.eval("(query:pattern-sv-verification-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:pattern-sv-verification-stats returns hash");
        CHECK(hash_int(cs, "defuse-index-used") >= 0, "defuse-index-used present");
        CHECK(hash_int(cs, "defuse-index-visited") >= 0, "defuse-index-visited present");
        CHECK(hash_int(cs, "defuse-index-walk-fallback") >= 0,
              "defuse-index-walk-fallback present");
        CHECK(hash_int(cs, "defuse-version") >= 0, "defuse-version present");
        CHECK(hash_int(cs, "tag-arity-delta-hits") >= 0, "tag-arity-delta-hits present");
        CHECK(hash_int(cs, "tag-arity-dirty-marks") >= 0, "tag-arity-dirty-marks present");
        CHECK(hash_int(cs, "tag-arity-rebuild-time-us") >= 0, "tag-arity-rebuild-time-us present");
        CHECK(hash_int(cs, "structural-index-hits") >= 0, "structural-index-hits present");
        CHECK(hash_int(cs, "structural-index-misses") >= 0, "structural-index-misses present");
        CHECK(hash_int(cs, "hygiene-skips") >= 0, "hygiene-skips present");
        CHECK(hash_int(cs, "recursive-hygiene-skips") >= 0, "recursive-hygiene-skips present");
        CHECK(hash_int(cs, "hygiene-violations") >= 0, "hygiene-violations present");
        CHECK(hash_int(cs, "macro-marker-count") >= 0, "macro-marker-count present");
        CHECK(hash_int(cs, "sv-node-count") >= 3, "sv-node-count >= 3 after netlist parse");
        CHECK(hash_int(cs, "verification-dirty-count") >= 0, "verification-dirty-count present");
        CHECK(hash_int(cs, "incremental-hit-rate-pct") >= 0, "incremental-hit-rate-pct present");
        CHECK(hash_int(cs, "pattern-sv-verification-total") >= 0,
              "pattern-sv-verification-total present");
        CHECK(hash_int(cs, "pattern-sv-verification-recommendation") >= 0,
              "pattern-sv-verification-recommendation present");
    }

    const auto skips_before = hash_int(cs, "hygiene-skips");
    const auto total_before = hash_int(cs, "pattern-sv-verification-total");

    // AC2: query:pattern hygiene filter drives counters
    {
        std::println("\n--- AC2: query:pattern + :respect-hygiene ---");
        const auto default_cnt = result_count(cs, "(query:pattern \"*\")");
        const auto filtered_cnt = result_count(cs, "(query:pattern \"*\" :respect-hygiene #t)");
        CHECK(default_cnt >= 0 && filtered_cnt >= 0, "pattern match counts observable");
        const auto skips_after = hash_int(cs, "hygiene-skips");
        CHECK(skips_after >= skips_before,
              std::format("hygiene-skips non-decreasing ({} -> {})", skips_before, skips_after));
    }

    // AC3: mutate + verification feedback + pattern cycle
    {
        std::println("\n--- AC3: mutate + verification feedback ---");
        aura::ast::NodeId cover_id = aura::ast::NULL_NODE;
        if (auto* ws = cs.workspace_flat()) {
            for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
                if (ws->get(id).tag == aura::ast::NodeTag::Coverpoint) {
                    cover_id = id;
                    break;
                }
            }
        }
        CHECK(cover_id != aura::ast::NULL_NODE, "coverpoint node found");
        if (cover_id != aura::ast::NULL_NODE) {
            (void)cs.eval(
                std::format("(eda:run-verification-feedback \"coverage.log\" \"{} hole\")",
                            static_cast<int>(cover_id)));
        }
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
        (void)cs.eval("(query:pattern \"prop\")");
        const auto dirty_after = hash_int(cs, "verification-dirty-count");
        const auto total_after = hash_int(cs, "pattern-sv-verification-total");
        CHECK(dirty_after > 0,
              std::format("verification-dirty-count > 0 after feedback (got {})", dirty_after));
        CHECK(total_after >= total_before,
              std::format("pattern-sv-verification-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related pattern index/hygiene primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto prod = cs.eval("(query:pattern-production-index-stats)");
        auto pis = cs.eval("(engine:metrics \"query:pattern-index-stats\")");
        auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
        CHECK(prod && aura::compiler::types::is_hash(*prod),
              "query:pattern-production-index-stats hash regression");
        CHECK(pis && aura::compiler::types::is_int(*pis), "pattern-index-stats int regression");
        CHECK(phs && aura::compiler::types::is_int(*phs), "pattern-hygiene-stats int regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 134,
              "stats:count >= 134");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_541_observability_run();
}
#endif
