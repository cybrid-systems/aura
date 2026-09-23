// serve/serve_async.cpp — Async serve mode implementation
#include "serve_async.h"
#include "compiler/messaging_bridge.h"
#include "core/gc_hooks.h"
#include "scheduler.h"
#include "gc_coordinator.h"
#include "fiber.h"
#include "mailbox.h"
#include "thread_pool.h"
#include "aura_platform.h"
#include "runtime_production_abi.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
#if AURA_HAVE_EPOLL
#include <sys/epoll.h>
#endif
#include <poll.h>
#include <dlfcn.h>
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace aura::serve {

static Scheduler* g_metrics_scheduler = nullptr;

std::string prometheus_scheduler_metrics() {
    if (!g_metrics_scheduler)
        return {};
    return g_metrics_scheduler->metrics().to_prometheus();
}

// ── Helpers ─────────────────────────────────────────────
//
// `json_escape` and `json_field` live in `aura::serve::detail`
// (see serve_async.h) so they can be unit-tested. The .cpp
// delegates to them via short static aliases to keep call sites
// tidy — the actual logic is in the header.

using aura::serve::detail::json_escape;
using aura::serve::detail::json_field;
using aura::serve::detail::kMaxServeAsyncLineBytes;

// ── Value formatting ─────────────────────────────────────

using EvalValue = aura::compiler::types::EvalValue;
using aura::compiler::types::is_closure;

static std::string fmt_val(const EvalValue& v, aura::compiler::CompilerService& cs) {
    return aura::compiler::format_value(v, cs.evaluator().primitives().string_heap(),
                                        cs.evaluator().pairs(), 0, &cs.evaluator().primitives(),
                                        cs.evaluator().keyword_table());
}

// ── In-process libcurl (Issue #473 §8) ────────────────────────────
//
// The async HTTP handler used to fork+exec curl with the auth token in
// argv (which leaks into /proc/*/cmdline). We now do the HTTP POST in
// process via libcurl loaded with dlopen — identical to the path used
// by the `http-post` primitive — so the auth header stays in process
// memory (set via CURLOPT_HTTPHEADER) and never lands on a cmdline.
//
// This duplicates the small CurlAPI struct in evaluator_primitives_io.cpp.
// Consolidation into a shared header is a future refactor; the duplication
// keeps the security-critical change inside serve_async.cpp's audit
// surface.
namespace {

    typedef void CURL;
    struct curl_slist {};
    using CURLcode = int;
    using CURLoption = int;
    constexpr CURLoption CURLOPT_URL = 10002;
    constexpr CURLoption CURLOPT_POST = 47;
    constexpr CURLoption CURLOPT_POSTFIELDS = 10015;
    constexpr CURLoption CURLOPT_POSTFIELDSIZE = 60;
    constexpr CURLoption CURLOPT_HTTPHEADER = 10023;
    constexpr CURLoption CURLOPT_WRITEFUNCTION = 20011;
    constexpr CURLoption CURLOPT_WRITEDATA = 10001;
    constexpr CURLoption CURLOPT_TIMEOUT = 13;
    constexpr CURLoption CURLOPT_CONNECTTIMEOUT = 78;
    constexpr CURLoption CURLOPT_SSL_VERIFYPEER = 64;
    constexpr CURLoption CURLOPT_SSL_VERIFYHOST = 81;
    constexpr CURLoption CURLOPT_USERAGENT = 10018;
    constexpr CURLcode CURLE_OK = 0;

    struct CurlAPI {
        void* handle = nullptr;
        CURL* (*easy_init)() = nullptr;
        CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
        CURLcode (*easy_perform)(CURL*) = nullptr;
        void (*easy_cleanup)(CURL*) = nullptr;
        struct curl_slist* (*slist_append)(struct curl_slist*, const char*) = nullptr;
        void (*slist_free_all)(struct curl_slist*) = nullptr;

        bool load() {
            if (handle)
                return true;
            static constexpr const char* kSonames[] = {
                "libcurl.so.4", "libcurl.so", "libcurl.4.dylib", "libcurl.dylib", "libcurl.so.5",
            };
            std::vector<void*> candidates;
            if (auto* h = ::dlopen(nullptr, RTLD_LAZY | RTLD_LOCAL))
                candidates.push_back(h);
            for (auto* name : kSonames) {
                if (auto* h = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL))
                    candidates.push_back(h);
            }
            for (auto* h : candidates) {
                auto* ei = (CURL * (*)())::dlsym(h, "curl_easy_init");
                auto* es = (CURLcode (*)(CURL*, CURLoption, ...))::dlsym(h, "curl_easy_setopt");
                auto* ep = (CURLcode (*)(CURL*))::dlsym(h, "curl_easy_perform");
                auto* ec = (void (*)(CURL*))::dlsym(h, "curl_easy_cleanup");
                auto* sa = (struct curl_slist *
                            (*)(struct curl_slist*, const char*))::dlsym(h, "curl_slist_append");
                auto* sf = (void (*)(struct curl_slist*))::dlsym(h, "curl_slist_free_all");
                if (ei && es && ep && ec && sa && sf) {
                    handle = h;
                    easy_init = ei;
                    easy_setopt = es;
                    easy_perform = ep;
                    easy_cleanup = ec;
                    slist_append = sa;
                    slist_free_all = sf;
                    return true;
                }
            }
            return false;
        }
    };
    CurlAPI& get_curl_async() {
        static CurlAPI c;
        return c;
    }

    std::size_t curl_writer_buf(char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
        auto* out = static_cast<std::string*>(ud);
        std::size_t total = size * nmemb;
        out->append(ptr, total);
        return total;
    }

} // anonymous namespace

