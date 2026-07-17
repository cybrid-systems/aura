// serve/scheduler.cpp — Multi-threaded fiber scheduler
#include "scheduler.h"
#include "gc_coordinator.h"
#include "aura_platform.h"
#include "core/gc_hooks.h"
#include "core/resource_quota.hh"
#include <unistd.h>

import std;
#if AURA_HAVE_EPOLL
#include <sys/epoll.h>
#endif
#if AURA_HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

namespace aura::serve {

extern "C" void aura_scheduler_init_record_ok();
extern "C" void aura_scheduler_init_record_err();


// ── Constructor ───────────────────────────────────────

Scheduler::Scheduler(int num_workers) {
    // Initialize metrics
    metrics_on_ = true;
    // Default: hardware concurrency, capped at reasonable range
    if (num_workers <= 0) {
        num_workers = static_cast<int>(std::thread::hardware_concurrency());
        if (num_workers < 2)
            num_workers = 2;
        if (num_workers > 16)
            num_workers = 16;
    }
    num_workers_ = num_workers;

    // Create epoll instance
#if AURA_HAVE_EPOLL
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ == -1) {
        aura_scheduler_init_record_err();
        throw std::system_error(errno, std::generic_category(), "scheduler epoll_create");
    }

    // Register stdin (fd 0) with edge-triggered mode so the IO
    // thread can wake when input arrives (REPL / serve-async mode).
    // This is best-effort: in test environments stdin may be a
    // socket or a redirected file descriptor that the kernel
    // refuses to add to epoll with EPERM. We log and continue
    // rather than abort the scheduler — tests that don't drive
    // stdin don't care, and crashing here made the test_concurrent
    // binary flaky in CI sandboxes (Issue #115 follow-up).
    stdin_fd_ = STDIN_FILENO;
    struct epoll_event ee;
    ee.events = EPOLLIN | EPOLLET;
    ee.data.ptr = nullptr; // nullptr = stdin event
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, stdin_fd_, &ee) == -1) {
        std::fprintf(stderr,
                     "scheduler: stdin not epollable (errno=%d: %s); "
                     "REPL/serve-async stdin handling disabled for this scheduler\n",
                     errno, std::strerror(errno));
        stdin_fd_ = -1; // mark as not registered
    }
#else
    // macOS: no epoll. serve-async is disabled; the scheduler can
    // still be constructed (workers spin up) but run() is a no-op.
    epoll_fd_ = -1;
    stdin_fd_ = -1;
#endif

    // Also register the scheduler's own wakeup eventfd for fast shutdown
    // (self-wake from stop())

    // Create workers
    workers_.reserve(num_workers_);
    for (int i = 0; i < num_workers_; ++i) {
        auto w = std::make_unique<WorkerThread>(i, this);
        workers_.push_back(std::move(w));
    }

    // Size metrics to match workers
    metrics_.resize_workers(static_cast<size_t>(num_workers_));

    // Initialize GC collector
    gc_collector_ = std::make_unique<GCCollector>(this);

    // Issue #810: successful init path (AuraResult-style ok counter).
    aura_scheduler_init_record_ok();
}

// ── Destructor ───────────────────────────────────────

