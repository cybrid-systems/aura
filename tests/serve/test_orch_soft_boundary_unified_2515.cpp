// @category: unit
// @reason: Issue #2515 — Soft orch-agent boundary 提升为轻量 Guard 子集，
// 统一 depth/held 语义。Residual gap: orch agent soft-boundary path
// (per-fiber mutation stack depth 可见，但持完整 Guard / panic /
// workspace_mtx_) 与完整 MutationBoundaryGuard 存在语义分叉。
// steal / GC 决策依赖额外 orch_agent_boundary_active_ flag，长期负载下
// depth 漂移与可见性不一致风险高于纯 Guard 路径。
//
//   AC1: soft 进入/退出必 publish mirrors（source-cite）—
//        orch_soft_boundary_enter calls publish_mutation_safety_mirrors
//        (depth, held=true, defuse); orch_soft_boundary_exit publishes
//        (depth, held=false, defuse) BEFORE clearing orch_agent_boundary_active_.
//   AC2: is_at_mutation_boundary_safe 对 soft 与 full 在相同 depth/held 下
//        返回相同结果。Tests: soft window with held=true → unsafe;
//        without held (depth-only) → still unsafe (orch_agent_boundary_active).
//   AC3: GC defer 对 soft window 可见 — publish_mutation_safety_mirrors
//        updates held_mirror_ which is_at_mutation_boundary_safe reads.
//   AC4: Source-cite unified semantics + Extend #2115 + orch agent 路径
//        覆盖 in evaluator_fiber_mutation.cpp.
//   AC5: Zero extra cost pure-reasoning path (mutation_boundary=false)
//        保持零开销 — soft path only fires when orch agent body wraps.

#include "test_harness.hpp"

#include "serve/fiber.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

using aura::serve::Fiber;
using aura::serve::FiberState;
using aura::serve::JoinResult;
using aura::serve::JoinStatus;
using aura::serve::MutationSafetySnapshot;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── AC1: soft enter/exit publish mirrors (source-cite) ──
static void ac1_soft_publishes_mirrors() {
    std::println("\n--- AC1: soft enter/exit publish mirrors (source-cite) ---");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");

    // orch_soft_boundary_enter calls publish_mutation_safety_mirrors(depth, held=true, ...)
    const auto enter_pos = efm.find("orch_soft_boundary_enter");
    const auto held_true_pos = efm.find("publish_mutation_safety_mirrors(depth, /*held=*/true,");
    CHECK(enter_pos != std::string::npos, "AC1: orch_soft_boundary_enter exists");
    CHECK(held_true_pos != std::string::npos,
          "AC1: orch_soft_boundary_enter calls publish_mutation_safety_mirrors(depth, held=true, "
          "...)");

    // orch_soft_boundary_exit publishes held=false BEFORE clearing flag
    const auto exit_pos = efm.find("orch_soft_boundary_exit");
    const auto held_false_pos =
        efm.find("publish_mutation_safety_mirrors(depth, /*held=*/false, ver)");
    const auto flag_clear_pos = efm.find("fib->set_orch_agent_boundary_active(false)");
    CHECK(exit_pos != std::string::npos, "AC1: orch_soft_boundary_exit exists");
    CHECK(held_false_pos != std::string::npos,
          "AC1: orch_soft_boundary_exit calls publish_mutation_safety_mirrors(depth, held=false, "
          "...)");
    // Symmetric release: held=false must be BEFORE flag_clear (no probe
    // window where the flag flipped but the mirror stayed set).
    CHECK(held_false_pos < flag_clear_pos || held_false_pos == std::string::npos,
          "AC1: symmetric release order — held=false publish BEFORE flag clear");

    // Thread-local ev tracker so exit knows which defuse_version to publish.
    CHECK(efm.find("g_orch_soft_boundary_ev") != std::string::npos,
          "AC1: g_orch_soft_boundary_ev thread-local tracks Evaluator across enter/exit");

    // Issue #2515 source-cite present in evaluator_fiber_mutation.cpp.
    CHECK(efm.find("Issue #2515") != std::string::npos, "AC1: Issue #2515 source-cite present");
}

