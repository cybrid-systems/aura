// serve/fiber.h — Stackful fiber for async serve
#ifndef AURA_SERVE_FIBER_H
#define AURA_SERVE_FIBER_H

#include <ucontext.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// Issue #438: C-linkage forward declaration of the
// per-thread mutation boundary depth probe. Defined
// in evaluator_fiber_mutation.cpp.
extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
// Issue #2114 / #2188: outermost Guard held flag (weak default 0 in
// fiber_bridge when Evaluator not linked).
extern "C" int aura_evaluator_mutation_boundary_held();
extern "C" std::uint64_t aura_fiber_current_id();

// Issue #2491: TenantScope install / release hooks at fiber resume /
// yield boundary. Strong definitions live in evaluator_fiber_mutation.cpp;
// weak no-op stubs in fiber_bridge.cpp keep non-evaluator link units
// (test_concurrent / test_issue_*) resolving without dragging the full
// module into their link unit.
extern "C" void aura_fiber_install_tenant_scope_for_resume(void* fiber_ptr) noexcept;
extern "C" void aura_fiber_release_tenant_scope_after_yield() noexcept;

// Issue #588: per-fiber mutation stack depth from opaque storage.
// Used by is_at_mutation_boundary_safe() on the victim fiber
// during work-steal (thief thread must not read thread_local).
extern "C" std::size_t aura_evaluator_mutation_stack_depth_from_ptr(void* mutation_stack_storage);

// Issue #2184: process-wide steal snapshot mismatch counter
// (depth/held/yield inconsistent after resume or under steal).
extern "C" std::uint64_t aura_fiber_static_mutation_steal_snapshot_mismatch_total();

// Issue #2310: process-wide force-deopt counter bumped whenever a
// steal snapshot inconsistency triggers fail-closed (force-deopt +
// full refresh under exclusive recovery). Distinct from
// mutation_steal_snapshot_mismatch_total_ which is observed-only —
// this is the actual fail-closed enforcement counter for production.
extern "C" std::uint64_t aura_fiber_static_steal_snapshot_mismatch_force_deopt_total();
// Issue #2346: resume hard-fail total (fail-closed canary).
extern "C" std::uint64_t aura_fiber_static_steal_snapshot_hard_fail_total();
// Issue #2518: resume safety ticket mismatch total (sample seq ≠ current).
extern "C" std::uint64_t aura_fiber_static_steal_safety_ticket_mismatch_total();

// Issue #451: C-linkage shim for Fiber's static GC-pause
// counter (defined in fiber.cpp / fiber_bridge.cpp).
extern "C" std::uint64_t aura_fiber_static_gc_pause_attributed_to_mutation();

// Issue #783: C-linkage shims for the refined work-steal
// metrics (outermost vs inner MutationBoundary split +
// cross-fiber safe steal). All process-wide aggregates
// bumped from WorkerThread::steal() alongside the
// per-Fiber counters. The (query:orchestration-steal-
// outermost-stats) primitive reads these.
extern "C" std::uint64_t aura_fiber_static_steal_outermost_mutation_boundary_total();
extern "C" std::uint64_t aura_fiber_static_steal_inner_mutation_boundary_deferred_total();
extern "C" std::uint64_t aura_fiber_static_cross_fiber_mutation_safe_steal_total();

namespace aura::serve {

// Forward declare (full type in scheduler.h) — used by Fiber owner back-pointer.
struct Scheduler;

// ── Yield reason — why a fiber yielded (Issue #31) ────
// Used by the scheduler to determine if a fiber is at a safe
// point to steal. Only fibers that yielded for Explicit or
// MutationBoundary reasons can be safely stolen — they have
// completed an operation and their state is consistent.
// Fibers in Waiting or BlockingIO have an eventfd pending
// and should not be moved between workers.
enum class YieldReason : uint8_t {
    BlockingIO,        // waiting for external IO (eventfd)
    MutationBoundary,  // yield after completing a mutation/ast:* op
    Explicit,          // explicit yield() call
    SchedulerSteal,    // fiber was stolen by another worker
    OperationBoundary, // yield at sender/receiver boundary (exec adapter)
    PassPipeline,      // yield between incremental pass-pipeline stages (#494)
};

// Issue #2184: atomic MutationSafetySnapshot for steal decisions.
// depth from victim mutation_stack_storage_; held + defuse_version from
// fiber-local mirrors published on Guard enter/exit / checkpoint push
// / resume sync (seqlock — no torn depth/held pair under TSan).
// Contract: live outermost Guard on F's evaluator ⟺ snapshot.held
// (or depth>0 under soft orch agent window) ⇒ steal unsafe.
struct MutationSafetySnapshot {
    std::size_t depth = 0;
    bool held = false;
    std::uint64_t defuse_version = 0;
    YieldReason last_yield = YieldReason::Explicit;
    // Issue #2518: even safety_seq_ at sample time (ticket). Steal captures
    // this; resume must match current even seq or hard-fail (closes the
    // sample→enqueue→steal→resume window after Guard enter/exit).
    std::uint64_t ticket = 0;
};

// ── Fiber state ────────────────────────────────────────
enum class FiberState : uint8_t {
    Ready,   // can be scheduled
    Running, // currently executing on a worker
    Waiting, // waiting for eventfd
    Done,    // completed
};

// Issue #1584: structured Fiber::join result.
enum class JoinStatus : uint8_t {
    Ok = 0,        // target(s) completed
    Timeout = 1,   // deadline elapsed before Done
    Cancelled = 2, // joiner cancel requested
    Invalid = 3,   // null / self-join / missing target
    // Issue #2467: target was force-reclaimed via
    // Scheduler::reap_orphans_now but the body fiber is still
    // executing (non-yielding tight loop). Joiner must NOT free
    // shared resources on this path — the body may still touch
    // them. Cleanup is deferred until Fiber destructor runs after
    // state_==Done. Backward-compatible addition (existing callers
    // pattern-match Ok/Timeout/Cancelled/Invalid won't see change).
    Reclaimed = 4,
};

struct JoinResult {
    JoinStatus status = JoinStatus::Ok;
    std::uint64_t wait_us = 0; // wall time spent waiting
};

// ── Fiber — stackful coroutine with ucontext ───────────
// Each fiber has its own stack (mmap'd with guard page)
// and an eventfd for scheduler wakeup.
class Fiber {
public:
    using Func = std::function<void()>;

    Fiber(Func func, size_t stack_size = 2 * 1024 * 1024);
    ~Fiber();

    // Non-copyable, movable
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;

    // Switch FROM worker TO this fiber.
    // The worker saves its own loop context via thread_local.
    // After the fiber yields, control returns to the worker's loop.
    void resume();

    // Switch FROM this fiber TO the current worker's loop context.
    // Static — uses thread_local g_worker_ctx.
    static void yield();

    // Yield with reason — allows scheduler to make safe-steal decisions.
    // Fibers that yield with BlockingIO will not be stolen; fibers that
    // yield with MutationBoundary or Explicit are safe to steal.
    static void yield(YieldReason reason);

    // Current yield reason (for the scheduler to inspect)
    YieldReason last_yield_reason() const { return last_yield_reason_; }
    void set_yield_reason(YieldReason r) { last_yield_reason_ = r; }

    // Check if a GC safepoint has been requested (P2).
    // If so, block until the GC phase returns to None.
    // Called from yield() and alloc() paths.
    // Implementation in fiber.cpp (to avoid inline issues with C++26 modules).
    static void check_gc_safepoint();

