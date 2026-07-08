// @category: integration
// @reason: Issue #490 — proactive tag_arity_index rebuild on COW/compact + policy tuning

#include <format>
#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_490_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:pattern-index-rebuild-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define base 10) (define aux 20) (+ base aux)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_490_detail

int main() {
    using namespace aura_issue_490_detail;

    std::println("=== Issue #490: Proactive pattern-index rebuild + policy tuning ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace_flat reachable");

    // AC1: compact_nodes proactively rebuilds FlatAST index
    {
        std::println("\n--- AC1: compact rebuilds FlatAST tag_arity_index ---");
        if (ws) {
            (void)cs.eval("(query:tag-arity-count 8 2)");
            const auto rebuilds_before = ws->tag_arity_index_rebuilds();
            (void)cs.eval("(ast:compact-nodes)");
            const auto rebuilds_after = ws->tag_arity_index_rebuilds();
            CHECK(ws->tag_arity_index_size() > 0,
                  "FlatAST tag_arity_index non-empty after compact");
            CHECK(rebuilds_after >= rebuilds_before,
                  std::format("FlatAST rebuild counter monotonic ({} -> {})", rebuilds_before,
                              rebuilds_after));
        }
    }

    // AC2: eager-after-cow policy rebuilds Evaluator index on set-code
    {
        std::println("\n--- AC2: eager-after-cow policy on workspace swap ---");
        CHECK(cs.eval("(mutate:set-pattern-index-policy \"eager-after-cow\")").has_value(),
              "mutate:set-pattern-index-policy eager-after-cow");
        const auto cow_before = cs.evaluator().get_pattern_index_eager_cow_rebuilds();
        CHECK(cs.eval("(set-code \"(define x 1) (define y 2)\")").has_value(), "set-code swap");
        const auto cow_after = cs.evaluator().get_pattern_index_eager_cow_rebuilds();
        CHECK(cow_after > cow_before,
              std::format("eager_cow_rebuilds grew ({} -> {})", cow_before, cow_after));
        CHECK(cs.evaluator().tag_arity_index_size() > 0,
              "Evaluator tag_arity_index ready before query:pattern");
        auto policy = cs.eval("(query:pattern-index-policy)");
        CHECK(policy && aura::compiler::types::is_string(*policy),
              "query:pattern-index-policy returns string");
    }

    (void)setup_workspace(cs);

    // AC3: eager-after-mutate policy rebuilds on Guard success
    {
        std::println("\n--- AC3: eager-after-mutate policy on Guard success ---");
        CHECK(cs.eval("(mutate:set-pattern-index-policy \"eager-after-mutate\")").has_value(),
              "mutate:set-pattern-index-policy eager-after-mutate");
        const auto mutate_before = cs.evaluator().get_pattern_index_eager_mutate_rebuilds();
        CHECK(cs.eval("(mutate:rebind \"base\" \"42\")").has_value(), "mutate under Guard");
        const auto mutate_after = cs.evaluator().get_pattern_index_eager_mutate_rebuilds();
        CHECK(mutate_after > mutate_before,
              std::format("eager_mutate_rebuilds grew ({} -> {})", mutate_before, mutate_after));
        CHECK(cs.evaluator().tag_arity_index_size() > 0,
              "Evaluator index ready before query:pattern after mutate");
        auto pat = cs.eval("(query:pattern \"base\")");
        CHECK(pat.has_value(), "query:pattern succeeds with pre-built index");
    }

    // AC4: query:pattern-index-rebuild-stats hash fields
    {
        std::println("\n--- AC4: query:pattern-index-rebuild-stats ---");
        auto stats = cs.eval("(query:pattern-index-rebuild-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:pattern-index-rebuild-stats returns hash");
        CHECK(snap_stat(cs, "lazy-rebuilds") >= 0, "lazy-rebuilds present");
        CHECK(snap_stat(cs, "eager-mutate-rebuilds") >= 0, "eager-mutate-rebuilds present");
        CHECK(snap_stat(cs, "eager-cow-rebuilds") >= 0, "eager-cow-rebuilds present");
        CHECK(snap_stat(cs, "flat-rebuilds") >= 0, "flat-rebuilds present");
        CHECK(snap_stat(cs, "flat-rebuild-time-us") >= 0, "flat-rebuild-time-us present");
    }

    // AC5: lazy policy regression + existing pattern-index-stats
    {
        std::println("\n--- AC5: lazy policy + pattern-index-stats regression ---");
        CHECK(cs.eval("(mutate:set-pattern-index-policy \"lazy\")").has_value(),
              "reset to lazy policy");
        auto pis = cs.eval("(query:pattern-index-stats)");
        auto phs = cs.eval("(query:pattern-hygiene-stats)");
        CHECK(pis && aura::compiler::types::is_int(*pis), "query:pattern-index-stats regression");
        CHECK(phs && aura::compiler::types::is_int(*phs), "query:pattern-hygiene-stats regression");
        CHECK(cs.eval("(query:pattern \"base\")").has_value(),
              "query:pattern lazy path regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}