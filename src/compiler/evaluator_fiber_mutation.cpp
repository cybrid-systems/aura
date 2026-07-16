// evaluator_fiber_mutation.cpp — P1-l: per-fiber mutation stack + boundary hooks
// aura.compiler.evaluator module partition.

module;

#include "messaging_bridge.h"
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "observability_metrics.h"
#include <cassert>

module aura.compiler.evaluator;

import std;

extern "C" {
bool aura_aot_probe_checkpoint_version(std::uint64_t defuse_version, std::uint64_t bridge_epoch);
void aura_aot_record_deopt_on_steal();
}

namespace aura::compiler {

namespace fiber_stack_pool_detail {

    using MutationVec = Evaluator::MutationCheckpoint;
    using YieldVec = Evaluator::YieldBoundaryCheckpoint;

    inline auto& pool_stats() {
        return aura::serve::metrics::per_fiber_stack_pool_stats();
    }

    // Issue #652 / #707: bounded scheduler-local vector pool for per-fiber stacks.
    template <typename T> class BoundedVectorPool {
    public:
        static constexpr std::size_t kInitialCapacity = 12;
        static constexpr std::size_t kGrowthThreshold = 32;
        static constexpr std::size_t kMaxStackDepth = 64;
        static constexpr std::size_t kMaxPooled = 128;

        T* acquire() {
            {
                std::lock_guard lock(mutex_);
                if (!pool_.empty()) {
                    T* v = pool_.back();
                    pool_.pop_back();
                    pool_stats().pool_hits.fetch_add(1, std::memory_order_relaxed);
                    pool_stats().churn_reductions.fetch_add(1, std::memory_order_relaxed);
                    v->clear();
                    maybe_shrink(*v);
                    return v;
                }
            }
            pool_stats().lazy_allocs.fetch_add(1, std::memory_order_relaxed);
            auto* v = new T();
            v->reserve(kInitialCapacity);
            return v;
        }

        void release(T* v) {
            if (!v)
                return;
            v->clear();
            maybe_shrink(*v);
            std::lock_guard lock(mutex_);
            if (pool_.size() < kMaxPooled) {
                pool_.push_back(v);
                return;
            }
            delete v;
        }

        void track_depth(std::size_t depth) {
            if (depth > kMaxStackDepth)
                pool_stats().growth_warnings.fetch_add(1, std::memory_order_relaxed);
            auto& max_d = pool_stats().max_depth;
            std::uint64_t cur = max_d.load(std::memory_order_relaxed);
            const std::uint64_t capped = std::min(depth, kMaxStackDepth);
            while (capped > cur &&
                   !max_d.compare_exchange_weak(cur, capped, std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
            }
        }

    private:
        void maybe_shrink(T& v) {
            if (v.capacity() > kGrowthThreshold) {
                v.shrink_to_fit();
                pool_stats().growth_warnings.fetch_add(1, std::memory_order_relaxed);
            }
        }

        std::mutex mutex_;
        std::vector<T*> pool_;
    };

    BoundedVectorPool<std::vector<MutationVec>> g_mutation_pool;
    BoundedVectorPool<std::vector<YieldVec>> g_yield_pool;

    using MutationStackVec = std::vector<MutationVec>;
    using YieldStackVec = std::vector<YieldVec>;

    MutationStackVec* acquire_mutation_stack() {
        return g_mutation_pool.acquire();
    }

    YieldStackVec* acquire_yield_stack() {
        return g_yield_pool.acquire();
    }

    void release_mutation_stack(void* p) {
        g_mutation_pool.release(static_cast<MutationStackVec*>(p));
    }

    void release_yield_stack(void* p) {
        g_yield_pool.release(static_cast<YieldStackVec*>(p));
    }

    MutationStackVec& mutation_stack_from_ptr(void* p) {
        return *static_cast<MutationStackVec*>(p);
    }

    YieldStackVec& yield_stack_from_ptr(void* p) {
        return *static_cast<YieldStackVec*>(p);
    }

    MutationStackVec* ensure_mutation_stack_ptr(aura::serve::Fiber* fiber) {
        void* p = fiber->mutation_stack_ptr();
        if (p == nullptr) {
            p = acquire_mutation_stack();
            fiber->set_mutation_stack_ptr(p);
        }
        return static_cast<MutationStackVec*>(p);
    }

    YieldStackVec* ensure_yield_stack_ptr(aura::serve::Fiber* fiber) {
        void* p = fiber->yield_checkpoint_ptr();
        if (p == nullptr) {
            p = acquire_yield_stack();
            fiber->set_yield_checkpoint_ptr(p);
        }
        return static_cast<YieldStackVec*>(p);
    }

    // Issue #1404 (Option 1): restamp_yield_checkpoint_top used to
    // "detect-and-overwrite" — bump size_mismatches_caught, then
    // silently rewrite `top.*` to the new state. The continuation
    // then ran with the new state regardless of the drift. Each of
    // the 4 fields indicates a real anomaly:
    //   - mutation_stack_depth / boundary_depth drift → stack inconsistent
    //   - defuse_version drift → mutation epoch advanced during hold
    //   - thread_id drift → fiber migrated threads (worker pool steal)
    //
    // Now: on mismatch, bump the counter (for diagnostics) and RETURN
    // `true` without restamping. The caller is expected to surface an
    // merr via the trampoline (fiber-local error channel) so the
    // fiber aborts / deadlocks gracefully instead of running on a
    // silent state overwrite. On no mismatch, restamp as before and
    // return false.
    //
    // Returns: true if a mismatch was detected (caller should fail-loud);
    //          false if the checkpoint was successfully restamped.
    bool restamp_yield_checkpoint_top(Evaluator* ev, aura::serve::Fiber* fiber) {
        if (!ev || !fiber)
            return false;
        void* yp = fiber->yield_checkpoint_ptr();
        if (yp == nullptr)
            return false;
        auto& ystack = yield_stack_from_ptr(yp);
        if (ystack.empty())
            return false;
        void* mp = fiber->mutation_stack_ptr();
        const std::size_t mdepth = mp ? mutation_stack_from_ptr(mp).size() : 0;
        const std::size_t bdepth = Evaluator::mutation_boundary_depth();
        const std::uint64_t ver = ev->defuse_version_snapshot();
        auto& top = ystack.back();
        const bool mismatch = top.mutation_stack_depth != mdepth || top.boundary_depth != bdepth ||
                              top.defuse_version != ver ||
                              top.thread_id != std::this_thread::get_id();
        if (mismatch) {
            // Fail-loud: counter bumps for diagnostics, but the recorded
            // checkpoint top is preserved (NOT overwritten) so the caller
            // can surface an merr pointing at the drift.
            pool_stats().size_mismatches_caught.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        top.mutation_stack_depth = mdepth;
        top.boundary_depth = bdepth;
        top.defuse_version = ver;
        top.thread_id = std::this_thread::get_id();
        pool_stats().restamps.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

} // namespace fiber_stack_pool_detail

// Issue #177: per-fiber MutationCheckpoint stack. The
// declaration is in Evaluator (evaluator.ixx); the
// definition is here so the thread_local variable is in
// exactly one TU. Each fiber has its own stack (thread_local
// + the fibers are cooperative-scheduled on threads, so
// they share the thread's thread_local; the stack is
// per-fiber via the yield/enter mechanism — the fiber's
// Issue #213 Cycle 3: per-fiber state. The mutation
// stack now lives on the Fiber itself (fiber.h's
// `mutation_stack_` field), so a fiber that migrates
// between threads brings its stack with it. The
// `g_main_thread_stack` is a thread_local fallback for
// main-thread eval (no fiber active). The accessor
// `active_mutation_stack()` in evaluator.ixx routes
// between the two.
thread_local std::vector<aura::compiler::Evaluator::MutationCheckpoint>
    aura::compiler::Evaluator::g_main_thread_stack;
thread_local std::vector<aura::compiler::Evaluator::YieldBoundaryCheckpoint>
    aura::compiler::Evaluator::g_main_thread_yield_checkpoints;
thread_local void* aura::compiler::Evaluator::g_current_fiber_void;
// Issue #1403: thread-local stack of Evaluator* hooks (replaces the
// previous thread_local Evaluator* pointer). When fiber:spawn
// reuses a worker thread for multiple fibers, each fiber's RAII
// guard pushes its Evaluator onto this stack; the trampolines
// (flush_mutation_boundary_trampoline / mutation_boundary_held_trampoline)
// read the top of stack via yield_hook_evaluator(). LIFO pop on
// guard destruction. Cross-fiber state-corruption from a stale
// single-pointer write is eliminated.
thread_local std::vector<aura::compiler::Evaluator*> g_yield_hook_stack;
// Issue #456: per-thread pointer to the active Evaluator
// for observability primitives. Set by CompilerService
// ctor and used by (query:mutation-impact) /
// (query:epoch-stats) / (query:dirty-subtree) so they
// can find the Evaluator even when no
// MutationBoundaryGuard is currently active. Without
// this, primitives that read counters bumped in
// guard dtors would see nullptr (yield_hook_evaluator
// is only bound inside a guard).
thread_local aura::compiler::Evaluator* g_query_evaluator = nullptr;
// Issue #485: process-wide Evaluator for scheduler/fiber hooks
// that run on worker threads (g_query_evaluator is thread_local).
std::atomic<aura::compiler::Evaluator*> g_scheduler_stats_evaluator{nullptr};

namespace {
    aura::compiler::Evaluator* evaluator_for_scheduler_hooks() noexcept {
        if (auto* ev = aura::compiler::Evaluator::yield_hook_evaluator())
            return ev;
        if (auto* ev = aura::compiler::Evaluator::get_query_evaluator())
            return ev;
        return g_scheduler_stats_evaluator.load(std::memory_order_acquire);
    }
} // namespace

// Implementation of active_mutation_stack() — the
// header has the declaration only (to avoid pulling
// fiber.h into evaluator.ixx). Here we have full access
// to the Fiber class definition.
std::vector<aura::compiler::Evaluator::MutationCheckpoint>&
aura::compiler::Evaluator::active_mutation_stack() {
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        auto* stack = fiber_stack_pool_detail::ensure_mutation_stack_ptr(fiber);
        fiber_stack_pool_detail::g_mutation_pool.track_depth(stack->size());
        return *stack;
    }
    return g_main_thread_stack;
}

std::vector<aura::compiler::Evaluator::YieldBoundaryCheckpoint>&
aura::compiler::Evaluator::active_yield_checkpoint_stack() {
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        auto* stack = fiber_stack_pool_detail::ensure_yield_stack_ptr(fiber);
        fiber_stack_pool_detail::g_yield_pool.track_depth(stack->size());
        return *stack;
    }
    return g_main_thread_yield_checkpoints;
}

std::vector<aura::compiler::Evaluator::YieldBoundaryCheckpoint>&
aura::compiler::Evaluator::active_yield_checkpoint_stack_static() {
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        auto* stack = fiber_stack_pool_detail::ensure_yield_stack_ptr(fiber);
        fiber_stack_pool_detail::g_yield_pool.track_depth(stack->size());
        return *stack;
    }
    return g_main_thread_yield_checkpoints;
}

bool aura::compiler::Evaluator::any_active_mutation_boundary() const noexcept {
    int* slot = mutation_boundary_depth_slot(const_cast<Evaluator*>(this));
    return slot != nullptr && *slot > 0;
}

void aura::compiler::Evaluator::ensure_mutation_invariants() noexcept {
    auto& stack = active_mutation_stack();
    int* depth = mutation_boundary_depth_slot(this);
    if (!depth)
        return;
    const bool stack_empty = stack.empty();
    const bool depth_zero = (*depth == 0);
    if (stack_empty != depth_zero) {
        total_invariant_violations_.fetch_add(1, std::memory_order_relaxed);
    }
}

void aura::compiler::Evaluator::ensure_hygiene_violation_detection() const noexcept {
    // Issue #422: attempts are recorded at hygiene_protected_error
    // and replace-subtree block sites; this hook is a no-op probe
    // for tests and Guard-exit wiring verification.
}

void aura::compiler::Evaluator::checkpoint_yield_boundary(bool at_mutation_boundary_yield) {
    bool had_boundary = any_active_mutation_boundary() || !active_mutation_stack().empty();
    if (!had_boundary && !at_mutation_boundary_yield)
        return;
    YieldBoundaryCheckpoint cp;
    cp.defuse_version = defuse_version_snapshot();
    cp.boundary_depth = mutation_boundary_depth();
    cp.mutation_stack_depth = active_mutation_stack().size();
    cp.thread_id = std::this_thread::get_id();
    cp.had_active_boundary = had_boundary || at_mutation_boundary_yield;
    auto& ystack = active_yield_checkpoint_stack();
    ystack.push_back(cp);
    fiber_stack_pool_detail::g_yield_pool.track_depth(ystack.size());
    mutation_yield_count_.fetch_add(1, std::memory_order_relaxed);
    // Issue #1373: same-thread yield while a boundary is / was active
    // (checkpoint is taken on the yielding thread before any steal).
    if (cp.had_active_boundary) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->mutation_boundary_yield_same_thread_total.fetch_add(1, std::memory_order_relaxed);
    }
}


