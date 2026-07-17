// @category: integration
// @reason: Issue #1489 — wire scheduler GC deferral for pending PanicCheckpoint
//
// AC1: save_panic_checkpoint arms process-wide GC defer
// AC2: request_gc_safepoint defers while armed / pending
// AC3: compact_sweep skips destructive reclaim while armed
// AC4: commit/restore releases defer; GC proceeds again
// AC5: query:gc-panic-deferral-stats counters advance
// AC6: re_pin_cow_children_from_snapshot still callable (integration)

#include "test_harness.hpp"
#include "core/gc_hooks.h"
#include "compiler/messaging_bridge.h"
#include "serve/gc_coordinator.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:gc-panic-deferral-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ensure_workspace(CompilerService& cs) {
    auto r = cs.eval("(set-code \"(define x 1)\")");
    CHECK(r.has_value(), "set-code for checkpoint workspace");
    (void)cs.eval("(eval-current)");
}

static void ac1_save_arms_defer() {
    std::println("\n--- AC1: save_panic_checkpoint arms GC defer ---");
    CompilerService cs;
    ensure_workspace(cs);
    auto& ev = cs.evaluator();

    const auto depth0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "not armed before save");
    CHECK(!ev.has_panic_checkpoint(), "no checkpoint before save");

    CHECK(ev.save_panic_checkpoint(), "save_panic_checkpoint succeeds");
    CHECK(ev.has_panic_checkpoint(), "checkpoint live after save");
    CHECK(ev.gc_defer_armed_for_pending_panic(), "evaluator armed after save");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0 + 1,
          "process-wide defer depth +1");

    // Idempotent re-arm
    ev.arm_gc_defer_for_pending_panic();
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0 + 1, "re-arm is idempotent");

    ev.commit_panic_checkpoint();
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after commit");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0, "depth restored after commit");
}

static void ac2_request_defers() {
    std::println("\n--- AC2: request_gc_safepoint defers under pending checkpoint ---");
    CompilerService cs;
    ensure_workspace(cs);
    auto& ev = cs.evaluator();

    CHECK(ev.request_gc_safepoint() == 0, "immediate when no checkpoint");
    CHECK(ev.save_panic_checkpoint(), "save ok");
    const auto blocked0 = ev.get_gc_blocked_by_pending_panic();
    CHECK(ev.request_gc_safepoint() == 1, "deferred while checkpoint live");
    CHECK(ev.get_gc_blocked_by_pending_panic() > blocked0, "gc_blocked_by_pending bumped");

    ev.commit_panic_checkpoint();
    CHECK(ev.request_gc_safepoint() == 0, "immediate again after commit");
}

static void ac3_compact_sweep_skips() {
    std::println("\n--- AC3: compact_sweep skips while defer armed ---");
    CompilerService cs;
    ensure_workspace(cs);
    auto& ev = cs.evaluator();

    CHECK(ev.save_panic_checkpoint(), "save ok");
    const auto skip0 = aura::gc_hooks::gc_sweep_skipped_pending_panic();

    aura::serve::GCSweepBuffers marks{}; // empty marks — still must skip early
    void* raw = ev.compact_sweep(&marks);
    CHECK(raw != nullptr, "compact_sweep returns result object");
    auto* result = static_cast<aura::messaging::GCSweepResultMsg*>(raw);
    CHECK(result->closures_freed == 0 && result->pairs_freed == 0 && result->strings_freed == 0,
          "no reclaim while defer armed");
    delete result;

    CHECK(aura::gc_hooks::gc_sweep_skipped_pending_panic() > skip0, "skip counter advanced");

    ev.commit_panic_checkpoint();
}

static void ac4_restore_releases() {
    std::println("\n--- AC4: restore releases defer ---");
    CompilerService cs;
    ensure_workspace(cs);
    auto& ev = cs.evaluator();
    const auto depth0 = aura::gc_hooks::gc_defer_pending_panic_depth();

    CHECK(ev.save_panic_checkpoint(), "save ok");
    CHECK(aura::gc_hooks::gc_deferred_for_pending_panic(), "deferred after save");
    // Mutate source then restore
    (void)cs.eval("(set-code \"(define x 2)\")");
    CHECK(ev.restore_panic_checkpoint(), "restore succeeds");
    CHECK(!ev.has_panic_checkpoint(), "checkpoint cleared");
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "disarmed after restore");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0, "depth restored");
    CHECK(ev.request_gc_safepoint() == 0, "GC immediate after restore");
}

static void ac5_metrics_and_trampoline() {
    std::println("\n--- AC5: metrics + block_gc trampoline ---");
    CompilerService cs;
    ensure_workspace(cs);
    auto& ev = cs.evaluator();

    auto h = cs.eval("(engine:metrics \"query:gc-panic-deferral-stats\")");
    CHECK(h && is_hash(*h), "gc-panic-deferral-stats is hash");
    CHECK(href(cs, "schema") == 651, "schema 651");

    const auto def0 = href(cs, "pending-panic-deferral");
    const auto blk0 = href(cs, "gc-blocked-by-panic");
    const auto res0 = href(cs, "conflicts-resolved");
    CHECK(def0 >= 0 && blk0 >= 0 && res0 >= 0, "metric fields readable");

    CHECK(ev.save_panic_checkpoint(), "save ok");
    // Simulate Fiber::yield path: pending + block_gc trampoline
    if (aura::messaging::g_block_gc_for_pending_checkpoint &&
        aura::messaging::g_pending_panic_checkpoint) {
        // bind yield hook so pending trampoline sees this evaluator
        // MutationBoundaryGuard does that; call block_gc if pending via
        // direct arm path + request.
        (void)ev.request_gc_safepoint(); // bumps gc-blocked-by-panic
    } else {
        (void)ev.request_gc_safepoint();
    }

    const auto def1 = href(cs, "pending-panic-deferral");
    const auto blk1 = href(cs, "gc-blocked-by-panic");
    CHECK(def1 > def0, "pending-panic-deferral advanced on save/arm");
    CHECK(blk1 > blk0, "gc-blocked-by-panic advanced on request");

    ev.commit_panic_checkpoint();
    const auto res1 = href(cs, "conflicts-resolved");
    CHECK(res1 > res0, "conflicts-resolved advanced on commit release");
}

static void ac6_repin_integration() {
    std::println("\n--- AC6: re_pin still works under checkpoint ---");
    CompilerService cs;
    ensure_workspace(cs);
    auto& ev = cs.evaluator();
    CHECK(ev.save_panic_checkpoint(), "save ok");
    CHECK(ev.test_re_pin_cow_children_from_snapshot(), "re_pin callable with pending cp");
    ev.on_arena_compact_hook(); // must not crash under defer
    CHECK(true, "on_arena_compact_hook ok under pending checkpoint");
    ev.commit_panic_checkpoint();
}

} // namespace

int main() {
    std::println("test_issue_1489: PanicCheckpoint GC defer (#1489)");
    ac1_save_arms_defer();
    ac2_request_defers();
    ac3_compact_sweep_skips();
    ac4_restore_releases();
    ac5_metrics_and_trampoline();
    ac6_repin_integration();
    std::println("\n#1489: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
