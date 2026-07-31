// serve/scheduler.cpp — Multi-threaded fiber scheduler
#include "scheduler.h"
#include "gc_coordinator.h"
#include "aura_platform.h"
#include "core/gc_hooks.h"
#include "core/resource_quota.hh"
#include "compiler/lock_order_audit.h" // Issue #2354: rank audit
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

// Issue #1633: C ABI from aura_jit_bridge — Guard dtor invokes this when
// hold > long_mutation_threshold_us. Wire to g_scheduler->on_long_mutation_held.
extern "C" void aura_set_long_mutation_scheduler_hook(void (*fn)(std::uint64_t fiber_id,
                                                                 std::uint64_t duration_us));

// Issue #1641: weak C trampoline (strong def in evaluator_fiber_mutation.cpp).
extern "C" void aura_evaluator_bump_starvation_mitigated_for_boundary() __attribute__((weak));

static void long_mutation_hook_trampoline(std::uint64_t fiber_id,
                                          std::uint64_t duration_us) noexcept {
    if (g_scheduler != nullptr)
        g_scheduler->on_long_mutation_held(fiber_id, duration_us);
}

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
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            owned_fibers_mutex_, ::aura::compiler::lock_order::Level::OwnedFibers);
        owned_fibers_.clear();
    }
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

// ── spawn — create a new fiber ────────────────────────
// Creates the fiber and assigns it to a worker (round-robin).

Fiber* Scheduler::spawn(Fiber::Func func, size_t stack_size) {
    // Issue #1579 / #1618: process-wide fiber quota via ResourceQuotaManager
    // before spawn (misbehaving agent isolation). Returns nullptr when
    // fibers dimension is exceeded. Paired release in on_fiber_done.
    // Typed ResourceQuotaExceeded at orch callers (not PanicCheckpoint).
    using aura::core::resource_quota::Dimension;
    using aura::core::resource_quota::process_resource_quota;
    using aura::core::resource_quota::process_resource_quota_manager;
    if (auto err = process_resource_quota_manager().check_and_consume_fiber()) {
        (void)err;
        // Issue #1600: orchestration spawn reject metrics (typed error at caller).
        process_resource_quota().fiber_spawn_rejected_total.fetch_add(1, std::memory_order_relaxed);
        process_resource_quota().orchestration_quota_exceeded_total.fetch_add(
            1, std::memory_order_relaxed);
        return nullptr;
    }

    auto fb = std::make_unique<Fiber>(std::move(func), stack_size);
    auto* ptr = fb.get();

    // Issue #2227: owner Scheduler back-pointer so the orch join path
    // can register hard-reclaim orphans without a global FiberId → Scheduler
    // lookup. Set before any registration so a concurrent note_orphan
    // call from a join path sees a non-null owner_sched().
    ptr->set_owner_sched(this);

    // Register eventfd with epoll
#if AURA_HAVE_EPOLL
    struct epoll_event ee;
    ee.events = EPOLLIN;
    ee.data.ptr = ptr;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, ptr->eventfd(), &ee);
#endif

    {
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
        wait_map_[ptr->eventfd()] = ptr;
    }

    // Store fiber for lifetime management (Issue #707: per-scheduler).
    {
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            owned_fibers_mutex_, ::aura::compiler::lock_order::Level::OwnedFibers);
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
    // Issue #1579 / #1618: same process-wide fiber quota as spawn() via manager.
    using aura::core::resource_quota::process_resource_quota;
    using aura::core::resource_quota::process_resource_quota_manager;
    if (auto err = process_resource_quota_manager().check_and_consume_fiber()) {
        (void)err;
        // Issue #1600
        process_resource_quota().fiber_spawn_rejected_total.fetch_add(1, std::memory_order_relaxed);
        process_resource_quota().orchestration_quota_exceeded_total.fetch_add(
            1, std::memory_order_relaxed);
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
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
        wait_map_[ptr->eventfd()] = ptr;
    }

    {
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            owned_fibers_mutex_, ::aura::compiler::lock_order::Level::OwnedFibers);
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

    ::aura::compiler::lock_order::AuditedMutexLock lock(
        wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
    wait_map_[eventfd] = fiber;
}

// ── unregister_fiber ─────────────────────────────────

