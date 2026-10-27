// evaluator_primitives_messaging.cpp — P0 step 16: messaging / fiber / channel primitives
// aura.compiler.evaluator module partition; registered via evaluator_ctor.cpp.

module;

#include "runtime_shared.h"
#include "messaging_bridge.h"
#include "security_capabilities.h"
#include "core/fiber_context.hh" // Issue #4716: fiber-local execution context
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include "observability_metrics.h"
#include "hash_meta.h"
#include "fiber_result_roots.hh" // Issue #4710: fiber:join result roots

#include <unistd.h>

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

namespace {

    struct FiberResult {
        std::shared_ptr<std::optional<EvalValue>> value;
        bool ready = false;
    };
    std::unordered_map<int64_t, FiberResult> s_fiber_results;
    std::mutex s_fiber_results_mtx;
    std::condition_variable s_fiber_results_cv;

    // ---- Issue #4708: single-fiber cooperative cancellation ----
    // (fiber:cancel f) requests cancellation of ONE fiber by id. The
    // request is *cooperative*: the target observes it at its next
    // safepoint -- (fiber:yield), the fiber-parking channel prims, and
    // the fiber:join wait -- and raises the catchable 'fiber-cancelled'
    // condition there, so try/catch cleanup can run and a join on the
    // cancelled fiber returns instead of waiting to the 200s ceiling.
    //
    // A request is delivered AT MOST ONCE per request (kPending ->
    // kDelivered). A second (fiber:cancel f) re-arms it, so a body that
    // catches the condition and keeps running can be interrupted again.
    // A body that never reaches a safepoint cannot be interrupted --
    // the same documented limitation as the #2153 cooperative protocol
    // (there is no async unwind across a mid-turn block).
    enum class FiberCancelState : std::uint8_t { kNone = 0, kPending = 1, kDelivered = 2 };
    std::mutex s_fiber_cancel_mtx;
    std::unordered_map<int64_t, FiberCancelState> s_fiber_cancel;

    // Thread-backend current body id. Scheduler fibers answer through
    // aura::serve::g_current_fiber instead (current_fiber_body_id).
    thread_local std::int64_t s_tls_fiber_body_id = 0;

    // The fiber body running on this thread, 0 outside a body.
    std::int64_t current_fiber_body_id() {
        if (aura::serve::g_current_fiber)
            return static_cast<std::int64_t>(aura::serve::g_current_fiber->id());
        return s_tls_fiber_body_id;
    }

    // Arm cancellation for fid. Always re-arms (kDelivered -> kPending).
    void fiber_cancel_request(int64_t fid) {
        if (fid == 0)
            return;
        std::lock_guard<std::mutex> lock(s_fiber_cancel_mtx);
        s_fiber_cancel[fid] = FiberCancelState::kPending;
    }

    // One-shot: consume a pending request. True iff this call is the
    // first safepoint to observe the request.
    bool fiber_cancel_take(int64_t fid) {
        if (fid == 0)
            return false;
        std::lock_guard<std::mutex> lock(s_fiber_cancel_mtx);
        auto it = s_fiber_cancel.find(fid);
        if (it == s_fiber_cancel.end() || it->second != FiberCancelState::kPending)
            return false;
        it->second = FiberCancelState::kDelivered;
        return true;
    }

    // Drop all bookkeeping for a finished/joined fiber.
    void fiber_cancel_forget(int64_t fid) {
        std::lock_guard<std::mutex> lock(s_fiber_cancel_mtx);
        s_fiber_cancel.erase(fid);
    }

    // The catchable condition a cancelled fiber observes. Single source
    // so every safepoint reports the same payload (Agents match on it).
    // Takes the standard primitive error channels by reference: the
    // Evaluator members are private and this is not a friend function
    // (only the register_* entries are), so callers pass them through.
    template <typename Heap>
    EvalValue fiber_cancel_condition_value(Heap& string_heap, std::vector<EvalValue>& error_values,
                                           std::atomic<std::uint64_t>* error_counter) {
        return make_primitive_error(string_heap, error_values,
                                    "fiber-cancelled: cancel requested via (fiber:cancel)",
                                    error_counter);
    }

    // The condition a cancelled body observes at a safepoint. Returns
    // nullopt when this body has no pending request.
    template <typename Heap>
    std::optional<EvalValue> fiber_cancel_safepoint(Heap& string_heap,
                                                    std::vector<EvalValue>& error_values,
                                                    std::atomic<std::uint64_t>* error_counter) {
        if (!fiber_cancel_take(current_fiber_body_id()))
            return std::nullopt;
        return fiber_cancel_condition_value(string_heap, error_values, error_counter);
    }

    // Issue #4710: publish the registry's values as GC roots. A body's
    // return value is stored HERE and nowhere else until fiber:join
    // fetches and erases the entry, so without this provider the
    // evaluator's reclaim marks a finished fiber's (hash) table
    // unreachable, frees it, and the next mint reuses the slot: the
    // joiner reads an empty table (-1) or a sibling's table (two equal
    // values) instead of its own result.
    //
    // Both states are collected: a ready entry (the body published,
    // the join has not run) and a pending entry (the shared optional
    // already holds a value the body wrote before flipping ready).
    // Values here are this process's, and the mark is conservative —
    // an extra root only keeps a slot alive, it never frees one.
    std::size_t collect_fiber_result_roots(std::vector<std::int64_t>& out) {
        const auto before = out.size();
        std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
        for (const auto& entry : s_fiber_results) {
            const auto& slot = entry.second.value;
            if (!slot || !slot->has_value())
                continue;
            out.push_back(slot->value().val);
        }
        return out.size() - before;
    }

    // Issue #3394: joinable thread-fiber registry. Pre-#3394 the fallback
    // detached its worker, so an unjoined fiber could still be inside
    // ev.apply_closure while ~Evaluator tore down the workspace/arena →
    // use-after-free (PersistentChildVector OOB abort in
    // test_fiber_spawn_cli AC1, ~1/3 hit rate per abandoned spawn).
    // Workers are keyed by the globally-unique #2656 fid and tagged with
    // the owning Evaluator so ~Evaluator drains only its own fibers;
    // fiber:join joins + erases on successful result fetch.
    std::mutex s_thread_fiber_threads_mtx;
    std::unordered_map<std::int64_t, std::pair<const void*, std::thread>> s_thread_fiber_threads;
    std::atomic<std::uint64_t> g_thread_fiber_dtor_join_total{0};
    std::atomic<std::uint64_t> g_thread_fiber_dtor_abandon_total{0};

    // Issue #2738: CLI thread-fallback fibers share one Evaluator (string_heap_,
    // env, FlatAST readers outside MutationBoundary). Concurrent apply_closure
    // races those structures even when rebind/eval-current take workspace_mtx_.
    // Serialize the entire fiber body on the thread backend only; serve-async
    // scheduler fibers keep their own isolation. Product semantics of dual-name
    // rebind still hold (both bindings apply; no SIGABRT).
    //
    // Issue #2869: the body mutex must NOT be held while fiber:join waits for
    // a child on the same thread backend — the child needs that mutex to run.
    // Parent holding lock + join wait = classic deadlock (nested join hang).
    // TLS points at the owning unique_lock so join can unlock/relock around
    // the wait. Issue #4048 also sets this under Soft Ready --serve-async
    // denseness (shared Evaluator). Issue #4053: http-post unlocks via the
    // same TLS through DensenessBodyLockWaitGuard (messaging_bridge hooks).
    std::mutex s_cli_thread_fiber_body_mtx;
    // Issue #4716: the owning unique_lock is EXECUTION CONTEXT — the body
    // that holds the mutex parks on a fiber:join / channel wait and resumes
    // on another worker, and it must still find its own lock to unlock /
    // relock around the wait. A bare thread_local either lost the pointer
    // (UAF'd the #4054 way) or let a sibling fiber on the same worker unlock
    // a lock it does not own. It lives in the running fiber's context.
    std::unique_lock<std::mutex>*& s_tls_cli_body_lock() noexcept {
        return reinterpret_cast<std::unique_lock<std::mutex>*&>(
            aura::core::fiber_ctx::active().cli_body_lock);
    }

    // ── Issue #4699: cooperative-yield body-mutex handoff ─────────────
    //
    // The body mutex serializes the fiber bodies that share one Evaluator
    // (CLI thread backend + Soft Ready --serve-async denseness). It is
    // released across a WAIT (fiber:join, channel park, http-post) so a
    // sibling body can run while the waiter sleeps. A cooperative yield is
    // a wait too: without releasing the mutex across it, the sibling the
    // scheduler hands the worker to would block on the mutex forever (the
    // sibling runs on the SAME worker thread -> self-deadlock).
    //
    // The CLI thread backend has no scheduler that hands the worker over,
    // so the yielding body must WAIT until a sibling body has taken the
    // mutex over. Otherwise it re-acquires the mutex itself (barging) and
    // the bodies stay run-to-completion — the #4699 symptom. The takeover
    // is published as an epoch bump under s_cli_handoff_mtx; the yield
    // waits on the condition variable for the epoch to advance.
    //
    // The wait is skipped when no other body is alive (a single-fiber
    // program keeps the old no-op cost) and it is bounded, so a lost
    // wakeup can never hang a session.
    std::mutex s_cli_handoff_mtx;
    std::condition_variable s_cli_handoff_cv;
    std::uint64_t s_cli_handoff_epoch{0};
    // Pending fiber bodies (CLI thread backend + scheduler backend): a body
    // that was spawned and has not finished yet. The count is taken at SPAWN
    // time, not at body start: a sibling OS thread of the CLI thread backend
    // may not have started yet, and a yield that does not see it would hand
    // the turn over to nobody and run the body to completion.
    std::atomic<std::size_t> s_cli_body_pending{0};

    // Monotonic counter of SPAWNED bodies. The parent spawns its children
    // ONE AT A TIME, so a body that yields early can see "no sibling yet"
    // even though the parent is about to spawn the next one. Without this
    // counter the early body concludes that it is alone, skips the
    // hand-off and runs to completion - the run-to-completion symptom.
    // The counter tells the yielding body whether the sibling set changed
    // while it waited: if it did, a sibling exists and the body must keep
    // waiting for the hand-off instead of latching "solo".
    std::atomic<std::uint64_t> s_cli_spawn_epoch{0};

    // True while this thread runs a spawned fiber body (the CLI main / the
    // root program is not a body). Used by the yield hand-off to tell
    // "another body exists" (a body counts itself) from "a body exists".
    thread_local bool s_tls_in_fiber_body = false;

    // Sticky per-body flag: "this body waited for a hand-off and nothing
    // happened - no sibling took over and the parent spawned nobody else".
    // It keeps the yield a cheap no-op for a single-fiber program (the old
    // cost) instead of paying the bounded wait on every turn. It is
    // cleared as soon as a sibling is seen again.
    thread_local bool s_tls_cli_body_solo = false;

    // RAII: mark the end of a spawned fiber body (the pending count was
    // already taken by the spawn path).
    struct CliBodyDoneScope {
        CliBodyDoneScope() {
            s_tls_in_fiber_body = true;
            s_tls_cli_body_solo = false;
        }
        ~CliBodyDoneScope() {
            s_tls_in_fiber_body = false;
            s_cli_body_pending.fetch_sub(1, std::memory_order_acq_rel);
        }
        CliBodyDoneScope(const CliBodyDoneScope&) = delete;
        CliBodyDoneScope& operator=(const CliBodyDoneScope&) = delete;
    };

    // Publish that a body has taken the body mutex over (the epoch bump
    // wakes a yielding sibling that is waiting for its turn).
    void cli_body_lock_publish_takeover() {
        {
            std::lock_guard<std::mutex> handoff_lock(s_cli_handoff_mtx);
            ++s_cli_handoff_epoch;
        }
        s_cli_handoff_cv.notify_all();
    }

    // Acquire the body mutex and publish the takeover so a yielding sibling
    // wakes up (the CLI thread backend has no scheduler to do the hand-off,
    // so the takeover publish IS the hand-off).
    void cli_body_lock_acquire(std::unique_lock<std::mutex>& lock) {
        lock.lock();
        cli_body_lock_publish_takeover();
    }