// Issue #1500: batch refresh_if_stale over atomic-batch + COW-boundary
// pinned StableNodeRefs. Restamps full provenance (gen/wrap/cow/
// mutation_id/subtree_gen) while preserving fiber_id, workspace_id,
// and boundary_pinned. Called from fiber steal, Guard dtor, and
// re_pin_cow_children_from_snapshot.
std::size_t aura::compiler::Evaluator::restamp_pinned_stable_refs() noexcept {
    auto* ws = workspace_flat();
    if (!ws)
        return 0;
    std::size_t refreshed = 0;
    for (auto& ref : atomic_batch_pinned_refs_) {
        if (ref.refresh_if_stale(*ws))
            ++refreshed;
    }
    {
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        for (auto& ref : cow_boundary_pinned_refs_) {
            const bool was_pinned = ref.boundary_pinned;
            if (ref.refresh_if_stale(*ws)) {
                // refresh_if_stale preserves boundary_pinned; re-assert
                // so COW survival semantics stay intact after restamp.
                if (was_pinned)
                    ref.boundary_pinned = true;
                ++refreshed;
            }
        }
    }
    if (refreshed > 0) {
        bump_stable_ref_steal_auto_refresh(refreshed);
        bump_stable_ref_cross_cow_refresh(refreshed);
    }
    return refreshed;
}

