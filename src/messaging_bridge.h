#ifndef AURA_MESSAGING_BRIDGE_H
#define AURA_MESSAGING_BRIDGE_H

#include <string>
#include <optional>
#include <functional>

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

// Fiber join — wait for a fiber to complete and return its result (future use)
// Currently a placeholder — evaluator handles fiber:join internally.
using FiberJoinFn = void(*)();
extern FiberJoinFn g_fiber_join;

// Session create — set by serve_async.cpp, used by evaluator
// Returns true on success
using SessionCreateFn = bool(const std::string& name);
extern std::function<SessionCreateFn>* g_session_create;

// Session list — return all active session IDs
// Returns empty vector if no sessions or not in serve mode
using SessionListFn = std::vector<std::string>(*)();
extern SessionListFn g_session_list;


// Access a CompilerService's mailbox through a void*.
// Since evaluator can't import the CompilerService module,
// we provide a function that the CompilerService registers.
using MailboxReadFn = std::optional<std::string> (*)(void* compiler_service, int timeout_ms);
using MailboxSenderFn = std::string (*)(void* compiler_service);  // sender of last read
using MailboxCountFn = std::size_t (*)(void* compiler_service);
using SessionIdFn = std::string (*)(void* compiler_service);
using SessionExistsFn = bool (*)(const std::string& id);

// These are set by CompilerService during construction
extern MailboxReadFn g_mailbox_read;
extern MailboxSenderFn g_mailbox_last_sender;
extern MailboxCountFn g_mailbox_count;
extern SessionIdFn g_session_id;
extern SessionExistsFn g_session_exists;
extern void* g_current_compiler_service;  // set before each eval in serve

// Arena reset callback — set by CompilerService, called by Evaluator
// to reclaim arena memory between benchmark tasks.
// Takes a pointer to the service, returns void.
using ResetArenaFn = void(*)(void* compiler_service);
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

}  // namespace aura::messaging

#endif  // AURA_MESSAGING_BRIDGE_H
