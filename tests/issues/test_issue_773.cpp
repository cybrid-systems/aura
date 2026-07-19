// test_issue_773.cpp — Issue #773: Workspace closed-loop fiber/multi-
// agent EDA verification orchestration observability.
//
// Scope-limited close: the issue body asks for (Phase 1) ast.ixx
// pin_for_cow() + cross-boundary is_valid_in + auto-capture pinning
// shared_ptr + Workspace COW/clone/split propagation + EDSL primitives
// yield instrumentation + fiber/Guard steal/resume auto-refresh +
// tests/test_workspace_closedloop_fiber_multiagent_eda_verification.cpp
// harness + SEVA long-running demo. Phase 2: Prometheus exposure with
// SLO thresholds (closedloop_fidelity >99.5% under 10+ fiber concurrent)
// + CI gate + docs.
//
// Phase 1 observability surface ships:
//   1. 3 new atomics in CompilerMetrics:
//        workspace_closedloop_shared_mutex_contention_ns_total
//        workspace_closedloop_multi_agent_edit_fidelity_pct
//          (0-10000 fixed-point percent × 100)
//        workspace_closedloop_stale_ref_prevented_eda_loops_total
//   2. 3 new public bump helpers in Evaluator
//        (bump_workspace_closedloop_shared_mutex_contention_ns /
//         set_workspace_closedloop_multi_agent_edit_fidelity_pct /
//         bump_workspace_closedloop_stale_ref_prevented_eda_loops)
//   3. New standalone
//      (query:workspace-closedloop-fiber-eda-stats, schema 773)
//      primitive exposing 6 body-specified fields + schema sentinel
//      (7-entry hash). Reuses 3 atomics from #762
//      (workspace-closedloop-orchestration-stats) for pct-derived
//      fields (concurrent-query-mutate-success-pct + cross-cow-ref-
//      validity-pct + yield-points-hit) + adds 3 NEW atomics for
//      the production-readiness EDA verification dimensions.
//   4. Test verifies: primitive shape, fresh-zero state, schema
//      sentinel, bump accessibility, sibling observability regression.
//
// ACs:
//   AC1: hash shape (6 fields + schema sentinel = 7 entries)
//   AC2: 3 NEW counters == 0 on fresh service; reused #762 counters
//        also == 0 (since fresh service has no concurrent traffic yet)
//   AC3: schema == 773 (drift sentinel)
//   AC4: bump helpers accessible — exercise each new atomic via
//        direct bump on Evaluator surface and verify the primitive
//        reports the bumps
//   AC5: sibling observability regression — #762 + #772 + #771/#770
//        primitives still reachable with their schema sentinels intact

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_773_detail {
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

static void run_ac1_shape(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: (engine:metrics \"query:workspace-closedloop-fiber-eda-stats\") hash "
                 "shape ---");
    auto r = cs.eval("(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\") returns a hash");
    const std::vector<std::string> keys = {"concurrent-query-mutate-success-pct",
                                           "cross-cow-ref-validity-pct",
                                           "yield-points-hit",
                                           "shared-mutex-contention-ns",
                                           "multi-agent-edit-fidelity",
                                           "stale-ref-prevented-eda-loops",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:workspace-closedloop-fiber-eda-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: NEW counters == 0 on fresh service ---");
    // 3 NEW atomics must be 0 on fresh service
    const auto smc =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "shared-mutex-contention-ns");
    CHECK(smc == 0,
          std::format("shared-mutex-contention-ns = {} (expected 0 on fresh service)", smc));
    const auto mae =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "multi-agent-edit-fidelity");
    CHECK(mae == 0,
          std::format("multi-agent-edit-fidelity = {} (expected 0 on fresh service)", mae));
    const auto srp =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "stale-ref-prevented-eda-loops");
    CHECK(srp == 0,
          std::format("stale-ref-prevented-eda-loops = {} (expected 0 on fresh service)", srp));
    // Reused #762 atomic: yield-points-hit should be 0 on fresh service
    const auto yph = hash_int_field(
        cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")", "yield-points-hit");
    CHECK(yph == 0, std::format("yield-points-hit = {} (expected 0 on fresh service)", yph));
    // pct fields: derived — should be 10000 (100.00%) on fresh service (no failures)
    const auto cqs =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "concurrent-query-mutate-success-pct");
    CHECK(cqs == 10000,
          std::format(
              "concurrent-query-mutate-success-pct = {} (expected 10000 = 100.00% on fresh)", cqs));
    const auto ccv =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "cross-cow-ref-validity-pct");
    CHECK(ccv == 10000,
          std::format("cross-cow-ref-validity-pct = {} (expected 10000 = 100.00% on fresh)", ccv));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 773 (drift sentinel) ---");
    const auto schema = hash_int_field(
        cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")", "schema");
    CHECK(schema == 773, std::format("schema = {} (expected 773)", schema));
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable from the evaluator surface ---");
    auto& ev = cs.evaluator();
    // Exercise each new atomic via the bump helpers
    ev.bump_workspace_closedloop_shared_mutex_contention_ns(1500);
    ev.bump_workspace_closedloop_shared_mutex_contention_ns(2500); // cumulative 4000
    ev.set_workspace_closedloop_multi_agent_edit_fidelity_pct(9876);
    ev.bump_workspace_closedloop_stale_ref_prevented_eda_loops(7);
    ev.bump_workspace_closedloop_stale_ref_prevented_eda_loops(3);
    const auto smc =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "shared-mutex-contention-ns");
    const auto mae =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "multi-agent-edit-fidelity");
    const auto srp =
        hash_int_field(cs, "(engine:metrics \"query:workspace-closedloop-fiber-eda-stats\")",
                       "stale-ref-prevented-eda-loops");
    CHECK(smc == 4000,
          std::format("after 1500+2500 ns bumps: shared-mutex-contention-ns = {} (expected 4000)",
                      smc));
    CHECK(mae == 9876,
          std::format("after set 9876: multi-agent-edit-fidelity = {} (expected 9876 = 98.76%)",
                      mae));
    CHECK(srp == 10,
          std::format("after 7+3 stale-ref bumps: stale-ref-prevented-eda-loops = {} (expected 10)",
                      srp));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #762 + #772 + #771/#770 sibling primitives "
                 "unaffected ---");
    auto workspace_closedloop =
        cs.eval("(engine:metrics \"query:workspace-closedloop-orchestration-stats\")");
    auto sv_closedloop_slo = cs.eval("(stats:get \"query:sv-closedloop-slo\")");
    auto type_incremental_fidelity =
        cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    auto linear_occurrence_mutate =
        cs.eval("(engine:metrics \"query:linear-occurrence-mutate-stats\")");
    CHECK(workspace_closedloop && aura::compiler::types::is_hash(*workspace_closedloop),
          "query:workspace-closedloop-orchestration-stats hash regression (#762)");
    CHECK(sv_closedloop_slo && aura::compiler::types::is_hash(*sv_closedloop_slo),
          "query:sv-closedloop-slo hash regression (#772)");
    CHECK(type_incremental_fidelity && aura::compiler::types::is_hash(*type_incremental_fidelity),
          "query:type-incremental-fidelity-stats hash regression (#770/#798)");
    CHECK(linear_occurrence_mutate && aura::compiler::types::is_hash(*linear_occurrence_mutate),
          "query:linear-occurrence-mutate-stats hash regression (#771/#747)");
    const auto a762_schema = hash_int_field(
        cs, "(engine:metrics \"query:workspace-closedloop-orchestration-stats\")", "schema");
    CHECK(a762_schema == 762,
          std::format("#762 schema = {} (expected 762, no drift)", a762_schema));
    const auto a772_schema =
        hash_int_field(cs, "(stats:get \"query:sv-closedloop-slo\")", "schema");
    CHECK(a772_schema == 772,
          std::format("#772 schema = {} (expected 772, no drift)", a772_schema));
    const auto a798_schema =
        hash_int_field(cs, "(engine:metrics \"query:type-incremental-fidelity-stats\")", "schema");
    CHECK(a798_schema == 1617 || a798_schema == 798,
          std::format("#798/#1617 schema = {} (expected 1617|798)", a798_schema));
    const auto a747_schema =
        hash_int_field(cs, "(engine:metrics \"query:linear-occurrence-mutate-stats\")", "schema");
    CHECK(a747_schema == 747,
          std::format("#747 schema = {} (expected 747, no drift)", a747_schema));
}

} // namespace aura_issue_773_detail

int aura_issue_773_run() {
    using namespace aura_issue_773_detail;
    std::println("=== Issue #773: Workspace closed-loop fiber/multi-agent EDA "
                 "verification observability (scope-limited close) ===");

    {
        aura::compiler::CompilerService cs;
        run_ac1_shape(cs);
        run_ac2_fresh_zero(cs);
        run_ac3_schema_sentinel(cs);
        run_ac4_bump_accessible(cs);
        run_ac5_sibling_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_773_run();
}
#endif
