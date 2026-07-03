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

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
#if AURA_HAVE_EPOLL
#include <sys/epoll.h>
#endif
#include <poll.h>
#include <dlfcn.h>

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

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (auto c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

// Minimal JSON field extractor (no full parser needed for protocol)
static std::string json_field(const std::string& json, const std::string& field) {
    // Try with space after colon, then without
    auto key = "\"" + field + "\": \"";
    auto pos = json.find(key);
    if (pos == std::string::npos) {
        key = "\"" + field + "\":\"";
        pos = json.find(key);
    }
    if (pos == std::string::npos)
        return {};
    pos += key.size();
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            auto next = json[pos + 1];
            if (next == 'n')
                val += '\n';
            else if (next == 't')
                val += '\t';
            else if (next == 'r')
                val += '\r';
            else if (next == '"')
                val += '"';
            else if (next == '\\')
                val += '\\';
            else
                val += next;
            pos += 2;
        } else {
            val += json[pos++];
        }
    }
    return val;
}

// ── Value formatting ─────────────────────────────────────

using EvalValue = aura::compiler::types::EvalValue;
using aura::compiler::types::is_closure;

static std::string fmt_val(const EvalValue& v, aura::compiler::CompilerService& cs) {
    return aura::compiler::format_value(v, cs.evaluator().primitives().string_heap(),
                                        cs.evaluator().pairs(), 0, &cs.evaluator().primitives(),
                                        cs.evaluator().keyword_table());
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
        std::string result;
        s_thread_pool.enqueue(
            [svc, code, &result]() {
                auto r = svc->exec_with_cache(code);
                if (r) {
                    result = aura::compiler::format_value(
                        *r, svc->evaluator().primitives().string_heap(), svc->evaluator().pairs(),
                        0, &svc->evaluator().primitives(), svc->evaluator().keyword_table());
                } else {
                    result = "[error] " + r.error().format();
                }
            },
            evfd);
        // Yield and wait for completion
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
        // Drain eventfd
        uint64_t val;
        ::read(evfd, &val, sizeof(val));
        return result;
    };

    // 2. Create scheduler with worker threads
    Scheduler sched(num_workers);

    // Register fiber:spawn callback (captures scheduler for actual fiber creation)
    aura::messaging::g_fiber_spawn = [&sched](std::function<void()> fn) -> int64_t {
        auto* f = sched.spawn(std::move(fn));
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
            std::fprintf(stderr,
                "[#362] yield_mutation_boundary skipped: "
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
    // yielding — this lets work-stealing decisions (is_stealable())
    // see the fiber as being at a mutation boundary, but doesn't
    // suspend it. Used by atomic-batch to make per-op mutations
    // look like a single atomic mutation boundary to the scheduler.
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
    // Returns an opaque `GCSweepResultMsg*` (heap-allocated by
    // compact_sweep). The caller is responsible for `delete`-ing
    // it after reading the fields.
    aura::messaging::g_gc_sweep = [&sched](void* sweep_buffers) -> void* {
        auto* svc = static_cast<aura::compiler::CompilerService*>(
            aura::messaging::g_current_compiler_service);
        if (!svc || !sweep_buffers)
            return nullptr;
        return svc->evaluator().compact_sweep(sweep_buffers);
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
            struct PassThru {
                const aura::serve::MarkBitVector* s;
                const aura::serve::MarkBitVector* p;
                const aura::serve::MarkBitVector* c;
            } holder{bufs.string_marks, bufs.pair_marks, bufs.closure_marks};
            // compact_sweep returns void*; cast to the local
            // GCSweepResultMsg layout (defined in messaging_bridge.h).
            // The Evaluator's impl file uses a layout-compatible
            // local struct (with a static_assert) for the same
            // reason — to keep messaging_bridge.h from leaking
            // into the module interface.
            struct SweepResultMsg {
                std::size_t strings_freed = 0;
                std::size_t pairs_freed = 0;
                std::size_t closures_freed = 0;
                std::size_t fiber_results_freed = 0;
            };
            auto* msg_ptr = static_cast<SweepResultMsg*>(svc->evaluator().compact_sweep(&holder));
            if (!msg_ptr)
                return {};
            aura::serve::GCSweepResult r{msg_ptr->strings_freed, msg_ptr->pairs_freed,
                                         msg_ptr->closures_freed, msg_ptr->fiber_results_freed};
            delete msg_ptr;
            return r;
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

    // 3. Shared state between stdin_reader and session fibers
    std::deque<std::string> stdin_lines; // complete JSON lines from stdin
    bool stdin_eof = false;

    // 4. Spawn stdin_reader fiber

    auto* stdin_fiber = sched.spawn([&sched, &stdin_lines, &stdin_eof]() {
        std::string buf;
        bool local_eof = false;
        while (!local_eof) {
            // Edge-triggered: read until EAGAIN or EOF
            bool got_data = false;
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

            // Extract complete lines from buffer (do this BEFORE EOF check)
            auto nl = buf.find('\n');
            while (nl != std::string::npos) {
                auto line = buf.substr(0, nl);
                // Skip comment, blank, and empty lines
                auto s = line.find_first_not_of(" \t\r\n");
                if (s != std::string::npos && line[s] != ';')
                    stdin_lines.push_back(std::move(line));
                buf.erase(0, nl + 1);
                nl = buf.find('\n');
            }

            if (local_eof) {
                // EOF — remove stdin from epoll so it doesn't keep firing
#if AURA_HAVE_EPOLL
                ::epoll_ctl(sched.epoll_fd(), EPOLL_CTL_DEL, STDIN_FILENO, nullptr);
#endif
                stdin_eof = true;
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
    struct Session {
        std::string id;
        Fiber* fiber = nullptr;
        aura::compiler::CompilerService service;
        aura::serve::Mailbox mailbox;
        bool active = true;
    };

    // Shared workspace tree: all sessions see the same workspace hierarchy.
    // Created before any sessions so we can inject it into each one.
    void* shared_workspace_tree = aura::compiler::Evaluator::create_workspace_tree();

    std::unordered_map<std::string, std::unique_ptr<Session>> sessions;
    std::string active_session = "default";
    {
        auto& sess = sessions["default"];
        sess = std::make_unique<Session>();
        sess->id = "default";
        sess->service.set_session_id("default");
        sess->service.set_workspace_tree(shared_workspace_tree);
        aura::compiler::CompilerService::register_session("default", &sess->service);
    }

    // ── Multi-session GC root registration (Issue #113) ────
    // Each session has its own Evaluator (and therefore its own
    // string_heap_ / pairs_ / closures_). For the GC to know
    // about ALL live objects, each session's evaluator must be
    // registered as a separate root source. The GC collector's
    // `collect_roots` walks the map and calls every source's
    // flush callback, which appends to a single GCRootSet.
    //
    // The worker_id is just a stable index into the root_sources_
    // map — we use the session name's hash modulo a small prime
    // to spread them out, but the value doesn't matter as long
    // as it's unique per session. (The number 0 is the
    // "default" session; sessions are added on top.)
    auto gc_collect = sched.gc_collector();
    auto register_session_root = [&](Session& s, int worker_id) {
        gc_collect->register_root_source(worker_id, [&s](aura::serve::GCRootSet& out) {
            s.service.evaluator().flush_gc_roots(&out);
        });
    };
    {
        // Use 0 for default; the active-session g_gc_flush_root_set
        // (set below) will override this for the active session.
        register_session_root(*sessions["default"], 0);
    }

    // Register async HTTP handler (uses thread + fork+exec curl + pipe with drain)
    // Thread does blocking fork+exec; pipe read has a 30s timeout to avoid deadlock.
    aura::messaging::g_http_post_async = [](const std::string& url, const std::string& body,
                                            const std::string& auth) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber)
            return {};
        auto evfd = fiber->eventfd();
        if (evfd < 0)
            return {};

        std::string result;
        std::thread t([evfd, url, body, auth, &result]() {
            int in[2], out[2];
            if (::pipe(in) < 0 || ::pipe(out) < 0) {
                uint64_t v = 1;
                ::write(evfd, &v, sizeof(v));
                return;
            }

            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(in[0]);
                ::close(in[1]);
                ::close(out[0]);
                ::close(out[1]);
                uint64_t v = 1;
                ::write(evfd, &v, sizeof(v));
                return;
            }

            if (pid == 0) {
                ::close(in[1]);
                ::close(out[0]);
                ::dup2(in[0], STDIN_FILENO);
                ::dup2(out[1], STDOUT_FILENO);
                ::close(in[0]);
                ::close(out[1]);

                // Build argv
                std::vector<const char*> argv;
                argv.push_back("curl");
                argv.push_back("-s");
                argv.push_back("-X");
                argv.push_back("POST");
                argv.push_back("--data-binary");
                argv.push_back("@-");
                argv.push_back("-H");
                argv.push_back("Content-Type: application/json");
                if (!auth.empty()) {
                    auto auth_hdr = "Authorization: Bearer " + auth;
                    argv.push_back("-H");
                    argv.push_back(auth_hdr.c_str());
                }
                argv.push_back("--max-time");
                argv.push_back("30");
                argv.push_back("--connect-timeout");
                argv.push_back("10");
                argv.push_back(url.c_str());
                argv.push_back(nullptr);

                ::execvp("curl", const_cast<char* const*>(argv.data()));
                ::_exit(1);
            }

            // Parent: send body, close stdin pipe
            ::close(in[0]);
            ::close(out[1]);
            ::write(in[1], body.data(), body.size());
            ::close(in[1]);

            // Read response with timeout to prevent pipe deadlock
            // Large LLM responses (>64KB pipe buffer) can block child write
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
            std::array<char, 65536> fbuf; // Large buffer to drain pipe fast
            struct pollfd pfd = {out[0], POLLIN, 0};
            ssize_t nr;
            while (true) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     deadline - std::chrono::steady_clock::now())
                                     .count();
                if (remaining <= 0)
                    break;
                int pr = ::poll(&pfd, 1, static_cast<int>(remaining));
                if (pr <= 0)
                    break;
                nr = ::read(out[0], fbuf.data(), fbuf.size());
                if (nr <= 0)
                    break;
                result.append(fbuf.data(), static_cast<std::size_t>(nr));
            }
            ::close(out[0]);

            int cstat;
            ::waitpid(pid, &cstat, 0);

            uint64_t v = 1;
            ::write(evfd, &v, sizeof(v));
        });
        t.detach();

        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();

        return result;
    };

    // Register session:create for Aura code (primitive in evaluator)
    std::function<aura::messaging::SessionCreateFn> sc_fn =
        [&sessions, &sched, shared_workspace_tree, &stdin_lines,
         &stdin_eof](const std::string& name) -> bool {
        if (sessions.count(name) > 0)
            return false;
        auto [it, created] = sessions.try_emplace(name, std::make_unique<Session>());
        if (!created)
            return false;
        it->second->id = name;
        it->second->service.set_session_id(name);
        it->second->service.set_workspace_tree(shared_workspace_tree);
        aura::compiler::CompilerService::register_session(name, &it->second->service);
        // Spawn a fiber so the session can process commands from stdin_lines
        auto nsid = name;
        auto* nf = sched.spawn([nsid, &sess = *it->second, &stdin_lines, &stdin_eof]() {
            sess.mailbox.attach(aura::serve::g_current_fiber);
            sess.service.set_wake_eventfd(aura::serve::g_current_fiber->eventfd());
            while (sess.active) {
                std::string sl;
                for (auto sit = stdin_lines.begin(); sit != stdin_lines.end(); ++sit) {
                    if (sit->find("\"session\":\"" + nsid + "\"") != std::string::npos) {
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
                        aura::messaging::g_current_compiler_service = &sess.service;
                        auto r = sess.service.exec_with_cache(code);
                        if (r) {
                            std::println(
                                "{{\"session\":\"{}\" ,\"status\":\"ok\",\"value\":\"{}\" }}",
                                json_escape(nsid), json_escape(fmt_val(*r, sess.service)));
                        } else {
                            std::println(
                                "{{\"session\":\"{}\" ,\"status\":\"error\",\"msg\":\"{}\" }}",
                                json_escape(nsid), json_escape(r.error().format()));
                        }
                    }
                }
                std::fflush(stdout);
            }
        });
        it->second->fiber = nf;
        return true;
    };
    aura::messaging::g_session_create = &sc_fn;

    // 6. Spawn session fibers (one per session)
    // For now, spawn one fiber for the default session
    // In the future, spawn as needed
    for (auto& [sid, sess] : sessions) {
        auto* fiber = sched.spawn([sid = sid, &sess = *sess, &stdin_lines, &stdin_eof, &sessions,
                                   &sched, &shared_workspace_tree]() {
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
                        // Check if this line is for our session
                        // Lines without "session" field go to "default"
                        bool for_me = false;
                        if (sid == "default") {
                            for_me = (it->find("\"session\":\"") == std::string::npos) ||
                                     (it->find("\"session\":\"default\"") != std::string::npos);
                        } else {
                            for_me = (it->find("\"session\":\"" + sid + "\"") != std::string::npos);
                        }
                        if (for_me) {
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
                    continue;
                }

                if (cmd == "exec") {
                    auto code = json_field(line, "code");
                    if (code.empty()) {
                        std::println(
                            "{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"missing code\"}}",
                            json_escape(sid));
                        continue;
                    }
                    aura::messaging::g_current_compiler_service = &sess.service;
                    auto result = sess.service.exec_with_cache(code);
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
                                    json_escape(sid), json_escape(fmt_val(v, sess.service)));
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
                        std::fflush(stdout);
                    }

                } else if (cmd == "session") {
                    auto action = json_field(line, "action");
                    if (action == "create") {
                        auto name = json_field(line, "name");
                        if (name.empty()) {
                            std::println("{{\"session\":\"{}\" "
                                         ",\"status\":\"error\",\"msg\":\"missing name\"}}",
                                         json_escape(sid));
                        } else {
                            auto [it, created] =
                                sessions.try_emplace(name, std::make_unique<Session>());
                            if (created) {
                                it->second->id = name;
                                it->second->service.set_session_id(name);
                                it->second->service.set_workspace_tree(shared_workspace_tree);
                                aura::compiler::CompilerService::register_session(
                                    name, &it->second->service);
                                // Register this new session's evaluator as
                                // a GC root source so the GC walks its
                                // heaps during collect_roots(). Worker_id
                                // is the session's hash mod a small prime
                                // to spread them; the value only needs to
                                // be unique per session.
                                int wid =
                                    static_cast<int>(std::hash<std::string>{}(name) % 997) + 1;
                                sched.gc_collector()->register_root_source(
                                    wid, [&sess = *it->second](aura::serve::GCRootSet& out) {
                                        sess.service.evaluator().flush_gc_roots(&out);
                                    });
                                // Spawn fiber for new session
                                auto nsid = name;
                                auto* nf = sched.spawn([nsid, &sess = *it->second, &stdin_lines,
                                                        &stdin_eof, &sessions, &sched]() {
                                    sess.mailbox.attach(aura::serve::g_current_fiber);
                                    sess.service.set_wake_eventfd(
                                        aura::serve::g_current_fiber->eventfd());
                                    while (sess.active) {
                                        std::string sl;
                                        for (auto sit = stdin_lines.begin();
                                             sit != stdin_lines.end(); ++sit) {
                                            if (sit->find("\"session\":\"" + nsid + "\"") !=
                                                std::string::npos) {
                                                sl = std::move(*sit);
                                                stdin_lines.erase(sit);
                                                break;
                                            }
                                        }
                                        if (sl.empty()) {
                                            if (stdin_eof)
                                                break;
                                            aura::serve::g_current_fiber->set_state(
                                                aura::serve::FiberState::Waiting);
                                            aura::serve::Fiber::yield();
                                            continue;
                                        }
                                        auto c = json_field(sl, "cmd");
                                        if (c == "exec") {
                                            auto code = json_field(sl, "code");
                                            if (!code.empty()) {
                                                auto r = sess.service.exec_with_cache(code);
                                                if (r) {
                                                    std::println(
                                                        "{{\"session\":\"{}\" "
                                                        ",\"status\":\"ok\",\"value\":\"{}\" }}",
                                                        json_escape(nsid),
                                                        json_escape(fmt_val(*r, sess.service)));
                                                } else {
                                                    std::println(
                                                        "{{\"session\":\"{}\" "
                                                        ",\"status\":\"error\",\"msg\":\"{}\" }}",
                                                        json_escape(nsid),
                                                        json_escape(r.error().format()));
                                                }
                                            }
                                        }
                                    }
                                });
                                it->second->fiber = nf;
                                std::println("{{\"session\":\"{}\" "
                                             ",\"status\":\"created\",\"name\":\"{}\" }}",
                                             json_escape(sid), json_escape(name));
                            } else {
                                std::println("{{\"session\":\"{}\" "
                                             ",\"status\":\"error\",\"msg\":\"already exists\"}}",
                                             json_escape(sid));
                            }
                        }
                    } else {
                        std::println("{{\"session\":\"{}\" ,\"status\":\"error\",\"msg\":\"unknown "
                                     "action: {}\" }}",
                                     json_escape(sid), json_escape(action));
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

                } else if (cmd == "session-recv") {
                    auto msg = sess.mailbox.pop(true); // blocking pop (yields)
                    if (!msg.empty()) {
                        std::println("{{\"session\":\"{}\",\"status\":\"msg\",\"data\":\"{}\"}}",
                                     json_escape(sid), json_escape(msg));
                    } else {
                        std::println("{{\"session\":\"{}\",\"status\":\"timeout\"}}",
                                     json_escape(sid));
                    }

                } else {
                    std::println(
                        "{{\"session\":\"{}\",\"status\":\"error\",\"msg\":\"unknown cmd: {}\"}}",
                        json_escape(sid), json_escape(cmd));
                }
                std::fflush(stdout);
            }
        });

        sess->fiber = fiber;
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

    // Register fiber:spawn callback
    aura::messaging::g_fiber_spawn = [&sched](std::function<void()> fn) -> int64_t {
        auto* f = sched.spawn(std::move(fn));
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

    // Register async HTTP handler (same as run_serve_async)
    aura::messaging::g_http_post_async = [](const std::string& url, const std::string& body,
                                            const std::string& auth) -> std::string {
        auto* fiber = aura::serve::g_current_fiber;
        if (!fiber)
            return {};
        auto evfd = fiber->eventfd();
        if (evfd < 0)
            return {};

        std::string result;
        std::thread t([evfd, url, body, auth, &result]() {
            int in[2], out[2];
            if (::pipe(in) < 0 || ::pipe(out) < 0) {
                uint64_t v = 1;
                ::write(evfd, &v, sizeof(v));
                return;
            }
            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(in[0]);
                ::close(in[1]);
                ::close(out[0]);
                ::close(out[1]);
                uint64_t v = 1;
                ::write(evfd, &v, sizeof(v));
                return;
            }
            if (pid == 0) {
                ::close(in[1]);
                ::close(out[0]);
                ::dup2(in[0], STDIN_FILENO);
                ::dup2(out[1], STDOUT_FILENO);
                ::close(in[0]);
                ::close(out[1]);
                std::vector<const char*> argv;
                argv.push_back("curl");
                argv.push_back("-s");
                argv.push_back("-X");
                argv.push_back("POST");
                argv.push_back("--data-binary");
                argv.push_back("@-");
                argv.push_back("-H");
                argv.push_back("Content-Type: application/json");
                if (!auth.empty()) {
                    auto auth_hdr = "Authorization: Bearer " + auth;
                    argv.push_back("-H");
                    argv.push_back(auth_hdr.c_str());
                }
                argv.push_back("--max-time");
                argv.push_back("30");
                argv.push_back("--connect-timeout");
                argv.push_back("10");
                argv.push_back(url.c_str());
                argv.push_back(nullptr);
                ::execvp("curl", const_cast<char* const*>(argv.data()));
                ::_exit(1);
            }
            ::close(in[0]);
            ::close(out[1]);
            ::write(in[1], body.data(), body.size());
            ::close(in[1]);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(35);
            std::array<char, 65536> fbuf;
            struct pollfd pfd = {out[0], POLLIN, 0};
            ssize_t nr;
            while (true) {
                auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     deadline - std::chrono::steady_clock::now())
                                     .count();
                if (remaining <= 0)
                    break;
                int pr = ::poll(&pfd, 1, static_cast<int>(remaining));
                if (pr <= 0)
                    break;
                nr = ::read(out[0], fbuf.data(), fbuf.size());
                if (nr <= 0)
                    break;
                result.append(fbuf.data(), static_cast<std::size_t>(nr));
            }
            ::close(out[0]);
            int cstat;
            ::waitpid(pid, &cstat, 0);
            uint64_t v = 1;
            ::write(evfd, &v, sizeof(v));
        });
        t.detach();
        aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
        aura::serve::Fiber::yield();
        return result;
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
    sched.spawn([&sess, bench_code = std::move(bench_code), &sched]() {
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
    });

    // 6. Run the scheduler
    sched.run();
}

} // namespace aura::serve
