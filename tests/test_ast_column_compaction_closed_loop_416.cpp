// test_ast_column_compaction_closed_loop_416.cpp
// Issue #416: AST SoA column compaction strategy observability
// under heavy mutate + recycle/compact load.
//
// Non-duplicative with #405 (arena-compaction-stats ArenaGroup),
// #261 (ast:node-lifecycle-stats per-field hash), #414
// (generation-epoch-stats epoch slice).
//
// AC1: query:ast-column-compaction-stats reachable
// AC2: ast:recycle-nodes integration monotonic
// AC3: ast:node-lifecycle-stats EDSL integration
// AC4: ast:compact-nodes integration monotonic
// AC5: mutate + eval fragmentation path
// AC6: multi-round recycle/compact matrix monotonic
// AC7: query regression (arena-compaction-stats,
//      generation-epoch-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_416_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t column_compaction_stats(CompilerService& cs) {
    auto r = cs.eval("(query:ast-column-compaction-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(define extra 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:ast-column-compaction-stats ---");
    CHECK(setup_workspace(cs), "AST column compaction workspace setup");
    const auto s0 = column_compaction_stats(cs);
    std::println("  ast-column-compaction-stats = {}", s0);
    CHECK(s0 > 0, "column compaction stats positive after setup");

    std::println("\n--- AC2: ast:recycle-nodes integration ---");
    const auto stats2a = column_compaction_stats(cs);
    (void)cs.eval("(ast:recycle-nodes)");
    const auto stats2b = column_compaction_stats(cs);
    std::println("  column compaction stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "recycle monotonic for column stats");

    std::println("\n--- AC3: ast:node-lifecycle-stats integration ---");
    auto nls = cs.eval("(ast:node-lifecycle-stats)");
    CHECK(nls.has_value(), "ast:node-lifecycle-stats returns value");

    std::println("\n--- AC4: ast:compact-nodes integration ---");
    const auto stats4a = column_compaction_stats(cs);
    (void)cs.eval("(ast:compact-nodes)");
    const auto stats4b = column_compaction_stats(cs);
    std::println("  column compaction stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "compact monotonic for column stats");

    std::println("\n--- AC5: mutate + eval fragmentation path ---");
    const auto stats5a = column_compaction_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"42\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    const auto stats5b = column_compaction_stats(cs);
    std::println("  column compaction stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "mutate path monotonic for column stats");

    std::println("\n--- AC6: multi-round recycle/compact matrix ---");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace flat available for compaction counters");
    const auto cum6a = ws->node_recycle_total() + ws->node_compact_total();
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(ast:recycle-nodes)");
        (void)cs.eval("(ast:compact-nodes)");
    }
    const auto cum6b = ws->node_recycle_total() + ws->node_compact_total();
    std::println("  recycle+compact cumulative: {} -> {}", cum6a, cum6b);
    CHECK(cum6b >= cum6a, "cumulative compaction counters monotonic");
    auto vpr = cs.eval("(ast:validate-post-restore)");
    CHECK(vpr.has_value(), "validate-post-restore after compaction matrix");

    std::println("\n--- AC7: query regression ---");
    auto acs = cs.eval("(query:arena-compaction-stats)");
    auto ges = cs.eval("(query:generation-epoch-stats)");
    CHECK(acs && is_int(*acs), "arena-compaction-stats regression");
    CHECK(ges && is_int(*ges), "generation-epoch-stats regression");
}

} // namespace aura_416_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_416_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}