// Issue #473 §8: in-process HTTP POST — never puts auth on cmdline.
static std::string http_post_in_process(const std::string& url, const std::string& body,
                                        const std::string& auth) {
    auto& c = get_curl_async();
    if (!c.load())
        return {};
    CURL* curl = c.easy_init();
    if (!curl)
        return {};
    struct curl_slist* headers = nullptr;
    headers = c.slist_append(headers, "Content-Type: application/json");
    if (!auth.empty()) {
        std::string h = "Authorization: Bearer " + auth;
        headers = c.slist_append(headers, h.c_str());
    }
    std::string response;
    c.easy_setopt(curl, CURLOPT_URL, url.c_str());
    c.easy_setopt(curl, CURLOPT_POST, 1L);
    c.easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    c.easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    c.easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    c.easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_writer_buf);
    c.easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    c.easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    c.easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    c.easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    c.easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    c.easy_setopt(curl, CURLOPT_USERAGENT, "aura/1.0");
    CURLcode res = c.easy_perform(curl);
    c.slist_free_all(headers);
    c.easy_cleanup(curl);
    return res == CURLE_OK ? response : std::string{};
}

// ── run_serve_async ─────────────────────────────────────

void run_serve_async(int num_workers) {
    // 1. Set stdin to non-blocking
    int flags = ::fcntl(STDIN_FILENO, F_GETFL);
    ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // Register fiber blocking callback for CompilerService::pop_message
    aura::messaging::g_fiber_block = []() {
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
    };

    // 2. Create thread pool for blocking operations
    // Background threads handle compilation, type-checking, file I/O,
    // and any other blocking tasks without blocking the event loop.
    // Pool size = num CPUs is a reasonable default.
    unsigned pool_size = std::thread::hardware_concurrency();
    if (pool_size < 2)
        pool_size = 2;
    if (pool_size > 8)
        pool_size = 8;
    static aura::serve::ThreadPool s_thread_pool(pool_size);

    // Register thread pool enqueue callback:
    // Injects the current fiber's eventfd as the wakeup mechanism.
    aura::messaging::g_thread_pool_enqueue = [](std::function<void()> fn, int) {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber)
            return;
        int evfd = fiber->eventfd();
        if (evfd < 0)
            return;
        s_thread_pool.enqueue(std::move(fn), evfd);
    };

    // Register async eval callback: evaluates code on the thread pool.
    // Returns the result as a JSON-escaped string. Uses the current
    // session's CompilerService (only safe because the service's evaluator
    // state is captured in the closure and executed FIFO on the pool).
    aura::messaging::g_eval_async = [](const std::string& code) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber)
            return "";
        auto evfd = fiber->eventfd();
        if (evfd < 0)
            return "";
        // Capture current compiler service for eval
        auto* svc = static_cast<aura::compiler::CompilerService*>(
            aura::messaging::g_current_compiler_service);
        if (!svc)
            return "";
        // Issue #1097: heap-allocate result — do NOT capture fiber-stack
        // locals by reference (UAF if scheduler reaps the fiber).
        auto result = std::make_shared<std::string>();
        s_thread_pool.enqueue(
            [svc, code, result]() {
                auto r = svc->exec_with_cache(code);
                if (r) {
                    *result = aura::compiler::format_value(
                        *r, svc->evaluator().primitives().string_heap(), svc->evaluator().pairs(),
                        0, &svc->evaluator().primitives(), svc->evaluator().keyword_table());
                } else {
                    *result = "[error] " + r.error().format();
                }
            },
            evfd);
        // Yield and wait for completion
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
        // Drain eventfd
        uint64_t val;
        ::read(evfd, &val, sizeof(val));
        return *result;
    };

    // 2. Create scheduler with worker threads
    Scheduler sched(num_workers);

    // Register fiber:spawn callback (captures scheduler for actual fiber creation).
    // Issue #4048: denseness fibers share the session Evaluator (complete_fiber
    // captures &ev). Migrating parent/child across workers under Soft Ready
    // multi-worker corrupts TLS / heap and hangs the session after join.
    // Inherit the parent's worker affinity (and default to worker 0) so
    // spawn+join denseness stays on one worker — same serialization as
    // --worker-threads 1, which passes acceptance. Steal already skips
    // other-affinity fibers.
    aura::messaging::g_fiber_spawn = [&sched](std::function<void()> fn) -> int64_t {
        int aff = 0;
        if (aura::serve::g_current_fiber) {
            const int parent_aff = aura::serve::g_current_fiber->affinity();
            if (parent_aff >= 0)
                aff = parent_aff;
            else {
                // Pin parent too so subsequent wakes/spawns stay together.
                aura::serve::g_current_fiber->set_affinity(0);
                aff = 0;
            }
        }
        auto* f = sched.spawn_with_affinity(std::move(fn), aff);
        return static_cast<int64_t>(f ? f->id() : 0);
    };

    // Issue #119: register fiber-by-id lookup so the evaluator
    // can find the Fiber* for a given fiber ID. Used by the
    // proper-blocking fiber:join path (joiner registers itself
    // on the target's completion; the lookup is needed first to
    // check if the target is already done).
    aura::messaging::g_fiber_lookup = +[](int64_t fid) -> void* {
        return static_cast<void*>(aura::serve::g_scheduler ? aura::serve::g_scheduler->fiber_by_id(
                                                                 static_cast<std::uint64_t>(fid))
                                                           : nullptr);
    };

    // Register fiber:yield callback (non-blocking yield, fiber stays Ready)
    aura::messaging::g_fiber_yield = []() {
        if (aura::serve::g_current_fiber) {
            aura::serve::Fiber::yield();
        }
    };

    // Register mutation boundary yield callback (Issue #31):
    // Called by mutate:* and eval-current before/after mutation operations.
    //
    // Issue #362: when this callback is called INSIDE an active
    // MutationBoundaryGuard (i.e., the fiber currently holds the
    // exclusive workspace_mtx_), yielding would let another
    // fiber take over and try to acquire the same lock —
    // classic deadlock (the holder can''t release because it''s
    // yielded away; the waiter can''t acquire because it''s
    // held by the holder). The pre-#362 behavior was to yield
    // unconditionally, relying on Fiber::yield''s
    // mutation_boundary_held_ check to detect it (assert in
    // debug, warn + continue in release). The release path was
    // the bug: the warning fired but the yield still happened,
    // causing the deadlock in production.
    //
    // Fix: when g_mutation_boundary_held reports true, skip
    // the yield. The mutation work proceeds uninterrupted and
    // the fiber will yield at the NEXT safe point (after the
    // Guard''s dtor releases workspace_mtx_). This converts the
    // unconditional yield into a safe yield-point hint that
    // respects the lock-ownership protocol.
    aura::messaging::g_fiber_yield_mutation_boundary = []() {
        // Skip yield if a MutationBoundaryGuard is currently
        // active on this fiber. The bridge function is null
        // when no Evaluator is wired (test-binary path), in
        // which case we also skip the safety check.
        if (aura::messaging::g_mutation_boundary_held &&
            aura::messaging::g_mutation_boundary_held()) {
            // Debug: log to stderr so the skip is visible in CI.
            // In release builds the warning the old code emitted
            // is replaced by an info-level skip notice.
            std::fprintf(stderr, "[#362] yield_mutation_boundary skipped: "
                                 "MutationBoundaryGuard is alive (yield would "
                                 "deadlock under workspace_mtx_)\n");
            return;
        }
        if (aura::serve::g_current_fiber) {
            aura::serve::Fiber::yield(aura::serve::YieldReason::MutationBoundary);
        }
    };

    // Issue #396 Phase 1: lightweight yield-reason setter for
    // (mutate:atomic-batch) Guard entry. Sets the current fiber's
    // last_yield_reason_ to MutationBoundary WITHOUT actually
    // yielding — this lets work-stealing decisions
    // (is_stealable(snap) / is_steal_candidate) see the fiber as being
    // at a mutation boundary, but doesn't suspend it. Used by
    // atomic-batch to make per-op mutations look like a single atomic
    // mutation boundary to the scheduler.
    aura::messaging::g_fiber_set_yield_reason_mutation_boundary = []() {
        if (aura::serve::g_current_fiber) {
            aura::serve::g_current_fiber->set_yield_reason(
                aura::serve::YieldReason::MutationBoundary);
        }
    };

    // Register scheduler metrics callback (Issue #32):
    // Agents can call (orch:metrics) to get real-time scheduler stats.
    aura::messaging::g_get_scheduler_metrics = [&sched]() -> std::string {
        return sched.metrics().to_json();
    };
    aura::messaging::g_reset_scheduler_metrics = [&sched]() {
        // Re-initialize metrics (clear counters, keep worker count)
        auto n = sched.metrics().num_workers();
        sched.metrics().resize_workers(n);
    };

    // Fiber affinity — pin current fiber to a specific worker (P2)
    aura::messaging::g_fiber_set_affinity = [](int worker_id) {
        auto fb = aura::serve::g_current_fiber;
        if (fb) {
            fb->set_affinity(worker_id);
        }
    };

    // GC root flush — routes through the current session's Evaluator.
    // The g_gc_flush_root_set callback is called by the GC collector
    // (gc_coordinator.cpp) during the root collection phase. It passes
    // us a `void*` that is actually a `aura::serve::GCRootSet*`; we
    // cast it and call the active Evaluator's `flush_gc_roots` to
    // walk its vector heaps and populate the root set.
    //
    // For multi-session setups, each session's Evaluator has its own
    // vector heaps, so a single flush only captures the active
    // session. The full multi-session integration would call
    // GCCollector::register_root_source(worker_id, evaluator.flush_fn)
    // once per session; that's a future iteration (see #113 closing
    // doc for the remaining work).
    aura::messaging::g_gc_flush_root_set = [&sched](void* root_set_out) {
        auto* svc = static_cast<aura::compiler::CompilerService*>(
            aura::messaging::g_current_compiler_service);
        if (!svc)
            return;
        if (!root_set_out)
            return;
        svc->evaluator().flush_gc_roots(root_set_out);
    };

    // GC sweep — routes through the current session's Evaluator.
    // Called by the GC collector (gc_coordinator.cpp) during the
    // sweep phase. We call the active Evaluator's `compact_sweep`
    // which actually erases unmarked closures (the main memory-
    // reclamation path) and reports the dead-count for vector heaps
    // (string_heap_/pairs_ compaction is a future refactor).
    //
    // Issue #1732: compact_sweep returns typed CompactSweepResult by
    // value. Bridge edge still exposes void* GCSweepResultMsg* for
    // messaging_bridge (heap-allocate a layout-compatible copy).
    aura::messaging::g_gc_sweep = [&sched](void* sweep_buffers) -> void* {
        auto* svc = static_cast<aura::compiler::CompilerService*>(
            aura::messaging::g_current_compiler_service);
        if (!svc || !sweep_buffers)
            return nullptr;
        auto r = svc->evaluator().compact_sweep(sweep_buffers);
        auto* msg = new aura::messaging::GCSweepResultMsg{};
        msg->strings_freed = r.strings_freed;
        msg->pairs_freed = r.pairs_freed;
        msg->closures_freed = r.closures_freed;
        msg->fiber_results_freed = r.fiber_results_freed;
        return msg;
    };

    // Register the sweep callback with the GC collector. The
    // collector invokes it during `collect()` after the mark phase.
    // Note: only one sweep_fn_ per scheduler (not per session).
    // The callback grabs the active CompilerService via
    // g_current_compiler_service (same as the root-flush path)
    // and rebuilds a GCSweepBuffers view from the mark vectors.
    // We pass through the active evaluator's sweep method.

    // ── Wire the arena-alloc-path GC hooks (Issue #113 Phase 4) ──
    // The arena's allocate_raw() now calls gc_hooks::safepoint_check()
    // and gc_hooks::record_alloc() on every allocation. The
    // safepoint hook is Fiber::check_gc_safepoint (lets compute-heavy
    // fibers be interrupted by GC). The record_alloc hook bumps
    // the GC's alloc counter so the collector knows when to fire.
    // Both are null by default (stdin mode), so the arena is a
    // no-op when the scheduler isn't running.
    aura::gc_hooks::g_arena_safepoint_check.store(
        +[]() noexcept { aura::serve::Fiber::check_gc_safepoint(); });
    // The record_alloc hook captures `sched`, but std::atomic<fn_ptr>
    // doesn't accept a capture lambda. We resolve `sched` to its
    // collector at hook-set time and pass a plain function pointer
    // that the GC collector exposes for this purpose. The collector's
    // record_alloc is a static method, but we need to bind it to
    // this specific scheduler. We use a thread_local indirection:
    // set a thread_local gc_collector_ptr, then have the hook
    // dereference it. This is a tiny extra indirection on the
    // alloc hot path but keeps the API simple.
    thread_local aura::serve::GCCollector* tls_gc_collector = nullptr;
    tls_gc_collector = sched.gc_collector();
    aura::gc_hooks::g_arena_record_alloc.store(+[]() noexcept {
        if (auto* gc = tls_gc_collector)
            gc->record_alloc();
    });
    // Issue #604: let ASTArena::compact()/defrag() detect a fiber
    // context so a compaction requested from inside a fiber bumps
    // the yield-check counter and cooperates with the GC safepoint.
    aura::gc_hooks::g_fiber_active.store(
        +[]() noexcept { return aura::serve::g_current_fiber != nullptr; });
    sched.gc_collector()->register_sweep_fn(
        [&sched](const aura::serve::GCSweepBuffers& bufs) -> aura::serve::GCSweepResult {
            auto* svc = static_cast<aura::compiler::CompilerService*>(
                aura::messaging::g_current_compiler_service);
            if (!svc)
                return {};
            // Reconstruct a view that compact_sweep can consume.
            // We use a stack-allocated stub with the same shape
            // as GCSweepBuffers so we can pass it through the
            // void* API without exposing the gc_coordinator.h
            // type across module boundaries.
            // Issue #963 / #1732: PassThru in; typed CompactSweepResult out
            // (no void* cast of the result).
            aura::messaging::GCSweepPassThru holder{bufs.string_marks, bufs.pair_marks,
                                                    bufs.closure_marks};
            auto r = svc->evaluator().compact_sweep(&holder);
            return aura::serve::GCSweepResult{r.strings_freed, r.pairs_freed, r.closures_freed,
                                              r.fiber_results_freed};
        });

    // GC collect — triggers a GC cycle via the GC collector.
    // Called from (gc-heap) primitives.
    aura::messaging::g_gc_collect = [&sched]() -> bool {
        auto* gc = sched.gc_collector();
        if (!gc)
            return false;
        gc->set_alloc_threshold(1);
        gc->reset_alloc_counter();
        gc->record_alloc();
        return gc->request() && gc->collect();
    };

    // Issue #205: env-walk callback. The Evaluator's
    // env_frames_ SoA arena is walked (linear pass, O(frames))
    // to discover pair/closure refs reachable through env
    // bindings. This replaces the old pointer-chasing Env*
    // walk with a single linear pass — 3-5x mark-phase
    // speedup for large workspaces (per Issue #172).
    //
    // The callback reads g_current_compiler_service to get
    // the active Evaluator (set per-session on the IO
    // thread). The GC is called from the IO thread via
    // (gc-heap), so the active service is the session's.
    sched.gc_collector()->register_env_walk_fn([](aura::serve::EnvFrameRoots& out) {
        auto* svc = static_cast<aura::compiler::CompilerService*>(
            aura::messaging::g_current_compiler_service);
        if (!svc)
            return;
        // CompilerService exposes the Evaluator; walk
        // the Evaluator's env_frames_ arena. The walk
        // is read-only and SoA-friendly (no allocations
        // in the hot path; only pair/closure indices
        // are appended to the output vectors).
        svc->evaluator().walk_env_frame_roots(out.pair_roots, out.closure_roots);
    });

    // Issue #2084: register a size-provider callback so the GC can size
    // its MarkBitVectors to the actual current heap extent (string_heap_,
    // pairs_, closures_), not just max root index + 1. Without this,
    // high-water dead slots above any live root never enter the mark
    // vector and the live_mask walk silently under-covers the heap.
    // Same lookup pattern as the env-walk callback above (active service
    // via g_current_compiler_service); returns (0,0,0) when no active
    // service to preserve the pre-#2084 root-derived sizing fallback.
    sched.gc_collector()->register_size_fn(
        []() -> std::tuple<std::size_t, std::size_t, std::size_t> {
            auto* svc = static_cast<aura::compiler::CompilerService*>(
                aura::messaging::g_current_compiler_service);
            if (!svc)
                return {0, 0, 0};
            const auto& ev = svc->evaluator();
            return {ev.string_heap_size(), ev.pairs_size(), ev.closures_size()};
        });

    // 3. Shared state between stdin_reader and session fibers
    // Soft (#4047 B async): named sessions may share one CompilerService
    // graph (parity with Soft sync --serve). Production keeps isolation.
    const bool soft_shared_graph = !production_abi_selfcheck_required();
    if (soft_shared_graph) {
        static bool soft_async_b_logged = false;
        if (!soft_async_b_logged) {
            soft_async_b_logged = true;
            std::println(std::cerr, "aura: Soft shared_workspace (#4047 B): --serve-async named "
                                    "sessions share one CompilerService + workspace_tree — "
                                    "orch→project bindings visible. NOT production isolation.");
        }
    }

    struct Session {
        std::string id;
        Fiber* fiber = nullptr;
        aura::compiler::CompilerService service;
        aura::serve::Mailbox mailbox;
        bool active = true;
    };

    std::deque<std::string> stdin_lines; // complete JSON lines from stdin
    bool stdin_eof = false;

    // Declared before stdin fiber so push path can wake Waiting session fibers
    // (producer-consumer race: session parks after stdin push+park → hang without wake).
    std::unordered_map<std::string, std::unique_ptr<Session>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        sessions;

    auto wake_waiting_session_fibers = [&sessions]() {
        for (auto& [id, sess] : sessions) {
            (void)id;
            Fiber* f = sess ? sess->fiber : nullptr;
            if (!f || f->is_done())
                continue;
            if (f->state() != FiberState::Waiting)
                continue;
            int evfd = f->eventfd();
            if (evfd < 0)
                continue;
            uint64_t one = 1;
            (void)::write(evfd, &one, sizeof(one));
        }
    };

    // Route by JSON "session" field (json_field accepts "k":"v" and "k": "v").
    // Fragile find("\"session\":\"name\"") missed spaced JSON → default stole lines.
    auto line_for_session = [](const std::string& line, const std::string& sid) -> bool {
        auto sess = json_field(line, "session");
        if (sid == "default")
            return sess.empty() || sess == "default";
        return sess == sid;
    };

    // 4. Spawn stdin_reader fiber

    auto* stdin_fiber = sched.spawn([&sched, &stdin_lines, &stdin_eof,
                                     &wake_waiting_session_fibers]() {
        std::string buf;
        bool local_eof = false;
        while (!local_eof) {
            // Edge-triggered: read until EAGAIN or EOF
            bool got_data = false;
            bool pushed = false;
            while (true) {
                char tmp[4096];
                ssize_t n = ::read(STDIN_FILENO, tmp, sizeof(tmp));
                if (n > 0) {
                    got_data = true;
                    buf.append(tmp, static_cast<size_t>(n));
                } else if (n == 0) {
                    local_eof = true;
                    break;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; // no more data now
                } else {
                    local_eof = true;
                    break;
                }
            }

            // Extract complete lines from buffer (do this BEFORE EOF check).
            // Issue #473 §1: 1 MiB cap on per-line input. Lines without
            // \n past the cap are dropped (and logged to stderr) to bound
            // the reader's memory growth against malicious clients that
            // stream unbounded bytes without newlines.
            auto nl = buf.find('\n');
            while (nl != std::string::npos) {
                if (nl >= kMaxServeAsyncLineBytes) {
                    // Single line exceeded the cap — drop it and skip to next \n.
                    std::fprintf(stderr, "serve-async: dropping line of %zu bytes (cap=%zu)\n", nl,
                                 kMaxServeAsyncLineBytes);
                    buf.erase(0, nl + 1);
                    nl = buf.find('\n');
                    continue;
                }
                auto line = buf.substr(0, nl);
                // Skip comment, blank, and empty lines
                auto s = line.find_first_not_of(" \t\r\n");
                if (s != std::string::npos && line[s] != ';') {
                    stdin_lines.push_back(std::move(line));
                    pushed = true;
                }
                buf.erase(0, nl + 1);
                nl = buf.find('\n');
            }
            // Cap safety net: if buf grows without any \n and exceeds the
            // cap by a wide margin (e.g. 2× cap with no progress), the
            // request can never complete — evict it to prevent OOM. This
            // catches pathological inputs that never contain \n.
            if (buf.size() >= 2 * kMaxServeAsyncLineBytes) {
                std::fprintf(stderr,
                             "serve-async: dropping %zu bytes of unterminated input (no newline)\n",
                             buf.size());
                buf.clear();
            }

            // Wake session fibers that may have parked before seeing the line.
            if (pushed)
                wake_waiting_session_fibers();

            if (local_eof) {
                // EOF — remove stdin from epoll so it doesn't keep firing
#if AURA_HAVE_EPOLL
                ::epoll_ctl(sched.epoll_fd(), EPOLL_CTL_DEL, STDIN_FILENO, nullptr);
#endif
                stdin_eof = true;
                wake_waiting_session_fibers();
                break;
            }

            if (!got_data) {
                // Yield and wait for epoll to wake us on new data
                g_current_fiber->set_state(FiberState::Waiting);
                Fiber::yield();
            }
            // If got_data == true, loop back to try reading more
            // (allows consuming chained pipe data in one quantum)
        }
    });
    sched.set_stdin_fiber(stdin_fiber);

    // 5. Create default session with compiler service

    // Shared workspace tree: all sessions see the same workspace hierarchy.
    // Created before any sessions so we can inject it into each one.
    void* shared_workspace_tree = aura::compiler::Evaluator::create_workspace_tree();

    std::string active_session = "default";
    {
        auto& sess = sessions["default"];
        sess = std::make_unique<Session>();
        sess->id = "default";
        sess->service.set_session_id("default");
        sess->service.set_workspace_tree(shared_workspace_tree);
        aura::compiler::CompilerService::register_session("default", &sess->service);
    }

    // Soft shared graph: named-session exec uses default CompilerService.
    auto cs_for = [&sessions,
                   soft_shared_graph](Session& sess) -> aura::compiler::CompilerService& {
        if (soft_shared_graph)
            return sessions.at("default")->service;
        return sess.service;
    };

    // ── Multi-session GC root registration (Issue #113) ────
    // Each session has its own Evaluator (and therefore its own
    // string_heap_ / pairs_ / closures_). For the GC to know
    // about ALL live objects, each session's evaluator must be
    // registered as a separate root source. Soft shared graph
    // aliases named sessions onto default CS — only register
    // default's evaluator once under Soft.
    auto gc_collect = sched.gc_collector();
    auto register_session_root = [&](Session& s, int worker_id) {
        if (!gc_collect)
            return;
        gc_collect->register_root_source(worker_id, [&s](aura::serve::GCRootSet& out) {
            s.service.evaluator().flush_gc_roots(&out);
        });
    };
    if (gc_collect) {
        register_session_root(*sessions["default"], 0);
    }

    // Helper: spawn a named-session fiber that drains stdin_lines for `nsid`.
    // Soft shared graph (#4047 B / #4048): pin named sessions to worker 0 with
    // the default session so shared Evaluator denseness never migrates.
    // Production isolation: pin by name hash across workers.
    auto spawn_named_session_fiber =
        [&sched, &stdin_lines, &stdin_eof, &sessions, &line_for_session, &cs_for,
         soft_shared_graph](const std::string& nsid, Session& sess) -> Fiber* {
        const int aff =
            soft_shared_graph
                ? 0
                : (static_cast<int>(std::hash<std::string>{}(nsid) %
                                    static_cast<std::size_t>(std::max(1, sched.num_workers()))));
        return sched.spawn_with_affinity(
            [nsid, &sess, &stdin_lines, &stdin_eof, &line_for_session, &cs_for]() {
            sess.mailbox.attach(aura::serve::g_current_fiber);
            sess.service.set_wake_eventfd(aura::serve::g_current_fiber->eventfd());
            while (sess.active) {
                std::string sl;
                for (auto sit = stdin_lines.begin(); sit != stdin_lines.end(); ++sit) {
                    if (line_for_session(*sit, nsid)) {
                        sl = std::move(*sit);
                        stdin_lines.erase(sit);
                        break;
                    }
                }
                if (sl.empty()) {
                    if (stdin_eof)
                        break;
                    aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
                    aura::serve::Fiber::yield();
                    continue;
                }
                auto c = json_field(sl, "cmd");
                if (c == "exec") {
                    auto code = json_field(sl, "code");
                    if (!code.empty()) {
                        auto& cs = cs_for(sess);
                        aura::messaging::g_current_compiler_service = &cs;
                        auto r = cs.exec_with_cache(code);
                        if (r) {
                            std::println(
                                "{{\"session\":\"{}\",\"status\":\"ok\",\"value\":\"{}\"}}",
                                json_escape(nsid), json_escape(fmt_val(*r, cs)));
                        } else {
                            std::println(
                                "{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"{}\"}}",
                                json_escape(nsid), json_escape(r.error().format()));
                        }
                        std::fflush(stdout);
                    }
                }
            }
        }, aff);
    };

    // Register / create named session (Soft: alias CS to default).
    auto emplace_named_session = [&sessions, &sched, shared_workspace_tree, soft_shared_graph,
                                  gc_collect, &register_session_root,
                                  &spawn_named_session_fiber](const std::string& name) -> bool {
        if (sessions.count(name) > 0)
            return false;
        auto [it, created] = sessions.try_emplace(name, std::make_unique<Session>());
        if (!created)
            return false;
        it->second->id = name;
        it->second->service.set_session_id(name);
        it->second->service.set_workspace_tree(shared_workspace_tree);
        if (soft_shared_graph) {
            aura::compiler::CompilerService::register_session(name,
                                                              &sessions.at("default")->service);
            // Soft alias — do not register an extra GC root for the unused CS.
        } else {
            aura::compiler::CompilerService::register_session(name, &it->second->service);
            if (gc_collect) {
                int wid = static_cast<int>(std::hash<std::string>{}(name) % 997) + 1;
                register_session_root(*it->second, wid);
            }
        }
        it->second->fiber = spawn_named_session_fiber(name, *it->second);
        return true;
    };

    // Register session:create for Aura code (primitive in evaluator)
    std::function<aura::messaging::SessionCreateFn> sc_fn =
        [&emplace_named_session](const std::string& name) -> bool {
        return emplace_named_session(name);
    };
    aura::messaging::g_session_create = &sc_fn;

    // 6. Spawn session fibers (one per session)
    // For now, spawn one fiber for the default session
    // In the future, spawn as needed
    for (auto& [sid, sess] : sessions) {
        // Issue #4048: pin session fibers (esp. Soft shared default) to worker 0 so
        // denseness spawn/join inherits affinity and never migrates Evaluators.
        auto* fiber = sched.spawn_with_affinity(
            [sid = sid, &sess = *sess, &stdin_lines, &stdin_eof, &sessions, &sched,
             &shared_workspace_tree, &line_for_session, &cs_for, &emplace_named_session,
             soft_shared_graph]() {
            // Attach mailbox to this fiber
            sess.mailbox.attach(g_current_fiber);

            while (sess.active) {
                // Try to pop a line from stdin
                std::string line;
                {
                    // Lock-free: dequeue from shared buffer if available
                    // Since we're single-threaded, no actual lock needed
                    line.clear();
                    for (auto it = stdin_lines.begin(); it != stdin_lines.end(); ++it) {
                        if (line_for_session(*it, sid)) {
                            line = std::move(*it);
                            stdin_lines.erase(it);
                            break;
                        }
                    }
                }

                if (line.empty()) {
                    // No lines for us — check mailbox
                    auto msg = sess.mailbox.pop(false);
                    if (!msg.empty()) {
                        // Got a message from another session
                        std::println("{{\"session\":\"{}\",\"status\":\"msg\",\"data\":\"{}\"}}",
                                     json_escape(sid), json_escape(msg));
                        std::fflush(stdout);
                        continue;
                    }

                    // Nothing to do
                    if (stdin_eof)
                        break;
                    g_current_fiber->set_state(FiberState::Waiting);
                    Fiber::yield();
                    continue;
                }

                // Parse and execute
                auto cmd = json_field(line, "cmd");
                if (cmd.empty()) {
                    std::println(
                        "{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"missing cmd\"}}",
                        json_escape(sid));
                    std::fflush(stdout);
                    continue;
                }

                if (cmd == "exec") {
                    auto code = json_field(line, "code");
                    if (code.empty()) {
                        std::println(
                            "{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"missing code\"}}",
                            json_escape(sid));
                        std::fflush(stdout);
                        continue;
                    }
                    auto& cs = cs_for(sess);
                    aura::messaging::g_current_compiler_service = &cs;
                    auto result = cs.exec_with_cache(code);
                    if (result) {
                        try {
                            auto& v = *result;
                            // Check if closure
                            if (is_closure(v)) {
                                std::println("{{\"session\":\"{}\",\"status\":\"closure\","
                                             "\"value\":\"#<procedure>\"}}",
                                             json_escape(sid));
                            } else {
                                std::println(
                                    "{{\"session\":\"{}\",\"status\":\"ok\",\"value\":\"{}\"}}",
                                    json_escape(sid), json_escape(fmt_val(v, cs)));
                            }
                        } catch (const std::bad_alloc&) {
                            std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"out "
                                         "of memory\"}}",
                                         json_escape(sid));
                        }
                    } else {
                        auto& d = result.error();
                        std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"{}\"}}",
                                     json_escape(sid), json_escape(d.format()));
                    }
                    std::fflush(stdout);

                } else if (cmd == "session") {
                    auto action = json_field(line, "action");
                    auto name = json_field(line, "name");
                    // Soft/sync parity: missing action + name ⇒ create-or-activate.
                    if (action.empty() && !name.empty())
                        action = "create";
                    if (action == "create") {
                        if (name.empty()) {
                            std::println("{{\"session\":\"{}\","
                                         "\"status\":\"error\",\"msg\":\"missing name\"}}",
                                         json_escape(sid));
                            std::fflush(stdout);
                        } else if (sessions.count(name) > 0) {
                            // Already exists — ok (sync-style activate)
                            std::println("{{\"session\":\"{}\","
                                         "\"status\":\"ok\",\"name\":\"{}\"}}",
                                         json_escape(sid), json_escape(name));
                            std::fflush(stdout);
                        } else if (emplace_named_session(name)) {
                            std::println("{{\"session\":\"{}\","
                                         "\"status\":\"created\",\"name\":\"{}\"}}",
                                         json_escape(sid), json_escape(name));
                            std::fflush(stdout);
                        } else {
                            std::println("{{\"session\":\"{}\","
                                         "\"status\":\"error\",\"msg\":\"already exists\"}}",
                                         json_escape(sid));
                            std::fflush(stdout);
                        }
                    } else {
                        std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"unknown "
                                     "action: {}\"}}",
                                     json_escape(sid), json_escape(action));
                        std::fflush(stdout);
                    }

                } else if (cmd == "session-send") {
                    auto target = json_field(line, "target");
                    auto data = json_field(line, "data");
                    if (!target.empty() && !data.empty()) {
                        auto it = sessions.find(target);
                        if (it != sessions.end() && it->second->active) {
                            it->second->mailbox.push(data);
                            std::println(
                                "{{\"session\":\"{}\",\"status\":\"sent\",\"target\":\"{}\"}}",
                                json_escape(sid), json_escape(target));
                        } else {
                            std::println("{{\"session\":\"{}\",\"status\":\"error\",\"msg\":"
                                         "\"session not found\"}}",
                                         json_escape(sid));
                        }
                    }
                    std::fflush(stdout);

                } else if (cmd == "session-recv") {
                    auto msg = sess.mailbox.pop(true); // blocking pop (yields)
                    if (!msg.empty()) {
                        std::println("{{\"session\":\"{}\",\"status\":\"msg\",\"data\":\"{}\"}}",
                                     json_escape(sid), json_escape(msg));
                    } else {
                        std::println("{{\"session\":\"{}\",\"status\":\"timeout\"}}",
                                     json_escape(sid));
                    }
                    std::fflush(stdout);

                } else {
                    std::println(
                        "{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"unknown cmd: {}\"}}",
                        json_escape(sid), json_escape(cmd));
                    std::fflush(stdout);
                }
            }
        }, 0);

        sess->fiber = fiber;
        (void)soft_shared_graph;
        (void)shared_workspace_tree;
        (void)sched;
    }

    // 7. Run the scheduler
    sched.run();

    std::fflush(stdout);
}

