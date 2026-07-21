// test_issue_1908.cpp — orphan restored (AC drift; not in CI batch)
#include "test_harness.hpp"
import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
// @category: integration
// @reason: uses Evaluator + Aura C bridge hook + CompilerMetrics + CI linter
//
// test_issue_1908.cpp — Verify Issue #1908 acceptance criteria
// ("Harden MutationBoundaryGuard + macro clone provenance under
//  concurrent fiber steal / GC / hot-swap for reliable long-running
//  self-evolution (refine #1014 #1047)").
//
// #1908 closes the macro clone provenance + boundary interaction loop
// for production long-running self-evolving Agents. The instrumentation
// surface (clean module-boundary separation):
//   - 2 new CompilerMetrics counters
//     (macro_provenance_repin_on_steal_total,
//      hygiene_violation_prevented_on_boundary_total)
//   - 2 bump + 2 getter helpers on Evaluator
//   - 1 bridge hook (aura_macro_provenance_repin_on_steal) + 2 accessors
//     in aura_jit_bridge.cpp — bumps FILE-LEVEL ATOMIC ONLY (for
//     module-unaware call sites like clone_macro_body)
//   - 3 wire-up sites in evaluator_fiber_mutation.cpp that call
//     Evaluator::bump_* directly (per-CompilerMetrics path, for
//     flush_mutation_boundary outermost + complete_post_resume_steal_refresh
//     + transfer_and_revalidate_panic_checkpoint)
//   - 1 wire-up site in macro_expansion.cpp (clone_macro_body MacroIntroduced
//     path, calls the bridge hook with nullptr)
//   - (engine:metrics "query:macro-provenance-stats") primitive reads
//     from Evaluator getters (per-CompilerMetrics view)
//   - scripts/check_macro_provenance_coverage.py CI linter
//
// Module-boundary rationale: aura_jit_bridge.cpp is a C-linkage shim TU
// that cannot import the C++20 Evaluator module. So the bridge hook
// (file-level atomic fallback) is for module-unaware call sites only.
// The per-CompilerMetrics path goes through Evaluator::bump_* helpers
// in the wire-up sites (evaluator_fiber_mutation.cpp has the Evaluator
// module imported).
//
// Acceptance Criteria covered (mirrors #1908 body, updated for
// clean module-boundary separation):
//   AC1: 2 #1908 accessors reachable (baseline 0 on fresh evaluator)
//   AC2: fresh evaluator reports 0 from (query:macro-provenance-stats)
//   AC3: bridge hook bumps file-level atomic (visible in accessors)
//   AC4: bridge hook returns 1 on successful bump
//   AC5: Evaluator bump helpers bump per-CompilerMetrics counters
//   AC6: primitive returns sum of 2 per-CompilerMetrics counters when
//        no regression (call Evaluator bump helpers first)
//   AC7: primitive returns -1 sentinel when repin > 0 && prevented == 0
//        (call only bump_macro_provenance_repin_on_steal_total)
//   AC8: scripts/check_macro_provenance_coverage.py --self-test passes
//   AC9: linter scans 6 prod files (all #1908 surfaces wired)
//   AC10: counters monotonic across multiple invocations
//         (accessors via bridge hook + per-CompilerMetrics via bump_*)


using aura::test::g_failed;
using aura::test::g_passed;


// Forward declarations for the C bridge hooks (defined in aura_jit_bridge.cpp).
// extern "C" must be at file scope, not inside a function body.
extern "C" int aura_macro_provenance_repin_on_steal(void* ev_ptr, std::uint64_t cloned_marker);
extern "C" std::uint64_t aura_macro_provenance_repin_on_steal_total(void);
extern "C" std::uint64_t aura_hygiene_violation_prevented_on_boundary_total(void);

namespace aura_issue_1908_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

// ── AC1: 2 #1908 accessors reachable (baseline 0) ──
bool test_two_accessors_reachable() {
    std::println("\n--- AC1: 2 #1908 accessors reachable (baseline 0) ---");
    CHECK(aura_macro_provenance_repin_on_steal_total() == 0,
          "fresh state: aura_macro_provenance_repin_on_steal_total == 0");
    CHECK(aura_hygiene_violation_prevented_on_boundary_total() == 0,
          "fresh state: aura_hygiene_violation_prevented_on_boundary_total == 0");
    return true;
}

