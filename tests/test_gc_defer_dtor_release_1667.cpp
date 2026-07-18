// @category: unit
// @reason: Issue #1667 — ~Evaluator must release process-wide PanicCheckpoint
// GC defer so depth cannot leak when commit/restore is skipped (exception /
// early destroy mid-window).
//
//   AC1: arm raises process-wide depth; release restores it
//   AC2: ~Evaluator with armed flag restores depth (no leak)
//   AC3: commit releases before clear; depth restored; dtor is no-op
//   AC4: save then destroy without commit (CompilerService scope) restores depth
//   AC5: double release / dtor after commit is idempotent
//   AC6: request_gc_safepoint is immediate after dtor released mid-window

#include "test_harness.hpp"
#include "core/gc_hooks.h"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac1_arm_release_roundtrip() {
    std::println("\n--- AC1: arm/release roundtrip ---");
    Evaluator ev;
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "not armed initially");
    ev.arm_gc_defer_for_pending_panic();
    CHECK(ev.gc_defer_armed_for_pending_panic(), "armed after arm");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "depth +1");
    ev.release_gc_defer_for_pending_panic();
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after release");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth restored");
}

static void ac2_dtor_releases_armed() {
    std::println("\n--- AC2: ~Evaluator releases armed defer ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        Evaluator ev;
        ev.arm_gc_defer_for_pending_panic();
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "depth +1 while live");
        // no commit/restore — dtor must release
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0,
          "depth restored after ~Evaluator mid-window");
    CHECK(!aura::gc_hooks::gc_deferred_for_pending_panic() || d0 > 0,
          "no spurious process-wide defer from this Evaluator");
}

static void ac3_commit_then_dtor() {
    std::println("\n--- AC3: commit releases; dtor idempotent ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        Evaluator ev;
        ev.arm_gc_defer_for_pending_panic();
        CHECK(ev.gc_defer_armed_for_pending_panic(), "armed");
        ev.commit_panic_checkpoint();
        CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after commit");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth after commit");
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth after dtor (idempotent)");
}

static void ac4_cs_save_destroy_no_commit() {
    std::println("\n--- AC4: CompilerService save then destroy without commit ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        CompilerService cs;
        auto r = cs.eval("(set-code \"(define x 1)\")");
        CHECK(r.has_value(), "set-code ok");
        (void)cs.eval("(eval-current)");
        auto& ev = cs.evaluator();
        CHECK(ev.save_panic_checkpoint(), "save_panic_checkpoint");
        CHECK(ev.gc_defer_armed_for_pending_panic(), "armed after save");
        CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "depth +1 after save");
        // destroy CS / evaluator without commit or restore
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0,
          "depth restored after CS dtor without commit");
}

static void ac5_double_release_idempotent() {
    std::println("\n--- AC5: double release / arm idempotent ---");
    Evaluator ev;
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    ev.arm_gc_defer_for_pending_panic();
    ev.arm_gc_defer_for_pending_panic(); // idempotent
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0 + 1, "re-arm no double count");
    ev.release_gc_defer_for_pending_panic();
    ev.release_gc_defer_for_pending_panic(); // no-op
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "double release safe");
}

static void ac6_safepoint_after_dtor_release() {
    std::println("\n--- AC6: safepoint immediate after dtor mid-window release ---");
    const auto d0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    {
        Evaluator ev;
        ev.arm_gc_defer_for_pending_panic();
        CHECK(ev.request_gc_safepoint() == 1, "deferred while armed");
    }
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == d0, "depth clean");
    // Fresh evaluator must see process-wide defer clear (when d0==0).
    Evaluator ev2;
    if (d0 == 0) {
        CHECK(ev2.request_gc_safepoint() == 0, "immediate GC after prior dtor release");
    } else {
        CHECK(true, "skipped absolute defer check (pre-existing depth)");
    }
}

} // namespace

int main() {
    std::println("=== Issue #1667: PanicCheckpoint GC defer dtor exception-safety ===");
    ac1_arm_release_roundtrip();
    ac2_dtor_releases_armed();
    ac3_commit_then_dtor();
    ac4_cs_save_destroy_no_commit();
    ac5_double_release_idempotent();
    ac6_safepoint_after_dtor_release();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
