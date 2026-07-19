// tests/test_issue_1637.cpp — Issue #1637
//
// AC list (per docs/design/1637-panic-checkpoint-lifecycle.md):
//   AC1: source cites #1637; restore_panic_checkpoint_on_fiber_resume_if_needed
//        contains the full lifecycle close body (truncate_env_frames_to_checkpoint
//        + env_generation bump + invalidate_post_rollback_env_frames +
//        walk_active_closures + clear_panic_checkpoint).
//   AC2: 3 restore_<event>_if_needed variants + run_post_restore_lifecycle_close
//        helper declared and defined in evaluator.ixx +
//        evaluator_workspace_tree.cpp.
//   AC3: 5 new metrics declared in observability_metrics.h + 5 X-macro fields
//        in compiler_metrics_fields.inc.
//   AC4: 5 bump_/getter pair declarations in evaluator.ixx.
//   AC5: 4 prod files wire restore_<event>_if_needed callsites
//        (workspace_tree + fiber_mutation + primitives_types + aura_jit_bridge).
//   AC6: 5 keys in query:mutation-boundary-coverage-stats primitive output
//        (evaluator_primitives_obs_eval_05.cpp kv list).
//   AC7: 3 file-scope atomic fallbacks + 5 C accessors in aura_jit_bridge.cpp.
//   AC8: hot-swap:fn callback wires panic restore after invocation
//        (evaluator_primitives_types.cpp).
//   AC9: cross-layer regression — CompilerService can be constructed and a
//        basic (set-code) + (eval-current) sequence still round-trips.
//   AC10: linter scripts/check_panic_checkpoint_lifecycle_coverage.py exits 0.
//
// Pattern references: tests/test_issue_1908.cpp (10 ACs), tests/test_gc_roots_
// bridge_epoch_drift_1734.cpp (4 ACs, source-driven), tests/test_fiber_steal_
// panic_checkpoint_nested_gc.cpp (98-line smoke — replaced by this run).

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