// ── AC2: fresh evaluator -> (query:macro-provenance-stats) = 0 ──
bool test_fresh_evaluator_primitive_zero() {
    std::println("\n--- AC2: fresh evaluator -> (query:macro-provenance-stats) = 0 ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:macro-provenance-stats\")");
    CHECK(r.has_value(), "primitive returns a value");
    if (!r || !is_int(*r)) {
        ++g_failed;
        return false;
    }
    const auto v = as_int(*r);
    CHECK(v == 0,
          "no Guard exits + no steals + no PanicCheckpoint transfers yet -> 0 (vacuous baseline)");
    return true;
}

// ── AC3: bridge hook bumps file-level atomic (visible in accessors) ──
bool test_bridge_hook_bumps_accessors() {
    std::println("\n--- AC3: bridge hook bumps file-level atomic (visible in accessors) ---");
    const auto repin_before = aura_macro_provenance_repin_on_steal_total();
    const auto prevented_before = aura_hygiene_violation_prevented_on_boundary_total();

    // Bridge hook called with nullptr ev_ptr (module-unaware call site
    // pattern, mirrors clone_macro_body in macro_expansion.cpp).
    const int rc = aura_macro_provenance_repin_on_steal(nullptr, /*cloned_marker=*/1);

    const auto repin_after = aura_macro_provenance_repin_on_steal_total();
    const auto prevented_after = aura_hygiene_violation_prevented_on_boundary_total();
    CHECK(rc == 1, "bridge hook returns 1 on successful bump");
    CHECK(repin_after == repin_before + 1,
          "bridge hook bumps file-level atomic repin accessor by 1");
    CHECK(prevented_after == prevented_before + 1,
          "bridge hook bumps file-level atomic prevented accessor by 1");
    return true;
}

// ── AC4: bridge hook returns 1 on successful bump ──
bool test_bridge_hook_returns_one() {
    std::println("\n--- AC4: bridge hook returns 1 on successful bump ---");
    const int rc = aura_macro_provenance_repin_on_steal(nullptr, /*cloned_marker=*/0);
    CHECK(rc == 1, "bridge hook returns 1 (file-level atomic always bumped)");
    return true;
}

// ── AC5: Evaluator bump helpers bump per-CompilerMetrics counters ──
bool test_evaluator_bump_helpers() {
    std::println("\n--- AC5: Evaluator bump helpers bump per-CompilerMetrics ---");
    CompilerService cs;
    const auto repin_before = cs.evaluator().get_macro_provenance_repin_on_steal_total();
    const auto prevented_before =
        cs.evaluator().get_hygiene_violation_prevented_on_boundary_total();

    // Call the Evaluator bump helpers directly (per-CompilerMetrics path,
    // mirrors the wire-up sites in evaluator_fiber_mutation.cpp).
    cs.evaluator().bump_macro_provenance_repin_on_steal_total();
    cs.evaluator().bump_hygiene_violation_prevented_on_boundary_total();

    const auto repin_after = cs.evaluator().get_macro_provenance_repin_on_steal_total();
    const auto prevented_after = cs.evaluator().get_hygiene_violation_prevented_on_boundary_total();
    CHECK(repin_after == repin_before + 1,
          "bump_macro_provenance_repin_on_steal_total bumps per-eval counter by 1");
    CHECK(prevented_after == prevented_before + 1,
          "bump_hygiene_violation_prevented_on_boundary_total bumps per-eval counter by 1");
    return true;
}

// ── AC6: primitive returns sum of 2 per-CompilerMetrics counters when no regression ──
bool test_primitive_sum_path() {
    std::println(
        "\n--- AC6: no regression -> primitive returns sum of 2 per-CompilerMetrics counters ---");
    CompilerService cs;
    // Bump the QUERY evaluator directly (same one the primitive reads via
    // Evaluator::get_query_evaluator()). This mirrors the #1905 test pattern
    // and ensures the bump is visible to the primitive.
    auto* qe = Evaluator::get_query_evaluator();
    if (qe) {
        qe->bump_macro_provenance_repin_on_steal_total();
        qe->bump_hygiene_violation_prevented_on_boundary_total();
    }
    auto r = cs.eval("(engine:metrics \"query:macro-provenance-stats\")");
    CHECK(r && is_int(*r), "primitive still int after query evaluator bump helpers");
    if (!r || !is_int(*r)) {
        ++g_failed;
        return false;
    }
    // Both per-CompilerMetrics counters bumped by 1 -> sum = 2, no sentinel.
    CHECK(as_int(*r) == 2, "query evaluator bump helpers bumped 2 per-CompilerMetrics counters -> "
                           "sum = 2 (sum-path, no sentinel)");
    return true;
}

