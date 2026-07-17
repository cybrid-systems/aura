#ifndef AURA_MESSAGING_BRIDGE_H
#define AURA_MESSAGING_BRIDGE_H

#include <cstddef>
#include <string>
#include <optional>
#include <functional>
#include <mutex>

// ── Messaging Bridge ───────────────────────────────────────────
//
// Cross-module function pointers for inter-agent messaging.
// Set by CompilerService (module aura.compiler.service) and
// read by Evaluator primitives (module aura.compiler.evaluator).
//
// This header is intentionally NOT in a module to avoid circular
// dependency issues.

namespace aura::messaging {

// Fiber blocking callback — set by serve_async.cpp, used by pop_message
// for async recv yield. Null means non-blocking fallback.
extern void (*g_fiber_block)();

// Function pointer types
using SendFn = bool (*)(const std::string& target, const std::string& msg);
using RecvFn = std::optional<std::string> (*)(int timeout_ms);
using MyIdFn = std::string (*)();

// Global bridge — set once by CompilerService during construction
struct MessagingBridge {
    SendFn send = nullptr;
    RecvFn recv = nullptr;
    MyIdFn my_id = nullptr;
};

// The global bridge instance
extern MessagingBridge g_messaging_bridge;

// Fiber spawn — set by serve_async.cpp, used by evaluator
// Returns 0 on failure, non-zero fiber ID on success
// Changed from raw function pointer to std::function to allow lambda captures
using FiberSpawnFn = std::function<int64_t(std::function<void()>)>;
extern FiberSpawnFn g_fiber_spawn;

// Issue #119: Fiber lookup by ID — set by serve_async.cpp.
// Returns the Fiber* for a given fiber ID, or nullptr if no
// such fiber exists. Used by the proper-blocking fiber:join
// (Issue #119) to check if a target fiber is already done and
// to register a joiner for the target's completion.
//
// Opaque void* return type to keep the bridge header non-module
// (Fiber is exported from aura::serve, which would create a
// circular import if the bridge imported it). The actual
// aura::serve::Fiber* is reinterpret_cast'd at the call site.
using FiberLookupFn = void* (*)(int64_t fiber_id);
extern FiberLookupFn g_fiber_lookup;

// Fiber yield — called by (fiber:yield) primitive
// Does a non-blocking yield (fiber stays Ready, scheduler re-enqueues)
using FiberYieldFn = void (*)();
extern FiberYieldFn g_fiber_yield;

// Fiber yield at mutation boundary — called by mutate:* and eval-current
// primitives before/after mutation operations. This ensures the fiber is
// at a safe point for the scheduler to steal (Issue #31).
// Set by serve_async.cpp when the fiber scheduler is active.
// nullptr when not in serve mode.
using FiberYieldMutationFn = void (*)();
extern FiberYieldMutationFn g_fiber_yield_mutation_boundary;

// Fiber SET yield reason to MutationBoundary (lightweight) —
// called by (mutate:atomic-batch) on Guard entry so work-
// stealing decisions (Fiber::is_stealable()) see this fiber
// as being at a mutation boundary. The "lightweight" part:
// unlike g_fiber_yield_mutation_boundary (which actually
// yields the fiber), this hook only sets the field, no
// yield. Set by serve_async.cpp when the fiber scheduler is
// active. nullptr when not in serve mode (no-op then).
// Issue #396 Phase 1.
using FiberSetYieldReasonMutationFn = void (*)();
extern FiberSetYieldReasonMutationFn g_fiber_set_yield_reason_mutation_boundary;

// Fiber join — wait for a fiber to complete and return its result (future use)
// Currently a placeholder — evaluator handles fiber:join internally.
using FiberJoinFn = void (*)();
extern FiberJoinFn g_fiber_join;

// Session create — set by serve_async.cpp, used by evaluator
// Returns true on success
using SessionCreateFn = bool(const std::string& name);
extern std::function<SessionCreateFn>* g_session_create;

// Session list — return all active session IDs
// Returns empty vector if no sessions or not in serve mode
using SessionListFn = std::vector<std::string> (*)();
extern SessionListFn g_session_list;


// Access a CompilerService's mailbox through a void*.
// Since evaluator can't import the CompilerService module,
// we provide a function that the CompilerService registers.
using MailboxReadFn = std::optional<std::string> (*)(void* compiler_service, int timeout_ms);
using MailboxSenderFn = std::string (*)(void* compiler_service); // sender of last read
using MailboxCountFn = std::size_t (*)(void* compiler_service);
using SessionIdFn = std::string (*)(void* compiler_service);
using SessionExistsFn = bool (*)(const std::string& id);

// These are set by CompilerService during construction
extern MailboxReadFn g_mailbox_read;
extern MailboxSenderFn g_mailbox_last_sender;
extern MailboxCountFn g_mailbox_count;
extern SessionIdFn g_session_id;
extern SessionExistsFn g_session_exists;
extern void* g_current_compiler_service; // set before each eval in serve

// Arena reset callback — set by CompilerService, called by Evaluator
// to reclaim arena memory between benchmark tasks.
// Takes a pointer to the service, returns void.
using ResetArenaFn = void (*)(void* compiler_service);
extern ResetArenaFn g_reset_arena;

// Async HTTP callback — set by serve_async.cpp, used by http-post primitive.
// When set, http-post uses thread + eventfd for non-blocking HTTP.
// Returns the response string (or "" on failure).
using HttpPostAsyncFn = std::string (*)(const std::string& url, const std::string& body,
                                        const std::string& auth);
extern HttpPostAsyncFn g_http_post_async;

// Thread pool enqueue — set by serve_async.cpp, used by thread_pool:enqueue primitive.
// Enqueues a blocking task to the background thread pool.
// fn runs on a pool thread; wake_evfd receives 1 on completion.
// The wake_evfd must already be registered with the scheduler's epoll.
using ThreadPoolEnqueueFn = void (*)(std::function<void()> fn, int wake_evfd);
extern ThreadPoolEnqueueFn g_thread_pool_enqueue;

// Async eval — set by serve_async.cpp. Evaluates code in the thread pool
// and returns the result as a string. If unavailable, callers fall back
// to synchronous eval.
using EvalAsyncFn = std::string (*)(const std::string& code);
extern EvalAsyncFn g_eval_async;

// Scheduler metrics — set by serve_async.cpp (Issue #32).
// Returns scheduler performance counters as a JSON string.
// nullptr when not in serve-async mode.
using GetMetricsFn = std::function<std::string()>;
extern GetMetricsFn g_get_scheduler_metrics;

// Reset scheduler metrics — set by serve_async.cpp.
// Resets all counters to zero. nullptr when not in serve-async mode.
using ResetMetricsFn = std::function<void()>;
extern ResetMetricsFn g_reset_scheduler_metrics;

// Fiber affinity — set by serve_async.cpp (P2).
// Pins the current fiber to a specific worker thread.
// nullptr when not in serve-async mode.
using FiberSetAffinityFn = void (*)(int worker_id);
extern FiberSetAffinityFn g_fiber_set_affinity;

// GC root flush — set by serve_async.cpp (P2 Phase 2).
// Each evaluator registers a callback that fills a GCRootSet
// with reachable roots (string_heap indices, pair indices, etc.).
// The GC coordinator calls this during the root collection phase.
// This uses an opaque void* to avoid circular includes between
// messaging_bridge.h (in non-module translator) and gc_coordinator.h.
using GCRootFlushFn = std::function<void(void* root_set_out)>;
extern GCRootFlushFn g_gc_flush_root_set;

// GC collect — set by serve_async.cpp (P2).
// Triggers a full GC cycle on the scheduler's GC collector.
// The evaluator calls this instead of blindly clearing heaps.
// If no GC collector is available (stdin mode), gc-heap falls
// back to the original clear behavior. nullptr when not available.
using GCCollectFn = std::function<bool()>;
extern GCCollectFn g_gc_collect;

// GC sweep result message — the POD that flows from
// serve_async.cpp's g_gc_sweep callback back to the GC collector.
// Both the Evaluator's compact_sweep and serve_async.cpp define
// a local struct with the same layout (verified by static_assert
// in evaluator_fiber_mutation.cpp). We use void* in the function signature
// so the messaging bridge can be a non-module .h and the layout
// stays an implementation detail.

// GC sweep — set by serve_async.cpp (Issue #113 Phase 3).
// Called by the GC collector during the sweep phase. The
// callback should erase unmarked entries from the active
// Evaluator's vector heaps and return how many were freed.
//
// `sweep_buffers_out` is an opaque `aura::serve::GCSweepBuffers*`
// (cast at the call site to keep the include surface minimal).
//
// Return is an opaque `void*` — heap-allocated by the callback
// (GCSweepResultMsg*, owned and `delete`-d by the caller).
//
// Issue #963: single layout definition here — both evaluator_gc.cpp
// and serve_async.cpp MUST use this struct (no local duplicates).
struct GCSweepResultMsg {
    std::size_t strings_freed = 0;
    std::size_t pairs_freed = 0;
    std::size_t closures_freed = 0;
    std::size_t fiber_results_freed = 0;
};
// Opaque PassThru for mark vectors (matches GCSweepBuffers field order).
struct GCSweepPassThru {
    const void* string_marks = nullptr; // MarkBitVector*
    const void* pair_marks = nullptr;
    const void* closure_marks = nullptr;
};
using GCSweepFn = std::function<void*(void* sweep_buffers_out)>;
extern GCSweepFn g_gc_sweep;

// Evaluator heap mutex — set by evaluator (P2).
// Protects string_heap_, pairs_, closures_ during GC.
// nullptr when not in serve-async mode or not initialized.
using HeapMutexFn = std::function<std::mutex&()>;
extern HeapMutexFn g_heap_mutex;

// Issue #285: Evaluator::flush_mutation_boundary indirection.
// Set by evaluator at module init; called by Fiber::yield right
// before swapcontext when the yield reason is MutationBoundary.
// nullptr means "no active evaluator" (test-binary / no-mutation
// paths), in which case Fiber::yield treats it as a no-op.
using FlushMutationBoundaryFn = void (*)();
extern FlushMutationBoundaryFn g_flush_mutation_boundary;

// Issue #354: "yield while holding a mutation boundary"
// check. Returns true when an outermost MutationBoundaryGuard
// is currently alive (i.e. the per-Evaluator atomic flag
// `mutation_boundary_held_` is true). Set by evaluator at
// module init; called by Fiber::yield right before swapcontext.
// In debug builds, Fiber::yield asserts when the check returns
// true (a yield-while-holding is a programmer error). In
// release builds, Fiber::yield logs a warning + continues.
// nullptr means "no active evaluator" (test-binary), in which
// case Fiber::yield treats it as a no-op (no check).
using MutationBoundaryHeldFn = bool (*)();
extern MutationBoundaryHeldFn g_mutation_boundary_held;

// Issue #453: Panic Checkpoint lifecycle hooks across fiber
// migration. Three orthogonal indirection points:
//
//   1. g_pending_panic_checkpoint() — returns true if a MutationBoundaryGuard
//      is currently active AND has captured a panic checkpoint (i.e.
//      `has_pending_checkpoint() == true` on the outermost guard).
//      Called by Fiber::yield(MutationBoundary) and Fiber::resume()
//      to decide whether a checkpoint transfer or GC defer is needed.
//   2. g_transfer_panic_checkpoint() — called by Fiber::resume() after
//      the g_fiber_resume_validate_ hook (and after the swapcontext
//      return). Bumps `panic_checkpoint_transfer_count_` and re-stamps
//      any per-fiber storage. No-op when no pending checkpoint.
//   3. g_block_gc_for_pending_checkpoint() — called by Fiber::yield
//      when reason == MutationBoundary AND a pending checkpoint
//      exists. Issue #1489: arms process-wide GC defer
//      (gc_hooks::g_gc_defer_pending_panic_depth), bumps
//      `gc_blocked_by_pending_panic_` + metrics, so
//      GCCollector::collect / compact_sweep skip destructive
//      reclaim until commit/restore releases the arm.
//
// All three are set by evaluator_fiber_mutation.cpp at static init.
// nullptr means "not in evaluator module context" — callers
// (fiber.cpp) treat each as a no-op.
using PendingPanicCheckpointFn = bool (*)();
extern PendingPanicCheckpointFn g_pending_panic_checkpoint;
using TransferPanicCheckpointFn = void (*)();
extern TransferPanicCheckpointFn g_transfer_panic_checkpoint;
using BlockGCForPendingCheckpointFn = void (*)();
extern BlockGCForPendingCheckpointFn g_block_gc_for_pending_checkpoint;

} // namespace aura::messaging

#endif // AURA_MESSAGING_BRIDGE_H