    // Issue #4699: release the body mutex across a cooperative yield.
    //
    // wait_for_takeover is true in the CLI thread backend (no scheduler to
    // hand the worker over): the yielding body waits until a sibling body
    // has taken the mutex over, so the bodies interleave instead of
    // running to completion. In the scheduler backend the scheduler FIFO
    // (serve/worker.cpp) hands the worker to a sibling, so the yield only
    // releases the mutex and re-acquires it after the fiber resumes.
    struct CliBodyYieldHandoff {
        // Issue #4711: see CliBodyLockJoinGuard — M1 dropped across the
        // yield and re-taken after M0 (declared first = destroyed last).
        aura::messaging::EvalMutexWaitLockGuard eval_wait_lock;
        std::unique_lock<std::mutex>* lock = nullptr;
        bool released = false;
        explicit CliBodyYieldHandoff(bool wait_for_takeover) {
            if (!s_tls_cli_body_lock() || !s_tls_cli_body_lock()->owns_lock())
                return;
            lock = s_tls_cli_body_lock();
            // Another body can take the turn over. A body counts itself in
            // the pending registry, the CLI main / the root program does
            // not.
            const std::size_t pending = s_cli_body_pending.load(std::memory_order_acquire);
            const std::size_t others =
                s_tls_in_fiber_body ? (pending > 0 ? pending - 1 : 0) : pending;
            // A body CANNOT know whether the parent will spawn MORE
            // siblings: the parent spawns them one at a time, so "no
            // sibling yet" is not proof that there is none (the early body
            // would skip the hand-off and run to completion). A body
            // therefore waits once even when it looks alone, and latches
            // "solo" only if neither a hand-over nor a new spawn
            // happened - that keeps the yield a cheap no-op for a
            // single-fiber program.
            const bool sibling_alive =
                s_tls_in_fiber_body ? (others > 0 || !s_tls_cli_body_solo) : (others > 0);
            // Sample the takeover epoch BEFORE the unlock: the sibling that
            // takes the mutex over publishes its takeover after the unlock,
            // so the epoch must be read first or the takeover would be
            // missed and the yield would wait for the next one.
            std::uint64_t seen_epoch = 0;
            {
                std::lock_guard<std::mutex> handoff_lock(s_cli_handoff_mtx);
                seen_epoch = s_cli_handoff_epoch;
            }
            // Sample the spawn epoch too: a spawn that lands while this
            // body waits means a sibling exists (or is about to run).
            const std::uint64_t seen_spawn = s_cli_spawn_epoch.load(std::memory_order_acquire);
            lock->unlock();
            // Clear the TLS slot while unlocked: a sibling body on this
            // OS thread must not observe our unlocked lock (#4054).
            s_tls_cli_body_lock() = nullptr;
            released = true;
            if (!wait_for_takeover || !sibling_alive)
                return;
            // Wait until a sibling body has taken the mutex over. Bounded:
            // a lost wakeup or a sibling that finished must never hang a
            // session - the yield then takes the mutex back.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            std::unique_lock<std::mutex> handoff_lock(s_cli_handoff_mtx);
            while (s_cli_handoff_epoch == seen_epoch) {
                if (s_cli_body_pending.load(std::memory_order_acquire) == 0)
                    break; // no sibling body left to hand over to
                if (std::chrono::steady_clock::now() >= deadline)
                    break; // bounded: never hang a session
                s_cli_handoff_cv.wait_for(handoff_lock, std::chrono::milliseconds(5));
            }
            if (s_tls_in_fiber_body) {
                // Latch "solo" only when the body is provably alone: no
                // hand-over happened, the parent spawned nobody else while
                // we waited, and no other body is pending. A body that saw
                // a sibling (or a new spawn) keeps waiting for the
                // hand-off on its next turn.
                const bool handed_over = (s_cli_handoff_epoch != seen_epoch);
                const bool alone =
                    !handed_over &&
                    (s_cli_spawn_epoch.load(std::memory_order_acquire) == seen_spawn) &&
                    (s_cli_body_pending.load(std::memory_order_acquire) <= 1);
                s_tls_cli_body_solo = alone;
            }
        }
        ~CliBodyYieldHandoff() {
            if (!released || !lock)
                return;
            cli_body_lock_acquire(*lock);
            s_tls_cli_body_lock() = lock;
        }
        CliBodyYieldHandoff(const CliBodyYieldHandoff&) = delete;
        CliBodyYieldHandoff& operator=(const CliBodyYieldHandoff&) = delete;
    };

    // RAII: temporarily release denseness body mutex around wait/yield.
    //
    // Issue #4054: Soft Ready workers=1 runs denseness fibers as cooperative
    // fibers on ONE OS thread, so they share this thread_local. The old
    // TlsBodyLockScope prev-chain + unlock-without-clearing-TLS was wrong:
    // fiber A completing while sibling B was mid http-post/join wait restored
    // TLS to null (wiping B) or left a dangling pointer to A's destroyed
    // unique_lock. The next batch's fiber:join then UAF'd s_tls_cli_body_lock
    // (owns_lock on garbage) and Soft stopped answering stdin — classic
    // "first denseness MiniMax batch OK, second wedges session" (#4054).
    //
    // Protocol (mutex still serializes Evaluator touch; unlock only across wait):
    //   hold  → TLS points at our unique_lock
    //   wait  → unlock mutex AND clear TLS (sibling may install its own)
    //   resume→ re-lock mutex AND republish TLS
    //   end   → clear TLS only if it still points at us (never restore a
    //           yielded sibling's prev — that is the #4054 dangling chain)
    struct CliBodyLockJoinGuard {
        // Issue #4711: declared FIRST so it is destroyed LAST — the eval
        // mutex (M1) comes back AFTER the body mutex (M0) re-acquire in the
        // destructor body, keeping the single order M0 -> M1. Without this
        // the resume was M1 -> M0 against eval()'s M0 -> M1 (TSan
        // lock-order-inversion) and deadlocked against a fiber body that
        // holds M0 and enters eval (eval-current / JIT recompile).
        aura::messaging::EvalMutexWaitLockGuard eval_wait_lock;
        std::unique_lock<std::mutex>* lock = nullptr;
        bool unlocked = false;
        CliBodyLockJoinGuard() {
            if (s_tls_cli_body_lock() && s_tls_cli_body_lock()->owns_lock()) {
                lock = s_tls_cli_body_lock();
                lock->unlock();
                // Clear TLS while unlocked so a sibling denseness fiber on
                // this OS thread does not observe our unlocked lock.
                s_tls_cli_body_lock() = nullptr;
                unlocked = true;
            }
        }
        ~CliBodyLockJoinGuard() {
            if (unlocked && lock && !lock->owns_lock()) {
                // Issue #4699: tracked acquire - the takeover publish
                // wakes a yielding sibling that waits for its turn.
                cli_body_lock_acquire(*lock);
                s_tls_cli_body_lock() = lock;
            }
        }
        CliBodyLockJoinGuard(const CliBodyLockJoinGuard&) = delete;
        CliBodyLockJoinGuard& operator=(const CliBodyLockJoinGuard&) = delete;
    };

    // ── Issue #4700: fiber-cooperative channel waits ───────────────
    //
    // The channel send/recv primitives used to block the CALLING OS THREAD
    // on the channel condition_variable. Under --serve-async the caller is a
    // FIBER running on a worker thread, so the condvar wait froze the
    // whole worker: a peer fiber on the same worker (the sender) could
    // never run → deadlock (#4700).
    //
    // Cooperative protocol: a fiber that has to wait parks ITSELF
    // (FiberState::Waiting + Fiber::yield(BlockingIO)) and is woken by a
    // 1-byte write to its eventfd — the same wake protocol as fiber:join's
    // joiner wake (Scheduler::on_fiber_done) and MultiFiberMailbox's
    // notify_all_unlocked. The channel mutex is NEVER held across the
    // park (a parked fiber may be resumed on another worker thread).
    //
    // The waiter registry lives here (not in Evaluator::Channel) so the
    // evaluator module interface stays untouched: the channel address is
    // the registry key and the waiters are opaque Fiber pointers.
    // Issue #4700: a non-owning view of Evaluator::Channel. The Channel
    // type is private to the Evaluator, so the cooperative helpers take the
    // fields they need instead of the type — evaluator.ixx stays untouched.
    struct ChannelView {
        const void* key = nullptr; // stable channel identity (the Channel address)
        std::mutex& mtx;
        std::condition_variable& cv;
        std::deque<std::string>& queue;
        std::size_t& buffer_size;
        std::size_t& waiting_receivers;
        bool& closed;
    };
    struct ChannelWaiter {
        aura::serve::Fiber* fiber = nullptr;
        bool sender = false; // true = parked sender, false = parked receiver
    };
    std::mutex s_channel_waiters_mtx;
    std::unordered_map<std::uint64_t, std::vector<ChannelWaiter>> s_channel_waiters;

