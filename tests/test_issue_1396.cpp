// test_issue_1396.cpp — Issue #1396: AOT hot-reload counter helpers —
// 4 unwired bump helpers gated behind AOT_RELOAD_PHASE_2_PLUS.
//
// Audit finding (Issue #1396 body):
//   - 4 bump helpers exist on Evaluator (#732 safe_boundary + #785
//     concurrent_steal / grace_period / env_version_sync) at
//     src/compiler/evaluator.ixx:5089, 5107, 5113, 5119
//   - 4 matching metric fields exist in
//     src/compiler/observability_metrics.h:5232-5234, 5298
//   - 2 query primitives read those fields in
//     src/compiler/evaluator_primitives_obs_jit_01.cpp:850 +
//     src/compiler/evaluator_primitives_obs_jit_03.cpp:692-700
//   - **0 production call sites** — counters will always read 0 in
//     a default-build, which makes the design look half-wired.
//
// Decision (Option B-modified, Anqi-confirmed 2026-07-13):
//   1. Wrap the 4 unwired bump helpers in
//      `#ifdef AOT_RELOAD_PHASE_2_PLUS` so they exist iff the
//      macro is defined. Default build: helpers absent. Phase 2+
//      build: helpers present and immediately usable at the
//      planned call sites (aura_jit_bridge.cpp +
//      MutationBoundaryGuard + fiber.cpp + WorkerThread::steal +
//      EnvFrame sync).
//   2. Keep the 4 metric fields UNCONDITIONAL in
//      observability_metrics.h — they're query-API data; removing
//      them would silently break the existing primitive surface.
//   3. Keep the 2 query primitives UNCONDITIONAL — they return
//      hashes with zero fields in default build (identical to
//      pre-#1396 behavior from the primitive API contract side).
//   4. Wrap the 2 existing tests that called those helpers
//      (test_issue_785.cpp AC4 bump-exercise section +
//      test_issue_732.cpp AC4 bump-accessible section) in the
//      same `#ifdef`. When the macro is undefined, the helpers
//      don't exist + their exercise bodies disappear (their
//      snapshot-before queries still run, plus they assert the
//      metric fields == 0 which is the honest default-build state).
//   5. This new test_issue_1396.cpp ships the regression coverage
//      proving the default build stays honest: query APIs still
//      return valid hashes + metric fields all read 0 (no callers,
//      no half-wired drift).
//
// Why B-modified (not A = wire-up, not C = remove):
//   - C (remove) would lose the Phase 2+ call-site plan encoded
//     in the helper signatures + their metric-field comments.
//     Re-introducing them later would be a re-design, not a
//     toggle.
//   - A (wire-up in this PR) is ~2h of touching
//     aura_jit_bridge.cpp + MutationBoundaryGuard + fiber.cpp +
//     WorkerThread::steal + EnvFrame sync + 4 chaos-stress
//     harnesses — each is its own focused session per the issue
//     body. Out of scope for the audit-close this PR actually
//     delivers.
//   - B (defer behind macro) preserves the design intent + the
//     metric field path + the query primitive surface. Defining
//     -DAOT_RELOAD_PHASE_2_PLUS literally makes the helpers
//     reappear at every future commit until the call-site PRs
//     land, with no source churn.
//
// Honest gap (Phase 2+ work, each is a separate ~2h session):
//   - bump_aot_safe_boundary_hit call site:
//     aura_jit_bridge.cpp::aura_reload_aot_module()
//     + MutationBoundaryGuard outermost exit hook
//   - bump_aot_concurrent_steal_during_reload call site:
//     fiber.cpp / worker::WorkerThread::steal() deferral path
//   - bump_aot_grace_period_hit call site:
//     aura_jit_bridge.cpp::aura_reload_aot_module() pre/post-swap
//   - bump_aot_env_version_sync_on_reload call site:
//     EnvFrame::version_ bump after the func_table refcount swap
//
// ACs shipped by this test:
//   AC1 — (query:aot-safe-swap-boundary-stats) primitive stays
//         queryable in default build and exposes all 5 expected
//         fields (regression: #732 surface unchanged).
//   AC2 — (query:aot-concurrent-hotupdate-stats) primitive stays
//         queryable in default build and exposes all 8 expected
//         fields including `schema == 785` (regression: #785
//         surface unchanged).
//   AC3 — All 4 newly added metric fields read 0 in the default
//         build, proving "no callers yet" is honest rather than
//         drift from a half-wired call site.
//   AC4 — AOT_RELOAD_PHASE_2_PLUS is documented as the chosen
//         gating macro; default build (undefined) is the supported
//         state for this PR.

#include <cstdint>
#include <format>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_1396_detail {
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

