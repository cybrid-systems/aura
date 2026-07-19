// tests/test_incremental_relower_perblock.cpp — Issue #1639
//
// AC list (per docs/design/1639-incremental-relower.md):
//   AC1: source cites #1639; per-block dirty bitmask (IRCacheEntry)
//        wires into relower_define_blocks partial path via the
//        existing run_incremental_dirty_pipeline(ir_mod, pass, mask_ptr)
//        call sequence (ConstantFolding / ComputeKind / TypePropagation
//        / Shape / EscapeAnalysis — 5 passes).
//   AC2: 5 new metric slots in observability_metrics.h
//        (full_relower_count + dirty_block_ratio_{numerator,denominator}_total
//         + relower_block_hit_rate_{numerator,denominator}_total).
//   AC3: 5 bump_/getter pairs declared in evaluator.ixx
//        (bump_full_relower_count / bump_dirty_block_ratio /
//         bump_relower_block_hit_rate + their get_ counterparts).
//   AC4: 5 wire-up sites in service.ixx::relower_define_blocks:
//        - relower_full_called_count path → bump_full_relower_count +
//          bump_relower_block_hit_rate(0, 1) [full-fallback attempt]
//        - partial success path (relower_define_function true) →
//          bump_relower_block_hit_rate(1, 1) [partial hit]
//        - entry lookup path → bump_dirty_block_ratio(dirty_blocks,
//          total_blocks_seen) [per-call ratio contribution]
//   AC5: query:incremental-relower-stats primitive extended with 6 new
//        keys (full-relower-count + dirty-block-ratio-numerator-total +
//        dirty-block-ratio-denominator-total +
//        relower-block-hit-rate-numerator-total +
//        relower-block-hit-rate-denominator-total + relower-block-hit-rate)
//        and schema bumped 1623 → 1639.
//   AC6: 5 X-macro fields in compiler_metrics_fields.inc.
//   AC7: cross-layer regression — CompilerService can be constructed
//        and a basic (set-code) + (eval-current) round-trip still works
//        after the wire-up.
//
// Pattern references: tests/test_issue_1638.cpp (9 ACs, source-driven),
// tests/test_incremental_relower.cpp (251 lines, 6 ACs covering #1605),
// tests/test_gc_roots_bridge_epoch_drift_1734.cpp (4 ACs, source-driven).

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1639_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& s, std::string_view needle) noexcept {
    return s.find(needle) != std::string::npos;
}

bool check_partial_path_pipeline_ac1() {
    std::println("\n--- AC1: per-block dirty bitmask wired into 5 local passes ---");
    std::string svc = read_file("src/compiler/service.ixx");
    bool wired = contains(svc, "run_incremental_dirty_pipeline(ir_mod, ck_pass") &&
                 contains(svc, "run_incremental_dirty_pipeline(ir_mod, cf_pass") &&
                 contains(svc, "run_incremental_dirty_pipeline(ir_mod, tp_pass") &&
                 contains(svc, "run_incremental_dirty_pipeline(ir_mod, shape_pass") &&
                 contains(svc, "run_incremental_dirty_pipeline(ir_mod, escape_pass") &&
                 contains(svc, "Issue #1639");
    if (!wired) {
        std::println("FAIL: per-block dirty pipeline not wired for all 5 passes");
        return false;
    }
    std::println("OK: per-block dirty bitmask wired into 5 local passes (ck/cf/tp/shape/escape)");
    return true;
}

