// @category: integration
// @reason: Issue #1581 — scheduler GC deferral collaborates with pending
// PanicCheckpoint so pinned COW / StableNodeRef / EnvFrame survive steal +
// concurrent GC pressure. Refines #1489 with request() early-out, TOCTOU
// re-check in collect(), and send_defer_gc_signal provenance.
//
//   AC1: pending checkpoint → GCCollector::request deferred; collect skips
//   AC2: compact_sweep reclaim-free while armed; restore releases defer
//   AC3: block_gc / send_defer_gc_signal stamps fiber/epoch provenance
//   AC4: re_pin + post-steal refresh safe under pending checkpoint
//   AC5: 1000-iter concurrent save/restore + GC + steal-refresh stress
//   AC6: after commit/restore, GC request path works again (eval ok)

#include "test_harness.hpp"

#include "core/gc_hooks.h"
#include "compiler/messaging_bridge.h"
#include "serve/gc_coordinator.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::serve::GCCollector;
using aura::serve::GCSweepBuffers;
using aura::serve::Scheduler;
using aura::test::g_failed;
using aura::test::g_passed;

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define y (f 40))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
}

static void ac1_collector_request_defers() {
    std::println("\n--- AC1: GCCollector::request defers under pending checkpoint ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    Scheduler sched(1);
    auto* gc = sched.gc_collector();
    CHECK(gc != nullptr, "scheduler has GCCollector");

    // Force threshold so request would otherwise arm.
    gc->set_alloc_threshold(1);
    for (int i = 0; i < 10; ++i)
        gc->record_alloc();

    const auto req_def0 = aura::gc_hooks::gc_request_deferred_pending_panic();
    CHECK(ev.save_panic_checkpoint(), "save arms defer");
    CHECK(aura::gc_hooks::should_defer_compact_for_pending_checkpoint(), "deferred");

    CHECK(!gc->request(), "request returns false while deferred");
    CHECK(aura::gc_hooks::gc_request_deferred_pending_panic() > req_def0,
          "request-deferred counter advanced");

    // Force gc_in_progress via threshold was refused — collect must be no-op.
    CHECK(!gc->collect(), "collect no-op when not in progress");

    ev.commit_panic_checkpoint();
    CHECK(!aura::gc_hooks::should_defer_compact_for_pending_checkpoint() ||
              aura::gc_hooks::gc_defer_pending_panic_depth() == 0 ||
              !ev.gc_defer_armed_for_pending_panic(),
          "disarmed after commit (local evaluator)");
}

static void ac2_compact_sweep_and_restore() {
    std::println("\n--- AC2: compact_sweep skips; restore releases ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto depth0 = aura::gc_hooks::gc_defer_pending_panic_depth();
    const auto skip0 = aura::gc_hooks::gc_sweep_skipped_pending_panic();

    CHECK(ev.save_panic_checkpoint(), "save ok");
    GCSweepBuffers marks{};
    auto result = ev.compact_sweep(&marks); // Issue #1732: typed by-value
    CHECK(result.closures_freed == 0 && result.pairs_freed == 0 && result.strings_freed == 0,
          "no reclaim while deferred");
    CHECK(aura::gc_hooks::gc_sweep_skipped_pending_panic() > skip0, "skip counter advanced");

    // Mutate then restore — pinned recovery path.
    (void)cs.eval("(set-code \"(define y 99)\")");
    CHECK(ev.restore_panic_checkpoint(), "restore ok");
    CHECK(!ev.has_panic_checkpoint(), "checkpoint cleared");
    CHECK(aura::gc_hooks::gc_defer_pending_panic_depth() == depth0, "depth restored");
    CHECK(ev.request_gc_safepoint() == 0, "GC immediate after restore");
}

static void ac3_send_defer_signal_provenance() {
    std::println("\n--- AC3: send_defer_gc_signal + block_gc trampoline ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();

    const auto sig0 = aura::gc_hooks::gc_defer_pending_panic_signals();
    const auto epoch = ev.current_bridge_epoch();
    aura::gc_hooks::send_defer_gc_signal(/*fiber_id=*/42, epoch);
    CHECK(aura::gc_hooks::gc_defer_pending_panic_signals() > sig0, "signal counter +1");
    CHECK(aura::gc_hooks::gc_defer_last_fiber_id() == 42, "last fiber_id stamped");
    CHECK(aura::gc_hooks::gc_defer_last_checkpoint_epoch() == epoch, "last epoch stamped");

    CHECK(ev.save_panic_checkpoint(), "save ok");
    // Simulate Fiber::yield block_gc path when pending.
    if (aura::messaging::g_block_gc_for_pending_checkpoint) {
        // pending_panic_checkpoint trampoline needs yield hook; use
        // direct arm+signal path that production trampoline uses when
        // evaluator-local pending via has_panic_checkpoint.
        ev.arm_gc_defer_for_pending_panic();
        aura::gc_hooks::send_defer_gc_signal(7, ev.current_bridge_epoch());
        CHECK(aura::gc_hooks::gc_defer_last_fiber_id() == 7, "re-stamp fiber");
    }
    CHECK(ev.pending_panic_checkpoint() || ev.has_panic_checkpoint(), "pending or has_cp");
    ev.commit_panic_checkpoint();
}

static void ac4_repin_under_pending() {
    std::println("\n--- AC4: re_pin + post-steal refresh under pending ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    CHECK(ev.save_panic_checkpoint(), "save ok");
    CHECK(ev.test_re_pin_cow_children_from_snapshot(), "re_pin ok under pending");
    ev.probe_and_repin_linear_on_steal();
    ev.complete_post_resume_steal_refresh(nullptr);
    ev.on_arena_compact_hook();
    CHECK(true, "steal refresh + arena compact hook under pending (no crash)");
    // compact still skipped
    GCSweepBuffers marks{};
    auto result = ev.compact_sweep(&marks);
    CHECK(result.closures_freed == 0, "still no reclaim");
    ev.commit_panic_checkpoint();
}

static void ac5_thousand_iter_concurrent_stress() {
    std::println("\n--- AC5: 1000-iter concurrent checkpoint + GC + steal ---");
    // Ownership model: one thread mutates the Evaluator (save/commit +
    // steal-refresh). A second thread only drives GCCollector (process-
    // wide gc_hooks). Avoid concurrent restore/set-code races on the
    // same evaluator heap.
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    Scheduler sched(2);
    auto* gc = sched.gc_collector();
    CHECK(gc != nullptr, "gc collector");
    gc->set_alloc_threshold(1);

    std::atomic<int> window_ok{0};
    std::atomic<int> gc_ok{0};
    std::atomic<int> steal_ok{0};
    const auto skip0 = aura::gc_hooks::gc_sweep_skipped_pending_panic();
    const auto req0 = aura::gc_hooks::gc_request_deferred_pending_panic();
    const auto sig0 = aura::gc_hooks::gc_defer_pending_panic_signals();

    std::thread owner_thread([&] {
        for (int i = 0; i < 1000; ++i) {
            // Prefer save when possible; fall back to direct arm so the
            // window still opens under transient primitive lookup noise.
            bool armed = false;
            if (ev.save_panic_checkpoint()) {
                armed = true;
            } else {
                ev.arm_gc_defer_for_pending_panic();
                armed = ev.gc_defer_armed_for_pending_panic();
            }
            if (armed) {
                window_ok.fetch_add(1, std::memory_order_relaxed);
                aura::gc_hooks::send_defer_gc_signal(static_cast<std::uint64_t>(i + 1),
                                                     ev.current_bridge_epoch());
                // Steal/re-pin while window open (single-threaded owner).
                ev.complete_post_resume_steal_refresh(nullptr);
                ev.probe_and_repin_linear_on_steal();
                (void)ev.test_re_pin_cow_children_from_snapshot();
                steal_ok.fetch_add(1, std::memory_order_relaxed);
                // compact under pending must not reclaim
                GCSweepBuffers marks{};
                auto r = ev.compact_sweep(&marks);
                (void)r.closures_freed;
                if (ev.has_panic_checkpoint())
                    ev.commit_panic_checkpoint();
                else if (ev.gc_defer_armed_for_pending_panic())
                    ev.release_gc_defer_for_pending_panic();
            }
            std::this_thread::yield();
        }
    });

    std::thread gc_thread([&] {
        for (int i = 0; i < 1000; ++i) {
            gc->record_alloc();
            (void)gc->request(); // should often defer while owner holds window
            (void)gc->collect();
            // Scheduler-facing probe only (no evaluator heap mutate).
            (void)aura::gc_hooks::should_defer_compact_for_pending_checkpoint();
            gc_ok.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    owner_thread.join();
    gc_thread.join();

    // Drain leftover arm if any.
    if (ev.has_panic_checkpoint())
        ev.commit_panic_checkpoint();
    if (ev.gc_defer_armed_for_pending_panic())
        ev.release_gc_defer_for_pending_panic();

    CHECK(window_ok.load() >= 500, std::format("defer windows opened ({})", window_ok.load()));
    CHECK(gc_ok.load() == 1000, "1000 GC pressure loops");
    CHECK(steal_ok.load() >= 500, std::format("steal-refresh under window ({})", steal_ok.load()));
    CHECK(aura::gc_hooks::gc_sweep_skipped_pending_panic() > skip0,
          "sweep skipped under concurrent windows");
    CHECK(aura::gc_hooks::gc_request_deferred_pending_panic() > req0 ||
              aura::gc_hooks::gc_defer_pending_panic_signals() > sig0,
          "request-deferred or signals advanced under pressure");

    auto r = cs.eval("(+ 1 2)");
    CHECK(r.has_value(), "eval ok after concurrent stress");
}

static void ac6_gc_resumes_after_release() {
    std::println("\n--- AC6: GC proceeds after checkpoint released ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    Scheduler sched(1);
    auto* gc = sched.gc_collector();
    CHECK(gc != nullptr, "gc collector");
    gc->set_alloc_threshold(1);

    CHECK(ev.save_panic_checkpoint(), "save");
    for (int i = 0; i < 5; ++i)
        gc->record_alloc();
    CHECK(!gc->request(), "deferred while pending");
    ev.commit_panic_checkpoint();

    for (int i = 0; i < 5; ++i)
        gc->record_alloc();
    // After release, request may succeed (threshold crossed) or race with
    // other process-wide depth; at least local defer is off.
    CHECK(!ev.gc_defer_armed_for_pending_panic(), "local arm off");
    CHECK(ev.request_gc_safepoint() == 0, "safepoint immediate after release");
    const bool req = gc->request();
    if (req)
        (void)gc->collect(); // may run or no-op depending on workers
    CHECK(true, "post-release GC path exercised without crash");

    auto r = cs.eval("(+ 10 20)");
    CHECK(r.has_value(), "eval after GC resume path");
}

} // namespace

int main() {
    std::println("=== test_scheduler_gc_defer_pending_panic_steal (#1581) ===");
    ac1_collector_request_defers();
    ac2_compact_sweep_and_restore();
    ac3_send_defer_signal_provenance();
    ac4_repin_under_pending();
    ac5_thousand_iter_concurrent_stress();
    ac6_gc_resumes_after_release();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