Scheduler::~Scheduler() {
    stop();
    for (auto& w : workers_) {
        w->join();
    }
    workers_.clear();
    // Issue #707: destroy owned fibers so per-fiber stack vectors
    // return to the bounded pool instead of leaking until process exit.
    {
        std::lock_guard<std::mutex> lock(owned_fibers_mutex_);
        owned_fibers_.clear();
    }
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

// ── spawn — create a new fiber ────────────────────────
// Creates the fiber and assigns it to a worker (round-robin).

Fiber* Scheduler::spawn(Fiber::Func func, size_t stack_size) {
    // Issue #1579: process-wide fiber quota before spawn (misbehaving agent isolation).
    // Returns nullptr when ResourceQuota fibers dimension is exceeded.
    // Paired release in on_fiber_done.
    using aura::core::resource_quota::Dimension;
    using aura::core::resource_quota::process_resource_quota;
    if (auto err = process_resource_quota().check_and_consume(Dimension::Fibers, 1)) {
        (void)err;
        return nullptr;
    }

    auto fb = std::make_unique<Fiber>(std::move(func), stack_size);
    auto* ptr = fb.get();

    // Register eventfd with epoll
#if AURA_HAVE_EPOLL
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = ptr;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ptr->eventfd(), &ee);
#endif

    {
        std::lock_guard<std::mutex> lock(wait_map_mutex_);
        wait_map_[ptr->eventfd()] = ptr;
    }

    // Store fiber for lifetime management (Issue #707: per-scheduler).
    {
        std::lock_guard<std::mutex> lock(owned_fibers_mutex_);
        owned_fibers_.push_back(std::move(fb));
    }

    // Assign to a worker (load-aware when enabled, fallback to round-robin)
    int wid;
    if (ptr->affinity() >= 0) {
        // Pinned fiber: respect affinity, clamp to valid range
        wid = std::min(ptr->affinity(), static_cast<int>(workers_.size()) - 1);
        if (wid < 0)
            wid = 0;
    } else {
        wid = use_load_aware_distribution_ ? next_worker_id_load_aware() : next_worker_id();
    }
    workers_[wid]->enqueue(ptr);
    // Issue #119: register the fiber in the worker's
    // registry so fiber:join can find the Fiber* by ID.
    workers_[wid]->register_fiber(ptr);

    // Metrics
    if (metrics_on_) {
        metrics_.fibers_spawned.fetch_add(1, std::memory_order_relaxed);
    }

    return ptr;
}

Fiber* Scheduler::spawn_with_affinity(Fiber::Func func, int worker_id, size_t stack_size) {
    // Issue #1579: same process-wide fiber quota as spawn().
    using aura::core::resource_quota::Dimension;
    using aura::core::resource_quota::process_resource_quota;
    if (auto err = process_resource_quota().check_and_consume(Dimension::Fibers, 1)) {
        (void)err;
        return nullptr;
    }

    auto fb = std::make_unique<Fiber>(std::move(func), stack_size);
    auto* ptr = fb.get();
    if (worker_id >= 0 && worker_id < static_cast<int>(workers_.size())) {
        ptr->set_affinity(worker_id);
    }

#if AURA_HAVE_EPOLL
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = ptr;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ptr->eventfd(), &ee);
#endif

    {
        std::lock_guard<std::mutex> lock(wait_map_mutex_);
        wait_map_[ptr->eventfd()] = ptr;
    }

    {
        std::lock_guard<std::mutex> lock(owned_fibers_mutex_);
        owned_fibers_.push_back(std::move(fb));
    }

    workers_[worker_id]->enqueue(ptr);
    // Issue #119: register the fiber for fiber:join lookup.
    workers_[worker_id]->register_fiber(ptr);

    if (metrics_on_) {
        metrics_.fibers_spawned.fetch_add(1, std::memory_order_relaxed);
    }

    return ptr;
}

// ── stop ─────────────────────────────────────────────

void Scheduler::stop() {
    running_.store(false, std::memory_order_release);
    for (auto& w : workers_) {
        w->stop();
    }
}

// ── register_event_fiber ─────────────────────────────

void Scheduler::register_event_fiber(int eventfd, Fiber* fiber) {
#if AURA_HAVE_EPOLL
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = fiber;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, eventfd, &ee);
#endif

    std::lock_guard<std::mutex> lock(wait_map_mutex_);
    wait_map_[eventfd] = fiber;
}

// ── unregister_fiber ─────────────────────────────────

void Scheduler::unregister_fiber(int eventfd) {
#if AURA_HAVE_EPOLL
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, eventfd, nullptr);
#endif
    std::lock_guard<std::mutex> lock(wait_map_mutex_);
    wait_map_.erase(eventfd);
}

// ── on_fiber_done — called by worker when fiber completes ──
// Removes the fiber's eventfd from epoll and cleans up wait map.

