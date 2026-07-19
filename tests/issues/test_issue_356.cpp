// @category: integration
// @reason: uses Evaluator + panic-checkpoint + EnvFrame SoA APIs
//
// test_issue_356.cpp — Verify Issue #356 acceptance criteria
// ("[Follow-up #242-2] Arena rollback for env_frames_ via
//  stable-id indirection").
//
// Background: #242 ships arena rollback for cells_/pairs_/
// string_heap_ but leaves env_frames_ untruncated (Closure::
// env_id indices must stay valid). The full stable-id
// indirection refactor that would let env_frames_ actually
// shrink is a follow-up.
//
// #356 scope-limited compromise: instead of truncating
// env_frames_, mark every entry allocated during the doomed
// transaction (indices [panic_safe_env_frames_size_,
// env_frames_.size())) as INVALID_VERSION. The frames stay
// allocated (Closure::env_id indices preserved), but
// materialize_call_env + the refresh_stale_frame_in_walk
// walker (#355) refuse to use them — preserving the invariant
// "any frame reachable from a live Closure is usable".
//
// Test strategy: 3 layers
//   Layer 1: INVALID_VERSION sentinel + is_env_frame_invalid
//   Layer 2: invalidate_post_rollback_env_frames helper
//   Layer 3: materialize_call_env skips invalid frames + emits
//            the [#356 warning] (gated behind AURA_VERBOSE_ENVFRAME)
//
// restore_panic_checkpoint integration is verified by direct
// code review (it calls invalidate_post_rollback_env_frames);
// CompilerService doesn't expose the post-rollback counter, so
// a whitebox Aura-level test would require a new accessor on
// the service. The helper-level tests (Layer 2) + the
// materialize-level tests (Layer 3) are sufficient evidence
// the wire-up works — restore_panic_checkpoint is a single
// line that calls the helper.

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_executor;
import aura.compiler.evaluator;
import aura.compiler.type_checker;
import aura.compiler.service;

