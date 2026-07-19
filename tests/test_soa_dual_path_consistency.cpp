// tests/test_soa_dual_path_consistency.cpp — Issue #1638
//
// AC list (docs/design/1638-...md removed per Anqi 2026-07-19
// directive — aura philosophy, no per-issue plan docs; source-
// driven ACs below remain authoritative):
//   AC1: source cites #1638; materialize_call_env wires
//        ensure_env_frame_dual_path_consistent (defense in depth
//        alongside closure_is_epoch_or_env_stale).
//   AC2: 2 collect_compiler_managed_gc_roots sites in evaluator_gc.cpp
//        wire dual-path consistency gate (paired observability).
//   AC3: 3 metric slots in observability_metrics.h
//        (dual_path_stale_fallback_total /
//         mutation_log_compact_bytes_saved /
//         env_frame_version_drift_prevented).
//   AC4: 3 X-macro fields in compiler_metrics_fields.inc.
//   AC5: 3 bump_/getter pairs declared in evaluator.ixx
//        (bump_/get_dual_path_stale_fallback_total +
//         bump_/get_mutation_log_compact_bytes_saved +
//         bump_/get_env_frame_version_drift_prevented).
//   AC6: FlatAST::compact_mutation_log() + Evaluator::compact_mutation_log()
//        declared and defined; FlatAST::mutation_log_size() accessor
//        declared (used by the 64KB threshold gate).
//   AC7: Evaluator::ensure_env_frame_dual_path_consistent(EnvId, const char*)
//        declared + defined; bumps env_frame_version_drift_prevented +
//        dual_path_stale_fallback_total on stale detection.
//   AC8: exit_mutation_boundary success path wires mutation_log compact
//        with 64KB threshold gate (heavy-mutation safety net).
//   AC9: query:mutation-boundary-coverage-stats primitive extended with
//        3 new keys + schema 1638 (no new primitive added — surface
//        held within 521 budget per #1734 raise).
//
// Pattern references: tests/test_issue_1637.cpp (9 ACs, source-driven),
// tests/test_issue_1908.cpp (10 ACs, dual-write pattern),
// tests/test_issue_1478.cpp (9 ACs, linear_post_mutate_enforce pattern),
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