    inline std::uint64_t channel_waiter_key(const void* ch) noexcept {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ch));
    }

    // Register the fiber as a waiter of the channel. The caller MUST hold
    // ch.mtx so a peer cannot slip between the predicate check and the
    // registration (the peer pushes under the same lock).
    void channel_register_waiter(const void* ch, aura::serve::Fiber* fiber, bool sender) {
        std::lock_guard lk(s_channel_waiters_mtx);
        s_channel_waiters[channel_waiter_key(ch)].push_back(ChannelWaiter{fiber, sender});
    }

    // Drop the registration made by channel_register_waiter. Idempotent.
    void channel_unregister_waiter(const void* ch, aura::serve::Fiber* fiber, bool sender) {
        std::lock_guard lk(s_channel_waiters_mtx);
        auto it = s_channel_waiters.find(channel_waiter_key(ch));
        if (it == s_channel_waiters.end())
            return;
        auto& vec = it->second;
        for (auto i = vec.begin(); i != vec.end(); ++i) {
            if (i->fiber == fiber && i->sender == sender) {
                vec.erase(i);
                break;
            }
        }
        if (vec.empty())
            s_channel_waiters.erase(it);
    }

    // Wake every parked waiter of the channel with the given role. Mirrors
    // MultiFiberMailbox::notify_all_unlocked: write the fiber's eventfd and
    // flip Waiting -> Ready so the scheduler may pick it up.
    void channel_wake_waiters(const void* ch, bool sender) {
        std::vector<aura::serve::Fiber*> to_wake;
        {
            std::lock_guard lk(s_channel_waiters_mtx);
            auto it = s_channel_waiters.find(channel_waiter_key(ch));
            if (it == s_channel_waiters.end())
                return;
            auto& vec = it->second;
            for (auto i = vec.begin(); i != vec.end();) {
                if (i->sender == sender) {
                    to_wake.push_back(i->fiber);
                    i = vec.erase(i);
                } else {
                    ++i;
                }
            }
            if (vec.empty())
                s_channel_waiters.erase(it);
        }
        for (auto* f : to_wake) {
            if (!f || f->is_done())
                continue;
            const int evfd = f->eventfd();
            if (evfd >= 0) {
                std::uint64_t one = 1;
                (void)::write(evfd, &one, sizeof(one));
            }
            if (f->state() == aura::serve::FiberState::Waiting)
                f->set_state(aura::serve::FiberState::Ready);
        }
    }

    // Issue #2554 / #3692: Fiber::yield is rejected while a mutation
    // boundary is live — the yield never swaps out, so parking would
    // hot-spin. Fall back to the host-thread wait in that case.
    bool channel_can_park_fiber() {
        if (aura_evaluator_mutation_boundary_depth() > 0)
            return false;
        if (aura::messaging::g_mutation_boundary_held &&
            aura::messaging::g_mutation_boundary_held())
            return false;
        return true;
    }

    // Park the calling fiber until it is woken. Returns true when the fiber
    // suspended, false when a wake was already pending (the caller must
    // re-check its predicate).
    bool channel_park_fiber(aura::serve::Fiber* fiber) {
        if (!fiber)
            return false;
        const int evfd = fiber->eventfd();
        if (evfd < 0)
            return false;
        // Issue #4700: a peer fiber on this worker may have written the wake
        // eventfd between our predicate check and the park. Consume it here
        // instead of parking on an already-signalled eventfd (which would
        // sleep forever).
        std::uint64_t primed = 0;
        if (::read(evfd, &primed, sizeof(primed)) == static_cast<ssize_t>(sizeof(primed)))
            return false; // wake already pending → caller re-checks
        // Issue #2554: park as Waiting BEFORE the yield;
        // Fiber::yield(BlockingIO) keeps the fiber off the worker queue
        // (unstealable) until the IO thread re-queues it after the wake.
        fiber->set_state(aura::serve::FiberState::Waiting);
        {
            // Issue #4700 / #2869: release the denseness body mutex across
            // the park. The parked fiber still owns that mutex, so a peer
            // fiber on the same worker would block on it forever (the same
            // protocol as the fiber:join wait).
            CliBodyLockJoinGuard body_lock_guard;
            aura::serve::Fiber::yield(aura::serve::YieldReason::BlockingIO);
        }
        // Drain the eventfd so a stale wake cannot confuse the next park.
        std::uint64_t val = 0;
        while (::read(evfd, &val, sizeof(val)) > 0) {
        }
        fiber->set_state(aura::serve::FiberState::Running);
        return true;
    }

    // Issue #4700: cooperative (fiber) channel receive. Parks the calling
    // FIBER (never the worker thread) until a message is queued or the
    // channel is closed. Returns the message, or nullopt when the channel
    // was closed (the caller then returns the empty string).
    std::optional<std::string> channel_recv_cooperative(ChannelView& ch, aura::serve::Fiber* fiber,
                                                        bool* cancelled_out = nullptr) {
        const void* key = ch.key;
        for (;;) {
            // Issue #4708: cancellation safepoint -- this is one of the
            // "blocking prims that already park", so a cancelled fiber must
            // leave the park loop instead of waiting for a message.
            if (fiber && fiber_cancel_take(static_cast<int64_t>(fiber->id()))) {
                if (cancelled_out)
                    *cancelled_out = true;
                return std::nullopt;
            }
            {
                std::unique_lock ul(ch.mtx);
                if (!ch.queue.empty()) {
                    auto msg = std::move(ch.queue.front());
                    ch.queue.pop_front();
                    ul.unlock();
                    // A parked sender may have buffer room again (buffered
                    // channel) — wake it so it can push.
                    channel_wake_waiters(key, /*sender=*/true);
                    ch.cv.notify_all();
                    return msg;
                }
                if (ch.closed) {
                    ul.unlock();
                    channel_wake_waiters(key, /*sender=*/true);
                    ch.cv.notify_all();
                    return std::nullopt;
                }
                // Issue #2483: announce the waiting receiver so a rendezvous
                // send can proceed.
                ++ch.waiting_receivers;
                channel_register_waiter(key, fiber, /*sender=*/false);
                ul.unlock();
            }
            // We are a posted receiver: a parked sender (rendezvous) can
            // hand its message over now — wake it so it can push.
            channel_wake_waiters(key, /*sender=*/true);
            if (!channel_can_park_fiber()) {
                // The fiber cannot suspend (Fiber::yield is rejected while a
                // mutation boundary is live) — fall back to the host-thread
                // wait so the channel still makes progress.
                std::unique_lock ul(ch.mtx);
                ch.cv.wait(ul, [&]() { return ch.closed || !ch.queue.empty(); });
                --ch.waiting_receivers;
                channel_unregister_waiter(key, fiber, /*sender=*/false);
                if (ch.queue.empty()) {
                    ul.unlock();
                    ch.cv.notify_all();
                    return std::nullopt;
                }
                auto msg = std::move(ch.queue.front());
                ch.queue.pop_front();
                ul.unlock();
                ch.cv.notify_all();
                channel_wake_waiters(key, /*sender=*/true);
                return msg;
            }
            // Park the fiber — the worker thread stays free for peers.
            channel_park_fiber(fiber);
            {
                std::lock_guard lk(ch.mtx);
                --ch.waiting_receivers;
            }
            channel_unregister_waiter(key, fiber, /*sender=*/false);
            // Re-check: the wake may have been spurious, or another
            // receiver may have taken the message.
        }
    }

    // Issue #4700: cooperative (fiber) channel send. Parks the calling FIBER
    // (never the worker thread) until the buffer has room (buffered
    // channel) or a receiver is posted (rendezvous).
    bool channel_send_cooperative(ChannelView& ch, std::string msg, aura::serve::Fiber* fiber,
                                  bool* cancelled_out = nullptr) {
        const void* key = ch.key;
        for (;;) {
            // Issue #4708: cancellation safepoint (see
            // channel_recv_cooperative).
            if (fiber && fiber_cancel_take(static_cast<int64_t>(fiber->id()))) {
                if (cancelled_out)
                    *cancelled_out = true;
                return false;
            }
            {
                std::unique_lock ul(ch.mtx);
                if (ch.closed) {
                    ul.unlock();
                    return false;
                }
                // Issue #2483: buffer_size==0 must NOT short-circuit the wait
                // predicate (rendezvous waits for a posted receiver).
                const bool ready = ch.buffer_size > 0
                                       ? ch.queue.size() < ch.buffer_size
                                       : (ch.waiting_receivers > 0 && ch.queue.empty());
                if (ready) {
                    ch.queue.push_back(std::move(msg));
                    ul.unlock();
                    ch.cv.notify_all();
                    // A parked receiver can take the message now.
                    channel_wake_waiters(key, /*sender=*/false);
                    return true;
                }
                // No room yet — park as a sender.
                channel_register_waiter(key, fiber, /*sender=*/true);
                ul.unlock();
            }
            if (!channel_can_park_fiber()) {
                // Cannot suspend (live mutation boundary) — fall back to the
                // host-thread wait.
                std::unique_lock ul(ch.mtx);
                ch.cv.wait(ul, [&]() {
                    if (ch.closed)
                        return true;
                    if (ch.buffer_size > 0)
                        return ch.queue.size() < ch.buffer_size;
                    return ch.waiting_receivers > 0 && ch.queue.empty();
                });
                channel_unregister_waiter(key, fiber, /*sender=*/true);
                if (ch.closed) {
                    ul.unlock();
                    return false;
                }
                ch.queue.push_back(std::move(msg));
                ul.unlock();
                ch.cv.notify_all();
                channel_wake_waiters(key, /*sender=*/false);
                return true;
            }
            // Park the fiber — the worker thread stays free for peers.
            channel_park_fiber(fiber);
            channel_unregister_waiter(key, fiber, /*sender=*/true);
            // Re-check the predicate (the wake may have been spurious).
        }
    }


    void denseness_body_unlock_hook(void** lock_out, bool* unlocked_out) {
        *lock_out = nullptr;
        *unlocked_out = false;
        if (s_tls_cli_body_lock() && s_tls_cli_body_lock()->owns_lock()) {
            *lock_out = s_tls_cli_body_lock();
            s_tls_cli_body_lock()->unlock();
            s_tls_cli_body_lock() = nullptr; // #4054: sibling-safe across yield
            *unlocked_out = true;
        }
    }

    void denseness_body_relock_hook(void* lock, bool unlocked) {
        if (unlocked && lock) {
            auto* l = static_cast<std::unique_lock<std::mutex>*>(lock);
            if (l && !l->owns_lock()) {
                // Issue #4699: tracked acquire (takeover publish).
                cli_body_lock_acquire(*l);
                s_tls_cli_body_lock() = l; // republish after wait
            }
        }
    }

    // Issue #4356: CLI main does not run inside complete_fiber, so it never
    // took s_cli_thread_fiber_body_mtx. read-line then pushed string_heap_
    // while a thread fiber mutated the same Evaluator. Acquire publishes
    // this unique_lock in the TLS slot the join / denseness wait hooks
    // already unlock. A thread that already holds the lock (the fiber
    // body, or an outer guard) gets nullptr and must not lock again.
    void* cli_main_body_lock_acquire_impl() {
        if (s_tls_cli_body_lock() && s_tls_cli_body_lock()->owns_lock())
            return nullptr;
        auto* lk = new std::unique_lock<std::mutex>(s_cli_thread_fiber_body_mtx, std::defer_lock);
        // Issue #4699: tracked acquire (takeover publish).
        cli_body_lock_acquire(*lk);
        s_tls_cli_body_lock() = lk;
        return lk;
    }

    void cli_main_body_lock_release_impl(void* p) {
        if (!p)
            return;
        auto* lk = static_cast<std::unique_lock<std::mutex>*>(p);
        // #4054: clear TLS only when it still names this lock.
        if (s_tls_cli_body_lock() == lk)
            s_tls_cli_body_lock() = nullptr;
        delete lk;
    }

    void wire_denseness_body_lock_hooks() {
        aura::messaging::g_denseness_body_unlock_for_wait = &denseness_body_unlock_hook;
        aura::messaging::g_denseness_body_relock_after_wait = &denseness_body_relock_hook;
        aura::messaging::g_cli_main_body_lock_acquire = &cli_main_body_lock_acquire_impl;
        aura::messaging::g_cli_main_body_lock_release = &cli_main_body_lock_release_impl;
    }

} // namespace