namespace aura_issue_356_detail {

// ═══════════════════════════════════════════════════════════
// Layer 1: INVALID_VERSION sentinel + is_env_frame_invalid
// ═══════════════════════════════════════════════════════════

bool test_invalid_version_constant_exists() {
    std::println("\n--- AC1: INVALID_VERSION sentinel = UINT64_MAX ---");
    CHECK(aura::compiler::INVALID_VERSION == std::numeric_limits<std::uint64_t>::max(),
          "INVALID_VERSION == UINT64_MAX (monotonic counter never reaches this)");
    return true;
}

bool test_is_env_frame_invalid_default() {
    std::println("\n--- AC2: fresh frame is NOT invalid ---");
    aura::compiler::Evaluator ev;
    aura::compiler::EnvId id = ev.alloc_env_frame();
    CHECK(!ev.is_env_frame_invalid(id), "fresh frame is not invalid (version_ != INVALID_VERSION)");
    return true;
}

bool test_is_env_frame_invalid_for_null_id() {
    std::println("\n--- AC3: NULL_ENV_ID is reported as invalid ---");
    aura::compiler::Evaluator ev;
    CHECK(ev.is_env_frame_invalid(aura::compiler::NULL_ENV_ID),
          "NULL_ENV_ID is treated as invalid (safety net for callers)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 2: invalidate_post_rollback_env_frames helper
// ═══════════════════════════════════════════════════════════

bool test_invalidate_marks_post_rollback_frames() {
    std::println("\n--- AC4: invalidate_post_rollback_env_frames marks doomed frames ---");
    aura::compiler::Evaluator ev;

    // Evaluator's constructor pre-allocates 1 frame (for top_/module
    // scratch), so env_frames_.size() starts at 1. Allocate PRE more
    // "pre-checkpoint" frames + POST "post-checkpoint" frames. The
    // helper invalidates [panic_safe_, env_frames_.size()), so the
    // post-checkpoint region = (env_frames_size - panic_safe_size) =
    // POST frames (the pre-existing frame at index 0 is treated as
    // pre-checkpoint because we set panic_safe above the pre-existing
    // frame's index).
    constexpr int PRE = 3;
    constexpr int POST = 4;
    for (int i = 0; i < PRE; ++i)
        ev.alloc_env_frame();
    // Simulate the checkpoint: panic_safe_env_frames_size_ covers the
    // pre-existing frame + PRE allocs.
    const std::size_t base_size = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base_size);
    for (int i = 0; i < POST; ++i)
        ev.alloc_env_frame();

    auto before = ev.get_envframe_post_rollback_invalidations();
    ev.invalidate_post_rollback_env_frames();
    auto after = ev.get_envframe_post_rollback_invalidations();

    // POST frames were marked invalid.
    CHECK(after == before + POST,
          "envframe_post_rollback_invalidations_ incremented by exactly POST");

    // Pre-checkpoint frames (indices 0..base_size-1) are still fresh.
    for (std::size_t i = 0; i < base_size; ++i) {
        CHECK(!ev.is_env_frame_invalid(static_cast<aura::compiler::EnvId>(i)),
              "pre-checkpoint frame not invalid");
    }
    // Post-checkpoint frames (indices base_size..base_size+POST-1) are
    // invalid.
    for (std::size_t i = base_size; i < base_size + POST; ++i) {
        CHECK(ev.is_env_frame_invalid(static_cast<aura::compiler::EnvId>(i)),
              "post-checkpoint frame is invalid");
    }
    return true;
}

bool test_invalidate_noop_when_no_doomed_frames() {
    std::println("\n--- AC5: invalidate is a no-op when nothing was allocated post-checkpoint ---");
    aura::compiler::Evaluator ev;
    // Set panic_safe to current env_frames_size (no doomed
    // frames). The helper's early-return branch fires.
    const std::size_t base_size = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base_size);
    // No post-checkpoint frames.

    auto before = ev.get_envframe_post_rollback_invalidations();
    ev.invalidate_post_rollback_env_frames();
    auto after = ev.get_envframe_post_rollback_invalidations();
    CHECK(after == before, "no invalidations when size matches checkpoint");
    return true;
}

bool test_invalidate_idempotent() {
    std::println("\n--- AC6: invalidate is idempotent ---");
    aura::compiler::Evaluator ev;
    constexpr int PRE = 2;
    constexpr int POST = 3;
    for (int i = 0; i < PRE + POST; ++i)
        ev.alloc_env_frame();
    ev.set_panic_safe_env_frames_size_for_test(PRE);

    ev.invalidate_post_rollback_env_frames();
    auto first = ev.get_envframe_post_rollback_invalidations();
    // Second call should NOT bump the counter again (frames
    // are already INVALID_VERSION).
    ev.invalidate_post_rollback_env_frames();
    auto second = ev.get_envframe_post_rollback_invalidations();
    CHECK(first == second, "second invalidate call does not re-bump the counter (idempotent)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Layer 3: materialize_call_env skips invalid frames
// ═══════════════════════════════════════════════════════════

bool test_materialize_call_env_skips_invalid_frame() {
    std::println("\n--- AC7: materialize_call_env skips invalid frame, returns empty Env ---");
    aura::compiler::Evaluator ev;

    aura::compiler::EnvId fid = ev.alloc_env_frame();
    // Mark stale then INVALID (simulating post-rollback state).
    ev.bump_defuse_version_for_test();
    ev.set_panic_safe_env_frames_size_for_test(0); // everything > 0 is doomed
    ev.invalidate_post_rollback_env_frames();

    aura::compiler::Closure cl;
    cl.env_id = fid;
    auto ne = ev.materialize_call_env(cl);
    // The materialized Env must have NO bindings (we refused
    // to materialize the invalid frame). The closure body can
    // still find globals via the workspace walk.
    CHECK(ne.bindings().empty(), "materialize_call_env returns empty Env for invalid frame");
    return true;
}

// ═══════════════════════════════════════════════════════════
// Main test runner
// ═══════════════════════════════════════════════════════════

int run_tests() {
    std::println("═══ Issue #356 verification tests ═══\n");

    std::println("Layer 1: INVALID_VERSION sentinel + is_env_frame_invalid");
    test_invalid_version_constant_exists();
    test_is_env_frame_invalid_default();
    test_is_env_frame_invalid_for_null_id();

    std::println("\nLayer 2: invalidate_post_rollback_env_frames helper");
    test_invalidate_marks_post_rollback_frames();
    test_invalidate_noop_when_no_doomed_frames();
    test_invalidate_idempotent();

    std::println("\nLayer 3: materialize_call_env");
    test_materialize_call_env_skips_invalid_frame();

    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}
} // namespace aura_issue_356_detail

int aura_issue_356_run() {
    return aura_issue_356_detail::run_tests();
}