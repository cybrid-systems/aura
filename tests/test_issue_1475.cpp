// @category: integration
// @reason: Issue #1475 — EnvFrame version_ dual-check helper
// (parallel to is_bridge_stale) for IR closure apply paths.
//
// Scope-limited close matching #1459 / #1470 / #1473 / #1474
// pattern. The actual hot-path wiring of the dual check into
// IR executor (apply_closure dual-path) + JIT aura_closure_call +
// closure_bridge_ callback + multi-fiber stress test is deferred
// to follow-up issues (see close comment for the list).
//
// This test verifies the new pure helper `is_env_frame_stale`
// (added next to `is_bridge_stale` in evaluator.ixx) behaves
// correctly across the 7 documented Invariant cases:
//
//   1. current_defuse_version == 0 → tracking inactive → false
//   2. env_id == NULL_ENV_ID → no env_id → false (legacy path)
//   3. env_id != NULL_ENV_ID + frame_version == 0 → strict stale
//   4. frame_version < current_defuse_version → stale (Issue #242)
//   5. frame_version == current_defuse_version → fresh
//   6. frame_version > current_defuse_version → defensive false
//
// And the integration with defuse_version_for_test /
// bump_defuse_version_for_test (public accessors at
// evaluator.ixx:2729-2730).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"

import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_1475_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++::aura::test::g_passed;                                                              \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++::aura::test::g_failed;                                                              \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

} // namespace aura_issue_1475_detail

int aura_issue_1475_run() {
    using namespace aura_issue_1475_detail;

    // Force strict default (don't inherit AURA_BRIDGE_EPOCH_LEGACY_TRUST
    // from a shell env that could mask the stale path).
    setenv("AURA_BRIDGE_EPOCH_LEGACY_TRUST", "0", 1);

    aura::compiler::CompilerService cs;
    auto& ev = cs.evaluator();

    constexpr std::uint64_t kSomeEnvId = 42; // arbitrary non-NULL id

    // ── AC1: helper is accessible through Evaluator instance ──
    // is_env_frame_stale is a static method; C++ allows calling
    // it through instance references (verify the export works).
    const bool accessible = ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 100, 200);
    CHECK(accessible == true,
          "is_env_frame_stale is accessible via Evaluator instance (static method works)");

    // ── AC2: current_defuse_version == 0 → false (tracking inactive) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 0, 0) == false,
          "current==0 with env_id!=NULL returns false (tracking inactive)");
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 100, 0) == false,
          "current==0 with non-zero frame_version returns false (tracking inactive)");

    // ── AC3: env_id == NULL_ENV_ID → false (closure without env_id) ──
    CHECK(ev.is_env_frame_stale(aura::compiler::NULL_ENV_ID, 0, 200) == false,
          "env_id==NULL_ENV_ID returns false (legacy / pre-SoA path)");
    CHECK(ev.is_env_frame_stale(aura::compiler::NULL_ENV_ID, 100, 200) == false,
          "env_id==NULL_ENV_ID with frame_version < current returns false");

    // ── AC4: env_id != NULL + frame_version == 0 + current != 0 → STALE (strict) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 0, 100) == true,
          "frame_version==0 with current>0 returns true (strict default)");

    // ── AC5: frame_version < current → stale (Issue #242 invariant) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 50, 100) == true,
          "frame_version<current returns true (stale capture)");
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 99, 100) == true,
          "frame_version=current-1 returns true (off-by-one check)");

    // ── AC6: frame_version == current → false (fresh) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 100, 100) == false,
          "frame_version==current returns false (fresh capture)");

    // ── AC7: frame_version > current → false (defensive; impossible in practice) ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), 150, 100) == false,
          "frame_version>current returns false (defensive)");

    // ── AC8: bump_defuse_version_for_test increments by 1 ──
    const auto before_bump = ev.defuse_version_for_test();
    ev.bump_defuse_version_for_test();
    const auto after_one_bump = ev.defuse_version_for_test();
    CHECK(after_one_bump == before_bump + 1,
          std::format("bump_defuse_version_for_test increments by 1 ({} -> {})", before_bump,
                      after_one_bump));

    // ── AC9: post-bump, frame with version == before_bump is stale ──
    // The frame was "captured" at before_bump (== pre-mutation defuse
    // version); after bumping, the current is after_one_bump and the
    // capture pre-dates it → stale.
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), before_bump,
                                after_one_bump) == true,
          "post-bump: frame with pre-bump version is stale");

    // ── AC10: post-bump, frame with version == after_one_bump is fresh ──
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), after_one_bump,
                                after_one_bump) == false,
          "post-bump: frame with same-as-current version is fresh");

    // ── AC11: 1000 bump iterations drive the helper deterministically ──
    // Stresses the helper across many bumps to verify no state leaks
    // and the math stays correct.
    for (int i = 0; i < 1000; ++i) {
        ev.bump_defuse_version_for_test();
    }
    const auto after_1001 = ev.defuse_version_for_test();
    CHECK(after_1001 == before_bump + 1 + 1000,
          std::format("1000 bump iterations produce expected count ({} expected {})", after_1001,
                      before_bump + 1 + 1000));
    // A frame with version = before_bump is now 1001 versions behind
    // → still stale.
    CHECK(ev.is_env_frame_stale(static_cast<std::uint32_t>(kSomeEnvId), before_bump, after_1001) ==
              true,
          "frame_version = pre-storm baseline is stale after 1001 bumps");

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int main() {
    return aura_issue_1475_run();
}
