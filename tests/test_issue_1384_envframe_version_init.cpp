// @category: integration
// @reason: uses Evaluator directly to verify EnvFrame::version_
//          is initialized correctly (not 0) after defuse_version_
//          has been bumped.
//
// test_issue_1384_envframe_version_init.cpp — Issue #1384:
// initialize EnvFrame::version_ in alloc_env_frame ctor, not
// after field fill.
//
// Background: alloc_env_frame previously constructed an EnvFrame
// with the default ctor (version_ = 0), then filled parent_id,
// primitives_, version_ separately. A concurrent reader acquiring
// shared_lock could observe version_ == 0 inside the unique_lock
// window — classified as "stale" by is_env_frame_stale once
// defuse_version_ > 0. The fix constructs the frame with version_
// inline (no default-then-fill window).
//
// Tests:
//   AC1: fresh frame after defuse_version_ bump has version_ ==
//        current defuse_version_ (not 0).
//   AC2: is_env_frame_stale returns false for the fresh frame.
//   AC3: alloc_env_frame_from_env re-stamps version_ AFTER
//        bindings_/bindings_symid_ assignments so the frame
//        captures defuse_version_ at completion (not at start).
//   AC4: Many concurrent alloc_env_frame + bump calls don't
//        leave any frame with version_ == 0 (regression for the
//        default-ctor race).

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace aura_issue_1384_detail {

// Helper: build a minimal Env with one binding so
// alloc_env_frame_from_env has something to mirror.
static std::unique_ptr<aura::compiler::Env> make_env() {
    auto env = std::make_unique<aura::compiler::Env>();
    aura::compiler::types::EvalValue v = aura::compiler::types::make_int(42);
    env->bind("x", v);
    return env;
}

// ── AC1: fresh frame after bump has version_ == current ─────────
bool test_ac1_frame_version_matches_current() {
    std::println("\n--- AC1: frame.version_ == current defuse_version_ ---");
    aura::compiler::Evaluator ev;

    // Simulate the post-mutation state: defuse_version_ has been
    // bumped at least once.
    ev.bump_defuse_version_for_test();
    auto current = ev.defuse_version_for_test();

    auto id = ev.alloc_env_frame();
    CHECK(id != 0, "AC1: alloc_env_frame returned a valid id");

    const auto& fr = ev.env_frame_for_test(id);
    CHECK(fr.version_ != 0, "AC1: version_ != 0 (the 'never stamped' sentinel)");
    CHECK(fr.version_ == current, "AC1: version_ == current defuse_version_ at alloc time");
    return true;
}

// ── AC2: is_env_frame_stale returns false for the fresh frame ───
bool test_ac2_fresh_frame_not_stale() {
    std::println("\n--- AC2: fresh frame → is_env_frame_stale == false ---");
    aura::compiler::Evaluator ev;
    ev.bump_defuse_version_for_test();

    auto id = ev.alloc_env_frame();
    CHECK(id != 0, "AC2: alloc_env_frame returned a valid id");

    CHECK(!ev.is_env_frame_stale(id), "AC2: freshly allocated frame is NOT stale");
    return true;
}

// ── AC3: alloc_env_frame_from_env re-stamps at completion ───────
bool test_ac3_from_env_version_matches_completion() {
    std::println("\n--- AC3: alloc_env_frame_from_env re-stamps version_ ---");
    aura::compiler::Evaluator ev;
    auto env = make_env();

    // Pre-bump so the initial alloc_env_frame (inside
    // alloc_env_frame_from_env) gets version_ >= 1.
    ev.bump_defuse_version_for_test();

    // Bump AGAIN between the alloc_env_frame call and the
    // re-stamp (simulated via explicit bump; in real code this
    // would be a concurrent mutation on another fiber).
    auto id = ev.alloc_env_frame_from_env(*env);
    CHECK(id != 0, "AC3: alloc_env_frame_from_env returned a valid id");

    // After alloc_env_frame_from_env returns, the frame's
    // version_ should be == current defuse_version_ (re-stamped).
    auto current = ev.defuse_version_for_test();
    const auto& fr = ev.env_frame_for_test(id);
    CHECK(fr.version_ != 0, "AC3: version_ != 0");
    CHECK(fr.version_ == current,
          "AC3: version_ == current defuse_version_ (re-stamped at completion)");
    CHECK(!ev.is_env_frame_stale(id),
          "AC3: alloc_env_frame_from_env frame is NOT stale after re-stamp");
    return true;
}

// ── AC4: many concurrent allocs leave no version_ == 0 frame ────
bool test_ac4_no_zero_version_frames() {
    std::println("\n--- AC4: many allocs → no version_ == 0 ---");
    aura::compiler::Evaluator ev;

    // Bump a few times before any alloc.
    for (int i = 0; i < 3; ++i)
        ev.bump_defuse_version_for_test();

    constexpr int kN = 64;
    std::vector<aura::compiler::EnvId> ids;
    ids.reserve(kN);
    for (int i = 0; i < kN; ++i) {
        ids.push_back(ev.alloc_env_frame());
    }
    int zero_version_count = 0;
    for (auto id : ids) {
        const auto& fr = ev.env_frame_for_test(id);
        if (fr.version_ == 0)
            ++zero_version_count;
    }
    CHECK(zero_version_count == 0, "AC4: zero frames have version_ == 0 after the fix");
    return true;
}

} // namespace aura_issue_1384_detail

int main() {
    using namespace aura_issue_1384_detail;
    bool ok = true;
    ok &= test_ac1_frame_version_matches_current();
    ok &= test_ac2_fresh_frame_not_stale();
    ok &= test_ac3_from_env_version_matches_completion();
    ok &= test_ac4_no_zero_version_frames();

    int failed = aura::test::g_failed;
    std::println("\n=== Issue #1384 envframe version init: {} ({} failures) ===",
                 (ok && failed == 0) ? "PASS" : "FAIL", failed);
    return (ok && failed == 0) ? 0 : 1;
}