void Scheduler::on_fiber_done(Fiber* fiber) {
    if (!fiber)
        return;
    // Issue #1579: release process fiber quota reserved at spawn.
    aura::core::resource_quota::process_resource_quota().release(
        aura::core::resource_quota::Dimension::Fibers, 1);

    int evfd = fiber->eventfd();
    if (evfd >= 0) {
#if AURA_HAVE_EPOLL
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, evfd, nullptr);
#endif
        std::lock_guard<std::mutex> lock(wait_map_mutex_);
        wait_map_.erase(evfd);
    }

    // Issue #119: remove the fiber from its worker's registry.
    // We don't know which worker it was on, so scan all. The
    // per-worker register/unregister uses a small mutex, so
    // this is cheap. (The fiber is also no longer on any
    // worker's queue at this point — the worker has
    // decremented running_fiber_count and the queue push
    // happens during dispatch.)
    for (auto& w : workers_) {
        w->unregister_fiber(fiber);
    }

    // Issue #119: wake all fibers that joined on this one. The
    // joiner_map_ entry is cleared after notification so a
    // future join on the same (now-destroyed) target ID won't
    // try to wake dead fibers. Joiners are notified by writing
    // a 1 to their eventfds — the IO thread's epoll will pick
    // up the write and resume the joiner.
    std::vector<Fiber*> joiners;
    {
        std::lock_guard<std::mutex> lock(joiner_map_mutex_);
        auto it = joiner_map_.find(fiber->id());
        if (it != joiner_map_.end()) {
            joiners = std::move(it->second);
            joiner_map_.erase(it);
        }
    }
    for (Fiber* joiner : joiners) {
        if (!joiner)
            continue;
        int joiner_evfd = joiner->eventfd();
        if (joiner_evfd >= 0) {
            uint64_t one = 1;
            // Best-effort: ignore short writes. The joiner's
            // eventfd is non-blocking (EFD_NONBLOCK), so this
            // write either succeeds or is dropped (already 1).
            ::write(joiner_evfd, &one, sizeof(one));
        }
    }

    if (metrics_on_) {
        metrics_.fibers_completed.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #119: add a joiner fiber to a target's wait list.
// Returns true on success, false if the target fiber can't
// be found. The target may be in any state except Done —
// callers should check `fiber_by_id(id)->is_done()` first.
bool Scheduler::add_joiner(std::uint64_t target_fiber_id, Fiber* joiner) {
    if (!joiner)
        return false;
    Fiber* target = fiber_by_id(target_fiber_id);
    if (!target)
        return false;
    std::lock_guard<std::mutex> lock(joiner_map_mutex_);
    auto& list = joiner_map_[target_fiber_id];
    // Idempotent: if the joiner is already in the list, skip.
    for (auto* f : list) {
        if (f == joiner)
            return true;
    }
    list.push_back(joiner);
    return true;
}

// Issue #119: remove a joiner. Idempotent.
void Scheduler::remove_joiner(std::uint64_t target_fiber_id, Fiber* joiner) {
    if (!joiner)
        return;
    std::lock_guard<std::mutex> lock(joiner_map_mutex_);
    auto it = joiner_map_.find(target_fiber_id);
    if (it == joiner_map_.end())
        return;
    auto& list = it->second;
    list.erase(std::remove(list.begin(), list.end(), joiner), list.end());
    if (list.empty())
        joiner_map_.erase(it);
}

// Issue #119: lookup a fiber by ID. Returns nullptr if no
// such fiber exists. Used by the evaluator's fiber:join to
// check if the target is done before registering as a joiner.
Fiber* Scheduler::fiber_by_id(std::uint64_t fiber_id) const {
    // Linear scan over workers. The number of workers is small
    // (typically <= 8), so this is fine for now. If joiner-map
    // traffic becomes a hotspot, switch to a per-worker hashmap.
    for (auto& w : workers_) {
        Fiber* f = w->fiber_by_id(fiber_id);
        if (f)
            return f;
    }
    return nullptr;
}

// ── has_waiting_fibers — check epoll wait map ─────────

void Scheduler::on_long_mutation_held(std::uint64_t fiber_id, std::uint64_t duration_us) {
    (void)duration_us;
    // Issue #1445 AC6: bump starvation_mitigated_count so observability
    // surfaces the long-holder event. Default impl is telemetry-only;
    // production deployments may override via Scheduler subclass or
    // by calling AdaptiveStealStats counters directly.
    metrics::adaptive_steal_stats().starvation_mitigated_count.fetch_add(1,
                                                                         std::memory_order_relaxed);
    // Issue #1445 follow-up: priority-degrade signal — also bump
    // deferred_pressure_boosts so worker.cpp's adaptive budget path
    // (which already consumes this counter) prefers ring-neighbor steal
    // + extra budget for the next round. The actual queue manipulation
    // is handled by WorkerThread; this hook just signals.
    metrics::adaptive_steal_stats().deferred_pressure_boosts.fetch_add(1,
                                                                       std::memory_order_relaxed);
    // Issue #1492: long-mutation held often coincides with nested
    // MutationBoundary (inner Guard). Link the same starvation-
    // mitigation signal used by the steal-defer path so agents can
    // correlate long-holder events with inner-defer fairness.
    metrics::adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.fetch_add(
        1, std::memory_order_relaxed);
    metrics::adaptive_steal_stats().starvation_priority_boosts.fetch_add(1,
                                                                         std::memory_order_relaxed);
    // Boost the long-holding fiber if we can resolve it (helps it
    // finish outermost and release the nested guard sooner).
    if (fiber_id != 0) {
        if (Fiber* f = fiber_by_id(fiber_id))
            f->apply_steal_priority_boost();
    }
}

bool Scheduler::has_waiting_fibers() const {
    std::lock_guard<std::mutex> lock(wait_map_mutex_);
    // Issue #63723: skip entries whose Fiber* is not currently
    // owned by this scheduler (defensive — the underlying
    // corruption that produces such entries is a separate
    // root-cause investigation; this prevents the worker
    // from SIGSEGV'ing on a stale entry).
    for (auto& [evfd, fiber] : wait_map_) {
        if (!fiber)
            continue;
        if (!owned_fibers_end_contains(fiber))
            continue;
        if (fiber->state() == FiberState::Waiting)
            return true;
    }
    return false;
}

// ── worker — access worker by index ──────────────────

WorkerThread* Scheduler::worker(int idx) {
    if (idx < 0 || idx >= num_workers_)
        return nullptr;
    return workers_[idx].get();
}

// ── next_worker_id — round-robin worker assignment ────

int Scheduler::next_worker_id() {
    int id = next_worker_.fetch_add(1, std::memory_order_acq_rel);
    return id % num_workers_;
}

// ── next_worker_id_load_aware — pick least-loaded worker ──
// Scans workers' local queue sizes and picks the one with
// the smallest queue. Falls back to round-robin if
// all queues are empty (avoids unnecessary scanning).

int Scheduler::next_worker_id_load_aware() {
    int best_id = 0;
    size_t best_size = SIZE_MAX;
    bool any_nonempty = false;

    for (int i = 0; i < num_workers_; ++i) {
        auto* w = workers_[i].get();
        if (!w)
            continue;
        size_t qs = w->queue_size();
        if (qs > 0)
            any_nonempty = true;
        if (qs <= best_size) {
            best_size = qs;
            best_id = i;
        }
    }

    if (!any_nonempty) {
        // All empty — fall back to simple round-robin
        return next_worker_id();
    }

    return best_id;
}

// ── run — main IO event loop ─────────────────────────
//
// The IO thread (main thread) runs the epoll event loop.
// It monitors:
//   - stdin (fd 0): new commands for session fibers
//   - Worker wake eventfds: not used directly here since workers
//     self-wake via their own eventfd
//   - Fiber eventfds: when a fiber in Waiting state gets woken
//     (e.g., by send/recv or thread pool completion), the IO thread
//     enqueues it to a worker.

void Scheduler::run() {
    g_scheduler = this;

    // Issue #743: wire arena fiber-context probes for tests and
    // serve paths that construct Scheduler without serve_async.
    aura::gc_hooks::g_fiber_active.store(
        +[]() noexcept { return aura::serve::g_current_fiber != nullptr; });
    aura::gc_hooks::g_arena_safepoint_check.store(
        +[]() noexcept { aura::serve::Fiber::check_gc_safepoint(); });

    // Link metrics to workers before starting
    for (size_t i = 0; i < workers_.size(); ++i) {
        workers_[i]->set_metrics(&metrics_.worker(i));
    }

    // Start all workers
    for (auto& w : workers_) {
        w->start();
    }

#if AURA_HAVE_EPOLL
    struct epoll_event events[64];

    while (running_.load(std::memory_order_acquire)) {
        // Block on epoll_wait for events
        // Timeout: check running_ periodically (in case all fibers are busy)
        int n = ::epoll_wait(epoll_fd_, events, 64, 1000);

        if (!running_.load(std::memory_order_acquire))
            break;

        if (n < 0) {
            if (errno == EINTR)
                continue;
            std::fprintf(stderr, "scheduler: epoll_wait failed: %s\n", std::strerror(errno));
            break;
        }

        // Process events
        for (int i = 0; i < n; ++i) {
            if (events[i].data.ptr == nullptr) {
                // stdin event — wake the stdin fiber AND all waiting fibers
                if (metrics_on_) {
                    metrics_.io_stdin_events.fetch_add(1, std::memory_order_relaxed);
                }
                if (stdin_fiber_) {
                    int wid;
                    if (stdin_fiber_->affinity() >= 0) {
                        wid = std::min(stdin_fiber_->affinity(),
                                       static_cast<int>(workers_.size()) - 1);
                        if (wid < 0)
                            wid = 0;
                    } else {
                        wid = next_worker_id();
                    }
                    workers_[wid]->enqueue(stdin_fiber_);
                }
                // Always wake all waiting fibers on stdin activity
                // (they may be waiting for stdin data from the reader)
                {
                    std::lock_guard<std::mutex> lock(wait_map_mutex_);
                    for (auto& [evfd, fiber] : wait_map_) {
                        // Issue #63723: same defensive guard as
                        // in has_waiting_fibers() — skip entries
                        // whose Fiber* is not currently owned by
                        // this scheduler (stale/corrupt entries
                        // that would crash on fiber->state()).
                        if (!fiber)
                            continue;
                        if (!owned_fibers_end_contains(fiber))
                            continue;
                        if (fiber->state() == FiberState::Waiting) {
                            int wid;
                            if (fiber->affinity() >= 0) {
                                wid = std::min(fiber->affinity(),
                                               static_cast<int>(workers_.size()) - 1);
                                if (wid < 0)
                                    wid = 0;
                            } else {
                                wid = next_worker_id();
                            }
                            workers_[wid]->enqueue(fiber);
                        }
                    }
                }
            } else {
                // Fiber eventfd event
                auto* fiber = static_cast<Fiber*>(events[i].data.ptr);
                // Issue #63723: defensive guard against corrupted
                // Fiber* pointers (test_issue_226 crash). If the
                // fiber is not currently owned by this scheduler,
                // skip the event rather than dereference a stale
                // pointer. The eventfd may have been left in
                // epoll after a fiber was completed and removed
                // from wait_map_; the spurious wakeup is harmless
                // once we drop the event.
                if (!fiber)
                    continue;
                if (!owned_fibers_end_contains(fiber)) {
                    // Drop the event. Drain the eventfd so it
                    // doesn't re-fire (EFD_NONBLOCK on fiber
                    // eventfds).
                    uint64_t val;
                    ::read(fiber->eventfd(), &val, sizeof(val));
                    continue;
                }
                if (fiber->is_done())
                    continue;

                // Drain the eventfd (read the 8-byte counter)
                uint64_t val;
                ::read(fiber->eventfd(), &val, sizeof(val));

                // Metrics
                if (metrics_on_) {
                    metrics_.io_events_processed.fetch_add(1, std::memory_order_relaxed);
                }

                // Enqueue to a worker for resumption (respect affinity)
                int wid;
                if (fiber->affinity() >= 0) {
                    wid = std::min(fiber->affinity(), static_cast<int>(workers_.size()) - 1);
                    if (wid < 0)
                        wid = 0;
                } else {
                    wid = next_worker_id();
                }
                workers_[wid]->enqueue(fiber);
            }
        }

        // Check if all fibers are done
        {
            std::lock_guard<std::mutex> lock(wait_map_mutex_);
            bool all_idle = wait_map_.empty();
            if (all_idle) {
                // No fibers in epoll — check if any have pending work
                for (auto& w : workers_) {
                    if (w->queue_size() > 0 || w->pending_count() > 0) {
                        all_idle = false;
                        break;
                    }
                }
                if (all_idle && !running_.load(std::memory_order_acquire))
                    break;
                // If idle for multiple cycles, auto-stop (avoids hang)
                // Use a counter to prevent premature stop
                static thread_local int idle_cycles = 0;
                if (all_idle) {
                    ++idle_cycles;
                    if (idle_cycles >= 3) {
                        // All fibers completed — auto-stop
                        running_.store(false, std::memory_order_release);
                        break;
                    }
                } else {
                    idle_cycles = 0;
                }
            }
        }
    }
#else
    // macOS: no epoll. Workers start then immediately stop.
    // serve-async is disabled; main.cpp blocks --serve-async.
    std::fprintf(stderr, "scheduler: serve-async not supported on macOS\n");
#endif

    // Stop all workers
    for (auto& w : workers_) {
        w->stop();
    }
    for (auto& w : workers_) {
        w->join();
    }

    g_scheduler = nullptr;
}

// ── GC safepoint support (P2) ─────────────────────────

int Scheduler::request_gc_safepoint() {
    // Broadcast GCPhase::Requested to all workers
    int acknowledged = 0;
    for (auto& w : workers_) {
        if (!w)
            continue;
        auto& gc = w->gc_state();
        gc.phase.store(GCPhase::Requested, std::memory_order_release);
        gc.fibers_at_safepoint.store(0, std::memory_order_release);
        ++acknowledged;
    }
    return acknowledged;
}

bool Scheduler::wait_for_safepoint(int timeout_ms) {
    // Helper: check if all workers are quiescent (no running
    // fibers, no fibers arrived at safepoint, no queued fibers).
    //
    // Issue #115: also check `running_fiber_count_`. A fiber
    // currently executing on a worker holds the worker's stack
    // with live references. If the GC proceeds while a fiber
    // is running, those stack references are missed during
    // root collection, leading to use-after-free during sweep.
    //
    // The fiber's own `check_gc_safepoint` will increment
    // `fibers_at_safepoint` when the fiber next yields or
    // allocates. The running-fiber counter is the worker's
    // accounting: it's > 0 while the worker is in `resume()`
    // and the fiber hasn't yielded back yet.
    auto all_quiescent = [this]() {
        for (auto& w : workers_) {
            if (!w)
                continue;
            auto& gc = w->gc_state();
            // Skip workers with no active fibers (empty queue, nothing
            // pending, no running fiber). These workers are
            // participating in the safepoint trivially (they have
            // nothing to wait for).
            if (w->queue_size() == 0 && w->pending_count() == 0 &&
                gc.fibers_at_safepoint.load(std::memory_order_acquire) == 0 &&
                gc.running_fiber_count.load(std::memory_order_acquire) == 0) {
                continue;
            }
            // Worker has active state. Wait for the running fiber
            // (if any) to finish, AND for a fiber to have
            // arrived at the safepoint. The `running_fiber_count
            // == 0` check ensures the fiber is no longer
            // executing; the `fibers_at_safepoint >= 1` check
            // ensures it arrived at the safepoint.
            //
            // Important: a fiber that has called check_gc_safepoint()
            // and entered the spin-wait IS at the safepoint, even
            // though running_fiber_count is still 1 (the fiber
            // is in resume() spin-waiting, not yielded). So we
            // only require running_fiber_count == 0 if no fiber
            // has yet arrived — the "running but not arrived"
            // case is the one that the Issue #115 fix targets.
            if (gc.fibers_at_safepoint.load(std::memory_order_acquire) < 1 &&
                gc.running_fiber_count.load(std::memory_order_acquire) > 0) {
                return false;
            }
            if (gc.fibers_at_safepoint.load(std::memory_order_acquire) < 1) {
                return false;
            }
        }
        return true;
    };

    // Spin for a short time first (fast path)
    constexpr int SPIN_US = 100;
    int elapsed_us = 0;
    while (elapsed_us < SPIN_US * 10) { // max ~1ms spin
        if (all_quiescent())
            return true;
        // Tiny pause to avoid hammering
#if defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#else
        asm volatile("" ::: "memory");
#endif
        elapsed_us += 1;
    }

    // After spin fails, fall back to epoll timeout wait
    for (int attempt = 0; attempt < std::max(1, timeout_ms); ++attempt) {
        if (all_quiescent())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return false; // timeout
}

void Scheduler::resume_from_gc() {
    for (auto& w : workers_) {
        if (!w)
            continue;
        auto& gc = w->gc_state();
        gc.phase.store(GCPhase::None, std::memory_order_release);
    }
}

} // namespace aura::serve
