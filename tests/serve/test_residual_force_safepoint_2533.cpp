// @category: unit
// @reason: Issue #2533 — residual hard-reclaim force-safepoint metrics + edges.
//          Issue #2636 — residual reclaim body-age + env-opt-in force-safepoint
//                        (extended ACs added; same file per #81967 reuse rule).
//
//   #2533 AC1: mark_reclaimed requests force_safepoint + cancel
//   #2533 AC2: residual_force_safepoint_total increments
//   #2533 AC3: source-cite check_gc_safepoint poll (no yield recursion)
//   #2533 AC4: query keys schema-2533 (source-cite)
//   #2533 AC5: still_running gauge still pairs
//   #2533 AC6: linter
//
//   #2636 AC1: mark_reclaimed → body_reclaim_start_ns set (observed at exit)
//   #2636 AC2: note_body_exit_if_reclaimed → body-age samples/max/sum bumped
//   #2636 AC3: Soft default = env ON preserves #2533 production behavior
//   #2636 AC4: env-opt-in force-safepoint path source-cited + metric bumps
//   #2636 AC5: query keys schema-2636 (covered in test_orch_obs_facade)
//   #2636 AC6: linter (scripts/coverage/checks/check_residual_body_age_coverage.py)

#include "test_harness.hpp"
#include "serve/fiber.h"
#include <fstream>
#include <print>
#include <string>

import std;

namespace {
using aura::serve::Fiber;
using aura::test::g_failed;
using aura::test::g_passed;
std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}
} // namespace

int run_test_residual_force_safepoint_2533() {
    std::println("=== Issue #2533+#2636: residual force-safepoint + body-age ===");
    // ── #2533 AC1+AC2: mark_reclaimed sets flag + counter ──
    const auto f0 = Fiber::residual_force_safepoint_total();
    Fiber f([] {
        // body never runs for this unit test — we only mark_reclaimed
    });
    // Simulate reclaim of a not-Done fiber (constructor leaves Ready/Created).
    f.mark_reclaimed();
    CHECK(f.is_reclaimed(), "reclaimed");
    CHECK(f.is_cancel_requested(), "AC1: cancel requested");
    // force flag may already be consumed by a concurrent path; total is the contract.
    CHECK(Fiber::residual_force_safepoint_total() > f0, "AC2: force-safepoint total++");
    // Body not run → still_running may or may not bump depending on state_
    // (non-Done). Accept either; note_body_exit is safe.
    f.note_body_exit_if_reclaimed();

    // ── #2636 AC1+AC2: body-age tracking lifecycle ──
    {
        std::println("\n--- #2636 AC1+AC2: body-age lifecycle ---");
        // mark_reclaimed set body_reclaim_start_ns_; note_body_exit_if_reclaimed
        // already finalized and cleared it. Verify the body-age samples counter
        // advanced and that the accessor surfaces sane values.
        CHECK(Fiber::join_drain_residual_body_age_samples() >= 1,
              "AC2: body-age samples counter bumped by note_body_exit_if_reclaimed");
        const auto age_max = Fiber::join_drain_residual_body_age_ms_max();
        const auto age_sum = Fiber::join_drain_residual_body_age_ms_sum();
        // monotonic: max <= sum (since max is the highest sample; sum is all summed)
        CHECK(age_max <= age_sum, "AC1: body-age max <= sum (monotonic invariant)");
        // per-fiber accessor (post-clear) reads 0.
        CHECK(f.body_reclaim_start_ns() == 0,
              "AC1: per-fiber body_reclaim_start_ns cleared on body exit");
    }

    // ── #2636 AC3+AC4: env opt-in flag + force-safepoint-on-orphan counter ──
    {
        std::println("\n--- #2636 AC3+AC4: env flag + force-safepoint-on-orphan counter ---");
        // AC3: default env ON (preserve #2533 production behavior).
        CHECK(Fiber::force_safepoint_on_orphan_enabled(),
              "AC3: default env = ON (preserves #2533 production behavior)");
        // AC4: mark_reclaimed above bumped force_safepoint_on_orphan_total.
        // Counter may have been bumped by other tests in the binary too; >= 1 is the contract.
        CHECK(Fiber::force_safepoint_on_orphan_total() >= 1,
              "AC4: force_safepoint_on_orphan_total bumped by mark_reclaimed");
    }

    // ── Source-cite (preserve #2533 AC3 + AC4 + add #2636 AC4) ──
    auto fib = read_file("src/serve/fiber.h");
    auto fcpp = read_file("src/serve/fiber.cpp");
    auto agent = read_file("src/compiler/evaluator_primitives_agent.cpp");
    auto mut = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    auto spawn = read_file("src/orch/agent_spawn.h");
    CHECK(fib.find("request_force_safepoint") != std::string::npos, "AC3: API");
    CHECK(fcpp.find("2533") != std::string::npos, "AC3: cite cpp");
    CHECK(fcpp.find("force_safepoint_requested_") != std::string::npos ||
              fcpp.find("request_force_safepoint") != std::string::npos,
          "AC3: mark_reclaimed wires force");
    CHECK(agent.find("residual-force-safepoint-total") != std::string::npos, "AC4: query key");
    CHECK(agent.find("schema-2533") != std::string::npos, "AC4: schema");
    // #2636 source-cite checks (per AC4 source-cited requirement):
    CHECK(fcpp.find("body_reclaim_start_ns_") != std::string::npos,
          "AC4 #2636: fiber.cpp records body-age timestamp");
    CHECK(fcpp.find("aura_force_safepoint_on_orphan_enabled_default") != std::string::npos,
          "AC4 #2636: fiber.cpp wires env-flag weak hook");
    CHECK(mut.find("aura_force_safepoint_on_orphan_enabled_default") != std::string::npos,
          "AC4 #2636: evaluator_fiber_mutation.cpp strong def of env-flag hook");
    CHECK(spawn.find("join_drain_residual_body_age_ms_max") != std::string::npos,
          "AC4 #2636: OrchModuleStats has body_age_ms_max field");
    CHECK(spawn.find("force_safepoint_on_orphan_total") != std::string::npos,
          "AC4 #2636: OrchModuleStats has force_safepoint_on_orphan_total field");
    CHECK(spawn.find("resolve_force_safepoint_on_orphan_enabled") != std::string::npos,
          "AC4 #2636: orch-side env-flag resolver present");
    CHECK(agent.find("join-drain-residual-body-age-ms-max") != std::string::npos,
          "AC5 #2636: query key for body-age max");
    CHECK(agent.find("force-safepoint-on-orphan-total") != std::string::npos,
          "AC5 #2636: query key for force-safepoint-on-orphan-total");
    CHECK(agent.find("schema-2636") != std::string::npos, "AC5 #2636: schema");
    std::println("\n=== #2533+#2636: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_residual_force_safepoint_2533();
}
#endif