bool check_metrics_ac2() {
    std::println("\n--- AC2: 5 new metric slots in observability_metrics.h ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    bool all = contains(om, "full_relower_count") &&
               contains(om, "dirty_block_ratio_numerator_total") &&
               contains(om, "dirty_block_ratio_denominator_total") &&
               contains(om, "relower_block_hit_rate_numerator_total") &&
               contains(om, "relower_block_hit_rate_denominator_total");
    if (!all) {
        std::println("FAIL: 5 metric slots missing");
        return false;
    }
    std::println("OK: 5 metric slots present");
    return true;
}

bool check_bump_getter_ac3() {
    std::println("\n--- AC3: 5 bump_/getter pairs in evaluator.ixx ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool all =
        contains(ixx, "bump_full_relower_count") && contains(ixx, "bump_dirty_block_ratio") &&
        contains(ixx, "bump_relower_block_hit_rate") && contains(ixx, "get_full_relower_count") &&
        contains(ixx, "get_dirty_block_ratio_numerator_total") &&
        contains(ixx, "get_dirty_block_ratio_denominator_total") &&
        contains(ixx, "get_relower_block_hit_rate_numerator_total") &&
        contains(ixx, "get_relower_block_hit_rate_denominator_total");
    if (!all) {
        std::println("FAIL: missing bump_/getter pair in evaluator.ixx");
        return false;
    }
    std::println("OK: 5 bump_/getter pairs declared");
    return true;
}

bool check_wires_ac4() {
    std::println("\n--- AC4: 5 wire-up sites in relower_define_blocks ---");
    std::string svc = read_file("src/compiler/service.ixx");
    bool full_bump = contains(svc, "evaluator_.bump_full_relower_count()");
    bool partial_bump = contains(svc, "evaluator_.bump_relower_block_hit_rate(1, 1)");
    bool full_hit_rate = contains(svc, "evaluator_.bump_relower_block_hit_rate(0, 1)");
    bool ratio_bump =
        contains(svc, "evaluator_.bump_dirty_block_ratio(dirty_blocks, total_blocks_seen)");
    if (!full_bump || !partial_bump || !full_hit_rate || !ratio_bump) {
        std::println("FAIL: full_bump={} partial_bump={} full_hit_rate={} ratio_bump={}", full_bump,
                     partial_bump, full_hit_rate, ratio_bump);
        return false;
    }
    std::println(
        "OK: 4 wire-up sites present (full_bump + partial_bump + full_hit_rate + ratio_bump)");
    return true;
}

bool check_query_surface_ac5() {
    std::println("\n--- AC5: query:incremental-relower-stats extended ---");
    std::string prim = read_file("src/compiler/evaluator_primitives_obs_eval_05.cpp");
    bool all = contains(prim, "\"full-relower-count\"") &&
               contains(prim, "\"dirty-block-ratio-numerator-total\"") &&
               contains(prim, "\"dirty-block-ratio-denominator-total\"") &&
               contains(prim, "\"relower-block-hit-rate-numerator-total\"") &&
               contains(prim, "\"relower-block-hit-rate-denominator-total\"") &&
               contains(prim, "\"relower-block-hit-rate\"") && contains(prim, "make_int(1639)");
    if (!all) {
        std::println("FAIL: 6 new keys or schema 1639 missing in primitive output");
        return false;
    }
    std::println("OK: 6 new keys present + schema bumped to 1639");
    return true;
}

bool check_xmacro_ac6() {
    std::println("\n--- AC6: 5 X-macro fields in compiler_metrics_fields.inc ---");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    bool all =
        contains(fields, "AURA_COMPILER_METRICS_FIELD(full_relower_count)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(dirty_block_ratio_numerator_total)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(dirty_block_ratio_denominator_total)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(relower_block_hit_rate_numerator_total)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(relower_block_hit_rate_denominator_total)");
    if (!all) {
        std::println("FAIL: 5 X-macro fields missing");
        return false;
    }
    std::println("OK: 5 X-macro fields present");
    return true;
}

bool check_baseline_ac7(CompilerService& cs) {
    std::println("\n--- AC7: cross-layer baseline round-trip ---");
    if (!cs.eval("(set-code \"(define x 100)\")")) {
        std::println("FAIL: set-code broke");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r)) {
        std::println("FAIL: eval-current broke");
        return false;
    }
    std::println("OK: cross-layer baseline round-trip survived #1639 wire-up");
    return true;
}

} // namespace aura_1639_detail

int main() {
    using namespace aura_1639_detail;
    int passed = 0;
    int failed = 0;
    auto run = [&](bool ok) {
        if (ok)
            ++passed;
        else
            ++failed;
        g_passed = passed;
        g_failed = failed;
    };
    std::println("=== Issue #1639: per-block dirty bitmask \u2192 partial re-lower wiring ===");
    run(check_partial_path_pipeline_ac1());
    run(check_metrics_ac2());
    run(check_bump_getter_ac3());
    run(check_wires_ac4());
    run(check_query_surface_ac5());
    run(check_xmacro_ac6());
    {
        CompilerService cs;
        run(check_baseline_ac7(cs));
    }
    if (failed > 0) {
        std::println("\ntest_incremental_relower_perblock FAILED ({} passed, {} failed)", passed,
                     failed);
        return 1;
    }
    std::println("\ntest_incremental_relower_perblock PASS ({} acs, all green)", passed);
    return 0;
}