// Issue #1446: re_pin_cow_children_from_snapshot — walk the
// outermost MutationBoundaryGuard's pinned StableNodeRef / COW
// children and re-pin them after a steal or GC compact event.
// Called from restore_post_yield_or_rollback (AC1) and from the
// arena compact hook (AC2). Bumps cow_repin_on_steal metric on
// success; bumps checkpoint_lost_on_compact if the checkpoint is
// missing (memory-safety event — should be 0 in steady state).
// Issue #1500: actually batch-restamp pinned refs (no longer a
// metrics-only stub).
bool aura::compiler::Evaluator::re_pin_cow_children_from_snapshot() {
    int* slot = mutation_boundary_depth_slot(this);
    if (!slot || *slot == 0) {
        // No active Guard — still restamp any lingering pins (steal
        // / compact can leave refs without a live Guard).
        (void)restamp_pinned_stable_refs();
        return true;
    }
    auto* m = static_cast<aura::compiler::CompilerMetrics*>(compiler_metrics());
    // Walk pinned StableNodeRef registries and refresh full provenance.
    auto* ws = workspace_flat();
    if (!ws) {
        if (m)
            m->checkpoint_lost_on_compact.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Issue #1473: walk cow_boundary_pinned_refs_ and validate_or_refresh.
    std::size_t validated = 0;
    {
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        for (auto& pinned : cow_boundary_pinned_refs_) {
            if (pinned.validate_or_refresh(*ws))
                ++validated;
        }
    }
    if (m) {
        m->cow_repin_on_steal.fetch_add(1, std::memory_order_relaxed);
        m->stable_ref_validations_at_steal.fetch_add(validated, std::memory_order_relaxed);
        m->panic_transfer_nested_success.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

// Issue #1446 AC2: on_arena_compact_hook — registered with
// arena.set_on_compact_hook() during Evaluator ctor. When the arena
// runs compact/defrag, this hook is invoked AFTER reclaim; we walk
// the active Guard stack and call re_pin_cow_children_from_snapshot()
// on each (outermost + nested) to keep StableNodeRef / COW pins in
// sync with the post-compact arena state.
void aura::compiler::Evaluator::on_arena_compact_hook() {
    re_pin_cow_children_from_snapshot();
}

bool aura::compiler::Evaluator::restore_post_yield_or_rollback() {
    // Issue #921: strengthen post-yield/steal checkpoint consistency.
    // Always restamp top-of-stack metadata on any desync (not only
    // thread migration), then re-evaluate drift. Debug builds assert
    // that mutation-stack depth and boundary depth stay coherent.
    auto& stack = active_yield_checkpoint_stack();
    if (stack.empty())
        return true;
    auto cp = stack.back();
    const std::size_t current_mdepth = active_mutation_stack().size();
    const std::size_t current_bdepth = mutation_boundary_depth();
    const std::uint64_t current_ver = defuse_version_snapshot();
    bool thread_migrated = cp.thread_id != std::this_thread::get_id();
    bool size_mismatch = cp.mutation_stack_depth != current_mdepth;
    bool depth_mismatch = current_bdepth != cp.boundary_depth;
    bool version_drift_pre = !is_version_current(cp.defuse_version);
    // Issue #1373: record cross-thread migration *before* restamp
    // clears the flag (restamp re-keys thread_id to current).
    if (thread_migrated && cp.had_active_boundary) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->mutation_boundary_cross_thread_migration_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
    }
    if (thread_migrated || size_mismatch || depth_mismatch || version_drift_pre) {
        fiber_stack_pool_detail::pool_stats().size_mismatches_caught.fetch_add(
            1, std::memory_order_relaxed);
        // Issue #921: restamp on *any* desync (steal/resume path
        // previously only restamped on thread_migrated). Keeps
        // yield-checkpoint top aligned with live mutation state so
        // a subsequent restore after nested yields does not false-fail.
        cp.thread_id = std::this_thread::get_id();
        cp.boundary_depth = current_bdepth;
        cp.mutation_stack_depth = current_mdepth;
        cp.defuse_version = current_ver;
        fiber_stack_pool_detail::pool_stats().restamps.fetch_add(1, std::memory_order_relaxed);
        // Also restamp live fiber yield stack storage if present.
        if (g_current_fiber_void != nullptr) {
            auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
            // Issue #1404 Option 1: capture mismatch flag. Caller-side
            // merr surfacing via the fiber-local error channel is the
            // follow-up (the existing rollback path already uses
            // size_mismatch / thread_migrated signals for this caller).
            [[maybe_unused]] const bool yc_mismatch =
                fiber_stack_pool_detail::restamp_yield_checkpoint_top(this, fiber);
        }
        thread_migrated = false;
        size_mismatch = false;
        depth_mismatch = false;
    }
#ifndef NDEBUG
    // Strict assert: after restamp, stack depth vs boundary slot must agree
    // when a boundary was active at yield (production migration safety).
    if (cp.had_active_boundary) {
        int* slot = mutation_boundary_depth_slot(this);
        const int depth_slot = slot ? *slot : 0;
        assert(static_cast<std::size_t>(depth_slot) == current_bdepth ||
               current_mdepth == cp.mutation_stack_depth);
    }
#endif
    stack.pop_back();
    if (!cp.had_active_boundary)
        return true;
    bool version_drift = !is_version_current(cp.defuse_version);
    if (aura_aot_probe_checkpoint_version(cp.defuse_version, current_bridge_epoch())) {
        version_drift = true;
        aura_aot_record_deopt_on_steal();
    }
    // After restamp, version_drift against the *current* stamp should be false
    // unless AOT deopt probe fired or concurrent mutation advanced again.
    if (thread_migrated || version_drift || depth_mismatch || size_mismatch) {
        cross_fiber_rollback_count_.fetch_add(1, std::memory_order_relaxed);
        // Issue #1373: yield-path forced rollback / failure signal.
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->mutation_boundary_yield_rollback_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1461: Agent Decision Metrics — recovery-failure is the
        // panic-count source for (agent:decision-metrics). Fiber resume
        // mismatch is the production recovery-failure path.
        bump_mutation_boundary_recovery_failure();
        bump_mutation_boundary_rollback();
        bump_guard_panic_reflect_boundary_violation_prevented();
        if (outermost_mutation_success_flag_)
            *outermost_mutation_success_flag_ = false;
        // Issue #1260: transfer pending panic checkpoint across steal/resume
        // mismatch so panic signals survive fiber migration.
        if (pending_panic_checkpoint()) {
            bump_panic_checkpoint_transfer_count();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
                m->panic_transfer_on_steal.fetch_add(1, std::memory_order_relaxed);
            if (Evaluator::g_current_fiber_void != nullptr) {
                auto* fiber = static_cast<aura::serve::Fiber*>(Evaluator::g_current_fiber_void);
                // Issue #1404 Option 1: capture mismatch flag. Panic-transfer
                // path already forces rollback via success flag below.
                [[maybe_unused]] const bool yc_mismatch =
                    fiber_stack_pool_detail::restamp_yield_checkpoint_top(this, fiber);
            }
            // Force rollback path via success flag so Guard dtor restores.
            if (panic_auto_rollback_ && outermost_mutation_success_flag_)
                *outermost_mutation_success_flag_ = false;
        } else if (cp.had_active_boundary) {
            // Had boundary but lost checkpoint — record lost-on-steal.
            bump_panic_checkpoint_lost_on_steal();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
                m->panic_transfer_failed.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    // Issue #1260 happy path: successful resume with pending checkpoint —
    // restamp so the new fiber owns the checkpoint provenance.
    if (pending_panic_checkpoint() && cp.had_active_boundary) {
        bump_panic_checkpoint_transfer_count();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->panic_transfer_on_steal.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

// Issue #213 Cycle 3: function pointer implementations
// that the fiber side calls. The setter is called by
// Fiber::resume() to update g_current_fiber_void. The
// deleter is called by ~Fiber() to free the per-fiber
// storage. Both are defined here because the storage type
// (std::vector<MutationCheckpoint>) is opaque to fiber.cpp.
namespace {
    void* fiber_setter_impl(void* f) {
        auto prev = aura::compiler::Evaluator::get_current_fiber();
        aura::compiler::Evaluator::set_current_fiber(f);
        return prev;
    }
    void fiber_storage_deleter_impl(void* p) {
        fiber_stack_pool_detail::release_mutation_stack(p);
    }
    void fiber_yield_checkpoint_deleter_impl(void* p) {
        fiber_stack_pool_detail::release_yield_stack(p);
    }
    void fiber_yield_checkpoint_impl(uint8_t reason) {
        // Issue #1403: read top of thread-local stack instead
        // of a single thread_local pointer.
        auto* ev = Evaluator::yield_hook_evaluator();
        if (!ev)
            return;
        bool at_boundary =
            reason == static_cast<uint8_t>(aura::serve::YieldReason::MutationBoundary);
        ev->checkpoint_yield_boundary(at_boundary);
    }
    void fiber_resume_validate_impl() {
        // Issue #1403: read top of thread-local stack instead
        // of a single thread_local pointer.
        auto* ev = Evaluator::yield_hook_evaluator();
        if (!ev)
            return;
        const std::uint64_t ver = ev->defuse_version_snapshot();
        const std::uint64_t bridge = ev->current_bridge_epoch();
        if (aura_aot_probe_checkpoint_version(ver, bridge))
            aura_aot_record_deopt_on_steal();
        (void)ev->restore_post_yield_or_rollback();
        ev->restore_panic_checkpoint_on_fiber_resume_if_needed();
    }
    void fiber_sync_mutation_stack_impl(void* per_fiber_stack) {
        Evaluator::sync_per_fiber_mutation_stack(per_fiber_stack);
    }
} // namespace

// Register the function pointers at static-init time. The
// fiber side calls them; we don't need the Evaluator to
// know about Fiber (one-way dependency).
struct FiberHookRegistrar {
    FiberHookRegistrar() {
        aura::serve::g_fiber_setter_ = fiber_setter_impl;
        aura::serve::g_fiber_sync_mutation_stack_ = fiber_sync_mutation_stack_impl;
        aura::serve::g_fiber_storage_deleter_ = fiber_storage_deleter_impl;
        aura::serve::g_fiber_yield_checkpoint_deleter_ = fiber_yield_checkpoint_deleter_impl;
        aura::serve::g_fiber_yield_checkpoint_ = fiber_yield_checkpoint_impl;
        aura::serve::g_fiber_resume_validate_ = fiber_resume_validate_impl;
    }
};
static FiberHookRegistrar g_fiber_hook_registrar{};

std::vector<aura::compiler::Evaluator::MutationCheckpoint>&
aura::compiler::Evaluator::active_mutation_stack_static() {
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        auto* stack = fiber_stack_pool_detail::ensure_mutation_stack_ptr(fiber);
        fiber_stack_pool_detail::g_mutation_pool.track_depth(stack->size());
        return *stack;
    }
    return g_main_thread_stack;
}

// Issue #236 follow-up: per-Evaluator, per-thread depth slot
// for MutationBoundaryGuard. We use a thread_local
// std::unordered_map keyed by Evaluator* address. Each fiber
// has its own slot for each Evaluator it touches. When the
// last guard for a (thread, evaluator) pair destructs, the
// map entry stays (cheap) so we don't churn the heap.
//
// Returns a pointer to an int initialized to 0 the first
// time it's accessed for a given (thread, evaluator).
int* aura::compiler::Evaluator::mutation_boundary_depth_slot(Evaluator* ev) {
    struct Slot {
        std::unordered_map<Evaluator*, int> depths;
    };
    thread_local Slot* slot = new Slot();
    auto it = slot->depths.find(ev);
    if (it == slot->depths.end()) {
        it = slot->depths.emplace(ev, 0).first;
    }
    return &it->second;
}
// ═════════════════════════════════════════════════════════════════════════
// Issue #157 Phase 1: yield_mutation_boundary implementation.
//
// The lock + version accessors are public inline methods on Evaluator
// (in evaluator.ixx), but yield_mutation_boundary must be defined here
// in the .cpp (not the .ixx) because the extern function pointer
// g_fiber_yield_mutation_boundary lives in messaging_bridge.h, a
// non-module header that the module interface cannot include.
//
void Evaluator::bind_yield_hook_evaluator() {
    // Issue #1403: push onto thread-local stack. Trampolines
    // read the top of stack via yield_hook_evaluator(). RAII
    // pairing with unbind_yield_hook_evaluator() keeps the stack
    // LIFO.
    g_yield_hook_stack.push_back(this);
}

// Issue #456: per-thread query-evaluator accessors.
void Evaluator::set_query_evaluator(Evaluator* ev) noexcept {
    g_query_evaluator = ev;
    g_scheduler_stats_evaluator.store(ev, std::memory_order_release);
}
Evaluator* Evaluator::get_query_evaluator() noexcept {
    // Issue #1133: thread_local may be null on worker threads spawned
    // mid-session; fall back to process-wide scheduler stats pointer.
    if (g_query_evaluator)
        return g_query_evaluator;
    return g_scheduler_stats_evaluator.load(std::memory_order_acquire);
}

// Issue #63723: clear all per-thread/process-wide Evaluator
// pointers that point at this dying instance. Without this,
// when the closure that owned this Evaluator returns, the
// worker thread's g_query_evaluator (thread_local) and
// g_scheduler_stats_evaluator (std::atomic) still point at
// the dead stack — and the next
// aura_evaluator_bump_mutation_steal_attempt() /
// aura_evaluator_bump_mutation_steal_violation() /
// aura_evaluator_resume_fiber_migration() path dereferences
// a use-after-return (verified by ASan:
// stack-use-after-return in bump_mutation_steal_attempt at
// evaluator.ixx:3130). This is what caused test_issue_226 to
// hang on t.join() — the worker's steal code called
// bump_mutation_steal_attempt on a dead Evaluator and
// crashed/hung inside the atomic fetch_add.
//
// Use CAS for the atomic so we don't clobber a freshly-stored
// pointer from a different CompilerService that was constructed
// after this Evaluator's ~Evaluator started running.
void Evaluator::unbind_query_evaluator() noexcept {
    if (g_query_evaluator == this)
        g_query_evaluator = nullptr;
    Evaluator* expected = this;
    g_scheduler_stats_evaluator.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
}

void Evaluator::unbind_yield_hook_evaluator() {
    // Issue #1403: LIFO removal from the thread-local stack.
    // Search from the top — the matching entry should normally
    // be at the top of the stack (RAII pairing with bind). If
    // an unbalanced guard happens (defensive), we still find
    // and remove the correct entry without disturbing others.
    for (auto it = g_yield_hook_stack.rbegin(); it != g_yield_hook_stack.rend(); ++it) {
        if (*it == this) {
            g_yield_hook_stack.erase((it + 1).base());
            return;
        }
    }
    // Not found — already unbound. Silently no-op (matches the
    // existing defensive semantics: unbind_yield_hook_evaluator
    // is idempotent under RAII use).
}

// Issue #1403 + #285: yield_hook_evaluator() getter returns top
// of the thread-local stack (or nullptr if empty).
Evaluator* Evaluator::yield_hook_evaluator() noexcept {
    return g_yield_hook_stack.empty() ? nullptr : g_yield_hook_stack.back();
}

void Evaluator::yield_mutation_boundary() {
    // Issue #1146: per-fiber exception-state telemetry on yield/mutation boundary.
    bump_per_fiber_ex_state();
    if (aura::messaging::g_fiber_yield_mutation_boundary) {
        aura::messaging::g_fiber_yield_mutation_boundary();
        bump_per_fiber_ex_state_hit();
        bump_per_fiber_ex_state_savings();
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Issue #285: install the flush_mutation_boundary hook into the
// messaging bridge. The hook runs on the fiber thread (set/cleared
// at the outermost guard). The static-init pattern matches
// g_fiber_yield_mutation_boundary above.
//
// Bridge-callable trampoline (file-local): wraps the thread-local
// `g_yield_hook_stack` (top-of-stack via `yield_hook_evaluator()` getter) so Fiber::yield can
// invoke the flush without needing the module include.
namespace {
    void flush_mutation_boundary_trampoline() {
        if (aura::compiler::Evaluator::yield_hook_evaluator())
            aura::compiler::Evaluator::yield_hook_evaluator()->flush_mutation_boundary();
    }

    // Issue #354: "yield while holding a mutation
    // boundary" check trampoline. Returns true when an
    // outermost MutationBoundaryGuard is currently
    // alive. Used by Fiber::yield to detect a
    // programmer error (yielding inside a mutate:*
    // primitive body).
    bool mutation_boundary_held_trampoline() {
        auto* ev = aura::compiler::Evaluator::yield_hook_evaluator();
        return ev ? ev->mutation_boundary_held() : false;
    }
} // anonymous namespace

// Static initializer: register the trampoline at module load.
// Runs once per process; safe under serve-async / standalone.
namespace {
    struct FlushHookRegistrar {
        FlushHookRegistrar() {
            aura::messaging::g_flush_mutation_boundary = &flush_mutation_boundary_trampoline;
            // Issue #354: register the
            // mutation-boundary-held check trampoline.
            aura::messaging::g_mutation_boundary_held = &mutation_boundary_held_trampoline;
        }
    };
    const FlushHookRegistrar g_flush_hook_registrar{};
} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════
// Issue #285: flush_mutation_boundary.
//
// Ensures per-fiber mutation state is consistent at the exact yield
// point. The implementation has three responsibilities:
//
//   1. Touch the active mutation stack (per-fiber when in a fiber,
//      thread-local otherwise) so any std::vector resizes / lazy
//      allocations are committed before the yield.
//   2. Issue a release barrier on defuse_version_ so other threads
//      can observe the current version on their next acquire.
//   3. No-op when not in a mutation boundary (the stack is empty
//      AND version is at its quiescent value — we conservatively
//      always touch the stack but only emit the barrier when the
//      stack is non-empty, since an empty stack means no
//      boundary is active).
void Evaluator::flush_mutation_boundary() {
    // (1) Touch the active mutation stack so any pending mutations
    // to the stack itself (push/pop, lazy allocation) are visible.
    // Using const-ref ensures no copy / no resize.
    auto& stack = active_mutation_stack();
    if (stack.empty()) {
        return; // not in a mutation boundary; nothing to flush
    }
    // (2) Release barrier on defuse_version_ so other threads see
    // the current version on their next acquire.
    defuse_version_.fetch_add(0, std::memory_order_release);
    // Issue #1268: outermost-only panic checkpoint lifecycle restamp
    // on flush (steal/yield boundary). Ensures defuse_version_ +
    // pending checkpoint visibility before fiber migration.
    const bool outermost_active = mutation_boundary_depth() == 1 ||
                                  (mutation_boundary_depth() == 0 &&
                                   !mutation_boundary_held_.load(std::memory_order_acquire));
    if (outermost_active || mutation_boundary_depth() == 1) {
        if (pending_panic_checkpoint()) {
            bump_panic_checkpoint_transfer_count();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
                m->panic_checkpoint_flush_outermost.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #1274: outermost flush feeds macro dirty → IR cache
        // invalidation + epoch bump so typecheck/lower never hit stale IR.
        if (mark_all_defines_dirty_fn_) {
            mark_all_defines_dirty_fn_();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
                m->dirty_propagation_to_ir_count.fetch_add(1, std::memory_order_relaxed);
                m->epoch_bump_for_macro.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Issue #1272: structured observability sample on flush.
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
            m->runtime_obs_mutation_boundary_flush_samples.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Issue #453: Panic Checkpoint lifecycle hooks across fiber migration.
//
// Three bridge trampolines. All three are file-local and use the
// thread-local `g_yield_hook_stack` (read top-of-stack via `yield_hook_evaluator()` getter, set by
// the outermost active MutationBoundaryGuard via bind_yield_hook_evaluator) to find the active
// evaluator. When no guard is active, the evaluator is null and each trampoline is a no-op (the
// bridge hooks are nullable; Fiber::yield/resume treat null as "skip").
//
// Why file-local + trampoline: matches the existing pattern from
// #285 (g_flush_mutation_boundary). The bridge is non-module
// (messaging_bridge.h is a plain header), so the static-init
// registrar wires the function pointer at module load.
namespace {

    // (1) g_pending_panic_checkpoint: true if the active outermost
    // guard captured a checkpoint. Reads thread-local evaluator
    // pointer (set by bind_yield_hook_evaluator). Returns false
    // when no guard is active.
    bool pending_panic_checkpoint_trampoline() {
        auto* ev = Evaluator::yield_hook_evaluator();
        return ev ? ev->pending_panic_checkpoint() : false;
    }

    // (2) g_transfer_panic_checkpoint: bumps transfer count and
    // re-stamps any per-fiber storage. Called by Fiber::resume()
    // after the swapcontext return. No-op when no pending checkpoint.
    void transfer_panic_checkpoint_trampoline() {
        auto* ev = Evaluator::yield_hook_evaluator();
        if (!ev)
            return;
        if (!ev->pending_panic_checkpoint())
            return;
        ev->bump_panic_checkpoint_transfer_count();
        if (Evaluator::g_current_fiber_void != nullptr) {
            auto* fiber = static_cast<aura::serve::Fiber*>(Evaluator::g_current_fiber_void);
            // Issue #1404 Option 1: capture mismatch flag.
            [[maybe_unused]] const bool yc_mismatch =
                fiber_stack_pool_detail::restamp_yield_checkpoint_top(ev, fiber);
        }
        // Issue #1127: snapshot both counters under one acquire fence so
        // probe sees a consistent side of concurrent AOT/mutate updates.
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto defuse_v = ev->defuse_version_snapshot();
        const auto bridge_e = ev->current_bridge_epoch();
        if (aura_aot_probe_checkpoint_version(defuse_v, bridge_e)) {
            aura_aot_record_deopt_on_steal();
            ev->bump_concurrent_safety_aot_reload_at_guard();
        }
        ev->bump_macro_hygiene_panic_restamp_from_workspace();
    }

    // (3) g_block_gc_for_pending_checkpoint: bumps the GC-block
    // counter. Called by Fiber::yield(MutationBoundary) when a
    // pending checkpoint exists. The actual GC defer is a
    // follow-up (requires scheduler.cpp + gc_coordinator.cpp
    // integration; out of scope for the P0 ship).
    void block_gc_for_pending_checkpoint_trampoline() {
        auto* ev = Evaluator::yield_hook_evaluator();
        if (!ev)
            return;
        if (!ev->pending_panic_checkpoint())
            return;
        ev->bump_gc_blocked_by_pending_panic();
    }

    struct PanicCheckpointRegistrar {
        PanicCheckpointRegistrar() {
            aura::messaging::g_pending_panic_checkpoint = &pending_panic_checkpoint_trampoline;
            aura::messaging::g_transfer_panic_checkpoint = &transfer_panic_checkpoint_trampoline;
            aura::messaging::g_block_gc_for_pending_checkpoint =
                &block_gc_for_pending_checkpoint_trampoline;
        }
    };
    const PanicCheckpointRegistrar g_panic_checkpoint_registrar{};

} // anonymous namespace

// Evaluator::pending_panic_checkpoint: returns true if the
// outermost active guard (tracked via thread-local
// `g_yield_hook_stack`) read via `yield_hook_evaluator()` getter has a captured checkpoint. Returns
// false when no guard is active.
bool Evaluator::pending_panic_checkpoint() const noexcept {
    // The bridge trampoline checks the same path; this is
    // the in-module entry point used by tests + future
    // scheduler hooks. The thread-local slot is set by
    // bind_yield_hook_evaluator (called by the guard ctor)
    // and cleared by unbind_yield_hook_evaluator (dtor).
    auto* active = Evaluator::yield_hook_evaluator();
    if (active != this)
        return false; // not the active evaluator
    // Walk the active mutation stack to find the outermost guard
    // and check its checkpoint state. The stack stores
    // YieldBoundaryCheckpoint records; the guard's
    // `had_panic_checkpoint_` is on the RAII object itself,
    // not on the stack record (the stack records are for
    // unwind / yield metadata, not guard state). So we
    // check via the bind hook: when bind_yield_hook_evaluator
    // was called, a guard is active. The "has checkpoint"
    // property is then inferred from `panic_safe_source_`
    // (set by save_panic_checkpoint). When non-empty, the
    // guard captured a checkpoint.
    return !panic_safe_source_.empty();
}

// Issue #438: C-linkage shim so fiber.h can call the
// per-thread mutation boundary depth without pulling in
// the Evaluator module (fiber.h is a low-level header
// included by tests that don't have the Evaluator
// module available).
extern "C" std::size_t aura_evaluator_mutation_boundary_depth() {
    return Evaluator::mutation_boundary_depth();
}

// Issue #1518: wire arena auto live_compact soft-gate to mutation boundary.
// Static init once: first call from any TU that links fiber mutation.
namespace {
    struct ArenaBoundaryDepthWire {
        ArenaBoundaryDepthWire() {
            aura::ast::set_arena_mutation_boundary_depth_fn(
                +[]() noexcept -> std::size_t { return Evaluator::mutation_boundary_depth(); });
        }
    };
    const ArenaBoundaryDepthWire g_arena_boundary_depth_wire{};
} // namespace

// Issue #588: depth from a fiber's opaque mutation_stack_storage_.
extern "C" std::size_t aura_evaluator_mutation_stack_depth_from_ptr(void* mutation_stack_storage) {
    if (mutation_stack_storage == nullptr)
        return 0;
    using C = Evaluator::MutationCheckpoint;
    return static_cast<std::vector<C>*>(mutation_stack_storage)->size();
}

void Evaluator::sync_per_fiber_mutation_stack(void* per_fiber_stack) noexcept {
    g_main_thread_stack.clear();
    g_main_thread_yield_checkpoints.clear();
    if (g_current_fiber_void == nullptr)
        return;
    auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
    if (per_fiber_stack != nullptr)
        fiber->set_mutation_stack_ptr(per_fiber_stack);
    (void)fiber_stack_pool_detail::ensure_mutation_stack_ptr(fiber);
}

// Test seam (#588): push/pop a synthetic checkpoint on the
// active (per-fiber) mutation stack for steal-safety tests.
extern "C" void aura_evaluator_bump_macro_expand_checkpoint_save() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        ev = evaluator_for_scheduler_hooks();
    if (ev)
        ev->bump_macro_expand_checkpoint_save();
}

extern "C" void aura_evaluator_test_push_mutation_checkpoint() {
    Evaluator::active_mutation_stack_static().push_back({0, 0});
}

extern "C" void aura_evaluator_test_pop_mutation_checkpoint() {
    auto& stack = Evaluator::active_mutation_stack_static();
    if (!stack.empty())
        stack.pop_back();
}

// Issue #439: C-linkage shims for GC safepoint
// coordination. Same rationale as
// aura_evaluator_mutation_boundary_depth above: the
// fiber.cpp / scheduler.cpp code calls these without
// pulling in the Evaluator module.
extern "C" int aura_evaluator_request_gc_safepoint() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return 0; // no evaluator → no guard
    return ev->request_gc_safepoint();
}

extern "C" void aura_evaluator_wait_for_safepoint(std::uint64_t timeout_ms) {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return; // no evaluator → no wait
    ev->wait_for_safepoint(timeout_ms);
}

// Issue #485: bump evaluator counters when the scheduler
// defers a steal at MutationBoundary (inner guard active).
extern "C" void aura_evaluator_bump_steal_deferred_violation() {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->bump_mutation_steal_violation_count();
    ev->bump_boundary_violation_count();
    ev->bump_guard_panic_reflect_boundary_violation_prevented();
    // Issue #673: cross-module correlation — also bump the
    // "steal-deferred-during-mutation" correlation counter
    // so query:runtime-observability-correlated-stats reports it.
    ev->bump_runtime_observability_steal_deferred_correlated();
}

// Issue #500: log scheduler steal attempts in evaluator metrics.
extern "C" void aura_evaluator_bump_mutation_steal_attempt() {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->bump_mutation_steal_attempt();
    // Issue #673: baseline correlation counter — every steal
    // attempt bumped, so the primitive's denominator is the
    // global steal-attempt rate. Per-correlation counters
    // (deferred / ownership) are subsets of this.
    ev->bump_runtime_observability_steal_attempt_correlated();
}

// Issue #485: transfer mutation stack on Fiber::resume.
extern "C" void aura_evaluator_resume_fiber_migration() {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->transfer_mutation_stack_to_current_fiber();
    ev->probe_arena_auto_policy_on_fiber_transition();
    ev->bump_concurrent_safety_gc_safepoint_during_steal();
    ev->bump_linear_postmutate_post_rollback_revalidate();
    // Issue #818: steal/resume auto-refresh of Workspace-active StableRefs.
    // Marks that migration completed with provenance-aware restamp hooks.
    ev->bump_stable_ref_steal_auto_refresh();
}

// Issue #683: linear ownership enforcement on work-steal.

// Issue #812: steal+arena+GC coordination hooks
extern "C" void aura_evaluator_bump_steal_arena_yield() {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->bump_steal_arena_yield_during_compact();
}
extern "C" void aura_evaluator_bump_steal_outermost_enforced() {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->bump_steal_outermost_only_enforced();
}

extern "C" void aura_evaluator_probe_linear_on_steal() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return;
    ev->probe_arena_auto_policy_on_fiber_transition();
    ev->probe_linear_ownership_on_fiber_steal();
    ev->bump_steal_linear_probe_on_success();
    ev->bump_concurrent_safety_steal_boundary_success();
    // Issue #1127: paired snapshot under acquire fence.
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto defuse_v = ev->defuse_version_snapshot();
    const auto bridge_e = ev->current_bridge_epoch();
    if (aura_aot_probe_checkpoint_version(defuse_v, bridge_e)) {
        aura_aot_record_deopt_on_steal();
        ev->bump_concurrent_safety_aot_reload_at_guard();
    }
}

extern "C" {
std::uint64_t aura_per_fiber_stack_pool_hits() {
    return aura::serve::metrics::per_fiber_stack_pool_stats().pool_hits.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_per_fiber_stack_pool_lazy_allocs() {
    return aura::serve::metrics::per_fiber_stack_pool_stats().lazy_allocs.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_per_fiber_stack_pool_max_depth() {
    return aura::serve::metrics::per_fiber_stack_pool_stats().max_depth.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_per_fiber_stack_pool_churn_reductions() {
    return aura::serve::metrics::per_fiber_stack_pool_stats().churn_reductions.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_per_fiber_stack_pool_size_mismatches() {
    return aura::serve::metrics::per_fiber_stack_pool_stats().size_mismatches_caught.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_per_fiber_stack_pool_growth_warnings() {
    return aura::serve::metrics::per_fiber_stack_pool_stats().growth_warnings.load(
        std::memory_order_relaxed);
}
std::uint64_t aura_per_fiber_stack_pool_restamps() {
    return aura::serve::metrics::per_fiber_stack_pool_stats().restamps.load(
        std::memory_order_relaxed);
}
}

} // namespace aura::compiler
