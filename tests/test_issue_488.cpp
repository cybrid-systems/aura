// @category: integration
// @reason: Issue #488 — post-mutate reflect validation + Guard impact snapshot

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_488_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:mutation-impact-snapshot) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) "
                 "(define base 10) (+ base 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_488_detail

int aura_issue_488_run() {
    using namespace aura_issue_488_detail;

    std::println("=== Issue #488: Reflect validation + Guard impact snapshot ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "macro workspace setup");

    const auto pass_before = cs.evaluator().get_schema_validation_pass_count();
    const auto fail_before = cs.evaluator().get_schema_validation_fail_count();
    const auto dirty_before = cs.evaluator().get_dirty_nodes_in_snapshot();

    // AC1: query:mutation-impact-snapshot hash fields
    {
        std::println("\n--- AC1: query:mutation-impact-snapshot ---");
        auto stats = cs.eval("(query:mutation-impact-snapshot)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:mutation-impact-snapshot returns hash");
        CHECK(snap_stat(cs, "epoch-after") >= 0, "epoch-after present");
        CHECK(snap_stat(cs, "epoch-delta") >= 0, "epoch-delta present");
        CHECK(snap_stat(cs, "nodes-changed") >= 0, "nodes-changed present");
        CHECK(snap_stat(cs, "dirty-nodes") >= 0, "dirty-nodes present");
        CHECK(snap_stat(cs, "macro-markers") >= 0, "macro-markers present");
        CHECK(snap_stat(cs, "schema-pass") >= 0, "schema-pass present");
        CHECK(snap_stat(cs, "schema-fail") >= 0, "schema-fail present");
    }

    // AC2: Guard mutate wires reflect validation + snapshot
    {
        std::println("\n--- AC2: post-mutate reflect validation on Guard success ---");
        CHECK(cs.eval("(mutate:rebind \"base\" \"42\")").has_value(), "mutate:rebind under Guard");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current after mutate");
        const auto pass_after = cs.evaluator().get_schema_validation_pass_count();
        const auto fail_after = cs.evaluator().get_schema_validation_fail_count();
        const auto dirty_after = cs.evaluator().get_dirty_nodes_in_snapshot();
        CHECK(pass_after > pass_before,
              std::format("schema_validation_pass grew ({} -> {})", pass_before, pass_after));
        CHECK(fail_after == fail_before,
              std::format("schema_validation_fail unchanged on healthy mutate ({} -> {})",
                          fail_before, fail_after));
        CHECK(
            dirty_after >= dirty_before,
            std::format("dirty_nodes_in_snapshot monotonic ({} -> {})", dirty_before, dirty_after));
    }

    const auto impact_before = cs.evaluator().get_mutation_impact_count();
    const auto epoch_before = snap_stat(cs, "epoch-after");

    // AC3: impact snapshot updates after second mutate
    {
        std::println("\n--- AC3: impact snapshot consumable by AI primitives ---");
        CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "second mutate under Guard");
        const auto impact_after = cs.evaluator().get_mutation_impact_count();
        const auto epoch_after = snap_stat(cs, "epoch-after");
        const auto nodes_changed = snap_stat(cs, "nodes-changed");
        CHECK(impact_after > impact_before,
              std::format("mutation_impact grew ({} -> {})", impact_before, impact_after));
        CHECK(epoch_after >= epoch_before,
              std::format("epoch-after monotonic ({} -> {})", epoch_before, epoch_after));
        CHECK(nodes_changed >= 0, "nodes-changed observable in snapshot hash");
    }

    // AC4: mutate-then-query self-evolution cycle
    {
        std::println("\n--- AC4: mutate-then-query closed loop ---");
        const auto selfmod_before = cs.eval("(query:reflection-selfmod-stats)");
        (void)cs.eval("(query:pattern \"base\")");
        (void)cs.eval("(mutate:rebind \"base\" \"100\")");
        (void)cs.eval("(query:mutation-impact-snapshot)");
        (void)cs.eval("(query:reflect-postmutate-stats)");
        const auto selfmod_after = cs.eval("(query:reflection-selfmod-stats)");
        CHECK(selfmod_before && aura::compiler::types::is_int(*selfmod_before),
              "reflection-selfmod-stats before cycle");
        CHECK(selfmod_after && aura::compiler::types::is_int(*selfmod_after),
              "reflection-selfmod-stats after cycle");
        if (selfmod_before && selfmod_after && aura::compiler::types::is_int(*selfmod_before) &&
            aura::compiler::types::is_int(*selfmod_after)) {
            CHECK(aura::compiler::types::as_int(*selfmod_after) >=
                      aura::compiler::types::as_int(*selfmod_before),
                  "reflection-selfmod-stats monotonic over cycle");
        }
    }

    // AC5: regression — existing reflect/Guard primitives
    {
        std::println("\n--- AC5: reflect/Guard regression ---");
        auto rps = cs.eval("(query:reflect-postmutate-stats)");
        auto mi = cs.eval("(query:mutation-impact)");
        auto mrs = cs.eval("(query:macro-reflect-self-evo-stats)");
        CHECK(rps && aura::compiler::types::is_hash(*rps), "reflect-postmutate-stats regression");
        CHECK(mi && aura::compiler::types::is_int(*mi), "mutation-impact regression");
        CHECK(mrs && aura::compiler::types::is_int(*mrs),
              "macro-reflect-self-evo-stats regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 86,
              "stats:count >= 86");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_488_run();
}
#endif