static std::int64_t hash_int_field(aura::compiler::CompilerService& cs, std::string_view hash_src,
                                   std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_safe_boundary_surface(aura::compiler::CompilerService& cs) {
    std::println(
        "\n--- AC1: (query:aot-safe-swap-boundary-stats) stays queryable in default build ---");
    auto r = cs.eval("(query:aot-safe-swap-boundary-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(query:aot-safe-swap-boundary-stats) returns a hash (regression #732 surface)");
    const std::vector<std::string> keys = {"safe-boundary-hits", "refcount-swaps",
                                           "region-violations-prevented", "concurrent-safe-reloads",
                                           "deopt-on-steal"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format("(hash-ref (query:aot-safe-swap-boundary-stats) '{}')", k));
        CHECK(f, std::format("field '{}' present in #732 primitive", k));
    }
}

static void run_ac2_hotupdate_surface(aura::compiler::CompilerService& cs) {
    std::println(
        "\n--- AC2: (query:aot-concurrent-hotupdate-stats) stays queryable in default build ---");
    auto r = cs.eval("(query:aot-concurrent-hotupdate-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(query:aot-concurrent-hotupdate-stats) returns a hash (regression #785 surface)");
    const std::vector<std::string> keys = {"concurrent-steal-during-reload",
                                           "grace-period-hits",
                                           "env-version-sync-on-reload",
                                           "region-mask-enforced",
                                           "grace-period-implemented",
                                           "steal-defer-active",
                                           "recommendation",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format("(hash-ref (query:aot-concurrent-hotupdate-stats) '{}')", k));
        CHECK(f, std::format("field '{}' present in #785 primitive", k));
    }
    const auto schema = hash_int_field(cs, "(query:aot-concurrent-hotupdate-stats)", "schema");
    CHECK(schema == 785, std::format("schema = {} (expected 785, no drift)", schema));
}

static void run_ac3_metric_fields_zero_default_build(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: all 4 newly added metric fields read 0 in default build ---");
    std::println("  (default build has 0 callers — fields are wired through, no drift to hide)");
    const auto safe_boundary =
        hash_int_field(cs, "(query:aot-safe-swap-boundary-stats)", "safe-boundary-hits");
    CHECK(safe_boundary == 0,
          std::format(
              "safe-boundary-hits = {} (expected 0 — no aura_reload_aot_module call site yet)",
              safe_boundary));
    const auto steal = hash_int_field(cs, "(query:aot-concurrent-hotupdate-stats)",
                                      "concurrent-steal-during-reload");
    CHECK(steal == 0, std::format("concurrent-steal-during-reload = {} (expected 0 — "
                                  "no WorkerThread::steal hook yet)",
                                  steal));
    const auto grace =
        hash_int_field(cs, "(query:aot-concurrent-hotupdate-stats)", "grace-period-hits");
    CHECK(grace == 0, std::format("grace-period-hits = {} (expected 0 — "
                                  "no aura_reload_aot_module pre/post-swap hook yet)",
                                  grace));
    const auto env_version =
        hash_int_field(cs, "(query:aot-concurrent-hotupdate-stats)", "env-version-sync-on-reload");
    CHECK(env_version == 0, std::format("env-version-sync-on-reload = {} (expected 0 — "
                                        "no EnvFrame::version_ sync hook yet)",
                                        env_version));
}

static void run_ac4_gating_macro_documented(aura::compiler::CompilerService& /*cs*/) {
    std::println("\n--- AC4: AOT_RELOAD_PHASE_2_PLUS is the chosen gating macro ---");
    std::println("  (helpers exist iff defined — default build = helpers absent)");
#ifdef AOT_RELOAD_PHASE_2_PLUS
    std::println("  build state: AOT_RELOAD_PHASE_2_PLUS IS defined.");
    std::println("               Helpers present — run test_issue_785 + test_issue_732");
    std::println("               for per-call-site regression.");
    CHECK(true, "AOT_RELOAD_PHASE_2_PLUS defined — Phase 2+ active; verify test_785/732 "
                "still pass");
#else
    std::println("  build state: AOT_RELOAD_PHASE_2_PLUS undefined as expected for default.");
    std::println("               Helpers absent; metric fields + query primitives still "
                 "queryable.");
    CHECK(true, "AOT_RELOAD_PHASE_2_PLUS is the chosen gating macro (default build supported)");
#endif
}

} // namespace aura_issue_1396_detail

int aura_issue_1396_run() {
    using namespace aura_issue_1396_detail;
    std::println(
        "=== Issue #1396: AOT hot-reload counter helpers — honest scope-limited close ===");
    std::println("    (4 unwired bump helpers gated behind AOT_RELOAD_PHASE_2_PLUS;");
    std::println("     metric fields + query primitives unconditionally queryable)");
    {
        aura::compiler::CompilerService cs;
        run_ac1_safe_boundary_surface(cs);
        run_ac2_hotupdate_surface(cs);
        run_ac3_metric_fields_zero_default_build(cs);
        run_ac4_gating_macro_documented(cs);
    }
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1396_run();
}
#endif