    // Issue #2549: reason-class candidate filter only.
    // True when last_yield is Explicit / MutationBoundary / OperationBoundary /
    // PassPipeline. Does NOT consult MutationSafetySnapshot (depth / held).
    // Steal safety is defined solely by MutationSafetySnapshot; reason class
    // is only a candidate filter. Prefer is_stealable() / is_stealable(snap)
    // for enqueue decisions.
    [[nodiscard]] bool is_steal_candidate() const noexcept {
        auto r = last_yield_reason_.load(std::memory_order_acquire);
        return r == YieldReason::Explicit || r == YieldReason::MutationBoundary ||
               r == YieldReason::OperationBoundary || r == YieldReason::PassPipeline;
    }

    // Issue #2549: candidate filter from a pre-sampled snapshot (zero extra
    // yield-reason load when snap already held on the steal path).
    [[nodiscard]] bool is_steal_candidate(const MutationSafetySnapshot& s) const noexcept {
        return s.last_yield == YieldReason::Explicit ||
               s.last_yield == YieldReason::MutationBoundary ||
               s.last_yield == YieldReason::OperationBoundary ||
               s.last_yield == YieldReason::PassPipeline;
    }

    // Issue #2549: authoritative steal-safe predicate.
    // is_steal_candidate() && is_at_mutation_boundary_safe() — returns false
    // when depth>0 or held under MutationBoundary (matches snapshot contract).
    // Happy path: one reason load + one snapshot sample (AC5: no extra atomics
    // beyond the existing safe probe). Prefer the snap overload when a
    // MutationSafetySnapshot is already held.
    [[nodiscard]] bool is_stealable() const noexcept {
        return is_steal_candidate() && is_at_mutation_boundary_safe();
    }

    // Issue #2549: joint decision from one pre-sampled snapshot (try_steal_from).
    [[nodiscard]] bool is_stealable(const MutationSafetySnapshot& s) const noexcept {
        return is_steal_candidate(s) && is_at_mutation_boundary_safe(s);
    }

    // Issue #2184: one acquire-ordered sample for steal path.
    // depth from victim mutation_stack_storage_ (not thief TLS);
    // held / defuse from fiber-local seqlock mirrors.
    [[nodiscard]] MutationSafetySnapshot mutation_safety_snapshot() const noexcept {
        MutationSafetySnapshot s;
        // Seqlock read of held/defuse mirrors (AC3: no torn pair).
        // Issue #2518: capture even seq as ticket for sample→resume check.
        for (;;) {
            const auto seq1 = safety_seq_.load(std::memory_order_acquire);
            if (seq1 & 1u)
                continue; // writer in progress
            s.held = held_mirror_.load(std::memory_order_relaxed) != 0;
            s.defuse_version = defuse_mirror_.load(std::memory_order_relaxed);
            const auto seq2 = safety_seq_.load(std::memory_order_acquire);
            if (seq1 == seq2) {
                s.ticket = seq1; // even stable seq at sample time
                break;
            }
        }
        s.depth = aura_evaluator_mutation_stack_depth_from_ptr(
            mutation_stack_storage_.load(std::memory_order_acquire));
        s.last_yield = last_yield_reason_.load(std::memory_order_acquire);
        return s;
    }

    // Issue #2518: current even safety_seq_ (one acquire load; spin if odd).
    // Cost: single atomic load on happy (even) path (AC4).
    [[nodiscard]] std::uint64_t current_safety_ticket() const noexcept {
        auto seq = safety_seq_.load(std::memory_order_acquire);
        while (seq & 1u)
            seq = safety_seq_.load(std::memory_order_acquire);
        return seq;
    }

    // Issue #2518: steal success stamps resume ticket from sample snap.
    void set_resume_safety_ticket(std::uint64_t ticket) noexcept {
        resume_safety_ticket_ = ticket;
        has_resume_safety_ticket_ = true;
    }
    void clear_resume_safety_ticket() noexcept {
        has_resume_safety_ticket_ = false;
        resume_safety_ticket_ = 0;
    }
    [[nodiscard]] bool has_resume_safety_ticket() const noexcept {
        return has_resume_safety_ticket_;
    }
    [[nodiscard]] std::uint64_t resume_safety_ticket() const noexcept {
        return resume_safety_ticket_;
    }

    // Issue #2184: publish fiber-visible held/defuse mirrors (Guard
    // enter/exit, checkpoint push/pop, resume sync). Seqlock write.
    void publish_mutation_safety_mirrors(std::size_t depth, bool held,
                                         std::uint64_t defuse_version) noexcept {
        (void)depth; // depth is authoritative from mutation_stack_storage_
        safety_seq_.fetch_add(1, std::memory_order_release); // odd = writing
        held_mirror_.store(held ? 1u : 0u, std::memory_order_relaxed);
        defuse_mirror_.store(defuse_version, std::memory_order_relaxed);
        safety_seq_.fetch_add(1, std::memory_order_release); // even = stable
    }

    // Issue #438 / #588 / #1254 / #2115 / #2118 / #2184: depth-safe mutation
    // boundary probe for work-stealing. Uses MutationSafetySnapshot so
    // depth + held + yield are one logical sample (not separate loads).
    //
    // Returns true if steal is safe at this yield point:
    //   - not an orch agent soft-boundary window with depth>0 / held, AND
    //   - not held (outermost Guard live), AND
    //   - yield reason is not MutationBoundary, OR
    //   - yield is MutationBoundary and depth==0 && !held.
    [[nodiscard]] bool is_at_mutation_boundary_safe() const noexcept {
        return is_at_mutation_boundary_safe(mutation_safety_snapshot());
    }

    // Issue #2184: evaluate safety from a pre-sampled snapshot (try_steal_from
    // samples once and reuses for metrics).
    [[nodiscard]] bool
    is_at_mutation_boundary_safe(const MutationSafetySnapshot& s) const noexcept {
        // Issue #2118: orch agent soft boundary + (depth>0 | held) → never safe.
        // Issue #2515: soft path now publishes held_mirror_ via the same
        // publish_mutation_safety_mirrors the full Guard path uses
        // (orch_soft_boundary_enter/exit in evaluator_fiber_mutation.cpp).
        // Pre-check below stays identical — soft + full agree on the
        // depth/held input.
        if (orch_agent_boundary_active() && (s.depth > 0 || s.held))
            return false;
        // Outermost Guard live (held) → never steal-safe (#2184 contract).
        if (s.held)
            return false;
        if (s.last_yield != YieldReason::MutationBoundary)
            return true;
        // Issue #588 + #1254 + #2115 + #2184: only depth==0 && !held is safe.
        return s.depth == 0 && !s.held;
    }

    // Issue #2184: post-resume / steal invariant — depth>0 implies
    // MutationBoundary yield (or orch agent window). Returns true on mismatch.
    [[nodiscard]] bool
    mutation_safety_snapshot_inconsistent(const MutationSafetySnapshot& s) const noexcept {
        if (s.depth > 0 && s.last_yield != YieldReason::MutationBoundary &&
            !orch_agent_boundary_active())
            return true;
        // held without depth under MB yield is allowed briefly during
        // outermost enter; held with depth==0 after exit is a bug.
        if (s.held && s.depth == 0 && s.last_yield == YieldReason::MutationBoundary)
            return false; // enter path may publish held before stack push
        return false;
    }