// ── AC7: primitive returns -1 sentinel when repin > 0 && prevented == 0 ──
bool test_primitive_sentinel_on_regression() {
    std::println("\n--- AC7: repin > 0 && prevented == 0 -> -1 regression sentinel ---");
    CompilerService cs;
    // Bump only the repin counter on the query evaluator (no boundary
    // prevention) to exercise the regression sentinel path. Mirrors a
    // potential race condition where the boundary interaction didn't
    // prevent the violation.
    auto* qe = Evaluator::get_query_evaluator();
    if (qe) {
        qe->bump_macro_provenance_repin_on_steal_total();
    }
    auto r = cs.eval("(engine:metrics \"query:macro-provenance-stats\")");
    CHECK(r && is_int(*r), "primitive still int after single repin bump on query evaluator");
    if (!r || !is_int(*r)) {
        ++g_failed;
        return false;
    }
    CHECK(as_int(*r) == -1,
          "repin > 0 && prevented == 0 -> -1 sentinel (grep-friendly regression marker)");
    return true;
}

// ── AC8: linter --self-test passes ──
bool test_linter_self_test() {
    std::println("\n--- AC8: scripts/check_macro_provenance_coverage.py --self-test ---");
    const std::string cmd =
        "cd /home/dev/code/aura && python3 scripts/check_macro_provenance_coverage.py --self-test";
    const int rc = std::system(cmd.c_str());
    CHECK(rc == 0, "linter --self-test exits 0");
    return rc == 0;
}

// ── AC9: linter scans 6 prod files + reports wiring ──
bool test_linter_scans_production() {
    std::println("\n--- AC9: linter scans 6 prod files ---");
    const std::string cmd =
        "cd /home/dev/code/aura && python3 scripts/check_macro_provenance_coverage.py";
    const int rc = std::system(cmd.c_str());
    CHECK(rc == 0, "linter scans observability_metrics.h + evaluator.ixx + aura_jit_bridge.cpp + "
                   "evaluator_primitives_query.cpp + evaluator_fiber_mutation.cpp + "
                   "macro_expansion.cpp (all #1908 surfaces wired)");
    return rc == 0;
}

// ── AC10: counters monotonic across multiple invocations (file-level atomic + per-CompilerMetrics)
// ──
bool test_counters_monotonic() {
    std::println("\n--- AC10: counters monotonic across multiple invocations ---");
    CompilerService cs;
    // File-level atomic (via bridge hook) monotonic
    const auto fa_before = aura_macro_provenance_repin_on_steal_total();
    for (int i = 0; i < 5; ++i) {
        aura_macro_provenance_repin_on_steal(nullptr, /*cloned_marker=*/1);
    }
    const auto fa_after = aura_macro_provenance_repin_on_steal_total();
    CHECK(fa_after == fa_before + 5,
          "5 bridge hook invocations bump file-level atomic repin accessor by exactly 5");

    // Per-CompilerMetrics (via Evaluator bump helper) monotonic
    const auto pe_before = cs.evaluator().get_macro_provenance_repin_on_steal_total();
    for (int i = 0; i < 5; ++i) {
        cs.evaluator().bump_macro_provenance_repin_on_steal_total();
    }
    const auto pe_after = cs.evaluator().get_macro_provenance_repin_on_steal_total();
    CHECK(
        pe_after == pe_before + 5,
        "5 Evaluator bump helper invocations bump per-CompilerMetrics repin counter by exactly 5");
    return true;
}

} // namespace aura_issue_1908_detail

int main() {
    using namespace aura_issue_1908_detail;
    std::println("=== Issue #1908: MutationBoundaryGuard + macro clone provenance hardening ===");
    int rc = 0;
    rc |= !test_two_accessors_reachable();
    rc |= !test_fresh_evaluator_primitive_zero();
    rc |= !test_bridge_hook_bumps_accessors();
    rc |= !test_bridge_hook_returns_one();
    rc |= !test_evaluator_bump_helpers();
    rc |= !test_primitive_sum_path();
    rc |= !test_primitive_sentinel_on_regression();
    rc |= !test_linter_self_test();
    rc |= !test_linter_scans_production();
    rc |= !test_counters_monotonic();
    std::println("\n=== Summary: passed={} failed={} ===", g_passed, g_failed);
    return rc == 0 ? 0 : 1;
}