void register_messaging_primitives(PrimRegistrar add, Evaluator& ev) {
    // Issue #4710: install the fiber:join result-root provider so the
    // evaluator's reclaims (tree-walker + file-mode) mark the values of
    // finished-but-unjoined fibers. Registration is idempotent (first
    // install wins) and process-global, like the registry itself: a
    // serve session's later Evaluators reuse it.
    fiber_result_roots::install(&collect_fiber_result_roots);

    // Issue #4053: ensure denseness body unlock hooks are live for http-post
    // (module static ctor alone is not reliable across Soft --serve-async).
    wire_denseness_body_lock_hooks();

    // ═══════════════════════════════════════════════════════════════

    // P14: Inter-Agent Messaging (P0)

    // ═══════════════════════════════════════════════════════════════

    // ── Messaging primitives ───────────────────────────────────

    //

    // send: global bridge (pushing to a target is session-independent)

    // recv: uses compiler_service_ + g_mailbox_read (per-service mailbox access)

    // my-id: uses compiler_service_ + g_session_id (per-service identity)

    // (send target-id message) → #t on success
    add("send", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]))
            return make_bool(false);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send)
            return make_bool(false);
        auto target = ev.string_heap_[as_string_idx(a[0])];
        // If message is already a string, send directly (backward compat, efficient).
        // Otherwise, JSON-encode it.
        std::string msg;
        if (is_string(a[1])) {
            msg = ev.string_heap_[as_string_idx(a[1])];
        } else {
            // Use json-encode primitive to serialize
            auto json_fn = ev.primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[1]});
                if (is_string(result))
                    msg = ev.string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty())
            return make_bool(false);
        return make_bool(bridge.send(target, msg));
    });

    // (broadcast message) — Send a message to ALL registered sessions (P2)
    // Uses g_session_list to enumerate sessions and bridge.send for each.
    // Returns number of messages sent (0 if no sessions or no service).
    add("broadcast", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_int(0);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send)
            return make_int(0);
        std::string msg;
        if (is_string(a[0])) {
            msg = ev.string_heap_[as_string_idx(a[0])];
        } else {
            auto json_fn = ev.primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[0]});
                if (is_string(result))
                    msg = ev.string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty())
            return make_int(0);

        // Enumerate sessions and send to each
        int sent = 0;
        if (aura::messaging::g_session_list && *aura::messaging::g_session_list) {
            auto names = (*aura::messaging::g_session_list)();
            for (auto& name : names) {
                if (bridge.send(name, msg))
                    ++sent;
            }
        }
        return make_int(sent);
    });

    // (recv [timeout-ms]) → message value
    //   Returns: message (string or parsed JSON value) on success,
    //            #<void> on timeout (distinguishable from #f = no service),
    //            #f if no messaging service available.
    //   Uses evaluator's stored ev.compiler_service_ (not global) for safety.
    //   Global g_current_compiler_service may dangle after service destruction.
    add("recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto svc = ev.compiler_service_;
        if (!svc || !aura::messaging::g_mailbox_read)
            return make_bool(false);
        int timeout_ms = -1;
        if (a.size() >= 1 && is_int(a[0]))
            timeout_ms = static_cast<int>(as_int(a[0]));
        auto result = aura::messaging::g_mailbox_read(svc, timeout_ms);
        if (!result) {
            return make_bool(false);
        }
        auto& raw = *result;
        if (!raw.empty() && (raw[0] == '{' || raw[0] == '[' || raw[0] == '"')) {
            auto parse_fn = ev.primitives_.lookup("json-parse");
            if (parse_fn) {
                auto sid = ev.string_heap_.mint(raw);
                auto parsed = (*parse_fn)({make_string(sid)});
                if (!is_void(parsed))
                    return parsed;
            }
        }
        auto idx = ev.string_heap_.mint(std::move(*result));
        return make_string(idx);
    });

    // (my-id) → current session ID string
    add("my-id", [&ev](const auto&) -> EvalValue {
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_session_id)
            return make_string(0);
        auto id = aura::messaging::g_session_id(svc);
        if (id.empty())
            id = "(unknown)";
        auto idx = ev.string_heap_.mint(id);
        return make_string(idx);
    });

    // (reply msg) → #t on success (sends to last message's sender)
    // Supports non-string values via JSON encoding.
    add("reply", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_bool(false);
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_mailbox_last_sender)
            return make_bool(false);
        auto target = aura::messaging::g_mailbox_last_sender(svc);
        if (target.empty())
            return make_bool(false);
        auto& bridge = aura::messaging::g_messaging_bridge;
        if (!bridge.send)
            return make_bool(false);
        std::string msg;
        if (is_string(a[0])) {
            msg = ev.string_heap_[as_string_idx(a[0])];
        } else {
            auto json_fn = ev.primitives_.lookup("json-encode");
            if (json_fn) {
                auto result = (*json_fn)({a[0]});
                if (is_string(result))
                    msg = ev.string_heap_[as_string_idx(result)];
            }
        }
        if (msg.empty())
            return make_bool(false);
        return make_bool(bridge.send(target, msg));
    });

    // (session-active? id) → #t if session exists
    add("session-active?", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]) || !aura::messaging::g_session_exists)
            return make_bool(false);
        auto id = ev.string_heap_[as_string_idx(a[0])];
        return make_bool(aura::messaging::g_session_exists(id));
    });

    // (mailbox-count) → number of pending messages in our mailbox
    ObservabilityPrims::register_stats_impl("mailbox-count", [&ev](const auto&) -> EvalValue {
        auto svc = aura::messaging::g_current_compiler_service;
        if (!svc || !aura::messaging::g_mailbox_count)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(aura::messaging::g_mailbox_count(svc)));
    });

    // Issue #1585: process-global MultiFiberMailbox stats surface.
    // Full attach/broadcast/recv API is C++ (multi_fiber_mailbox.h);
    // agent frameworks bind via this dashboard + serve hooks.
    ObservabilityPrims::register_stats_impl(
        "query:mf-mailbox-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::serve::mf_mailbox;
            // Issue #1881: full health fields (priority / waits / linear).
            // Issue #2010: fanout-backpressure-rejects.
            // Issue #2188: recv-rejected-in-mutation-boundary.
            std::uint64_t pushes = 0, pops = 0, broadcasts = 0, bp = 0, attaches = 0, ph = 0,
                          waits = 0, tmo = 0, lchk = 0, lviol = 0, fbp = 0, rej_bound = 0,
                          def_mh = 0;
            MultiFiberMailbox::snapshot_global_full(pushes, pops, broadcasts, bp, attaches, ph,
                                                    waits, tmo, lchk, lviol, &fbp, &rej_bound,
                                                    &def_mh);
            // Capacity 64→128→256: #2188/#2312/#2347/#2378/#2987 + #2972 credit.
            auto* ht = FlatHashTable::create(256);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.mint(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("pushes", static_cast<std::int64_t>(pushes));
            insert_kv("pops", static_cast<std::int64_t>(pops));
            insert_kv("broadcasts", static_cast<std::int64_t>(broadcasts));
            insert_kv("backpressure-rejects", static_cast<std::int64_t>(bp));
            insert_kv("fanout-backpressure-rejects", static_cast<std::int64_t>(fbp));
            insert_kv("attaches", static_cast<std::int64_t>(attaches));
            insert_kv("priority-high", static_cast<std::int64_t>(ph));
            insert_kv("recv-waits", static_cast<std::int64_t>(waits));
            insert_kv("recv-timeouts", static_cast<std::int64_t>(tmo));
            insert_kv("linear-checks", static_cast<std::int64_t>(lchk));
            insert_kv("linear-violations", static_cast<std::int64_t>(lviol));
            insert_kv("phase", static_cast<std::int64_t>(kMultiFiberMailboxPhase));
            insert_kv("schema", 1585);
            insert_kv("schema-1881", 1881);
            insert_kv("schema-2010", 2010);
            // Issue #2188: forbid blocking recv under live MutationBoundary.
            insert_kv("recv-rejected-in-mutation-boundary", static_cast<std::int64_t>(rej_bound));
            insert_kv("recv-rejected-in-mutation-boundary-total",
                      static_cast<std::int64_t>(rej_bound));
            insert_kv("schema-2188", 2188);
            insert_kv("issue-2188", 2188);
            insert_kv("recv-boundary-gate-wired", 1);
            insert_kv("health-wired", 1);
            // Issue #2312: push-side delivery gate — push / broadcast_fanout
            // defer (BP) when target fiber(s) hold MutationBoundary. Distinct
            // from recv-side recv-rejected-in-mutation-boundary (#2188); this
            // is the producer-side gate that prevents lock-order inversion vs
            // workspace_mtx_ + GcDeferReason::MutationHold under multi-agent
            // fanout. Reuses #2184 MutationSafetySnapshot + is_at_mutation_
            // boundary_safe truth table.
            insert_kv("mailbox-deferred-mutation-hold-total", static_cast<std::int64_t>(def_mh));
            insert_kv("mailbox_deferred_mutation_hold_total", static_cast<std::int64_t>(def_mh));
            insert_kv("mailbox-mutation-hold-gate-wired", 1);
            insert_kv("schema-2312", 2312);
            insert_kv("issue-2312", 2312);
            // Issue #2378: defer drain SLA — depth / HWM / flush latency /
            // starvation (last-call + cumulative). Happy path depth=0 free.
            insert_kv("mailbox-deferred-depth",
                      static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth.load(
                          std::memory_order_relaxed)));
            insert_kv("mailbox_deferred_depth",
                      static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth.load(
                          std::memory_order_relaxed)));
            insert_kv(
                "mailbox-deferred-depth-high-water",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth_high_water.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox_deferred_depth_high_water",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_depth_high_water.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-deferred-flush-latency-us-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_deferred_flush_latency_us_total.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-deferred-flush-samples",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_deferred_flush_samples.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-deferred-flush-latency-us-max",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_deferred_flush_latency_us_max.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-defer-starvation-total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox_defer_starvation_total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_defer_starvation_total.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-deferred-drain-opportunity-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_deferred_drain_opportunity_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-defer-drain-sla-wired", 1);
            insert_kv("schema-2378", 2378);
            insert_kv("issue-2378", 2378);
            // Issue #2511: outermost Guard exit forced deferred drain under budget.
            insert_kv(
                "mailbox-hold-exit-drain-total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox-hold-exit-drain-us-total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_hold_exit_drain_us_total.load(
                    std::memory_order_relaxed)));
            insert_kv(
                "mailbox-hold-exit-drain-us-max",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_hold_exit_drain_us_max.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-hold-exit-starvation-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-hold-exit-force-resolved-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_hold_exit_force_resolved_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-hold-exit-drain-budget-us",
                      static_cast<std::int64_t>(mailbox_hold_drain_budget_us()));
            insert_kv("mailbox-hold-exit-drain-wired", 1);
            insert_kv("schema-2511", 2511);
            insert_kv("issue-2511", 2511);
            // Issue #2551: production hard starvation + Agent throttle flag.
            insert_kv("mailbox-hold-starvation-hard-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(
                              std::memory_order_relaxed)));
            insert_kv("agent-throttle-for-mailbox-starvation",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-hold-starvation-hard-wired", 1);
            insert_kv("schema-2551", 2551);
            insert_kv("issue-2551", 2551);
            // Issue #2680 / #2849: shared-Evaluator mid-mutation delivery gate.
            // under_boundary_* are the Agent-facing #2849 names (same authority
            // as shared_evaluator_*). Production: hard-total under Strict /
            // production defaults; Soft: soft_observe. Always Backpressure —
            // never mid-mutation enqueue.
            insert_kv("mailbox-shared-evaluator-deferred-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-shared-evaluator-deferred-hard-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_hard_total.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-shared-evaluator-deferred-soft-observe-total",
                static_cast<std::int64_t>(
                    g_mf_mailbox_stats.mailbox_shared_evaluator_deferred_soft_observe_total.load(
                        std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-deferred-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_deferred_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-deferred-hard-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_deferred_hard_total.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-under-boundary-deferred-soft-observe-total",
                static_cast<std::int64_t>(
                    g_mf_mailbox_stats.mailbox_under_boundary_deferred_soft_observe_total.load(
                        std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-gate-wired", 1);
            insert_kv("schema-2680", 2680);
            insert_kv("issue-2680", 2680);
            insert_kv("schema-2849", 2849);
            insert_kv("issue-2849", 2849);
            // Issue #2903: deferred-under-boundary wait histogram + p50/p99/max.
            // Agents poll wait latency without stitching multiple queries.
            // Soft / zero-defer path: counters stay 0 (no hist noise).
            insert_kv("mailbox-under-boundary-wait-us-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_us_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-samples",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_samples.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-us-max",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_us_max.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-us-p50",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_us_p50.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-us-p99",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_us_p99.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-drop-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_drop_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-hist-lt-100us",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_hist[0].load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-hist-lt-1ms",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_hist[1].load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-hist-lt-10ms",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_hist[2].load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-hist-lt-100ms",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_hist[3].load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-hist-ge-100ms",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_under_boundary_wait_hist[4].load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-under-boundary-wait-wired", 1);
            insert_kv("schema-2903", 2903);
            insert_kv("issue-2903", 2903);
            // Issue #2958: production hold-budget cancel on wait SLO breach.
            insert_kv("mailbox-defer-slo-hold-cancel-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_defer_slo_hold_cancel_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-defer-slo-soft-observe-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_defer_slo_soft_observe_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-defer-slo-breach-observe-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_defer_slo_breach_observe_total.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-defer-slo-no-holder-total",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_defer_slo_no_holder_total.load(
                    std::memory_order_relaxed)));
            insert_kv("mailbox-defer-slo-hold-cancel-wired",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_defer_slo_hold_cancel_wired.load(
                              std::memory_order_relaxed)));
            insert_kv("schema-2958", 2958);
            insert_kv("issue-2958", 2958);
            // Issue #3002: SSOT live p99 sample feeds #2947 deny + #2958 cancel.
            insert_kv("mailbox-hold-slo-ssot-wired",
                      static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_hold_slo_ssot_wired.load(
                          std::memory_order_relaxed)));
            insert_kv("schema-3002", 3002);
            insert_kv("issue-3002", 3002);
            // Issue #2987: mailbox delivery residual hard-AND (steal table).
            insert_kv("mailbox-delivery-reject-layout-stamp-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_delivery_reject_layout_stamp_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-delivery-reject-ticket-stale-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_delivery_reject_ticket_stale_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-delivery-reject-residual-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_delivery_reject_residual_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-delivery-reject-hard-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_delivery_reject_hard_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-delivery-reject-soft-observe-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_delivery_reject_soft_observe_total.load(
                              std::memory_order_relaxed)));
            insert_kv(
                "mailbox-delivery-safety-wired",
                static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_delivery_safety_wired.load(
                    std::memory_order_relaxed)));
            insert_kv("schema-2987", 2987);
            insert_kv("issue-2987", 2987);
            // Issue #3036: production residual RejectHard (no Soft escape).
            insert_kv("mailbox-residual-hard-reject-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_residual_hard_reject_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mailbox-residual-hard-reject-wired",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_residual_hard_reject_wired.load(
                              std::memory_order_relaxed)));
            insert_kv("schema-3036", 3036);
            insert_kv("issue-3036", 3036);
            // Issue #2587: mutate admission gate counter (hard reject
            // vs metric-only soft path; AC1 / AC2). Zero cost when
            // agent-throttle flag == 0 — single relaxed load at every
            // gate site (try_acquire + try_acquire_for_region +
            // public mutate prims + TransactionGuard host callback
            // transitively).
            insert_kv("mutate-rejected-mailbox-starvation-total",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mutate_rejected_mailbox_starvation_total.load(
                              std::memory_order_relaxed)));
            insert_kv("mutate-rejected-mailbox-starvation-wired", 1);
            insert_kv("schema-2587", 2587);
            insert_kv("issue-2587", 2587);
            // Issue #2347: Strict hard audit + optional Guard-window force-rollback.
            // Soft path leaves hard-total / force-rollback-total at 0; Policy A
            // soft reject remains on recv-rejected-in-mutation-boundary.
            // Capacity 48 holds pre-#2347 keys + these additive rows without drop.
            const auto hard_tot =
                g_mf_mailbox_stats.recv_rejected_in_mutation_boundary_hard_total.load(
                    std::memory_order_relaxed);
            const auto force_rb = g_mf_mailbox_stats.recv_boundary_force_rollback_total.load(
                std::memory_order_relaxed);
            insert_kv("recv-rejected-in-mutation-boundary-hard-total",
                      static_cast<std::int64_t>(hard_tot));
            insert_kv("recv_rejected_in_mutation_boundary_hard_total",
                      static_cast<std::int64_t>(hard_tot));
            insert_kv("recv-boundary-force-rollback-total", static_cast<std::int64_t>(force_rb));
            insert_kv("recv_boundary_force_rollback_total", static_cast<std::int64_t>(force_rb));
            insert_kv("mutate-mailbox-strict-wired", 1);
            insert_kv("recv-boundary-hard-wired", 1);
            insert_kv("schema-2347", 2347);
            insert_kv("issue-2347", 2347);
            // Issue #2972: per-mailbox inflight credit (additive).
            insert_kv("mailbox-credit-bp-total",
                      static_cast<std::int64_t>(g_mf_mailbox_stats.mailbox_credit_bp_total.load(
                          std::memory_order_relaxed)));
            insert_kv("mailbox-inflight-hwm",
                      static_cast<std::int64_t>(
                          g_mf_mailbox_stats.mailbox_inflight_hwm.load(std::memory_order_relaxed)));
            insert_kv("mailbox-credit-wired", 1);
            insert_kv("schema-2972", kMailboxCreditInflightIssue);
            insert_kv("issue-2972", kMailboxCreditInflightIssue);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            // Issue #4110: stamp the owner parallel to the table (SSOT helper; runtime_ssot
            // contract).
            aura_hash_stamp_new_table_owner();
            return make_hash(hidx);
        });

    // Issue #1586 / #1881: process-global parallel_orch / parallel_intend stats.
    // Full spawn/join/aggregate API is C++ (parallel_orch.h); agent EDSL
    // surface for (parallel-intend ...) is #1587.
    ObservabilityPrims::register_stats_impl(
        "query:parallel-orch-stats", [&ev](const auto&) -> EvalValue {
            using namespace aura::serve::parallel_orch;
            std::uint64_t batches = 0, spawned = 0, joined = 0, ok = 0, err = 0, ff = 0, to = 0,
                          mb = 0, qr = 0, inv = 0, bok = 0, bpart = 0, jwait = 0, elapsed = 0,
                          retries = 0, circuit = 0;
            snapshot_global_ext(batches, spawned, joined, ok, err, ff, to, mb, qr, inv, bok, bpart,
                                jwait, elapsed, &retries, &circuit);
            // Capacity 48: pre-#2153 keys + join-drain residual/us/schema (#2153).
            auto* ht = FlatHashTable::create(48);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.mint(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = make_int(v).val;
                        ht->size++;
                        return;
                    }
                }
            };
            insert_kv("batches", static_cast<std::int64_t>(batches));
            insert_kv("spawned", static_cast<std::int64_t>(spawned));
            insert_kv("joined", static_cast<std::int64_t>(joined));
            insert_kv("ok", static_cast<std::int64_t>(ok));
            insert_kv("err", static_cast<std::int64_t>(err));
            insert_kv("fail-fast-aborts", static_cast<std::int64_t>(ff));
            insert_kv("timeouts", static_cast<std::int64_t>(to));
            insert_kv("mailbox-posts", static_cast<std::int64_t>(mb));
            // Issue #1881 health fields
            insert_kv("quota-rejects", static_cast<std::int64_t>(qr));
            insert_kv("invalid-batches", static_cast<std::int64_t>(inv));
            insert_kv("batch-ok", static_cast<std::int64_t>(bok));
            insert_kv("batch-partial", static_cast<std::int64_t>(bpart));
            insert_kv("join-wait-us-total", static_cast<std::int64_t>(jwait));
            insert_kv("avg-join-us", batches > 0 ? static_cast<std::int64_t>(jwait / batches) : 0);
            insert_kv("parallel-elapsed-us", static_cast<std::int64_t>(elapsed));
            // Issue #2007 FailurePolicy counters
            insert_kv("retries-performed", static_cast<std::int64_t>(retries));
            insert_kv("circuit-opened-total", static_cast<std::int64_t>(circuit));
            insert_kv("phase", static_cast<std::int64_t>(kParallelOrchPhase));
            insert_kv("schema", 1586);
            insert_kv("schema-1881", 1881);
            insert_kv("schema-2007", 2007);
            insert_kv("health-wired", 1);
            // Issue #2153: Timeout path secondary drain residual / wait.
            insert_kv(
                "join-drain-residual-total",
                static_cast<std::int64_t>(g_parallel_orch_stats.join_drain_residual_total.load(
                    std::memory_order_relaxed)));
            insert_kv("join-drain-us-total",
                      static_cast<std::int64_t>(g_parallel_orch_stats.join_drain_us_total.load(
                          std::memory_order_relaxed)));
            insert_kv("schema-2153", 2153);
            insert_kv("issue-2153", 2153);
            insert_kv("join-drain-wired", 1);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            // Issue #4110: stamp the owner parallel to the table (SSOT helper; runtime_ssot
            // contract).
            aura_hash_stamp_new_table_owner();
            return make_hash(hidx);
        });

    // (fiber:spawn fn) — Spawn a fiber (async).
    // fn is a 0-arg closure. Result retrievable via (fiber:join fid).
    //
    // Issue #2656 contract (CLI denseness / Hephaestus H9):
    //   Success → positive integer fiber id
    //   Failure → #f (or capability error object under sandbox)
    //   Never returns 0 or -1 on success.
    //
    // Backends:
    //   1. Scheduler — when g_fiber_spawn is set (serve-async /
    //      --serve-async-bench). Positive scheduler Fiber::id().
    //   2. Thread fallback — CLI stdin / denseness without serve-async.
    //      Joinable std::thread (registered + drained at ~Evaluator,
    //      Issue #3394) + positive high-bit ids (0x4000_0000 | seq).
    //      Pre-#2656 used negative ids starting at -1; denseness
    //      probes treated -1 as failure even though join worked.
    //
    // Concurrent denseness: thread fallback is a valid multi-worker
    // backend for axis-C rebind under load. Real stackful fibers need
    // serve-async / epoll. Sequential-yield fanout remains a supported
    // denseness surrogate when spawn is denied by capability/sandbox.
    add("fiber:spawn", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_closure(a[0]))
            return make_bool(false);
        // Issue #1294 Phase 1: capability gate for fiber primitives.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapFiber) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: fiber required",
                                        ev.primitive_error_counter_ptr());
        }
        auto cid = as_closure_id(a[0]);
        auto result_ptr = std::make_shared<std::optional<EvalValue>>();
        // Issue #1291 (P0): shared atomic holds the real fiber id.
        // Pre-#1291 captured `fid` by value at lambda creation (fid==0),
        // so workers wrote s_fiber_results[0] while joiners waited on
        // the real fid — hang forever. Shared state is published before
        // the worker runs complete_fiber.
        auto fid_holder = std::make_shared<std::atomic<int64_t>>(0);
        auto complete_fiber = [&ev, cid, result_ptr, fid_holder]() {
            // Issue #2738: when using CLI std::thread backend, serialize body
            // evaluation against other thread-fibers on this process. Scheduler
            // path (g_fiber_spawn) does not take this lock — fibers are isolated.
            // Detect thread-backend after spawn by checking g_fiber_spawn absence
            // at run time would be racy; instead lock only when the launch path
            // sets the "thread" fid bit (see below). Here: always take the mutex
            // when g_fiber_spawn is null at body start (CLI denseness).
            //
            // Issue #2869: publish TLS lock pointer so nested fiber:join can
            // temporarily unlock while waiting for a child (child needs this
            // mutex to run on the thread backend).
            // Issue #4699: the pending-body count was taken by the spawn
            // path (before this body existed); this scope only marks the
            // end of the body so a cooperative yield knows that no sibling
            // body is left to hand the turn over to.
            // Issue #4708: publish this body's fiber id so a cancellation
            // safepoint can find it on the thread backend (scheduler fibers
            // answer through aura::serve::g_current_fiber instead).
            struct FiberBodyIdScope {
                std::int64_t prev;
                explicit FiberBodyIdScope(std::int64_t id) noexcept
                    : prev(s_tls_fiber_body_id) {
                    s_tls_fiber_body_id = id;
                }
                ~FiberBodyIdScope() noexcept { s_tls_fiber_body_id = prev; }
                FiberBodyIdScope(const FiberBodyIdScope&) = delete;
                FiberBodyIdScope& operator=(const FiberBodyIdScope&) = delete;
            } body_id_scope(fid_holder->load(std::memory_order_acquire));
            CliBodyDoneScope body_done_scope;
            std::unique_lock<std::mutex> body_lock(s_cli_thread_fiber_body_mtx, std::defer_lock);
            // Issue #4048: serialize denseness bodies for BOTH backends.
            // Soft Ready --serve-async denseness fibers share the session
            // Evaluator (complete_fiber captures &ev). Multi-worker affinity
            // pins help, but the parent may still run on another worker
            // until the first join yield — concurrent apply_closure races
            // TLS/heap and hangs the session. Always take the body mutex;
            // fiber:join / async http-post DensenessBodyLockWaitGuard unlocks
            // across the wait so siblings can overlap (#2869 / #4053).
            // Issue #4699: tracked acquire - the takeover publish wakes a
            // yielding sibling that waits for its turn (the CLI thread
            // backend has no scheduler to do the hand-off).
            cli_body_lock_acquire(body_lock);
            // Issue #4054: do NOT save/restore a prev TLS pointer across the
            // denseness body. Cooperative Soft fibers share one OS thread;
            // restoring prev after a sibling has published (or after wait
            // cleared TLS) either wipes the sibling or reinstalls a dangling
            // unique_lock*. Clear only if we still own the slot.
            struct TlsBodyLockScope {
                std::unique_lock<std::mutex>* cur;
                explicit TlsBodyLockScope(std::unique_lock<std::mutex>* c) noexcept
                    : cur(c) {
                    if (cur)
                        s_tls_cli_body_lock() = cur;
                }
                ~TlsBodyLockScope() noexcept {
                    if (cur && s_tls_cli_body_lock() == cur)
                        s_tls_cli_body_lock() = nullptr;
                }
                TlsBodyLockScope(const TlsBodyLockScope&) = delete;
                TlsBodyLockScope& operator=(const TlsBodyLockScope&) = delete;
            } tls_scope(body_lock.owns_lock() ? &body_lock : nullptr);
            *result_ptr = ev.apply_closure(cid, {});
            // Issue #4699: publish the body end so a sibling body that is
            // waiting for its turn (a cooperative yield) does not stall on
            // a takeover that will never come.
            cli_body_lock_publish_takeover();
            // Issue #1291: wait until spawn path publishes the real fid
            // AND registers s_fiber_results[fid] (g_fiber_spawn may run
            // the body before returning / before register).
            bool arena_published = false;
            for (int spin = 0; spin < 200000; ++spin) {
                const int64_t fid = fid_holder->load(std::memory_order_acquire);
                if (fid != 0 && !arena_published) {
                    // Issue #4714: copy-on-send. Promote the fiber's
                    // private string nursery into shared old space (one
                    // narrow lock) BEFORE the result is marked ready, so
                    // no joiner can observe a nursery-owned value.
                    // Deliberately outside s_fiber_results_mtx: the #4710
                    // result-root provider takes that mutex while the
                    // reclaim holds alloc_storage_lock_, and promoting
                    // under it would invert that order.
                    (void)ev.publish_fiber_string_arena(static_cast<std::uint64_t>(fid));
                    arena_published = true;
                }
                if (fid != 0) {
                    std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
                    auto it = s_fiber_results.find(fid);
                    if (it != s_fiber_results.end()) {
                        it->second.ready = true;
                        s_fiber_results_cv.notify_all();
                        return;
                    }
                }
                std::this_thread::yield();
            }
            // Best-effort notify even if registration raced past timeout.
            s_fiber_results_cv.notify_all();
        };
        int64_t fid = 0;
        // Issue #4699: the pending-body count is taken at SPAWN time, before
        // the body exists: a sibling body of the CLI thread backend may not
        // have started yet, and a cooperative yield that does not see it
        // would hand the turn over to nobody and run the body to
        // completion. The body scope drops the count when the body ends.
        s_cli_body_pending.fetch_add(1, std::memory_order_acq_rel);
        // Issue #4699: the spawn epoch tells a yielding sibling that the
        // parent spawned another body (the parent spawns one at a time, so
        // the sibling set grows while the sibling waits for its turn).
        s_cli_spawn_epoch.fetch_add(1, std::memory_order_acq_rel);
        // In serve-async mode: use g_fiber_spawn to create a real fiber
        if (aura::messaging::g_fiber_spawn) {
            // Issue #4176: at RLIMIT_NOFILE exhaustion the Fiber ctor
            // throws std::system_error ("fiber eventfd"). Swallow it and
            // fail per the #2656 contract (#f) instead of unwinding
            // through the fiber trampoline (kills the session fiber →
            // permanent sock stall). A failing scheduler must NOT fall
            // through to the thread backend either — spawn_failed skips
            // the fallback so the caller sees an honest failure.
            bool spawn_failed = false;
            try {
                fid = aura::messaging::g_fiber_spawn(complete_fiber);
            } catch (const std::exception&) {
                fid = 0;
                spawn_failed = true;
            }
            fid_holder->store(fid, std::memory_order_release);
            if (spawn_failed) {
                // No body will run: drop the pending-body count.
                s_cli_body_pending.fetch_sub(1, std::memory_order_acq_rel);
                return make_bool(false);
            }
        }
        // Fallback (CLI denseness / stdin): std::thread with positive ids.
        if (fid <= 0) {
            // Issue #2656: positive thread-fallback ids. High bit set so
            // they never collide with low scheduler Fiber::id() values
            // and never equal -1 (denseness failure sentinel).
            static std::atomic<std::int64_t> thread_fiber_seq{0};
            const auto seq = thread_fiber_seq.fetch_add(1, std::memory_order_relaxed) + 1;
            fid = static_cast<std::int64_t>(0x4000'0000LL) | seq;
            fid_holder->store(fid, std::memory_order_release);
            // Issue #4697: root the pending body closure BEFORE the worker
            // thread starts. The body closure is a tree-walker temporary
            // that this spawn drops on return, so the tree-walker reclaim
            // (the #4459 / #4474 safepoint) could erase it before the
            // worker applied it: apply_closure returned nullopt, the body
            // never ran and fiber:join returned void (silent result loss).
            aura_fiber_root_pending_body(static_cast<void*>(&ev), static_cast<std::uint64_t>(cid),
                                         static_cast<std::uint64_t>(fid));
            {
                std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
                s_fiber_results[fid] = FiberResult{result_ptr, false};
            }
            // Issue #3394: joinable (NOT detached). Registered so
            // fiber:join and ~Evaluator (Evaluator::drain_thread_fibers)
            // can join the worker before the Evaluator's workspace/arena
            // goes away.
            {
                std::lock_guard<std::mutex> lock(s_thread_fiber_threads_mtx);
                s_thread_fiber_threads.emplace(fid, std::make_pair(static_cast<const void*>(&ev),
                                                                   std::thread(complete_fiber)));
            }
            return make_int(fid);
        }
        if (fid > 0) {
            // Publish fid before registering so a fast worker still
            // sees the real id (complete_fiber loads with acquire).
            fid_holder->store(fid, std::memory_order_release);
            std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
            s_fiber_results[fid] = FiberResult{std::move(result_ptr), false};
            return make_int(fid);
        }
        return make_bool(false);
    });

    // Issue #2656: which backend (fiber:spawn) will use right now.
    // "scheduler" | "thread" — denseness hosts can assert concurrent path.
    add("fiber:spawn-backend", [](const auto&) -> EvalValue {
        // Interned at call time via make_string of heap push is heavier;
        // return a small int enum for probes + keep string via display path
        // optional. Use 1=scheduler, 2=thread (always one of these when
        // capability allows; capability deny is only on spawn).
        if (aura::messaging::g_fiber_spawn)
            return make_int(1); // scheduler
        return make_int(2);     // thread fallback (CLI denseness)
    });

    // (fiber:cancel fiber-id) -- request cooperative cancellation of ONE fiber.
    //
    // Issue #4708: before this a host had only a shared 'stop' flag that
    // workers polled between turns (or orch:scope-cancel-all). A worker
    // stopped mid-turn kept its claim, and a fiber parked inside a body
    // could not be stopped at all.
    //
    //   #t    -- request accepted for a live fiber (either backend)
    //   #f    -- no live fiber with that id (unknown / already done)
    //   error -- bad arg, or capability denial under sandbox
    //
    // The request is observed at the target's next safepoint: fiber:yield,
    // the fiber-parking channel prims (send / recv), and
    // the fiber:join wait. The observing body gets a catchable condition
    // ("fiber-cancelled: ...") so try/catch cleanup can run; an uncaught
    // condition is the body result, which fiber:join returns.
    add("fiber:cancel", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "fiber:cancel: usage: (fiber:cancel fiber-id)",
                                        ev.primitive_error_counter_ptr());
        // Issue #1294 Phase 1: same capability gate as fiber:spawn.
        if (ev.sandbox_mode() && !ev.has_capability(aura::compiler::security::kCapFiber) &&
            !ev.has_capability(aura::compiler::security::kCapWildcard)) {
            ev.bump_capability_denial();
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "capability denied: fiber required",
                                        ev.primitive_error_counter_ptr());
        }
        const auto fid = static_cast<int64_t>(as_int(a[0]));
        if (fid == 0)
            return make_bool(false);
        if (aura::messaging::g_fiber_lookup) {
            // Scheduler backend: Fiber already carries the #1584
            // cooperative cancel flag; also record it here so the
            // safepoint helper is backend-agnostic.
            auto* target = static_cast<aura::serve::Fiber*>(aura::messaging::g_fiber_lookup(fid));
            if (!target || target->is_done())
                return make_bool(false);
            target->request_cancel();
            fiber_cancel_request(fid);
            // Nudge a PARKED fiber: a Waiting fiber is woken by a 1-byte
            // write to its eventfd (the scheduler wake protocol). A
            // spurious wake is legal -- the parking loops re-check their
            // predicate after every wake (#4176).
            const int evfd = target->eventfd();
            if (evfd >= 0) {
                std::uint64_t one = 1;
                (void)::write(evfd, &one, sizeof(one));
            }
            if (target->state() == aura::serve::FiberState::Waiting)
                target->set_state(aura::serve::FiberState::Ready);
            return make_bool(true);
        }
        // Thread backend: the worker observes the flag at its next
        // safepoint. notify_all wakes a joiner blocked on the cv so it can
        // re-check its own cancellation promptly.
        {
            std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
            auto it = s_fiber_results.find(fid);
            if (it == s_fiber_results.end() || it->second.ready)
                return make_bool(false);
        }
        fiber_cancel_request(fid);
        s_fiber_results_cv.notify_all();
        return make_bool(true);
    });

    // (fiber:yield) — Yield current fiber to scheduler (serve mode only)
    // Uses g_fiber_yield callback if available.
    //
    // Issue #4699: a cooperative yield must hand the turn to a sibling
    // fiber, not run the body to completion. Two parts:
    //
    // 1. The body mutex is released across the yield (the same protocol
    //    as the fiber:join wait). The sibling body that runs next needs
    //    that mutex, and on a single worker it runs on the SAME OS thread:
    //    holding it across the yield would self-deadlock the worker.
    // 2. In the CLI thread backend there is no scheduler that hands the
    //    worker over, so the yielding body waits until a sibling body has
    //    taken the mutex over (CliBodyYieldHandoff). Without that wait the
    //    yielding body re-acquires the mutex itself (barging) and the
    //    bodies stay run-to-completion.
    //
    // In the scheduler backend the serve-layer FIFO ready queue
    // (serve/worker.cpp) does the hand-off: the yielded fiber is
    // re-dispatched behind the siblings that are already ready.
    add("fiber:yield", [&ev](const auto&) -> EvalValue {
        // Issue #4708: the yield is the primary cancellation safepoint. A
        // cancelled body raises a catchable condition here BEFORE the
        // hand-off, so try/catch cleanup runs on the fiber's own stack.
        if (auto cancel_cond = fiber_cancel_safepoint(ev.string_heap_, ev.error_values_,
                                                      ev.primitive_error_counter_ptr()))
            return *cancel_cond;
        const bool scheduler_backend = aura::messaging::g_fiber_yield != nullptr;
        CliBodyYieldHandoff body_handoff(!scheduler_backend);
        if (aura::messaging::g_fiber_yield) {
            aura::messaging::g_fiber_yield();
        }
        return make_void();
    });

    // (session:create name) — Create a new isolated session (serve mode only)
    // Returns #t on success, #f in stdin mode or if name already exists
    add("session:create", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto& name = ev.string_heap_[as_string_idx(a[0])];
        if (!aura::messaging::g_session_create || !(*aura::messaging::g_session_create))
            return make_bool(false);
        return make_bool((*aura::messaging::g_session_create)(name));
    });

    // (_agent:spawn name) — Create a new named agent session (internal primitive)
    // Called by the Aura-level agent:spawn wrapper.
    // In serve mode: creates a full cross-session agent via g_session_create.
    // In stdin mode: creates a lightweight in-process agent with a mailbox.
    // Returns the agent name on success, or error on failure.
    add("_agent:spawn", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // local merr removed; now centralized make_merr (phase complete)
        if (a.empty() || !is_string(a[0]))
            return ev.make_merr("bad-arg", "usage: (agent:spawn name)");
        auto& name = ev.string_heap_[as_string_idx(a[0])];
        if (name.empty())
            return ev.make_merr("bad-arg", "agent name must not be empty");

        // Try serve-mode first (full session isolation)
        if (aura::messaging::g_session_create && *aura::messaging::g_session_create) {
            if ((*aura::messaging::g_session_create)(name)) {
                auto sidx = ev.string_heap_.mint(name);
                return make_string(sidx);
            }
            return ev.make_merr("create-failed",
                                std::string("could not create session \"") + name + "\"");
        }

        // Stdin/pipe mode: lightweight in-process agent via Aura-level *agents* registry
        // The Aura-level agent:spawn wraps this C++ primitive and falls back to
        // the *agents* registry when g_session_create is unavailable.
        return ev.make_merr("no-serve", "agent:spawn requires serve mode or a local handler");
    });

    // (fiber:join fiber-id) — Wait for a fiber to complete and return its result.
    //
    // Issue #119: replaces the previous 200k-iteration spin-wait
    // (unreliable for slow fibers / heavily loaded schedulers)
    // with a proper blocking mechanism. Two paths:
    //
    // 1. **stdin mode** (no fiber scheduler): the OS thread
    //    blocks on s_fiber_results_cv via wait_for (200s ceiling
    //    via wait_for's timeout). Woken by the worker that
    //    completes the target fiber. Zero CPU burn during the
    //    wait. (This path was already in #109 5b; preserved.)
    //
    // 2. **serve-async mode** (fiber scheduler active): the
    //    joiner fiber registers itself on the target's
    //    joiner_map_ via the new g_fiber_lookup +
    //    Scheduler::add_joiner API. When the target completes,
    //    Scheduler::on_fiber_done writes a 1 to the joiner's
    //    eventfd, which is registered with the IO thread's
    //    epoll — the IO thread resumes the joiner via
    //    wait_map_ lookup. The joiner yields (yield reason:
    //    BlockingIO — unstealable), the IO thread picks up
    //    the eventfd notification, and the joiner is resumed
    //    in the next dispatch cycle. No spin, no busy poll.
    //
    // Both paths share the same s_fiber_results entry / ready
    // flag, so the result-fetch-and-erase is unchanged.
    add("fiber:join", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto fid = static_cast<int64_t>(as_int(a[0]));

        // Issue #4697: release the pending body root once this join has
        // consumed the fiber (the body has run). The root is the
        // evaluator's pinned-closure watermark, so the release is safe
        // from any thread: the watermark is restored only when the last
        // pending fiber is done.
        struct PendingBodyRootRelease {
            void* ev;
            std::uint64_t fid;
            ~PendingBodyRootRelease() { aura_fiber_release_pending_body(ev, fid); }
        };
        const PendingBodyRootRelease pending_body_root_release{static_cast<void*>(&ev),
                                                               static_cast<std::uint64_t>(fid)};

        auto is_ready = [fid] {
            auto it = s_fiber_results.find(fid);
            return it != s_fiber_results.end() && it->second.ready;
        };

        // Issue #164 sub-fix #4: capture the workspace's
        // ev.defuse_version_ at the start of the join, so we can
        // re-check it at wakeup to detect mutations that
        // happened DURING the join. Set to 0 at the start means
        // the post-wait check is "first join ever or just reset"
        // — guarded by `!= 0` so we don't false-positive on the
        // initial state.
        ev.defuse_version_at_wait_ = ev.defuse_version_.load(std::memory_order_relaxed);

        if (aura::messaging::g_fiber_yield && aura::messaging::g_fiber_lookup) {
            // Issue #119: serve-async proper-blocking path.
            //
            // 1. Check if the target is already done. If so, skip
            //    the joiner registration and go straight to
            //    result fetch.
            // 2. Register this fiber (the joiner) on the target's
            //    joiner_map via Scheduler::add_joiner.
            // 3. Yield with BlockingIO (unstealable) so the
            //    scheduler parks the joiner. The target's
            //    on_fiber_done wakes the joiner via eventfd;
            //    the IO thread's epoll resumes the joiner.
            // 4. After wakeup, unregister from joiner_map and
            //    fetch the result.
            auto* target = static_cast<aura::serve::Fiber*>(aura::messaging::g_fiber_lookup(fid));
            if (!target) {
                // Target fiber not found. Fall through to the
                // stdin-style result-fetch path: if a result
                // exists, return it; otherwise return void.
                std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
                auto it = s_fiber_results.find(fid);
                if (it == s_fiber_results.end())
                    return make_void();
                if (!it->second.ready)
                    return make_void();
            } else if (!target->is_done()) {
                // Target is alive. Register the joiner and
                // yield. We need to schedule the unregister for
                // after the wakeup — store the target pointer
                // in a thread_local so the post-yield cleanup
                // can find it.
                thread_local std::uint64_t joiner_target_id = 0;
                // (Note: thread_local is a per-fiber/thread trick;
                // a cleaner approach is to pass the target_id as
                // a stack value via the yield + resume protocol,
                // but that's invasive. The thread_local works
                // because the joiner fiber is single-threaded.)
                // Issue #4176: the BlockingIO park is a *wait
                // protocol*, not a one-shot wake. Spurious eventfd
                // wakes are legal (stdin-event broadcast across
                // wait_map_, recycled wake fds) and a wake that finds
                // the target still running must re-register and
                // re-park. The old single-shot shape treated ANY wake
                // as target-done: it removed the joiner, observed a
                // not-ready result slot, ERASED the pending slot and
                // returned void — the orphaned child kept the denseness
                // body mutex and the next spawn's body blocked on it
                // while running on the worker → permanent serve-async
                // sock stall (no reply, IO thread idle in ep_poll).
                // Mirrors the Fiber::join wait loop (predicate
                // re-checked after every wake). Escapes: target done,
                // target reclaimed (reap wakes the joiner once;
                // is_done() never fires), or target unregistered
                // (re-resolve via g_fiber_lookup → fall through to the
                // result fetch).
                // Issue #4176: capture the joiner fiber pointer ONCE,
                // before any yield. Re-reading thread_local g_current_fiber
                // after a BlockingIO park is unsafe: the compiler may cache
                // the TLS base (mrs tpidr_el0) across Fiber::yield, and the
                // joiner can be woken on a DIFFERENT worker after
                // work-stealing migration. The stale base then reads null,
                // add_joiner(..., nullptr) returns false, and the join
                // degrades / busy-spins (sock stall, IO thread idle in
                // ep_poll). `joiner_fiber` is migration-invariant.
                auto* joiner_fiber = aura::serve::g_current_fiber;
                while (true) {
                    if (target->is_done() || target->is_reclaimed())
                        break;
                    // Issue #4708: a cancelled JOINER stops waiting and
                    // raises the same catchable condition instead of
                    // parking until the target finishes.
                    if (joiner_fiber &&
                        fiber_cancel_take(static_cast<int64_t>(joiner_fiber->id()))) {
                        if (joiner_target_id && aura::serve::g_scheduler)
                            aura::serve::g_scheduler->remove_joiner(joiner_target_id, joiner_fiber);
                        return fiber_cancel_condition_value(ev.string_heap_, ev.error_values_,
                                                            ev.primitive_error_counter_ptr());
                    }
                    if (!static_cast<aura::serve::Fiber*>(aura::messaging::g_fiber_lookup(fid)))
                        break; // target unregistered — result fetch below
                    if (!(aura::serve::g_scheduler &&
                          aura::serve::g_scheduler->add_joiner(static_cast<std::uint64_t>(fid),
                                                               joiner_fiber)))
                        break;
                    joiner_target_id = static_cast<std::uint64_t>(fid);
                    // Issue #2869 / #4053: unlock denseness body mutex across
                    // the join wait so a nested denseness child can run.
                    CliBodyLockJoinGuard body_join_guard;
                    // Yield with BlockingIO so the scheduler
                    // doesn't steal this fiber while we wait.
                    aura::serve::Fiber::yield(aura::serve::YieldReason::BlockingIO);
                    // After wakeup: either the target's on_fiber_done
                    // wrote our eventfd (target done — fetch below),
                    // or a spurious/early wake fired (loop re-parks).
                    aura::serve::g_scheduler->remove_joiner(joiner_target_id, joiner_fiber);
                }
                // (If add_joiner failed — e.g. target was
                // destroyed between the lookup and the add —
                // we fall through to the result fetch below.
                // The fetch will find no entry and return void,
                // which is the correct error behavior.)
            }
            // else: target is already done. Skip the join — go
            // straight to the result fetch below.
        } else if (aura::messaging::g_fiber_yield) {
            // Issue #164: serve-async but no fiber_lookup. The
            // previous implementation did a 200k-iteration spin
            // with g_fiber_yield between iterations. This is a
            // bug:
            //   - Wastes CPU even with yield (the test reproduces
            //     5-15% CPU burn under 50+ fibers with mutations)
            //   - Creates transient ev.defuse_version_ ↔ workspace
            //     inconsistency: while spinning, mutate:*
            //     primitives may have incremented ev.defuse_version_
            //     (twice — once before and once after the
            //     mutation) and the joiner's view of the workspace
            //     is now stale.
            //   - Degrades to old behavior silently when
            //     g_fiber_lookup is missing (e.g. test contexts
            //     where the fiber scheduler hook isn't installed).
            //
            // The fix: degrade cleanly to the stdin-style
            // blocking wait on s_fiber_results_cv. The cv
            // semantics are already tested (Issue #109 5b) and
            // give us zero-CPU blocking. This is the "clean
            // degradation" option from the issue AC.
            //
            // Tradeoff: slower wakeup than the proper-blocking
            // path (cv needs a notify_all + epoll cycle vs the
            // eventfd + epoll cycle), but ZERO CPU burn during
            // the wait, and no version inconsistency.
            std::println(std::cerr, "[fiber:join] WARN: serve-async mode without g_fiber_lookup; "
                                    "degrading to cv-based blocking. This is a misconfiguration "
                                    "in production; check the fiber scheduler hook installation.");
            // Issue #2869: unlock CLI body mutex while waiting (nested join).
            CliBodyLockJoinGuard body_join_guard;
            std::unique_lock<std::mutex> lock(s_fiber_results_mtx);
            // Issue #4708: the wait is cancellation-aware -- a cancelled
            // joiner returns the catchable condition instead of sitting on
            // the cv until the target publishes (or the 200s ceiling).
            const auto join_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(200);
            while (!is_ready()) {
                if (fiber_cancel_take(current_fiber_body_id()))
                    return fiber_cancel_condition_value(ev.string_heap_, ev.error_values_,
                                                        ev.primitive_error_counter_ptr());
                if (std::chrono::steady_clock::now() >= join_deadline)
                    break;
                s_fiber_results_cv.wait_for(lock, std::chrono::milliseconds(50));
            }
        } else {
            // Stdin mode: real blocking wait on the cv. 200s
            // ceiling via wait_for (returns false on timeout,
            // true on notify).
            //
            // Issue #2869: if this join runs inside a thread-backend
            // fiber body that holds s_cli_thread_fiber_body_mtx,
            // release it for the wait so the child can acquire the
            // mutex and complete. Relock after wakeup (RAII).
            CliBodyLockJoinGuard body_join_guard;
            std::unique_lock<std::mutex> lock(s_fiber_results_mtx);
            // Issue #4708: the wait is cancellation-aware -- a cancelled
            // joiner returns the catchable condition instead of sitting on
            // the cv until the target publishes (or the 200s ceiling).
            const auto join_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(200);
            while (!is_ready()) {
                if (fiber_cancel_take(current_fiber_body_id()))
                    return fiber_cancel_condition_value(ev.string_heap_, ev.error_values_,
                                                        ev.primitive_error_counter_ptr());
                if (std::chrono::steady_clock::now() >= join_deadline)
                    break;
                s_fiber_results_cv.wait_for(lock, std::chrono::milliseconds(50));
            }
        }

        // Result is ready (or timed out). Fetch and return.
        std::shared_ptr<std::optional<EvalValue>> result_ptr;
        {
            std::lock_guard<std::mutex> lock(s_fiber_results_mtx);
            auto it = s_fiber_results.find(fid);
            if (it == s_fiber_results.end())
                return make_void();
            if (!it->second.ready || !it->second.value || !it->second.value->has_value()) {
                s_fiber_results.erase(it);
                return make_void();
            }
            result_ptr = std::move(it->second.value);
            s_fiber_results.erase(it);
            // Issue #4708: the fiber is joined -- drop its cancel state.
            fiber_cancel_forget(fid);
        }

        // Issue #3394: join the thread-backend worker now that its body
        // has published (registry miss = scheduler backend, no-op).
        // Extract under the registry mutex, join outside it so a slow
        // body tail cannot hold the registry hostage.
        {
            std::thread worker;
            {
                std::lock_guard<std::mutex> lock(s_thread_fiber_threads_mtx);
                auto it = s_thread_fiber_threads.find(fid);
                if (it != s_thread_fiber_threads.end()) {
                    worker = std::move(it->second.second);
                    s_thread_fiber_threads.erase(it);
                }
            }
            if (worker.joinable())
                worker.join();
        }

        // Issue #164 sub-fix #4: re-validate ev.defuse_version_ after
        // wakeup. While the joiner was blocked (via the proper-
        // blocking path OR the cv-degradation path), mutate:*
        // primitives in OTHER fibers may have incremented
        // ev.defuse_version_ (twice per mutation: once before and
        // once after, with a yield_mutation_boundary between).
        //
        // A mismatch between the version we observed at join start
        // and the version we observe at join wakeup means the
        // workspace mutated during the join — which is the
        // transient inconsistency the issue calls out.
        //
        // The fix: capture the version at the start of the wait,
        // re-check at the end. On mismatch, emit a loud warning
        // (stderr) so the operator sees the inconsistency. We
        // do NOT abort — the result is still valid; the warning
        // is for observability. Future work could surface this
        // as a structured diagnostic.
        //
        // The captured version is updated only when the wait
        // actually happened (i.e., the joiner didn't fast-path
        // the already-done target). For fast-path joins, there's
        // no wait, so no re-validation needed.
        auto dv_now = ev.defuse_version_.load(std::memory_order_acquire);
        if (ev.defuse_version_at_wait_ != dv_now && ev.defuse_version_at_wait_ != 0) {
            std::println(std::cerr,
                         "[fiber:join] WARN: workspace mutated during join "
                         "(defuse_version {} -> {}). The joiner resumed after "
                         "concurrent mutate:* primitives. Result is still valid; "
                         "re-validate callers if they depend on workspace state.",
                         ev.defuse_version_at_wait_, dv_now);
        }
        ev.defuse_version_at_wait_ = 0; // reset for next join

        return std::move(**result_ptr);
    });

    // ── orch:metrics — scheduler metrics (Issue #32) ─────────────
    // (orch:metrics) → Returns a JSON string with scheduler counters.
    // Fields: fibers_spawned, fibers_completed, io_events,
    // steal_attempts, steal_successes, per-worker breakdown.
    // Returns empty string when not in serve-async mode.
    ObservabilityPrims::register_stats_impl("orch:metrics", [&ev](const auto&) -> EvalValue {
        // Issue #1147: wire orch_telemetry counters on the real orch path.
        ev.bump_orch_telemetry();
        if (!aura::messaging::g_get_scheduler_metrics) {
            // Not in serve-async mode — return empty list
            return types::make_void();
        }
        auto json = aura::messaging::g_get_scheduler_metrics();
        ev.bump_orch_telemetry_hit();
        if (!json.empty())
            ev.bump_orch_telemetry_savings();
        auto idx = ev.string_heap_.mint(json);
        return types::make_string(idx);
    });

    // ── scheduler:pin — pin current fiber to a specific worker (P2) ─
    // (scheduler:pin worker-id) → Pins the current fiber to the given
    // worker thread for cache-aware scheduling. The fiber will always
    // run on that worker (won't be stolen). Returns #t on success, #f
    // when not in serve-async mode or invalid worker ID.
    // worker-id: 0..N-1 where N = number of workers.
    add("scheduler:pin", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto wid = static_cast<int>(as_int(a[0]));
        if (wid < 0)
            return make_bool(false);
        if (aura::messaging::g_fiber_set_affinity) {
            aura::messaging::g_fiber_set_affinity(wid);
            return make_bool(true);
        }
        return make_bool(false);
    });

    // ── orch:reset-metrics — reset scheduler counters (Issue #32) ─
    // (orch:reset-metrics) → Resets all metrics to zero.
    // Returns #t when done, #f when not in serve-async mode.
    add("orch:reset-metrics", [&ev](const auto&) -> EvalValue {
        if (aura::messaging::g_reset_scheduler_metrics) {
            aura::messaging::g_reset_scheduler_metrics();
            return types::make_bool(true);
        }
        return types::make_bool(false);
    });

    // (thread_pool:enqueue fn) — Offload a blocking function to the thread pool.
    // fn is a closure taking no arguments.
    // Returns the result on success (may block caller until done).
    // In serve/fiber mode: yields fiber, pool thread wakes it.
    // In stdin mode: uses std::async, blocks synchronously.
    add("thread_pool:enqueue", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_closure(a[0]))
            return make_bool(false);
        auto cid = as_closure_id(a[0]);
        // Serve/fiber mode: use g_thread_pool_enqueue callback
        if (aura::messaging::g_thread_pool_enqueue) {
            auto result_ptr = std::make_shared<std::optional<EvalValue>>();
            aura::messaging::g_thread_pool_enqueue(
                [&ev, cid, result_ptr]() { *result_ptr = ev.apply_closure(cid, {}); }, -1);
            if (aura::messaging::g_fiber_block) {
                aura::messaging::g_fiber_block();
            }
            if (result_ptr && result_ptr->has_value())
                return std::move(**result_ptr);
            return make_void();
        }
        // Stdin mode: use std::async
        auto future =
            std::async(std::launch::async, [&ev, cid]() { return ev.apply_closure(cid, {}); });
        auto result = future.get();
        if (result)
            return *result;
        return make_void();
    });

    // (channel:create [buffer-size]) — Create a new channel
    // Returns channel-id (fixnum) or error.
    // buffer-size defaults to 0 (rendezvous/synchronous).
    add("channel:create", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::size_t buf = 0;
        if (!a.empty() && is_int(a[0])) {
            auto v = as_int(a[0]);
            if (v < 0)
                return make_bool(false);
            buf = static_cast<std::size_t>(v);
        }
        auto ch = std::make_shared<Evaluator::Channel>();
        ch->buffer_size = buf;
        std::lock_guard lk(ev.channels_mtx_);
        auto id = ev.channels_.size();
        ev.channels_.push_back(ch);
        return make_int(static_cast<std::int64_t>(id));
    });

    // (channel:send channel-id msg) — Send a message to a channel
    // Returns #t on success, #f if channel does not exist / closed.
    // Blocks if buffer full (buffered) or until a recv is posted (rendezvous).
    // Issue #2483: buffer_size==0 must NOT short-circuit the wait predicate
    // (that made send non-blocking and let the queue grow unboundedly).
    add("channel:send", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]))
            return make_bool(false);
        // Issue #4494: messages are strings. A non-string used to return #f
        // silently, so the paired channel:recv blocked forever.
        if (!is_string(a[1]))
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        "channel:send: message must be a string",
                                        ev.primitive_error_counter_ptr());
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        auto msg = ev.string_heap_[as_string_idx(a[1])]; // copy before wait
        // Issue #2483: release channels_mtx_ before blocking — holding it
        // across cv.wait deadlocks concurrent channel:recv on the same
        // Evaluator (recv needs the global map lock to look up the channel).
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_bool(false);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        // Issue #4700: under the fiber scheduler the send must park the
        // FIBER, not the worker thread. The condvar wait below freezes the
        // whole worker, so a peer fiber on that worker (the receiver) could
        // never run → deadlock. Park cooperatively instead.
        aura::serve::Fiber* fiber = aura::serve::g_current_fiber;
        if (fiber && channel_can_park_fiber()) {
            ChannelView view{
                &ch, ch.mtx, ch.cv, ch.queue, ch.buffer_size, ch.waiting_receivers, ch.closed};
            bool cancelled = false;
            const bool sent = channel_send_cooperative(view, std::move(msg), fiber, &cancelled);
            if (cancelled)
                return fiber_cancel_condition_value(ev.string_heap_, ev.error_values_,
                                                    ev.primitive_error_counter_ptr());
            return make_bool(sent);
        }
        std::unique_lock ul(ch.mtx);
        ch.cv.wait(ul, [&]() {
            if (ch.closed)
                return true;
            // Buffered: wait for free slot (size never exceeds buffer_size).
            if (ch.buffer_size > 0)
                return ch.queue.size() < ch.buffer_size;
            // Rendezvous (buffer_size==0): wait until a recv is posted and
            // the handoff slot is free (at most one in-flight message).
            return ch.waiting_receivers > 0 && ch.queue.empty();
        });
        if (ch.closed)
            return make_bool(false);
        ch.queue.push_back(std::move(msg));
        ul.unlock();
        ch.cv.notify_all();
        return make_bool(true);
    });

    // (channel:recv channel-id) — Receive a message from a channel
    // Returns the message string, or empty string if channel closed.
    // Blocks until a message is available.
    add("channel:recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_string(0);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_string(0);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        // Issue #4700: under the fiber scheduler the receive must park the
        // FIBER, not the worker thread. The condvar wait below freezes the
        // whole worker, so a peer fiber on that worker (the sender) could
        // never run → deadlock. Park cooperatively instead.
        aura::serve::Fiber* fiber = aura::serve::g_current_fiber;
        if (fiber && channel_can_park_fiber()) {
            ChannelView view{
                &ch, ch.mtx, ch.cv, ch.queue, ch.buffer_size, ch.waiting_receivers, ch.closed};
            bool cancelled = false;
            auto msg = channel_recv_cooperative(view, fiber, &cancelled);
            if (cancelled)
                return fiber_cancel_condition_value(ev.string_heap_, ev.error_values_,
                                                    ev.primitive_error_counter_ptr());
            if (!msg)
                return make_string(0); // channel closed
            auto idx = ev.string_heap_.mint(std::move(*msg));
            return make_string(static_cast<std::uint64_t>(idx));
        }
        std::unique_lock ul(ch.mtx);
        // Issue #2483: announce waiting receiver so rendezvous send can proceed.
        ++ch.waiting_receivers;
        ch.cv.notify_all();
        ch.cv.wait(ul, [&]() { return ch.closed || !ch.queue.empty(); });
        --ch.waiting_receivers;
        if (ch.queue.empty()) {
            ul.unlock();
            ch.cv.notify_all();
            return make_string(0);
        }
        auto msg = std::move(ch.queue.front());
        ch.queue.pop_front();
        ul.unlock();
        ch.cv.notify_all();
        auto idx = ev.string_heap_.mint(std::move(msg));
        return make_string(static_cast<std::uint64_t>(idx));
    });

    // (channel:try-recv channel-id) — Non-blocking receive
    // Returns message string if available, or empty string if no message.
    add("channel:try-recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_string(0);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_string(0);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        std::unique_lock ul(ch.mtx);
        if (ch.queue.empty())
            return make_string(0);
        auto msg = std::move(ch.queue.front());
        ch.queue.pop_front();
        ul.unlock();
        ch.cv.notify_all(); // wake blocked senders (full buffer / next rendezvous)
        // Issue #4700: wake a parked fiber sender — the buffer slot is free.
        channel_wake_waiters(&ch, /*sender=*/true);
        auto idx = ev.string_heap_.mint(std::move(msg));
        return make_string(static_cast<std::uint64_t>(idx));
    });

    // (channel:close channel-id) — Close a channel
    // Wakes all waiters; subsequent recv returns empty string.
    add("channel:close", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto ch_id = static_cast<std::size_t>(as_int(a[0]));
        std::shared_ptr<Evaluator::Channel> ch_ptr;
        {
            std::lock_guard lk(ev.channels_mtx_);
            if (ch_id >= ev.channels_.size() || !ev.channels_[ch_id])
                return make_bool(false);
            ch_ptr = ev.channels_[ch_id];
        }
        auto& ch = *ch_ptr;
        {
            std::lock_guard ul(ch.mtx);
            ch.closed = true;
        }
        ch.cv.notify_all();
        // Issue #4700: wake parked fiber waiters (receivers and senders) so
        // they observe the closed channel instead of parking forever.
        channel_wake_waiters(&ch, /*sender=*/false);
        channel_wake_waiters(&ch, /*sender=*/true);
        return make_bool(true);
    });

    // (_agent:list) — List all active agent sessions (internal primitive)
    // Called by the Aura-level agent:list wrapper.
    add("_agent:list", [&ev](const auto&) -> EvalValue {
        EvalValue result = make_void();
        if (!aura::messaging::g_session_list || !(*aura::messaging::g_session_list))
            return result;
        auto names = (*aura::messaging::g_session_list)();
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            auto sidx = ev.string_heap_.mint(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });
}

} // namespace aura::compiler::primitives_detail