namespace aura_1637_detail {

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

bool check_source_ac1() {
    std::println("\n--- AC1: full closed-loop restore body in workspace_tree.cpp ---");
    std::string wst = read_file("src/compiler/evaluator_workspace_tree.cpp");
    bool has_full_lifecycle = contains(wst, "Issue #1637") &&
                              contains(wst, "restore_panic_checkpoint_on_fiber_resume_if_needed") &&
                              contains(wst, "truncate_env_frames_to_checkpoint()") &&
                              contains(wst, "env_generation_ = env_generation_ + 1") &&
                              contains(wst, "invalidate_post_rollback_env_frames()") &&
                              contains(wst, "walk_active_closures") &&
                              contains(wst, "clear_panic_checkpoint()") &&
                              contains(wst, "bump_post_steal_checkpoint_restore_total()") &&
                              contains(wst, "cross_fiber_panic_heal_success") &&
                              contains(wst, "mutation_boundary_steal_safe_total");
    if (!has_full_lifecycle) {
        std::println("FAIL: restore_panic_checkpoint_on_fiber_resume_if_needed missing full "
                     "lifecycle close body");
        return false;
    }
    std::println("OK: full lifecycle close body present (truncate + generation + invalidate + "
                 "walk_closures + clear)");
    return true;
}

bool check_decls_ac2() {
    std::println("\n--- AC2: 3 restore variants + helper declared + defined ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    std::string wst = read_file("src/compiler/evaluator_workspace_tree.cpp");
    bool fiber_resume_decl =
        contains(ixx, "void restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept;");
    bool compact_decl =
        contains(ixx, "void restore_panic_checkpoint_on_arena_compact_if_needed() noexcept;");
    bool hot_swap_decl =
        contains(ixx, "void restore_panic_checkpoint_on_hot_swap_if_needed() noexcept;");
    bool helper_decl = contains(ixx, "void run_post_restore_lifecycle_close(bool");
    bool compact_impl =
        contains(wst, "Evaluator::restore_panic_checkpoint_on_arena_compact_if_needed() noexcept");
    bool hot_swap_impl =
        contains(wst, "Evaluator::restore_panic_checkpoint_on_hot_swap_if_needed() noexcept");
    bool helper_impl = contains(wst, "Evaluator::run_post_restore_lifecycle_close(bool");
    bool all_decl = fiber_resume_decl && compact_decl && hot_swap_decl && helper_decl &&
                    compact_impl && hot_swap_impl && helper_impl;
    if (!all_decl) {
        std::println("FAIL: missing decls/impls (fiber_resume={} compact={} hot_swap={} "
                     "helper_decl={} compact_impl={} hot_swap_impl={} helper_impl={})",
                     fiber_resume_decl, compact_decl, hot_swap_decl, helper_decl, compact_impl,
                     hot_swap_impl, helper_impl);
        return false;
    }
    std::println("OK: 3 restore variants + helper declared + defined");
    return true;
}

bool check_metrics_ac3() {
    std::println("\n--- AC3: 5 new metrics in observability_metrics.h + 5 X-macro fields ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    bool om_all = contains(om, "post_steal_checkpoint_restore_total") &&
                  contains(om, "post_compact_checkpoint_restore_total") &&
                  contains(om, "post_hot_swap_checkpoint_restore_total") &&
                  contains(om, "cross_fiber_panic_heal_success") &&
                  contains(om, "mutation_boundary_steal_safe_total");
    bool fields_all =
        contains(fields, "AURA_COMPILER_METRICS_FIELD(post_steal_checkpoint_restore_total)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(post_compact_checkpoint_restore_total)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(post_hot_swap_checkpoint_restore_total)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(cross_fiber_panic_heal_success)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(mutation_boundary_steal_safe_total)");
    if (!om_all || !fields_all) {
        std::println("FAIL: observability_metrics.h={} fields_inc={}", om_all, fields_all);
        return false;
    }
    std::println("OK: 5 metric slots + 5 X-macro fields present");
    return true;
}

bool check_bump_getter_ac4() {
    std::println("\n--- AC4: 5 bump_/getter pairs in evaluator.ixx ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool all = contains(ixx, "bump_post_steal_checkpoint_restore_total") &&
               contains(ixx, "bump_post_compact_checkpoint_restore_total") &&
               contains(ixx, "bump_post_hot_swap_checkpoint_restore_total") &&
               contains(ixx, "bump_cross_fiber_panic_heal_success") &&
               contains(ixx, "bump_mutation_boundary_steal_safe_total") &&
               contains(ixx, "get_post_steal_checkpoint_restore_total") &&
               contains(ixx, "get_post_compact_checkpoint_restore_total") &&
               contains(ixx, "get_post_hot_swap_checkpoint_restore_total") &&
               contains(ixx, "get_cross_fiber_panic_heal_success") &&
               contains(ixx, "get_mutation_boundary_steal_safe_total");
    if (!all) {
        std::println("FAIL: missing bump_/getter pair in evaluator.ixx");
        return false;
    }
    std::println("OK: 5 bump_/getter pairs declared");
    return true;
}

bool check_wires_ac5() {
    std::println("\n--- AC5: restore_<event>_if_needed callsites wired in 4 prod files ---");
    std::string wst = read_file("src/compiler/evaluator_workspace_tree.cpp");
    std::string mut = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    std::string types = read_file("src/compiler/evaluator_primitives_types.cpp");
    std::string bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    bool wst_wired =
        contains(wst, "restore_panic_checkpoint_on_fiber_resume_if_needed() noexcept") &&
        contains(wst, "restore_panic_checkpoint_on_arena_compact_if_needed() noexcept") &&
        contains(wst, "restore_panic_checkpoint_on_hot_swap_if_needed() noexcept");
    bool mut_wired_arena = contains(mut, "on_arena_compact_hook") &&
                           contains(mut, "restore_panic_checkpoint_on_arena_compact_if_needed");
    bool mut_wired_trampolines = contains(mut, "aura_evaluator_post_steal_panic_restore") &&
                                 contains(mut, "aura_evaluator_post_compact_panic_restore") &&
                                 contains(mut, "aura_evaluator_hot_swap_panic_restore");
    bool types_wired = contains(types, "restore_panic_checkpoint_on_hot_swap_if_needed") &&
                       contains(types, "hot-swap:fn");
    bool bridge_wired =
        contains(bridge, "aura_evaluator_post_steal_panic_restore(void* ev_ptr)") &&
        contains(bridge, "aura_evaluator_post_compact_panic_restore(void* ev_ptr)") &&
        contains(bridge, "aura_evaluator_hot_swap_panic_restore(void* ev_ptr)");
    bool all = wst_wired && mut_wired_arena && mut_wired_trampolines && types_wired && bridge_wired;
    if (!all) {
        std::println("FAIL: wst={} mut_arena={} mut_tramp={} types={} bridge={}", wst_wired,
                     mut_wired_arena, mut_wired_trampolines, types_wired, bridge_wired);
        return false;
    }
    std::println("OK: all 4 prod files wire their restore_<event>_if_needed callsite(s)");
    return true;
}

bool check_query_surface_ac6() {
    std::println("\n--- AC6: query:mutation-boundary-coverage-stats extended with 5 keys ---");
    std::string prim = read_file("src/compiler/evaluator_primitives_obs_eval_05.cpp");
    bool all = contains(prim, "\"post-steal-checkpoint-restore-total\"") &&
               contains(prim, "\"post-compact-checkpoint-restore-total\"") &&
               contains(prim, "\"post-hot-swap-checkpoint-restore-total\"") &&
               contains(prim, "\"cross-fiber-panic-heal-success\"") &&
               contains(prim, "\"mutation-boundary-steal-safe-total\"") &&
               contains(prim, "make_int(1637)");
    if (!all) {
        std::println("FAIL: 5 new keys not all surfaced in primitive output");
        return false;
    }
    std::println("OK: 5 new keys present in primitive kv list + schema 1637");
    return true;
}

bool check_bridge_ac7() {
    std::println("\n--- AC7: 3 file-scope atomic fallbacks + 5 C accessors ---");
    std::string bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    bool atomics = contains(bridge, "g_1637_steal_restore_fallback_total") &&
                   contains(bridge, "g_1637_compact_restore_fallback_total") &&
                   contains(bridge, "g_1637_hot_swap_restore_fallback_total") &&
                   contains(bridge, "g_1637_panic_heal_success_fallback_total") &&
                   contains(bridge, "g_1637_boundary_steal_safe_fallback_total");
    bool accessors = contains(bridge, "aura_post_steal_checkpoint_restore_total") &&
                     contains(bridge, "aura_post_compact_checkpoint_restore_total") &&
                     contains(bridge, "aura_post_hot_swap_checkpoint_restore_total") &&
                     contains(bridge, "aura_cross_fiber_panic_heal_success_total") &&
                     contains(bridge, "aura_mutation_boundary_steal_safe_total");
    bool all = atomics && accessors;
    if (!all) {
        std::println("FAIL: atomics={} accessors={}", atomics, accessors);
        return false;
    }
    std::println("OK: 3 file-scope atomic fallbacks + 5 C accessors");
    return true;
}

bool check_hot_swap_wire_ac8() {
    std::println("\n--- AC8: hot-swap:fn wires panic restore after invocation ---");
    std::string types = read_file("src/compiler/evaluator_primitives_types.cpp");
    bool wired = contains(types, "ev.restore_panic_checkpoint_on_hot_swap_if_needed()") &&
                 contains(types, "Issue #1637: closed-loop panic checkpoint restore on hot-swap");
    if (!wired) {
        std::println("FAIL: hot-swap:fn callback doesn't wire panic restore");
        return false;
    }
    std::println("OK: hot-swap:fn callback wires panic restore");
    return true;
}

bool check_baseline_ac9(CompilerService& cs) {
    std::println("\n--- AC9: cross-layer baseline round-trip ---");
    if (!cs.eval("(set-code \"(define x 1)\")")) {
        std::println("FAIL: set-code broke");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r)) {
        std::println("FAIL: eval-current broke");
        return false;
    }
    std::println("OK: cross-layer baseline round-trip survived #1637 wire-up");
    return true;
}

} // namespace aura_1637_detail

int main() {
    using namespace aura_1637_detail;
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
    run(check_source_ac1());
    run(check_decls_ac2());
    run(check_metrics_ac3());
    run(check_bump_getter_ac4());
    run(check_wires_ac5());
    run(check_query_surface_ac6());
    run(check_bridge_ac7());
    run(check_hot_swap_wire_ac8());
    {
        CompilerService cs;
        run(check_baseline_ac9(cs));
    }
    if (failed > 0) {
        std::println("\ntest_issue_1637 FAILED ({} passed, {} failed)", passed, failed);
        return 1;
    }
    std::println("\ntest_issue_1637 PASS ({} acs, all green)", passed);
    return 0;
}