void Scheduler::unregister_fiber(int eventfd) {
#if AURA_HAVE_EPOLL
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, eventfd, nullptr);
#endif
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
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
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
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
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            joiner_map_mutex_, ::aura::compiler::lock_order::Level::Joiner);
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
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        joiner_map_mutex_, ::aura::compiler::lock_order::Level::Joiner);
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
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        joiner_map_mutex_, ::aura::compiler::lock_order::Level::Joiner);
    auto it = joiner_map_.find(target_fiber_id);
    if (it == joiner_map_.end())
        return;
    auto& list = it->second;
    list.erase(std::remove(list.begin(), list.end(), joiner), list.end());
    if (list.empty())
        joiner_map_.erase(it);
}

// Issue #2227: hard-reclaim orphan tracking. The orch join path
// registers a fiber here after observing !is_done() post-drain.
// The Scheduler holds the orphan for hard_deadline_ms, then
// reaps it via reap_orphans_now(). Idempotent: if the same
// fiber is already on the orphan list (e.g. multiple residual
// observations for the same fiber), the entry is refreshed in
// place (newest hard_deadline wins) instead of double-registered.
void Scheduler::note_orphan_fiber(Fiber* f, std::uint64_t hard_deadline_ms) noexcept {
    if (!f || hard_deadline_ms == 0)
        return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(hard_deadline_ms);
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        orphan_mutex_, ::aura::compiler::lock_order::Level::Orphan);
    // Refresh in place if already registered (newest deadline wins).
    for (auto& e : orphan_fibers_) {
        if (e.fiber == f) {
            e.hard_deadline = deadline;
            // Size unchanged; still publish for desync safety.
            orphan_count_cached_.store(orphan_fibers_.size(), std::memory_order_relaxed);
            return;
        }
    }
    orphan_fibers_.push_back(OrphanEntry{f, deadline});
    // Issue #2396: publish size for zero-cost empty tick check.
    orphan_count_cached_.store(orphan_fibers_.size(), std::memory_order_relaxed);
}

