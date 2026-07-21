// tests/test_orchestration_steal_boundary.cpp — Issue #1641
//
// AC list (per docs/design/1641-orchestration-steal-boundary.md):
//   AC1: source cites #1641; serve/worker.cpp::steal wires the
//        boundary_held_steal_safe_total bump (paired with the legacy
//        bump_cross_fiber_mutation_safe_steal on the Fiber level)
//        when steal succeeds while YieldReason::MutationBoundary is
//        the victim's last yield reason.
//   AC2: serve/worker.cpp::steal wires the
//        steal_mutation_boundary_deferred_total +
//        starvation_mitigated_for_boundary_count bumps in the inner
//        boundary block (paired with the legacy
//        bump_steal_inner_mutation_boundary_deferred +
//        apply_starvation_mitigation).
//   AC3: serve/scheduler.cpp wires
//        starvation_mitigated_for_boundary_count after
//        apply_starvation_mitigation(f) (Issue #1641: full
//        apply_starvation_mitigation on the scheduler side too).
//   AC4: 3 metric slots in observability_metrics.h
//        (steal_mutation_boundary_deferred_total +
//         starvation_mitigated_for_boundary_count +
//         boundary_held_steal_safe_total).
//   AC5: 3 AURA_COMPILER_METRICS_FIELD(...) entries in
//        compiler_metrics_fields.inc.
//   AC6: 3 bump_/getter pairs declared in evaluator.ixx.
//   AC7: cross-layer baseline regression — CompilerService can be
//        constructed and a basic (set-code) + (eval-current) round-trip
//        still works after the wire-up.
//
// Pattern references: tests/test_aot_hot_update_incremental.cpp
// (7 ACs, source-driven), tests/test_incremental_relower_perblock.cpp
// (7 ACs, source-driven), tests/test_soa_dual_path_consistency.cpp
// (9 ACs, source-driven).

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

namespace aura_1641_detail {

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

bool check_worker_safe_steal_bump_ac1() {
    std::println("\n--- AC1: worker.cpp::steal wires boundary_held_steal_safe_total ---");
    std::string worker = read_file("src/serve/worker.cpp");
    bool wired =
        contains(worker, "Issue #1641: paired boundary_held_steal_safe_total") &&
        contains(worker, "ev->bump_boundary_held_steal_safe_total()") &&
        contains(worker, "if (stolen->last_yield_reason() == YieldReason::MutationBoundary)");
    if (!wired) {
        std::println("FAIL: worker.cpp boundary_held_steal_safe_total bump missing");
        return false;
    }
    std::println("OK: worker.cpp wires boundary_held_steal_safe_total on safe-steal success");
    return true;
}

bool check_worker_inner_bumps_ac2() {
    std::println(
        "\n--- AC2: worker.cpp::steal wires deferred + mitigated bumps in inner block ---");
    std::string worker = read_file("src/serve/worker.cpp");
    bool wired = contains(worker, "Issue #1641: paired steal_mutation_boundary_deferred_total") &&
                 contains(worker, "ev->bump_steal_mutation_boundary_deferred_total()") &&
                 contains(worker, "ev->bump_starvation_mitigated_for_boundary_count()") &&
                 contains(worker, "apply_starvation_mitigation(stolen);");
    if (!wired) {
        std::println("FAIL: worker.cpp inner boundary bumps missing");
        return false;
    }
    std::println("OK: worker.cpp wires deferred + mitigated bumps in inner boundary block");
    return true;
}

bool check_scheduler_mitigation_bump_ac3() {
    std::println("\n--- AC3: scheduler.cpp wires starvation_mitigated_for_boundary_count ---");
    std::string scheduler = read_file("src/serve/scheduler.cpp");
    bool wired =
        contains(scheduler, "Issue #1641: paired starvation_mitigated_for_boundary_count") &&
        contains(scheduler, "ev->bump_starvation_mitigated_for_boundary_count()") &&
        contains(scheduler, "apply_starvation_mitigation(f);");
    if (!wired) {
        std::println("FAIL: scheduler.cpp starvation_mitigated_for_boundary_count bump missing");
        return false;
    }
    std::println("OK: scheduler.cpp wires starvation_mitigated_for_boundary_count after "
                 "apply_starvation_mitigation");
    return true;
}

bool check_metrics_ac4() {
    std::println("\n--- AC4: 3 new metric slots in observability_metrics.h ---");
    std::string om = read_file("src/compiler/observability_metrics.h");
    bool all = contains(om, "steal_mutation_boundary_deferred_total") &&
               contains(om, "starvation_mitigated_for_boundary_count") &&
               contains(om, "boundary_held_steal_safe_total");
    if (!all) {
        std::println("FAIL: 3 metric slots missing in observability_metrics.h");
        return false;
    }
    std::println("OK: 3 metric slots present");
    return true;
}

bool check_xmacro_ac5() {
    std::println("\n--- AC5: 3 X-macro fields in compiler_metrics_fields.inc ---");
    std::string fields = read_file("src/compiler/compiler_metrics_fields.inc");
    bool all =
        contains(fields, "AURA_COMPILER_METRICS_FIELD(steal_mutation_boundary_deferred_total)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(starvation_mitigated_for_boundary_count)") &&
        contains(fields, "AURA_COMPILER_METRICS_FIELD(boundary_held_steal_safe_total)");
    if (!all) {
        std::println("FAIL: 3 X-macro fields missing");
        return false;
    }
    std::println("OK: 3 X-macro fields present");
    return true;
}

bool check_bump_getter_ac6() {
    std::println("\n--- AC6: 3 bump_/getter pairs in evaluator.ixx ---");
    std::string ixx = read_file("src/compiler/evaluator.ixx");
    bool all = contains(ixx, "bump_steal_mutation_boundary_deferred_total") &&
               contains(ixx, "bump_starvation_mitigated_for_boundary_count") &&
               contains(ixx, "bump_boundary_held_steal_safe_total") &&
               contains(ixx, "get_steal_mutation_boundary_deferred_total") &&
               contains(ixx, "get_starvation_mitigated_for_boundary_count") &&
               contains(ixx, "get_boundary_held_steal_safe_total");
    if (!all) {
        std::println("FAIL: missing bump_/getter pair in evaluator.ixx");
        return false;
    }
    std::println("OK: 3 bump_/getter pairs declared");
    return true;
}

bool check_baseline_ac7(CompilerService& cs) {
    std::println("\n--- AC7: cross-layer baseline round-trip ---");
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        std::println("FAIL: set-code broke");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r)) {
        std::println("FAIL: eval-current broke");
        return false;
    }
    std::println("OK: cross-layer baseline round-trip survived #1641 wire-up");
    return true;
}

} // namespace aura_1641_detail

int main() {
    using namespace aura_1641_detail;
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
    std::println("=== Issue #1641: Scheduler/Worker work-stealing for MutationBoundary ===");
    run(check_worker_safe_steal_bump_ac1());
    run(check_worker_inner_bumps_ac2());
    run(check_scheduler_mitigation_bump_ac3());
    run(check_metrics_ac4());
    run(check_xmacro_ac5());
    run(check_bump_getter_ac6());
    {
        CompilerService cs;
        run(check_baseline_ac7(cs));
    }
    if (failed > 0) {
        std::println("\ntest_orchestration_steal_boundary FAILED ({} passed, {} failed)", passed,
                     failed);
        return 1;
    }
    std::println("\ntest_orchestration_steal_boundary PASS ({} acs, all green)", passed);
    return 0;
}