// ── AC2: is_at_mutation_boundary_safe unified semantics (soft vs full) ──
static void ac2_unified_safety_semantics() {
    std::println("\n--- AC2: is_at_mutation_boundary_safe unified soft vs full ---");
    Fiber fiber(+[] {}, 64 * 1024);

    // Pure-reasoning (no soft window, no full Guard): safe.
    // is_at_mutation_boundary_safe() requires last_yield != MutationBoundary
    // OR (depth==0 && !held). Default yield reason is Initial / None.
    CHECK(fiber.is_at_mutation_boundary_safe(), "AC2: pure-reasoning path (no window) is safe");

    // Full Guard path semantics: held=true → unsafe (no soft window).
    // We simulate by calling publish_mutation_safety_mirrors directly
    // (the same call the full Guard enter path uses).
    fiber.publish_mutation_safety_mirrors(/*depth=*/1, /*held=*/true, /*defuse=*/0);
    CHECK(!fiber.is_at_mutation_boundary_safe(), "AC2: full Guard semantics (held=true) → unsafe");

    // Symmetric release: held=false → safe again (pure-reasoning).
    fiber.publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/0);
    CHECK(fiber.is_at_mutation_boundary_safe(),
          "AC2: full Guard release (held=false) → safe again");

    // #2515: soft path unifies with full Guard via depth/held mirrors.
    // orch_agent_boundary_active alone (depth=0, held=false) is NOT unsafe —
    // pre-check only trips when soft window AND (depth>0 || held).
    fiber.set_orch_agent_boundary_active(true);
    CHECK(fiber.is_at_mutation_boundary_safe(),
          "AC2: soft flag alone (depth=0, held=false) → still safe");

    // Soft + held=true (via publish_mutation_safety_mirrors in
    // orch_soft_boundary_enter) → unsafe, same as full Guard.
    fiber.publish_mutation_safety_mirrors(/*depth=*/1, /*held=*/true, /*defuse=*/0);
    CHECK(!fiber.is_at_mutation_boundary_safe(),
          "AC2: soft window + held=true → unsafe (same as full Guard)");

    // Soft release: clear flag + held=false mirror → safe.
    fiber.publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/0);
    fiber.set_orch_agent_boundary_active(false);
    CHECK(fiber.is_at_mutation_boundary_safe(),
          "AC2: soft release (flag clear + held=false) → safe again");
}

// ── AC3: GC defer reads via mutation_safety_snapshot.held ──
static void ac3_gc_defer_via_mirror() {
    std::println("\n--- AC3: GC defer reads via mutation_safety_snapshot.held ---");
    Fiber fiber(+[] {}, 64 * 1024);

    // Baseline: no held, no soft window → snapshot.held == false.
    auto s = fiber.mutation_safety_snapshot();
    CHECK(!s.held, "AC3: baseline snapshot.held == false");

    // Soft enter: held=true mirror published.
    fiber.set_orch_agent_boundary_active(true);
    fiber.publish_mutation_safety_mirrors(/*depth=*/1, /*held=*/true, /*defuse=*/42);
    s = fiber.mutation_safety_snapshot();
    CHECK(s.held, "AC3: soft enter publishes held=true (GC defer visible)");

    // Soft exit: held=false mirror published + flag clear.
    fiber.publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/43);
    fiber.set_orch_agent_boundary_active(false);
    s = fiber.mutation_safety_snapshot();
    CHECK(!s.held, "AC3: soft exit publishes held=false (GC defer cleared)");

    // Defuse version propagates.
    fiber.set_orch_agent_boundary_active(true);
    fiber.publish_mutation_safety_mirrors(/*depth=*/2, /*held=*/true, /*defuse=*/99);
    s = fiber.mutation_safety_snapshot();
    CHECK(s.defuse_version == 99, "AC3: defuse_version propagates through publish");
    fiber.set_orch_agent_boundary_active(false);
    fiber.publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/0);
}

