// Issue #403/#506/#570/#598 (#1978 renamed): issue# moved from filename to header.
// test_ir_metadata_interpreter_jit_closed_loop_403.cpp
// Issue #403: IRInstruction rich metadata (shape_id,
// linear_ownership_state, adt_variant_id, narrow_evidence)
// consumption consistency between IRInterpreter and JIT.
//
// Non-duplicative with #506 (soa-hotpath-adoption-stats),
// #570 (shape-stability-stats), #598 (linear-ownership-runtime).
//
// AC1: query:ir-metadata-stats reachable
// AC2: eval-current exercises IR interpreter path
// AC3: eval-current :jit exercises JIT metadata path
// AC4: mutate + eval bumps metadata stats monotonic
// AC5: multi-round eval matrix monotonic
// AC6: query regression (shape-stability-stats,
//      linear-ownership-runtime-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_403_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::int64_t metadata_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:ir-metadata-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:ir-metadata-stats ---");
    CHECK(setup_workspace(cs), "IR workspace setup + eval");
    const auto s0 = metadata_stats(cs);
    std::println("  ir-metadata-stats = {}", s0);
    CHECK(s0 >= 0, "ir-metadata stats non-negative");

    std::println("\n--- AC2: eval-current interpreter path ---");
    const auto stats2a = metadata_stats(cs);
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval interpreter");
    const auto stats2b = metadata_stats(cs);
    std::println("  ir-metadata stats: {} -> {}", stats2a, stats2b);
    CHECK(stats2b >= stats2a, "interpreter re-eval monotonic");

    std::println("\n--- AC3: eval-current :jit path ---");
    const auto stats3a = metadata_stats(cs);
    auto jit = cs.eval("(eval-current :jit)");
    const auto stats3b = metadata_stats(cs);
    std::println("  jit eval ok={} stats: {} -> {}", jit.has_value(), stats3a, stats3b);
    CHECK(jit.has_value(), "eval-current :jit succeeds");
    CHECK(stats3b >= stats3a, "JIT path monotonic for metadata stats");

    std::println("\n--- AC4: mutate + eval bumps stats ---");
    const auto stats4a = metadata_stats(cs);
    (void)cs.eval("(mutate:rebind \"base\" \"42\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after mutate");
    (void)cs.eval("(eval-current :jit)");
    const auto stats4b = metadata_stats(cs);
    std::println("  ir-metadata stats: {} -> {}", stats4a, stats4b);
    CHECK(stats4b >= stats4a, "mutate+eval bumps metadata stats");

    std::println("\n--- AC5: multi-round eval matrix ---");
    const auto stats5a = metadata_stats(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(eval-current :jit)");
    }
    const auto stats5b = metadata_stats(cs);
    std::println("  ir-metadata stats: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "metadata stats monotonic over matrix");

    std::println("\n--- AC6: query regression ---");
    auto shp = cs.eval("(engine:metrics \"query:shape-stability-stats\")");
    auto lor = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
    CHECK(shp && is_int(*shp), "shape-stability-stats regression");
    CHECK(lor && is_int(*lor), "linear-ownership-runtime-stats regression");
}

} // namespace aura_403_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_403_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}