namespace aura::compiler {

// Issue #3394: join this Evaluator's outstanding thread-fiber workers
// before ~Evaluator proceeds to arena/workspace teardown (the fiber:spawn
// fallback captured `this` by reference). Bounded + best-effort, same
// contract as cleanup_orch_agents (#2078): a stuck body (not ready after
// the 2s wait) is detached + WARN + counted so a misbehaving fiber body
// cannot stall Evaluator teardown.
void Evaluator::drain_thread_fibers() noexcept {
    // Registry state from the primitives_detail anonymous namespace.
    using primitives_detail::g_thread_fiber_dtor_abandon_total;
    using primitives_detail::g_thread_fiber_dtor_join_total;
    using primitives_detail::s_fiber_results;
    using primitives_detail::s_fiber_results_cv;
    using primitives_detail::s_fiber_results_mtx;
    using primitives_detail::s_thread_fiber_threads;
    using primitives_detail::s_thread_fiber_threads_mtx;
    try {
        std::vector<std::pair<std::int64_t, std::thread>> mine;
        {
            std::lock_guard<std::mutex> lock(s_thread_fiber_threads_mtx);
            for (auto it = s_thread_fiber_threads.begin(); it != s_thread_fiber_threads.end();) {
                if (it->second.first == static_cast<const void*>(this)) {
                    mine.emplace_back(it->first, std::move(it->second.second));
                    it = s_thread_fiber_threads.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& [fid, worker] : mine) {
            const bool ready = [fid] {
                std::unique_lock<std::mutex> lock(s_fiber_results_mtx);
                return s_fiber_results_cv.wait_for(lock, std::chrono::milliseconds(2000), [fid] {
                    auto it = s_fiber_results.find(fid);
                    return it == s_fiber_results.end() || it->second.ready;
                });
            }();
            if (!worker.joinable())
                continue;
            if (ready) {
                worker.join();
                g_thread_fiber_dtor_join_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                // Degenerate body (infinite loop / blocked >2s): detach so
                // teardown stays bounded. Residual risk limited to this
                // path, surfaced via WARN + abandon counter.
                g_thread_fiber_dtor_abandon_total.fetch_add(1, std::memory_order_relaxed);
                std::println(std::cerr,
                             "[fiber:drain] WARN: thread-fiber {} not ready after "
                             "2000ms at ~Evaluator; detaching (stuck body?)",
                             fid);
                worker.detach();
            }
        }
    } catch (const std::system_error&) {
        // Best-effort: thread join/detach or cv wait failed — never
        // propagate out of ~Evaluator (noexcept).
    } catch (const std::bad_alloc&) {
        // Best-effort: registry growth failed — never propagate (noexcept).
    }
}

} // namespace aura::compiler