    static void bump_mutation_steal_snapshot_mismatch() noexcept {
        mutation_steal_snapshot_mismatch_total_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t mutation_steal_snapshot_mismatch_total() noexcept {
        return mutation_steal_snapshot_mismatch_total_.load(std::memory_order_relaxed);
    }
    // Issue #2310: process-wide force-deopt counter. Bumped by
    // aura_force_deopt_on_steal_snapshot_mismatch (C ABI hook in
    // aura_jit_bridge.cpp / evaluator_fiber_mutation.cpp) on every
    // fail-closed trigger. Distinct from
    // mutation_steal_snapshot_mismatch_total_ — that one is observed-
    // only (soft metric); this one is the actual enforcement counter.
    // See WorkerThread::try_steal_from for the call site.
    static void bump_steal_snapshot_mismatch_force_deopt() noexcept {
        steal_snapshot_mismatch_force_deopt_total_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t steal_snapshot_mismatch_force_deopt_total() noexcept {
        return steal_snapshot_mismatch_force_deopt_total_.load(std::memory_order_relaxed);
    }
    // Issue #2346: resume hard-fail counter (fail-closed canary). Distinct
    // from force-deopt (#2310 steal path) and observed-only mismatch (#2184).
    static void bump_steal_snapshot_hard_fail() noexcept {
        steal_snapshot_hard_fail_total_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t steal_snapshot_hard_fail_total() noexcept {
        return steal_snapshot_hard_fail_total_.load(std::memory_order_relaxed);
    }
    // Issue #2518: resume ticket mismatch (sample seq ≠ current seq).
    static void bump_steal_safety_ticket_mismatch() noexcept {
        steal_safety_ticket_mismatch_total_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t steal_safety_ticket_mismatch_total() noexcept {
        return steal_safety_ticket_mismatch_total_.load(std::memory_order_relaxed);
    }
    // Issue #2346 / #2518: post-sync resume invariant. Samples one snapshot.
    // Soft → bump mismatch, return true (continue). Hard → bump mismatch +
    // hard-fail, mark cancel/Done, return false (no swapcontext).
    // Issue #2518: if resume_safety_ticket_ set (steal path) and current
    // even seq ≠ ticket → treat as inconsistent (Guard enter/exit mid-window).
    // Happy path: one snapshot load (+ ticket compare if set).
    // Tests may call this directly with injected ticket / mirrors.
    [[nodiscard]] bool check_and_enforce_resume_snapshot_invariant() noexcept;

    // Issue #2118: set when orch agent body soft-registers mutation depth
    // (lightweight; not a full MutationBoundaryGuard — fiber stack limit).
    [[nodiscard]] bool orch_agent_boundary_active() const noexcept {
        return orch_agent_boundary_active_.load(std::memory_order_acquire);
    }
    void set_orch_agent_boundary_active(bool v) noexcept {
        orch_agent_boundary_active_.store(v, std::memory_order_release);
    }

    // Issue #448 / #2115 / #2549: alias of depth-safe API. Historical #448
    // implementation always returned false for MutationBoundary yields
    // (no depth probe), so steal callers that used this name were
    // either over-conservative or — if mixed with is_steal_candidate alone —
    // incomplete. Unified to is_at_mutation_boundary_safe() so all
    // steal decision points share one truth. is_stealable() now also
    // requires this probe (reason class alone is only a candidate filter).
    [[nodiscard]] bool is_at_safe_mutation_boundary() const noexcept {
        return is_at_mutation_boundary_safe();
    }

    // Issue #1254 / #2184: true when yielded at MutationBoundary with
    // depth > 0 (inner nested Guard). Uses snapshot for joint sample.
    [[nodiscard]] bool is_at_inner_mutation_boundary() const noexcept {
        return is_at_inner_mutation_boundary(mutation_safety_snapshot());
    }
    [[nodiscard]] bool
    is_at_inner_mutation_boundary(const MutationSafetySnapshot& s) const noexcept {
        return s.last_yield == YieldReason::MutationBoundary && s.depth > 0;
    }

    // Issue #451: yield_classification() — returns a
    // sub-reason string for the current yield. Used
    // by (query:orchestration-metrics) to break down
    // yields by reason + sub-class. The P0 returns the
    // existing YieldReason as a string; the follow-up
    // adds the sub-reason enum (e.g. MutationBoundary
    // + outermost vs MutationBoundary + inner).
    const char* yield_classification() const {
        switch (last_yield_reason_.load(std::memory_order_acquire)) {
            case YieldReason::BlockingIO:
                return "BlockingIO";
            case YieldReason::MutationBoundary:
                return aura_evaluator_mutation_stack_depth_from_ptr(
                           mutation_stack_storage_.load(std::memory_order_acquire)) == 0
                           ? "MutationBoundary/outermost"
                           : "MutationBoundary/inner";
            case YieldReason::Explicit:
                return "Explicit";
            case YieldReason::SchedulerSteal:
                return "SchedulerSteal";
            case YieldReason::OperationBoundary:
                return "OperationBoundary";
            case YieldReason::PassPipeline:
                return "PassPipeline";
        }
        return "Unknown";
    }
    // Issue #451: orchestration observability
    // accessors (read by the (query:orchestration-metrics)
    // primitive via the C-linkage shim or via direct
    // Fiber access). All relaxed-ordering, stats-only.
    [[nodiscard]] std::uint64_t yield_mutation_boundary_count() const noexcept {
        return yield_mutation_boundary_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_explicit_count() const noexcept {
        return yield_explicit_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_scheduler_steal_count() const noexcept {
        return yield_scheduler_steal_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_blocking_io_count() const noexcept {
        return yield_blocking_io_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_operation_boundary_count() const noexcept {
        return yield_operation_boundary_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t steal_success_count() const noexcept {
        return steal_success_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t steal_deferred_mutation_boundary_count() const noexcept {
        return steal_deferred_mutation_boundary_count_.load(std::memory_order_relaxed);
    }
    // Issue #783: refined split (outermost vs inner) for
    // the (query:orchestration-steal-outermost-stats)
    // primitive. Per-Fiber counters; the process-wide
    // aggregate is read via the static C-linkage shim.
    [[nodiscard]] std::uint64_t steal_outermost_mutation_boundary_count() const noexcept {
        return steal_outermost_mutation_boundary_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t steal_inner_mutation_boundary_deferred_count() const noexcept {
        return steal_inner_mutation_boundary_deferred_count_.load(std::memory_order_relaxed);
    }
    // Issue #1492: temporary steal-priority boost after starvation
    // mitigation (inner MutationBoundary defer path). Cleared on
    // successful steal (outermost-safe) so boost is one-shot.
    void apply_steal_priority_boost() noexcept {
        steal_priority_boost_.store(1, std::memory_order_release);
    }
    [[nodiscard]] bool has_steal_priority_boost() const noexcept {
        return steal_priority_boost_.load(std::memory_order_acquire) != 0;
    }
    void clear_steal_priority_boost() noexcept {
        steal_priority_boost_.store(0, std::memory_order_release);
    }
    // Issue #2253: last-hold accessor (microseconds). Set by
    // Scheduler::on_long_mutation_held on outermost Guard dtor.
    [[nodiscard]] std::uint64_t last_hold_us() const noexcept {
        return last_hold_us_.load(std::memory_order_acquire);
    }
    void set_last_hold_us(std::uint64_t us) noexcept {
        last_hold_us_.store(us, std::memory_order_release);
    }
    [[nodiscard]] std::uint64_t cross_fiber_mutation_safe_steal_count() const noexcept {
        return cross_fiber_mutation_safe_steal_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t gc_pause_attributed_to_mutation_count() const noexcept {
        return gc_pause_attributed_to_mutation_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t static_gc_pause_attributed_to_mutation_total() noexcept {
        return static_gc_pause_attributed_to_mutation_count_.load(std::memory_order_relaxed);
    }
    // Issue #783: static aggregate accessors for the
    // refined steal counters. Mirror static_gc_pause_...
    // _total(). Read by the C-linkage shim (fiber.cpp)
    // which is called by the
    // (query:orchestration-steal-outermost-stats)
    // primitive.
    [[nodiscard]] static std::uint64_t static_steal_outermost_mutation_boundary_total() noexcept {
        return static_steal_outermost_mutation_boundary_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t
    static_steal_inner_mutation_boundary_deferred_total() noexcept {
        return static_steal_inner_mutation_boundary_deferred_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t static_cross_fiber_mutation_safe_steal_total() noexcept {
        return static_cross_fiber_mutation_safe_steal_count_.load(std::memory_order_relaxed);
    }
    // Issue #451: bump helpers (called by Fiber::yield()
    // + Fiber::check_gc_safepoint() + the work-steal path
    // follow-up).
    void bump_yield_mutation_boundary() noexcept {
        yield_mutation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
        static_yield_mutation_boundary_total_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #2119: process-wide MB yield count (all fibers).
    [[nodiscard]] static std::uint64_t static_yield_mutation_boundary_total() noexcept {
        return static_yield_mutation_boundary_total_.load(std::memory_order_relaxed);
    }
    // Issue #2200: process-wide rejects of yield under MutationBoundary
    // held/depth (no swapcontext). Agent / stress tests read this.
    [[nodiscard]] static std::uint64_t yield_while_mutation_held_total() noexcept;
    // Issue #2119: timestamp (steady ns) when last MB yield began; 0 if none.
    void note_mutation_boundary_yield_enter_ns(std::uint64_t ns) noexcept {
        mb_yield_enter_ns_.store(ns, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t mutation_boundary_yield_enter_ns() const noexcept {
        return mb_yield_enter_ns_.load(std::memory_order_relaxed);
    }
    void clear_mutation_boundary_yield_enter_ns() noexcept {
        mb_yield_enter_ns_.store(0, std::memory_order_relaxed);
    }
    void bump_yield_explicit() noexcept {
        yield_explicit_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_yield_scheduler_steal() noexcept {
        yield_scheduler_steal_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_yield_blocking_io() noexcept {
        yield_blocking_io_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_yield_operation_boundary() noexcept {
        yield_operation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #1085: PassPipeline has its own counter (not Explicit).
    void bump_yield_pass_pipeline() noexcept {
        yield_pass_pipeline_count_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t yield_pass_pipeline_count() const noexcept {
        return yield_pass_pipeline_count_.load(std::memory_order_relaxed);
    }
    void bump_steal_success() noexcept {
        steal_success_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_steal_deferred_mutation_boundary() noexcept {
        steal_deferred_mutation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #783: bump helpers for the refined split.
    // Each bumps the per-Fiber counter AND the
    // process-wide static aggregate so the primitive
    // can read a global total without walking fibers.
    void bump_steal_outermost_mutation_boundary() noexcept {
        steal_outermost_mutation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
        static_steal_outermost_mutation_boundary_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_steal_inner_mutation_boundary_deferred() noexcept {
        steal_inner_mutation_boundary_deferred_count_.fetch_add(1, std::memory_order_relaxed);
        static_steal_inner_mutation_boundary_deferred_count_.fetch_add(1,
                                                                       std::memory_order_relaxed);
    }
    void bump_cross_fiber_mutation_safe_steal() noexcept {
        cross_fiber_mutation_safe_steal_count_.fetch_add(1, std::memory_order_relaxed);
        static_cross_fiber_mutation_safe_steal_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_pause_attributed_to_mutation() noexcept {
        gc_pause_attributed_to_mutation_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Worker affinity (P2): -1 = any worker, 0..N-1 = specific worker
    int affinity() const { return affinity_; }
    void set_affinity(int worker_id) { affinity_ = worker_id; }

    // Accessors
    uint64_t id() const { return id_; }
    FiberState state() const { return state_.load(std::memory_order_acquire); }
    void set_state(FiberState s) { state_.store(s, std::memory_order_release); }
    int eventfd() const { return eventfd_; }
    // Issue #2467: is_done() now strictly requires state_==Done.
    // Previously returned true for force-reclaimed fibers too
    // (Issue #2227 conflated semantics) — but that let joiners
    // call the cleanup hook (aura_evaluator_on_fiber_join) +
    // release shared resources while the body fiber was STILL
    // EXECUTING on a worker (non-yielding tight loop after the
    // cooperative drain window expired). Use-after-free.
    //
    // New semantics: is_done() means "body has actually finished"
    // (state_==Done). Reclaimed-but-still-running is observable
    // via is_reclaimed(). Fiber::join returns JoinStatus::Reclaimed
    // when the target is reclaimed but state_!=Done (no cleanup
    // hook is called; the joiner must defer cleanup). Cleanup of
    // shared resources is deferred until the Fiber's destructor
    // runs after state_==Done (which only happens when the body
    // actually finishes; for non-yielding bodies this may never
    // happen — accept the leak vs the UAF).
    bool is_done() const { return state_.load(std::memory_order_acquire) == FiberState::Done; }

    // ── Issue #1584: structured join ───────────────────
    // Block the current fiber (or host thread) until `target`
    // reaches Done. Uses Scheduler joiner_map + eventfd when
    // running under the fiber scheduler; host-thread join
    // polls with short sleeps.
    //
    // timeout_ms: nullopt = wait indefinitely; 0 = poll once.
    // Cancel: if the *joiner* has request_cancel(), returns Cancelled.
    [[nodiscard]] static JoinResult join(Fiber* target,
                                         std::optional<std::uint64_t> timeout_ms = std::nullopt);
    // Wait until *all* targets are Done (or first timeout/cancel).
    [[nodiscard]] static JoinResult join(std::span<Fiber* const> targets,
                                         std::optional<std::uint64_t> timeout_ms = std::nullopt);

    // Issue #2498: epoch-scoped off-stack orphan-root table. Body code
    // (Fiber::resume context) can register a drop callback for any
    // global table entry it added (EnvFrame ref, mailbox ref, external
    // handle) so that on hard-reclaim (JoinStatus::Reclaimed path) the
    // global entries are released without touching the body's running
    // stack. Body stack copies remain valid; the off-stack global table
    // is the only thing leaked by design. Idempotent: clear + invoke
    // is a no-op on subsequent calls. Thread-safe: registrations from
    // the body's worker thread; release from the joiner thread.
    void register_orphan_root_release(std::function<void()> drop) noexcept;
    // Returns the number of callbacks invoked (for AC1 metrics).
    std::size_t release_orphan_roots() noexcept;
    // True iff the table has any pending callbacks (for tests).
    [[nodiscard]] bool has_orphan_roots() const noexcept;

    // Issue #2498: process-wide counter for orphan-root drops on
    // Reclaimed (counts callbacks invoked when JoinStatus::Reclaimed
    // returns to a joiner). Accessor for tests + query surface.
    [[nodiscard]] static std::uint64_t orphan_roots_dropped_on_reclaim_total() noexcept;
    // Process-wide HWM of pending orphan roots across all fibers
    // (production observability for AC1 root count bound).
    [[nodiscard]] static std::uint64_t orphan_roots_hwm() noexcept;

    // Cooperative cancellation. Target fibers may poll this
    // flag and exit early; joiners observe Cancelled when their
    // own cancel is set during wait.
    void request_cancel() noexcept { cancel_requested_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_cancel_requested() const noexcept {
        return cancel_requested_.load(std::memory_order_acquire);
    }
    // Issue #2533: force cooperative safepoint after hard-reclaim so non-yield
    // bodies still hit a bound edge (check_gc_safepoint / yield) and retire
    // still_running. Ok join / cooperative yield pays zero extra (flag unset).
    void request_force_safepoint() noexcept {
        force_safepoint_requested_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool is_force_safepoint_requested() const noexcept {
        return force_safepoint_requested_.load(std::memory_order_acquire);
    }
    // Process-wide metrics (#2533).
    [[nodiscard]] static std::uint64_t residual_force_safepoint_total() noexcept;
    [[nodiscard]] static std::uint64_t residual_cpu_budget_exceeded_total() noexcept;
    // Issue #2227: owner Scheduler back-pointer accessor. Returns
    // nullptr for fibers created outside a Scheduler (test / host
    // thread / static Fibers); the orch join path skips the
    // hard-reclaim orphan path when this is null.
    [[nodiscard]] Scheduler* owner_sched() const noexcept { return owner_sched_; }
    void set_owner_sched(Scheduler* s) noexcept { owner_sched_ = s; }
    // Issue #2227: hard-reclaim flag. Set by Scheduler::reap_orphans_now
    // when the fiber's hard_deadline has passed and !is_done(). Once
    // set, is_done() still returns the body-truth state, but the
    // scheduler treats the fiber as "logically done" (removed from
    // wait_map_ / joiner_map_ / owned_fibers_). Bodies that yield
    // post-reclaim are NOT re-dispatched; #2533 request_force_safepoint
    // + cancel nudge residual bodies to cooperative edges so still_running
    // converges (true preemption remains out of scope).
    [[nodiscard]] bool is_reclaimed() const noexcept {
        return reclaimed_.load(std::memory_order_acquire);
    }
    // Issue #2227 / #2397: set reclaimed_ (idempotent). When the body
    // has not yet returned (state_!=Done), bumps process-wide
    // still-running gauge so operators can distinguish logical
    // reclaim from body still burning CPU/stack. Body exit (or
    // Fiber dtor if abandoned) pairs the gauge.
    void mark_reclaimed() noexcept;
    // Issue #2397: body returned after reclaimed — still-running −1,
    // body-retired +1. Safe to call when not reclaimed (no-op).
    void note_body_exit_if_reclaimed() noexcept;

    // Process-wide join metrics (#1584 / #1595).
    [[nodiscard]] static std::uint64_t join_total() noexcept;
    [[nodiscard]] static std::uint64_t join_timeout_total() noexcept;
    [[nodiscard]] static std::uint64_t join_cancel_total() noexcept;
    // Issue #2467: counter for JoinStatus::Reclaimed returns
    // (target force-reclaimed but body still executing).
    [[nodiscard]] static std::uint64_t join_reclaim_total() noexcept;
    // Issue #2397: reclaimed-but-body-not-returned gauge + retired counter.
    [[nodiscard]] static std::uint64_t join_drain_residual_still_running() noexcept;
    [[nodiscard]] static std::uint64_t join_drain_residual_body_retired_total() noexcept;
    // Issue #2533 accessors declared with request_force_safepoint above.
    [[nodiscard]] static std::uint64_t join_wait_us_total() noexcept;
    [[nodiscard]] static std::uint64_t join_wait_us_max() noexcept;
    // Issue #1595: times join Ok path invoked linear/StableNodeRef enforcement.
    [[nodiscard]] static std::uint64_t join_linear_enforcement_total() noexcept;
    // Issue #1597: coarse join latency histogram (5 buckets, µs edges:
    //   [0]=<100, [1]=<1k, [2]=<10k, [3]=<100k, [4]≥100k).
    static constexpr std::size_t kJoinLatencyHistBuckets = 5;
    [[nodiscard]] static std::uint64_t join_latency_hist(std::size_t bucket) noexcept;
    [[nodiscard]] static std::uint64_t join_latency_hist_sum() noexcept;

    // Issue #213 Cycle 3: per-fiber mutation stack. The
    // Evaluator's enter/exit_mutation_boundary reads/writes
    // this stack (via active_mutation_stack()) instead of a
    // thread_local, so a fiber that migrates between threads
    // brings its stack with it.
    //
    // Type: opaque void* to avoid the circular dep between
    // fiber.h and evaluator.ixx. The Evaluator casts it to
    // `std::vector<MutationCheckpoint>*` and operates on
    // it via the pointer.
    // Issue #1992: getters/setters use atomic load/store
    // (acquire/release). The underlying storage is
    // std::atomic<void*> — see private fields below.
    // Plain read/write would be a data race on
    // concurrent ensure_mutation_stack_ptr init during
    // work-stealing handoff (last-writer wins, one
    // pointer leaks, plus use-after-free risk if the
    // fiber resumes on a stack another fiber still
    // holds a reference to).
    void* mutation_stack_ptr() const noexcept {
        return mutation_stack_storage_.load(std::memory_order_acquire);
    }
    void set_mutation_stack_ptr(void* p) noexcept {
        mutation_stack_storage_.store(p, std::memory_order_release);
    }
    // Issue #1992: CAS for concurrent init. Returns
    // true if the exchange succeeded (expected was
    // nullptr, storage now holds desired). On false,
    // `expected` is updated to the current value so the
    // caller can release its allocation back to the pool
    // and use the winner's pointer.
    bool compare_exchange_mutation_stack_ptr(void*& expected, void* desired) noexcept {
        return mutation_stack_storage_.compare_exchange_weak(
            expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    }
    // Issue #264: per-fiber yield-boundary checkpoint stack.
    void* yield_checkpoint_ptr() const noexcept {
        return yield_checkpoint_storage_.load(std::memory_order_acquire);
    }
    void set_yield_checkpoint_ptr(void* p) noexcept {
        yield_checkpoint_storage_.store(p, std::memory_order_release);
    }
    // Issue #1992: CAS for yield checkpoint stack init.
    bool compare_exchange_yield_checkpoint_ptr(void*& expected, void* desired) noexcept {
        return yield_checkpoint_storage_.compare_exchange_weak(
            expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Issue #1580: steal/resume provenance hints captured at yield so
    // post-resume refresh_stale_frames_after_steal can target the
    // fiber's active EnvFrame / bridge_epoch without a full scan.
    void set_resume_refresh_hints(std::uint64_t env_id, std::uint64_t bridge_epoch) noexcept {
        resume_env_hint_ = env_id;
        resume_bridge_epoch_hint_ = bridge_epoch;
    }
    [[nodiscard]] std::uint64_t resume_env_hint() const noexcept { return resume_env_hint_; }
    [[nodiscard]] std::uint64_t resume_bridge_epoch_hint() const noexcept {
        return resume_bridge_epoch_hint_;
    }
    void clear_resume_refresh_hints() noexcept {
        resume_env_hint_ = 0;
        resume_bridge_epoch_hint_ = 0;
    }
    // Issue #2250: LayoutStamp fence captured at outermost Guard Phase 5
    // exit. On Fiber::resume / refresh_stale_frames_after_steal, the
    // stored stamp is hard-compared vs Evaluator::current_layout_stamp();
    // any mismatch -> bump layout_stamp_resume_mismatch_total + force
    // dual-check (must not execute generation-behind AOT native code).
    void set_resume_layout_stamp(std::uint64_t arena_id, std::uint64_t arena_gen,
                                 std::uint64_t flat_gen, std::uint64_t mutation_epoch,
                                 std::uint64_t env_gen, std::uint64_t defuse,
                                 std::uint64_t shape_version,
                                 std::uint64_t ir_soa_generation = 0) noexcept {
        resume_arena_id_ = arena_id;
        resume_arena_gen_ = arena_gen;
        resume_flat_gen_ = flat_gen;
        resume_mutation_epoch_ = mutation_epoch;
        resume_env_gen_ = env_gen;
        resume_defuse_ = defuse;
        resume_shape_version_ = shape_version;
        resume_ir_soa_generation_ = ir_soa_generation;
        resume_layout_stamp_set_ = 1;
    }
    [[nodiscard]] std::uint64_t resume_arena_id() const noexcept { return resume_arena_id_; }
    [[nodiscard]] std::uint64_t resume_arena_gen() const noexcept { return resume_arena_gen_; }
    [[nodiscard]] std::uint64_t resume_flat_gen() const noexcept { return resume_flat_gen_; }
    [[nodiscard]] std::uint64_t resume_mutation_epoch() const noexcept {
        return resume_mutation_epoch_;
    }
    [[nodiscard]] std::uint64_t resume_env_gen() const noexcept { return resume_env_gen_; }
    [[nodiscard]] std::uint64_t resume_defuse() const noexcept { return resume_defuse_; }
    [[nodiscard]] std::uint64_t resume_shape_version() const noexcept {
        return resume_shape_version_;
    }
    // Issue #2432: 8th LayoutStamp field (IR SoA generation fence).
    [[nodiscard]] std::uint64_t resume_ir_soa_generation() const noexcept {
        return resume_ir_soa_generation_;
    }
    [[nodiscard]] bool has_resume_layout_stamp() const noexcept {
        return resume_layout_stamp_set_ != 0;
    }
    void clear_resume_layout_stamp() noexcept {
        resume_arena_id_ = 0;
        resume_arena_gen_ = 0;
        resume_flat_gen_ = 0;
        resume_mutation_epoch_ = 0;
        resume_env_gen_ = 0;
        resume_defuse_ = 0;
        resume_shape_version_ = 0;
        resume_ir_soa_generation_ = 0;
        resume_layout_stamp_set_ = 0;
    }

    // Issue #2491: assigned_tenant_id accessors (atomic for the
    // orch path that stamps at spawn + the resume hook that reads).
    void set_assigned_tenant_id(std::uint64_t t) noexcept {
        assigned_tenant_id_.store(t, std::memory_order_release);
    }
    [[nodiscard]] std::uint64_t assigned_tenant_id() const noexcept {
        return assigned_tenant_id_.load(std::memory_order_acquire);
    }
    // Issue #2491: process-wide TenantScope mismatch counter.
    static void bump_tenant_scope_mismatch() noexcept {
        static_tenant_scope_mismatch_total_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] static std::uint64_t tenant_scope_mismatch_total() noexcept {
        return static_tenant_scope_mismatch_total_.load(std::memory_order_relaxed);
    }

private:
    uint64_t id_;
    std::atomic<FiberState> state_{FiberState::Ready};
    std::atomic<YieldReason> last_yield_reason_{YieldReason::Explicit};

    // Issue #451: orchestration observability counters
    // (lifetime atomics, per-Fiber — the follow-up can
    // aggregate to GlobalMetrics for cross-fiber view).
    // Bumped in Fiber::yield() + Fiber::check_gc_safepoint
    // + the work-steal path (follow-up).
    //   yield_mutation_boundary_count_  (lifetime # of
    //     yields with reason == MutationBoundary)
    //   yield_explicit_count_  (lifetime # of yields
    //     with reason == Explicit)
    //   yield_scheduler_steal_count_  (lifetime # of
    //     yields with reason == SchedulerSteal)
    //   yield_blocking_io_count_  (lifetime # of yields
    //     with reason == BlockingIO)
    //   yield_operation_boundary_count_  (lifetime # of
    //     yields with reason == OperationBoundary)
    //   steal_success_count_  (lifetime # of successful
    //     work-steal attempts)
    //   steal_deferred_mutation_boundary_count_  (lifetime
    //     # of steal attempts deferred because the victim
    //     held an outermost MutationBoundary)
    //   gc_pause_attributed_to_mutation_count_  (lifetime
    //     # of GC safepoints where the wait was attributed
    //     to an active MutationBoundary)
    // All stats-only (relaxed-ordering). Exposed via
    // (query:orchestration-metrics).
    std::atomic<std::uint64_t> yield_mutation_boundary_count_{0};
    std::atomic<std::uint64_t> yield_explicit_count_{0};
    std::atomic<std::uint64_t> yield_pass_pipeline_count_{0}; // Issue #1085
    std::atomic<std::uint64_t> yield_scheduler_steal_count_{0};
    std::atomic<std::uint64_t> yield_blocking_io_count_{0};
    std::atomic<std::uint64_t> yield_operation_boundary_count_{0};
    std::atomic<std::uint64_t> steal_success_count_{0};
    std::atomic<std::uint64_t> steal_deferred_mutation_boundary_count_{0};
    // Issue #783: refined steal counters (outermost
    // vs inner) — see bump helpers above.
    std::atomic<std::uint64_t> steal_outermost_mutation_boundary_count_{0};
    std::atomic<std::uint64_t> steal_inner_mutation_boundary_deferred_count_{0};
    std::atomic<std::uint64_t> cross_fiber_mutation_safe_steal_count_{0};
    std::atomic<std::uint64_t> gc_pause_attributed_to_mutation_count_{0};
    // Issue #1492: 1 when apply_starvation_mitigation raised steal priority.
    std::atomic<std::uint32_t> steal_priority_boost_{0};
    // Issue #2253: last-hold hint for hold-aware steal scoring. Set
    // by Scheduler::on_long_mutation_held whenever a fiber releases
    // an outermost Guard (the duration of the outermost hold).
    // Read at the steal site to deprioritize long-hold victims when
    // p90 > 100 ms (per AC1 score -40 penalty).
    std::atomic<std::uint64_t> last_hold_us_{0};
    int affinity_ = -1; // -1 = any worker, [0,N) = pinned to specific worker
    ucontext_t ctx_;
    void* stack_ = nullptr;
    size_t stack_size_ = 0;
    int eventfd_ = -1;
    Func func_;

    static std::atomic<uint64_t> next_id_;

    // Issue #451: per-Fiber orchestration counters are
    // not appropriate for static-method counters (e.g.
    // gc_pause_attributed_to_mutation is called from
    // the static Fiber::check_gc_safepoint()). Use a
    // static atomic for cross-fiber aggregates. The
    // (query:orchestration-metrics) primitive reads
    // the static aggregate + sums the per-Fiber counters
    // for a process-wide total.
    static std::atomic<std::uint64_t> static_gc_pause_attributed_to_mutation_count_;
    // Issue #783: static aggregates for the refined
    // steal metrics (mirror static_gc_pause_..._ count_).
    static std::atomic<std::uint64_t> static_steal_outermost_mutation_boundary_count_;
    static std::atomic<std::uint64_t> static_steal_inner_mutation_boundary_deferred_count_;
    static std::atomic<std::uint64_t> static_cross_fiber_mutation_safe_steal_count_;
    // Issue #2119: process-wide MutationBoundary yield count.
    static std::atomic<std::uint64_t> static_yield_mutation_boundary_total_;

    // Trampoline: called when fiber starts
    static void trampoline(uint32_t high, uint32_t low);

    // Per-fiber state: the mutation stack (Issue #213 Cycle 3).
    // Opaque void* — see mutation_stack_ptr() / set_mutation_stack_ptr().
    //
    // Issue #1992: storage is std::atomic<void*> so that
    // concurrent ensure_mutation_stack_ptr (during work-steal
    // handoff, see evaluator_fiber_mutation.cpp) can use
    // compare_exchange_weak to avoid last-writer-wins / leaks /
    // cross-fiber use-after-free. Plain void* was UB under
    // concurrent read+write. Atomic load/store on getters +
    // compare_exchange on ensure_*_ptr are the only access
    // patterns allowed.
    std::atomic<void*> mutation_stack_storage_{nullptr};
    std::atomic<void*> yield_checkpoint_storage_{nullptr};
    // Issue #1580: captured at MutationBoundary yield for post-resume refresh.
    std::uint64_t resume_env_hint_ = 0;
    std::uint64_t resume_bridge_epoch_hint_ = 0;
    // Issue #2250: LayoutStamp fence captured at outermost Guard Phase 5
    // exit (before unlock). 6-field POD + set flag. Compared at Fiber::resume
    // / refresh_stale_frames_after_steal; mismatch -> hard fence path.
    std::uint64_t resume_arena_id_ = 0;
    std::uint64_t resume_arena_gen_ = 0;
    std::uint64_t resume_flat_gen_ = 0;
    std::uint64_t resume_mutation_epoch_ = 0;
    std::uint64_t resume_env_gen_ = 0;
    std::uint64_t resume_defuse_ = 0;
    // Issue #2255: ShapeProfiler monotonic generation captured at
    // outermost Guard Phase 5 exit (before unlock). Read at resume
    // / refresh_stale_frames_after_steal; mismatch on top 6 fields
    // OR this field forces scan_live_closures_for_linear_captures +
    // bumps shape_version_fence_reject_total.
    std::uint64_t resume_shape_version_ = 0;
    std::uint64_t resume_ir_soa_generation_ = 0; // Issue #2432
    std::uint32_t resume_layout_stamp_set_ = 0;
    // Issue #1584: cooperative cancel flag.
    std::atomic<bool> cancel_requested_{false};
    // Issue #2118: orch agent body soft mutation-boundary window active
    // (per-fiber stack depth registered; steal/GC visibility).
    std::atomic<bool> orch_agent_boundary_active_{false};
    // Issue #2184: fiber-local MutationSafetySnapshot mirrors (seqlock).
    // safety_seq_ even = stable, odd = writer updating held/defuse.
    mutable std::atomic<std::uint64_t> safety_seq_{0};
    std::atomic<std::uint32_t> held_mirror_{0};
    std::atomic<std::uint64_t> defuse_mirror_{0};
    // Issue #2518: steal-sample ticket for resume check (one-shot).
    // Set on successful steal; consumed (cleared) in resume invariant.
    // Independent of LayoutStamp restamp (#2510) — no dual-compute conflict.
    std::uint64_t resume_safety_ticket_ = 0;
    bool has_resume_safety_ticket_ = false;
    static std::atomic<std::uint64_t> mutation_steal_snapshot_mismatch_total_;
    // Issue #2310: see bump_steal_snapshot_mismatch_force_deopt().
    // Distinct from mutation_steal_snapshot_mismatch_total_ (observed-only).
    static std::atomic<std::uint64_t> steal_snapshot_mismatch_force_deopt_total_;
    // Issue #2346: resume hard-fail (mark-failed) total.
    static std::atomic<std::uint64_t> steal_snapshot_hard_fail_total_;
    // Issue #2518: ticket mismatch total (sample seq ≠ current seq at resume).
    static std::atomic<std::uint64_t> steal_safety_ticket_mismatch_total_;
    // Issue #2119: steady-clock ns at last MutationBoundary yield enter.
    std::atomic<std::uint64_t> mb_yield_enter_ns_{0};
    // Issue #2227: back-pointer to owner Scheduler so the orch join
    // path can register hard-reclaim orphans without going through
    // a global lookup. Set by Scheduler::spawn; nullptr for
    // out-of-scheduler fibers (test / host-thread / etc.).
    Scheduler* owner_sched_ = nullptr;
    // Issue #2227: reclaimed flag — set by Scheduler::reap_orphans_now
    // when the fiber's hard_deadline has passed and !is_done(). The
    // body may still be running (non-yielding tight loop); the flag
    // is a hint to the joiner / metric observer that this fiber has
    // been force-reclaimed and is "logically done" from the
    // scheduler's perspective. Bodies that don't poll is_reclaimed()
    // (or yield) will continue to consume stack until they return —
    // documented limitation, same as #2153 cooperative cancel
    // protocol.
    std::atomic<bool> reclaimed_{false};
    // Issue #2533: cooperative force-safepoint after hard-reclaim.
    std::atomic<bool> force_safepoint_requested_{false};
    // Issue #2491: fiber-local assigned tenant id. Stamped at
    // spawn time by the orch / agent path; Fiber::resume re-applies
    // TenantScope from this value (not from ambient Evaluator state)
    // so a stolen / resumed fiber cannot silently keep another
    // tenant's principal. Default 0 = "no assigned tenant" — the
    // resume hook skips TenantScope installation in that case (unit
    // / Soft path stays unchanged per AC5).
    std::atomic<std::uint64_t> assigned_tenant_id_{0};
    // Issue #2491: process-wide counter for TenantScope install
    // mismatch (resume detects current capability_tenant_id_ !=
    // assigned_tenant_id_). Mirrors Fiber::static_*_total() pattern
    // for process-wide aggregates. Accessors are public above.
    static std::atomic<std::uint64_t> static_tenant_scope_mismatch_total_;
    // Issue #2397: true iff this fiber contributed +1 to the
    // still-running gauge (mark_reclaimed while !Done). Cleared by
    // note_body_exit_if_reclaimed or ~Fiber (abandon without retired).
    std::atomic<bool> still_running_after_reclaim_counted_{false};

    // Issue #2498: per-fiber off-stack orphan-root table. Protected by
    // orphan_roots_mtx_ — registrations come from the body's worker thread
    // (Fiber::resume context), release calls come from the joiner thread
    // (or from ~Fiber on the destroying thread). Both sides take the mutex;
    // release invokes callbacks OUTSIDE the lock to keep the critical
    // section short and avoid re-entrant lock cycles (drop callbacks may
    // acquire other locks — Evaluator mutex, mailbox mutex, GC root table).
    // The vector is on the heap so the Fiber object stays small and
    // moveable; std::function captures (this, slot) for the unregister
    // callback are safe because the Evaluator outlives all fibers in
    // production (scheduler teardown ensures fibers stop first).
    mutable std::mutex orphan_roots_mtx_;
    std::vector<std::function<void()>> orphan_root_releases_;

    // Issue #1584 / #1595 join metrics (process-wide).
    static std::atomic<std::uint64_t> join_total_;
    static std::atomic<std::uint64_t> join_timeout_total_;
    static std::atomic<std::uint64_t> join_cancel_total_;
    // Issue #2467: counter for JoinStatus::Reclaimed returns.
    static std::atomic<std::uint64_t> join_reclaim_total_;
    // Issue #2397: process-wide still-running gauge + body-retired counter.
    static std::atomic<std::uint64_t> join_drain_residual_still_running_;
    static std::atomic<std::uint64_t> join_drain_residual_body_retired_total_;
    static std::atomic<std::uint64_t> join_wait_us_total_;
    static std::atomic<std::uint64_t> join_wait_us_max_;
    static std::atomic<std::uint64_t> join_linear_enforcement_total_;
    // Issue #2498: process-wide orphan-root drops + HWM.
    static std::atomic<std::uint64_t> orphan_roots_dropped_on_reclaim_total_;
    static std::atomic<std::uint64_t> orphan_roots_hwm_;
    // Issue #2533: force-safepoint / residual CPU budget metrics.
    static std::atomic<std::uint64_t> residual_force_safepoint_total_;
    static std::atomic<std::uint64_t> residual_cpu_budget_exceeded_total_;
    // Issue #1597 join latency histogram (process-wide).
    static std::atomic<std::uint64_t> join_latency_hist_[kJoinLatencyHistBuckets];
};

// Issue #213 Cycle 3: function pointers that the Evaluator
// registers at startup, to avoid the circular include between
// fiber.h and evaluator.ixx. The setter is called by
// Fiber::resume() to update the Evaluator's thread_local
// "current fiber" pointer. The deleter is called by
// ~Fiber() to free the per-fiber storage owned by the
// Evaluator. Both are void(void*) and void*(Fiber*) —
// the function signatures are minimal so the fiber side
// doesn't need to know about Evaluator internals.
extern void* (*g_fiber_setter_)(void*);
// Issue #588: sync per-fiber mutation stack on resume.
extern void (*g_fiber_sync_mutation_stack_)(void*);
extern void (*g_fiber_storage_deleter_)(void*);
// Issue #264: yield-boundary checkpoint hooks (registered by
// evaluator_fiber_mutation.cpp). Called before yield swapcontext
// and after resume swapcontext returns.
extern void (*g_fiber_yield_checkpoint_)(uint8_t reason);
extern void (*g_fiber_resume_validate_)();
extern void (*g_fiber_yield_checkpoint_deleter_)(void*);

// Issue #618: GC safepoint frequency tuning. The
// (orchestration:tune-gc-frequency ratio) primitive writes
// a 0..100 ratio here; the scheduler can opt-in to consult
// it when deciding whether to trigger a safepoint on the
// next allocation. P0 ships write/read/return; the actual
// scheduler-side consult is a separate follow-up.
//
// 0   = never safepoint (debug-only; will hurt tail latency
//       in production multi-agent workloads)
// 100 = safepoint on every allocation (very conservative;
//       maximizes GC responsiveness at the cost of throughput)
// 50  = the default (P0 ships this as the initial value;
//       matches the historical "every Nth allocation" heuristic)
extern std::atomic<std::uint32_t>& gc_frequency_tune_ratio() noexcept;

// ── GCPhase — GC safepoint state machine (P2) ────────
enum class GCPhase : uint8_t {
    None,      // 正常执行
    Requested, // GC 已请求，等待 fiber 到达安全点
    Sweeping,  // 同步 sweep 进行中
    Complete,  // GC 完成
};

// ── WorkerGCState — per-worker GC state (P2) ──────────
struct WorkerGCState {
    std::atomic<GCPhase> phase{GCPhase::None};
    std::atomic<int32_t> fibers_at_safepoint{0};
    std::atomic<int64_t> gc_epoch{0};

    // Issue #115: count of fibers currently executing on this
    // worker (i.e., the worker is inside fiber->resume() and
    // hasn't returned yet). A fiber that's actively running
    // holds the worker's stack with live references to the
    // heap; the GC must wait for it to either yield or complete
    // before proceeding, otherwise those stack references would
    // be missed during root collection.
    //
    // The fiber's own `check_gc_safepoint` increments
    // `fibers_at_safepoint` when it next yields/allocates.
    // The running-fiber counter is incremented by the worker
    // just before `fiber->resume()` and decremented after.
    // `Scheduler::wait_for_safepoint` considers the worker
    // quiescent only when BOTH counters are zero (or the
    // worker has no fibers at all).
    std::atomic<int32_t> running_fiber_count{0};

    // Issue #1256: safepoint wait latency under MutationBoundary hold.
    std::atomic<std::int64_t> eventfd_wakeup_latency_us{0};
    std::atomic<std::int64_t> safepoint_wait_while_mutation_held{0};
    std::atomic<std::int64_t> safepoint_blocked_by_long_mutation{0};

    // Spin-wait until phase returns to None (safepoint resume)
    void wait_for_resume() {
        while (phase.load(std::memory_order_acquire) != GCPhase::None) {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#else
            asm volatile("" ::: "memory");
#endif
        }
    }
};

// ── Worker context (thread-local) ─────────────────────
// Each WorkerThread sets this before running fibers.
// Fiber::yield() swaps back to this context.
// Fiber::resume() swaps from this context to the fiber.
struct WorkerContext {
    ucontext_t uctx;                   // worker's dispatch loop context
    WorkerGCState* gc_state = nullptr; // set by worker thread (P2)
};
extern thread_local WorkerContext* g_worker_ctx;

// ── Global scheduler reference ─────────────────────────
// Set during --serve-async init. Used for fiber spawn.
struct Scheduler;
extern Scheduler* g_scheduler;
extern thread_local Fiber* g_current_fiber;

// Issue #2310 / #2346 / #2372: AURA_STEAL_SNAPSHOT_SOFT=1 keeps metric-only
// mode for unit tests. Production default is fail-closed (force-deopt + full
// refresh under exclusive recovery). Under production security defaults
// (apply_production_security_defaults, sandbox != off) Soft env is IGNORED
// (production lock — mirror #2338 gc_defer). Test override wins over lock
// (set_steal_snapshot_soft_for_test) for unit Soft-path ergonomics.
// Happy path cost: one relaxed load of the production-locked flag, then
// getenv only when unlocked. Only consulted on the rare mismatch path.
//
// Decision table (steal Soft + resume Hard):
//
// | Mode / lock | Trigger | Soft (steal force-deopt) | Resume Hard |
// | Soft env    | AURA_STEAL_SNAPSHOT_SOFT=1 && !production_locked | metric-only continue |
// overrides Hard | | Soft test   | set_steal_snapshot_soft_for_test(true) | metric-only continue |
// overrides Hard | | Production lock | set_steal_snapshot_soft_production_locked(true) | Soft env
// ignored | Hard canary | | Soft default | neither HARD nor production canary | (not Soft) | Soft
// continue | | Hard env    | AURA_STEAL_SNAPSHOT_HARD=1 && !Soft | force-deopt | mark Done/cancel |
// | Production canary | production_defaults probe && !Soft | force-deopt | same as Hard |
// | Hard+abort  | AURA_STEAL_SNAPSHOT_HARD_ABORT=1 under Hard | force-deopt | std::abort |
// | Missing ABI | production_locked && force-deopt symbol null/weak-noop | std::abort (#2372) | n/a
// |
//
// Steal path (#2310 force-deopt / #2372 production ABI) is separate from
// Fiber::resume hard-invariant (#2346). Implementation in fiber.cpp.
[[nodiscard]] bool is_steal_snapshot_soft_mode() noexcept;
// Issue #2372: production Soft lock (set by apply_production_security_defaults).
void set_steal_snapshot_soft_production_locked(bool v) noexcept;
[[nodiscard]] bool steal_snapshot_soft_production_locked() noexcept;
// Issue #2372: test override — when set Soft, Soft wins over production lock
// (mirror set_gc_defer_overflow_policy_for_test). reset clears override.
void set_steal_snapshot_soft_for_test(bool soft) noexcept;
void reset_steal_snapshot_soft_for_test() noexcept;

// Issue #2346: resume MutationSafetySnapshot hard-invariant (see table above).
// Implementation in fiber.cpp (production canary via typed_mutation_audit).
[[nodiscard]] bool is_steal_snapshot_hard_mode() noexcept;
[[nodiscard]] bool is_steal_snapshot_hard_abort() noexcept;

} // namespace aura::serve

#endif // AURA_SERVE_FIBER_H