// ── run_serve_async_bench ────────────────────────────

void run_serve_async_bench(const std::string& file_path, int num_workers) {
    // 1. Set stdin to non-blocking
    int flags = ::fcntl(STDIN_FILENO, F_GETFL);
    ::fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // 2. Create scheduler with worker threads
    Scheduler sched(num_workers);

    // Register fiber:spawn callback (Issue #4048: affinity inheritance — see
    // run_serve_async). Bench denseness also shares one Evaluator.
    aura::messaging::g_fiber_spawn = [&sched](std::function<void()> fn) -> int64_t {
        int aff = 0;
        if (aura::serve::g_current_fiber) {
            const int parent_aff = aura::serve::g_current_fiber->affinity();
            if (parent_aff >= 0)
                aff = parent_aff;
            else {
                aura::serve::g_current_fiber->set_affinity(0);
                aff = 0;
            }
        }
        auto* f = sched.spawn_with_affinity(std::move(fn), aff);
        return static_cast<int64_t>(f ? f->id() : 0);
    };

    // Issue #119: register fiber-by-id lookup (see comment in
    // the main registration above for the rationale).
    aura::messaging::g_fiber_lookup = +[](int64_t fid) -> void* {
        return static_cast<void*>(aura::serve::g_scheduler ? aura::serve::g_scheduler->fiber_by_id(
                                                                 static_cast<std::uint64_t>(fid))
                                                           : nullptr);
    };

    // Register fiber:yield callback
    aura::messaging::g_fiber_yield = []() {
        if (aura::serve::g_current_fiber) {
            aura::serve::Fiber::yield();
        }
    };

    aura::messaging::g_fiber_yield_mutation_boundary = []() {
        if (aura::serve::g_current_fiber) {
            aura::serve::Fiber::yield(aura::serve::YieldReason::MutationBoundary);
        }
    };

    aura::messaging::g_get_scheduler_metrics = [&sched]() -> std::string {
        return sched.metrics().to_json();
    };
    aura::messaging::g_reset_scheduler_metrics = [&sched]() {
        auto n = sched.metrics().num_workers();
        sched.metrics().resize_workers(n);
    };

    // Register fiber blocking callback
    aura::messaging::g_fiber_block = []() {
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
    };

    // Issue #956: same in-process libcurl path as run_serve_async
    // (no fork/exec curl; auth never on cmdline; shared_ptr result).
    aura::messaging::g_http_post_async = [](const std::string& url, const std::string& body,
                                            const std::string& auth) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber)
            return {};
        auto evfd = fiber->eventfd();
        if (evfd < 0)
            return {};

        auto result = std::make_shared<std::string>();
        std::thread t([evfd, url, body, auth, result]() {
            *result = http_post_in_process(url, body, auth);
            uint64_t v = 1;
            ::write(evfd, &v, sizeof(v));
        });
        t.detach();

        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
        return std::move(*result);
    };

    // 3. Create default session
    void* shared_workspace_tree = aura::compiler::Evaluator::create_workspace_tree();
    struct BenchSession {
        std::string id;
        aura::compiler::CompilerService service;
    };
    auto sess = std::make_unique<BenchSession>();
    sess->id = "default";
    sess->service.set_session_id("default");
    sess->service.set_workspace_tree(shared_workspace_tree);
    aura::compiler::CompilerService::register_session("default", &sess->service);

    // 4. Read the bench file
    std::ifstream f(file_path);
    if (!f) {
        std::println(std::cerr, "error: cannot open '{}'", file_path);
        return;
    }
    std::string bench_code((std::istreambuf_iterator<char>(f)), {});

    // 5. Spawn a single fiber that runs the bench code
    // The bench code uses fiber:spawn for parallelism; spawned fibers
    // continue running via the scheduler even after this fiber completes.
    // Issue #4048: pin bench fiber to worker 0 so denseness inherits affinity.
    sched.spawn_with_affinity(
        [&sess, bench_code = std::move(bench_code), &sched]() {
        // Set wake eventfd for recv/send
        sess->service.set_wake_eventfd(aura::serve::g_current_fiber->eventfd());

        // Evaluate expressions one at a time (same as stdin pipe mode)
        // Split by balanced parentheses to evaluate each expression separately.
        std::string remaining = bench_code;
        bool any_error = false;
        while (!remaining.empty()) {
            // Skip whitespace and comments
            std::size_t start = 0;
            while (start < remaining.size()) {
                auto c = remaining[start];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ++start;
                } else if (c == ';') {
                    // Skip comment to end of line
                    auto nl = remaining.find('\n', start);
                    if (nl == std::string::npos) {
                        remaining.clear();
                        goto done;
                    }
                    start = nl + 1;
                } else {
                    break;
                }
            }
            if (start >= remaining.size())
                break;

            // Find the end of this balanced expression
            int depth = 0;
            bool in_str = false;
            std::size_t end = start;
            for (; end < remaining.size(); ++end) {
                auto c = remaining[end];
                if (in_str) {
                    if (c == '\\' && end + 1 < remaining.size()) {
                        ++end; // skip escaped char
                    } else if (c == '"') {
                        in_str = false;
                    }
                } else if (c == '"') {
                    in_str = true;
                } else if (c == '(' || c == '[') {
                    ++depth;
                } else if ((c == ')' || c == ']') && depth > 0) {
                    if (--depth == 0) {
                        ++end; // include closing paren
                        break;
                    }
                }
            }
            if (depth != 0) {
                // Unbalanced — evaluate what we have
                end = remaining.size();
            }

            auto expr = remaining.substr(start, end - start);
            remaining.erase(0, end);

            if (!expr.empty()) {
                auto result = sess->service.eval(expr);
                if (!result) {
                    std::print(std::cerr, "eval error on expr (len={}): {}\n  msg: {}\n",
                               expr.size(), expr.substr(0, 120), result.error().format());
                    std::fflush(stderr);
                    any_error = true;
                    break;
                }
            }
        }
    done:
        std::fflush(stdout);
        (void)any_error;
    }, 0);

    // 6. Run the scheduler
    sched.run();
}

} // namespace aura::serve