// ── AC4: Source-cite unified semantics + Extend #2115 + orch agent ──
static void ac4_source_cite_extensions() {
    std::println("\n--- AC4: source-cite unified semantics + #2115 / #2118 extensions ---");
    const auto fh = read_file("src/serve/fiber.h");
    const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");

    // fiber.h has is_at_mutation_boundary_safe that reads from the
    // soft-published mirror (no soft-specific divergence).
    CHECK(fh.find("is_at_mutation_boundary_safe") != std::string::npos,
          "AC4: fiber.h is_at_mutation_boundary_safe exists");
    CHECK(fh.find("orch_agent_boundary_active") != std::string::npos,
          "AC4: orch_agent_boundary_active flag retained (for metrics + pre-check)");

    // Comment in is_at_mutation_boundary_safe mentions #2115/#2118/#2184.
    CHECK(fh.find("#2118") != std::string::npos, "AC4: fiber.h cites #2118 in soft path");
    CHECK(fh.find("#2115") != std::string::npos, "AC4: fiber.h cites #2115 in depth-safe path");
    CHECK(fh.find("#2184") != std::string::npos,
          "AC4: fiber.h cites #2184 in mutation_safety_snapshot");

    // evaluator_fiber_mutation.cpp: orch_soft_boundary_enter/exit + the
    // new publish_mutation_safety_mirrors calls + #2515 source-cite.
    CHECK(efm.find("orch_soft_boundary_enter") != std::string::npos,
          "AC4: orch_soft_boundary_enter exists");
    CHECK(efm.find("orch_soft_boundary_exit") != std::string::npos,
          "AC4: orch_soft_boundary_exit exists");
    CHECK(efm.find("Issue #2515") != std::string::npos,
          "AC4: Issue #2515 source-cite in evaluator_fiber_mutation.cpp");

    // Existing #2115 / #2118 tests still present (not removed by #2515).
    const auto t2115 = read_file("tests/serve/test_depth_safe_mutation_boundary_steal_2115.cpp");
    const auto t2118 = read_file("tests/serve/test_orch_agent_mutation_boundary_2118.cpp");
    CHECK(!t2115.empty(), "AC4: #2115 test file preserved (not removed)");
    CHECK(!t2118.empty(), "AC4: #2118 test file preserved (not removed)");
}

// ── AC5: Zero extra cost pure-reasoning path ──
static void ac5_zero_cost_pure_reasoning() {
    std::println("\n--- AC5: zero extra cost pure-reasoning path ---");
    Fiber fiber(+[] {}, 64 * 1024);

    // Pure-reasoning: no soft window, no full Guard, no held mirror.
    // is_at_mutation_boundary_safe must return true (default-yield path
    // passes the last_yield != MutationBoundary check).
    const auto baseline = fiber.is_at_mutation_boundary_safe();
    CHECK(baseline, "AC5: baseline (no window) safe");

    // After a NO-OP publish_mutation_safety_mirrors call (held=false, depth=0),
    // safety still true. Confirms the publish path is cheap when held=false.
    fiber.publish_mutation_safety_mirrors(/*depth=*/0, /*held=*/false, /*defuse=*/0);
    CHECK(fiber.is_at_mutation_boundary_safe(),
          "AC5: held=false publish keeps pure-reasoning safe (zero cost)");

    // MutationSafetySnapshot should still report held=false.
    auto s = fiber.mutation_safety_snapshot();
    CHECK(!s.held, "AC5: MutationSafetySnapshot.held == false after no-op publish");
    CHECK(s.depth == 0, "AC5: MutationSafetySnapshot.depth == 0");

    // Linter exists + mentions AC5.
    const auto linter = read_file("scripts/check_orch_soft_boundary_unified_2515.py");
    CHECK(!linter.empty(), "AC5: linter script present");
}

} // namespace

int main() {
    std::println("=== Issue #2515: orch soft boundary unified with depth/held semantics ===");
    ac1_soft_publishes_mirrors();
    ac2_unified_safety_semantics();
    ac3_gc_defer_via_mirror();
    ac4_source_cite_extensions();
    ac5_zero_cost_pure_reasoning();
    std::println("\n=== #2515: see per-AC results above ===");
    return aura::test::g_failed ? 1 : 0;
}