// Issue #2227: force-reap all orphans past their hard_deadline.
// For each candidate (!is_done && !is_reclaimed):
//   - mark fiber reclaimed_ (so joiners see "logically done")
//   - drop from wait_map_ / joiner_map_ (wake joiners with 1-byte write)
//   - drop from owned_fibers_ (releases the unique_ptr)
//   - unregister from all workers
//   - release process fiber quota (paired with spawn)
//   - bump orphans_reaped_total_
// Returns the number of fibers actually reaped. Idempotent: a
// second call past the same deadline is a no-op (all entries
// already removed from orphan_fibers_).
//
// Lock order: orphan_mutex_ is held throughout the reaping pass
// (the list is small, typically 0–1 entries under cancel storms).
// Per-fiber cleanup acquires wait_map_mutex_ → joiner_map_mutex_
// → owned_fibers_mutex_ in the same order as on_fiber_done
// (line 271) to avoid inversion. Per-worker unregister takes the
// worker's own internal mutex; order across workers doesn't matter.
std::size_t Scheduler::reap_orphans_now() noexcept {
    const auto now = std::chrono::steady_clock::now();
    // Issue #2469: two-phase extraction to minimize orphan_mutex_
    // hold time under cancel storms. Phase 1 (under orphan_mutex_):
    // identify candidates + move them out of orphan_fibers_ into a
    // local vector. Phase 2 (orphan_mutex_ RELEASED): do the
    // per-fiber cleanup (mark_reclaimed + wait_map/joiner_map/
    // owned_fibers mutex acquisitions + quota release + metrics)
    // without holding orphan_mutex_. This lets concurrent
    // note_orphan_fiber() interleave instead of blocking for the
    // entire reaping pass (which could be many milliseconds under
    // N=100 parallel timeouts all timing out simultaneously).
    std::vector<OrphanEntry> to_reap;
    {
        ::aura::compiler::lock_order::AuditedMutexLock lock(
            orphan_mutex_, ::aura::compiler::lock_order::Level::Orphan);
        // Phase 1: identify candidates and extract them
        for (auto it = orphan_fibers_.begin(); it != orphan_fibers_.end();) {
            Fiber* f = it->fiber;
            if (!f) {
                it = orphan_fibers_.erase(it);
                continue;
            }
            if (it->hard_deadline > now || f->is_done() || f->is_reclaimed()) {
                // Not yet due, or already cleaned up by on_fiber_done.
                // Keep the entry (will be removed on next pass if still
                // stale after its deadline).
                ++it;
                continue;
            }
            // Extract under lock; per-fiber cleanup happens after release.
            to_reap.push_back(std::move(*it));
            it = orphan_fibers_.erase(it);
        }
        // Issue #2396: keep empty-check atomic in sync after extract/erase.
        orphan_count_cached_.store(orphan_fibers_.size(), std::memory_order_relaxed);
    } // orphan_mutex_ released here

    // Phase 2: per-fiber cleanup WITHOUT orphan_mutex_ held.
    // note_orphan_fiber() can interleave freely during this loop.
    std::size_t reaped = 0;
    for (auto& entry : to_reap) {
        Fiber* f = entry.fiber;
        // Force-reclaim path. Mark reclaimed_ first so any
        // concurrent joiner sees the flag before they observe
        // removal from the maps.
        f->mark_reclaimed();
        // Drop from wait_map_ (epoll wake-ups for this fiber id
        // are no longer relevant; the body is detached).
        {
            ::aura::compiler::lock_order::AuditedMutexLock wl(
                wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
            const auto evfd = f->eventfd();
            if (evfd >= 0)
                wait_map_.erase(evfd);
        }
        // Unregister from all workers (no future dispatch).
        for (auto& w : workers_) {
            w->unregister_fiber(f);
        }
        // Drop from joiner_map_ and wake any registered joiners.
        // Best-effort: writing 1 to the joiner's eventfd wakes
        // them; they'll observe is_reclaimed() and return
        // JoinStatus::Ok (the joiner path treats reclaimed
        // fibers as done — see Fiber::join for the bit).
        {
            ::aura::compiler::lock_order::AuditedMutexLock jl(
                joiner_map_mutex_, ::aura::compiler::lock_order::Level::Joiner);
            auto jit = joiner_map_.find(f->id());
            if (jit != joiner_map_.end()) {
                for (Fiber* joiner : jit->second) {
                    if (!joiner)
                        continue;
                    const auto joiner_evfd = joiner->eventfd();
                    if (joiner_evfd >= 0) {
                        std::uint64_t one = 1;
                        (void)::write(joiner_evfd, &one, sizeof(one));
                    }
                }
                joiner_map_.erase(jit);
            }
        }
        // Drop from owned_fibers_ (releases the unique_ptr; the
        // fiber's destructor runs at this point if no other ref
        // holds it. The body stack is freed here; non-yielding
        // bodies leak stack until return — documented limitation).
        {
            ::aura::compiler::lock_order::AuditedMutexLock ol(
                owned_fibers_mutex_, ::aura::compiler::lock_order::Level::OwnedFibers);
            for (auto oit = owned_fibers_.begin(); oit != owned_fibers_.end(); ++oit) {
                if (oit->get() == f) {
                    owned_fibers_.erase(oit);
                    break;
                }
            }
        }
        // Release process fiber quota (paired with spawn).
        aura::core::resource_quota::process_resource_quota().release(
            aura::core::resource_quota::Dimension::Fibers, 1);
        if (metrics_on_) {
            metrics_.fibers_completed.fetch_add(1, std::memory_order_relaxed);
        }
        orphans_reaped_total_.fetch_add(1, std::memory_order_relaxed);
        ++reaped;
    }
    return reaped;
}

std::size_t Scheduler::orphan_count() const noexcept {
    // Issue #2396: single relaxed load (no orphan_mutex_ on hot/tick path).
    return orphan_count_cached_.load(std::memory_order_relaxed);
}

std::uint64_t Scheduler::orphans_reaped_total() const noexcept {
    return orphans_reaped_total_.load(std::memory_order_relaxed);
}

std::uint64_t Scheduler::orphans_tick_reap_total() const noexcept {
    return orphans_tick_reap_total_.load(std::memory_order_relaxed);
}

std::uint64_t Scheduler::tick_orphan_mutex_acquired_total() const noexcept {
    return tick_orphan_mutex_acquired_total_.load(std::memory_order_relaxed);
}

// Issue #2396: AURA_ORPHAN_REAP_INTERVAL_MS — min gap between tick-driven
// reaps (default 50ms). Clamped to [1, 5000] so tests can force fast cadence
// without spinning and production cannot set multi-minute gaps by accident.
std::uint64_t Scheduler::orphan_reap_interval_ms() noexcept {
    const char* e = std::getenv("AURA_ORPHAN_REAP_INTERVAL_MS");
    if (e == nullptr || e[0] == '\0')
        return 50;
    std::uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10 + static_cast<std::uint64_t>(*p - '0');
    if (v == 0)
        return 50;
    if (v > 5000)
        return 5000;
    return v;
}

// Issue #2396: production tick entry for residual hard-reclaim.
// Zero cost when orphan_count_cached_ == 0 (no mutex). Interval-gated
// when non-empty. Invoked from Scheduler::run() every IO loop pass and
// available to tests as the production path (prefer over ad-hoc
// reap_orphans_now when asserting tick-driven convergence).
std::size_t Scheduler::maybe_reap_orphans_on_tick() noexcept {
    if (orphan_count_cached_.load(std::memory_order_relaxed) == 0)
        return 0; // AC2: no orphan_mutex_ when empty
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::milliseconds(orphan_reap_interval_ms());
    if (last_orphan_reap_tp_.time_since_epoch().count() != 0 &&
        now - last_orphan_reap_tp_ < interval) {
        return 0;
    }
    last_orphan_reap_tp_ = now;
    // About to take orphan_mutex_ inside reap_orphans_now.
    tick_orphan_mutex_acquired_total_.fetch_add(1, std::memory_order_relaxed);
    orphans_tick_reap_total_.fetch_add(1, std::memory_order_relaxed);
    return reap_orphans_now();
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
    // Issue #2253: store last_hold_us on the fiber so the steal
    // scorer can read it back (the previous (void)duration_us cast
    // discarded the signal entirely). Done BEFORE the early-return
    // branch so the value persists even when starvation mitigation
    // is not applicable (e.g. fiber unknown — unit tests).
    if (fiber_id != 0) {
        if (Fiber* f = fiber_by_id(fiber_id)) {
            f->set_last_hold_us(duration_us);
        }
    }
    // Issue #1445 AC6 / #1633: long-holder event is a first-class
    // starvation-mitigation signal (linked to steal-defer fairness).
    metrics::adaptive_steal_stats().starvation_mitigated_count.fetch_add(1,
                                                                         std::memory_order_relaxed);
    // Issue #1633: full apply_starvation_mitigation when fiber is
    // resolvable — same package as steal-path inner defer (priority
    // boost + deferred_pressure + steal_inner_deferred_starvation_mitigated).
    if (fiber_id != 0) {
        if (Fiber* f = fiber_by_id(fiber_id)) {
            apply_starvation_mitigation(f);
            // Issue #1641: paired starvation_mitigated_for_boundary_count
            // bump (per-CompilerMetrics observability surface; pairs with
            // the legacy adaptive_steal_stats starvation-related counters
            // so dashboards can filter "starvation caused by mutation
            // boundary" specifically).
            // Issue #1641: C weak trampoline (serve TU cannot name Evaluator).
            if (aura_evaluator_bump_starvation_mitigated_for_boundary)
                aura_evaluator_bump_starvation_mitigated_for_boundary();
            return;
        }
    }
    // Fiber unknown (unit tests / pointer-id legacy): bump metrics only.
    metrics::adaptive_steal_stats().deferred_pressure_boosts.fetch_add(1,
                                                                       std::memory_order_relaxed);
    metrics::adaptive_steal_stats().steal_inner_deferred_starvation_mitigated_count.fetch_add(
        1, std::memory_order_relaxed);
    metrics::adaptive_steal_stats().starvation_priority_boosts.fetch_add(1,
                                                                         std::memory_order_relaxed);
}

bool Scheduler::has_waiting_fibers() const {
    ::aura::compiler::lock_order::AuditedMutexLock lock(
        wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
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
    // Issue #1633: wire MutationBoundaryGuard long-hold → on_long_mutation_held
    // so nested/long mutation always triggers starvation mitigation.
    aura_set_long_mutation_scheduler_hook(&long_mutation_hook_trampoline);

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
        // Timeout: check running_ periodically (in case all fibers are busy).
        // Issue #2396: when orphans are pending, shrink timeout to the
        // orphan reap interval so residual hard-reclaim is not delayed by
        // a full 1s idle wait (cancel-storm convergence).
        int wait_ms = 1000;
        if (orphan_count_cached_.load(std::memory_order_relaxed) > 0) {
            const auto iv = static_cast<int>(orphan_reap_interval_ms());
            wait_ms = iv > 0 ? iv : 50;
        }
        int n = ::epoll_wait(epoll_fd_, events, 64, wait_ms);

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
                    ::aura::compiler::lock_order::AuditedMutexLock lock(
                        wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
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
            ::aura::compiler::lock_order::AuditedMutexLock lock(
                wait_map_mutex_, ::aura::compiler::lock_order::Level::WaitMap);
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

        // Issue #2396: production residual hard-reclaim is tick-driven.
        // Zero cost when orphan_count_cached_ == 0; interval-gated when set.
        // Cancel storms converge without test-only reap_orphans_now calls.
        (void)maybe_reap_orphans_on_tick();
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

    aura_set_long_mutation_scheduler_hook(nullptr); // #1633: unwire long-hold hook
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