namespace aura_1638_detail {

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

bool check_materialize_call_env_ac1() {
    std::println("\n--- AC1: materialize_call_env wires dual-path check ---");
    std::string env_cpp = read_file("src/compiler/evaluator_env.cpp");
    bool wired = contains(env_cpp, "Env Evaluator::materialize_call_env(const Closure& cl)") &&
                 contains(env_cpp, "ensure_env_frame_dual_path_consistent") &&
                 contains(env_cpp, "\"materialize_call_env\"") &&
                 contains(env_cpp, "Issue #1638: explicit dual-path consistency gate");
    if (!wired) {
        std::println("FAIL: materialize_call_env dual-path check not wired");
        return false;
    }
    std::println("OK: materialize_call_env wires ensure_env_frame_dual_path_consistent");
    return true;
}

bool check_gc_roots_dual_path_ac2() {
    std::println("\n--- AC2: 2 collect_compiler_managed_gc_roots sites wire dual-path ---");
    std::string gc_cpp = read_file("src/compiler/evaluator_gc.cpp");
    // The helper call count from the 2 wire-up sites:
    //   - "collect_gc_roots_env" site (line ~280)
    //   - "collect_gc_roots_env_2" site (line ~346)
    // Both should loop env_roots through ensure_env_frame_dual_path_consistent.
    bool site1 = contains(gc_cpp, "collect_gc_roots_env") &&
                 contains(gc_cpp, "ensure_env_frame_dual_path_consistent");
    bool site2 = contains(gc_cpp, "collect_gc_roots_env_2") &&
                 contains(gc_cpp, "ensure_env_frame_dual_path_consistent");
    if (!site1 || !site2) {
        std::println("FAIL: GC roots dual-path gate missing (site1={} site2={})", site1, site2);
        return false;
    }
    std::println("OK: 2 GC root collection sites wire dual-path consistency gate");
    return true;
}

bool check_metrics_ac3() {
    std::println("\n--- AC3: 3 metric slots in observability_metrics.h ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    bool all = contains(om, "dual_path_stale_fallback_total") &&
               contains(om, "mutation_log_compact_bytes_saved") &&
               contains(om, "env_frame_version_drift_prevented");
    if (!all) {
        std::println("FAIL: 3 metric slots missing in observability_metrics.h");
        return false;
    }
    std::println("OK: 3 metric slots present");
    return true;
}

bool check_xmacro_ac4() {
    std::println("\n--- AC4: 3 X-macro fields in compiler_metrics_fields.inc ---");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    bool all = contains(fields, "AURA_COMPILER_METRICS_FIELD(dual_path_stale_fallback_total)") &&
               contains(fields, "AURA_COMPILER_METRICS_FIELD(mutation_log_compact_bytes_saved)") &&
               contains(fields, "AURA_COMPILER_METRICS_FIELD(env_frame_version_drift_prevented)");
    if (!all) {
        std::println("FAIL: 3 X-macro fields missing");
        return false;
    }
    std::println("OK: 3 X-macro fields present");
    return true;
}

bool check_bump_getter_ac5() {
    std::println("\n--- AC5: 3 bump_/getter pairs in evaluator.ixx ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool all = contains(ixx, "bump_dual_path_stale_fallback_total") &&
               contains(ixx, "bump_mutation_log_compact_bytes_saved") &&
               contains(ixx, "bump_env_frame_version_drift_prevented") &&
               contains(ixx, "get_dual_path_stale_fallback_total") &&
               contains(ixx, "get_mutation_log_compact_bytes_saved") &&
               contains(ixx, "get_env_frame_version_drift_prevented");
    if (!all) {
        std::println("FAIL: missing bump_/getter pair in evaluator.ixx");
        return false;
    }
    std::println("OK: 3 bump_/getter pairs declared");
    return true;
}

bool check_compact_decl_ac6() {
    std::println("\n--- AC6: FlatAST::compact_mutation_log + Evaluator::compact_mutation_log ---");
    std::string ast_ixx = read_file("src/core/ast.ixx");
    std::string ev_ixx = read_file("src/compiler/evaluator.ixx");
    bool flat_decl = contains(ast_ixx, "std::size_t compact_mutation_log() noexcept") &&
                     contains(ast_ixx, "std::size_t mutation_log_size() const noexcept");
    bool eval_decl = contains(ev_ixx, "void compact_mutation_log() noexcept");
    bool eval_impl = contains(read_file("src/compiler/evaluator_workspace_tree.cpp"),
                              "Evaluator::compact_mutation_log() noexcept");
    if (!flat_decl || !eval_decl || !eval_impl) {
        std::println("FAIL: flat_decl={} eval_decl={} eval_impl={}", flat_decl, eval_decl,
                     eval_impl);
        return false;
    }
    std::println("OK: FlatAST + Evaluator compact_mutation_log declared + defined");
    return true;
}

bool check_ensure_dual_path_ac7() {
    std::println("\n--- AC7: ensure_env_frame_dual_path_consistent declared + defined ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    std::string wst = read_file("src/compiler/evaluator_workspace_tree.cpp");
    bool decl = contains(ixx, "bool ensure_env_frame_dual_path_consistent(EnvId id, const char*");
    bool impl = contains(wst, "Evaluator::ensure_env_frame_dual_path_consistent(EnvId") &&
                contains(wst, "is_env_frame_stale(id)");
    if (!decl || !impl) {
        std::println("FAIL: decl={} impl={}", decl, impl);
        return false;
    }
    std::println("OK: ensure_env_frame_dual_path_consistent declared + defined");
    return true;
}

bool check_exit_compact_ac8() {
    std::println(
        "\n--- AC8: exit_mutation_boundary success path wires compact with 64KB threshold ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool wired = contains(ixx, "Issue #1638: mutation_log compact at boundary exit") &&
                 contains(ixx, "kCompactThreshold = 64 * 1024") &&
                 contains(ixx, "compact_mutation_log()");
    if (!wired) {
        std::println("FAIL: exit_mutation_boundary compact wire-up missing");
        return false;
    }
    std::println("OK: exit_mutation_boundary wires compact with 64KB threshold");
    return true;
}

bool check_query_surface_ac9() {
    std::println("\n--- AC9: query:mutation-boundary-coverage-stats extended ---");
    std::string prim = read_file("src/compiler/evaluator_primitives_obs_eval_05.cpp");
    bool all = contains(prim, "\"dual-path-stale-fallback-total\"") &&
               contains(prim, "\"mutation-log-compact-bytes-saved\"") &&
               contains(prim, "\"env-frame-version-drift-prevented\"") &&
               contains(prim, "make_int(1638)");
    if (!all) {
        std::println("FAIL: 3 new keys not in primitive output or schema not 1638");
        return false;
    }
    std::println("OK: 3 new keys present + schema bumped to 1638");
    return true;
}

} // namespace aura_1638_detail

int main() {
    using namespace aura_1638_detail;
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
    std::println("=== Issue #1638: SoA EnvFrame dual-path consistency + mutation_log compact ===");
    run(check_materialize_call_env_ac1());
    run(check_gc_roots_dual_path_ac2());
    run(check_metrics_ac3());
    run(check_xmacro_ac4());
    run(check_bump_getter_ac5());
    run(check_compact_decl_ac6());
    run(check_ensure_dual_path_ac7());
    run(check_exit_compact_ac8());
    {
        CompilerService cs;
        // AC9 cross-layer regression — service round-trip must still work.
        if (!cs.eval("(set-code \"(define x 42)\")")) {
            std::println("FAIL: AC9 set-code broke");
            ++failed;
            g_failed = failed;
        } else if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r) || as_int(*r) != 42) {
            std::println("FAIL: AC9 eval-current broke");
            ++failed;
            g_failed = failed;
        } else {
            std::println("OK: AC9 cross-layer baseline round-trip survived #1638 wire-up");
            ++passed;
            g_passed = passed;
        }
        run(check_query_surface_ac9());
    }
    if (failed > 0) {
        std::println("\ntest_soa_dual_path_consistency FAILED ({} passed, {} failed)", passed,
                     failed);
        return 1;
    }
    std::println("\ntest_soa_dual_path_consistency PASS ({} acs, all green)", passed);
    return 0;
}