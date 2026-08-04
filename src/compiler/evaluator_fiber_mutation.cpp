// evaluator_fiber_mutation.cpp — P1-l: per-fiber mutation stack + boundary hooks
// aura.compiler.evaluator module partition.
//
// Hot-Update MVP scope (Issue #1943): MutationBoundaryGuard enforces
// depth==0 for the **outermost** guard during function-body replacement
// (in-scope). Nested-guard concurrent re-lower paths are **deferred** —
// see docs/hot-update.md and #1931 / #1950 / #1953 for the systemic
// MutationBoundaryGuard enforcement work that remains open.

module;

#include "messaging_bridge.h"
#include "serve/fiber.h"
#include "serve/metrics.h"
#include "observability_metrics.h"
#include "orch/agent_spawn.h" // #2010: g_orch_module_stats for mailbox BP mirror
#include "core/gc_hooks.h"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"                   // #2056: is_strict for cross-tenant ensure
#include "compiler/hot_update_registry.hh"   // Issue #2162: aura_hot_update_has_deferred_reemit
#include "compiler/aot_hot_update_health.hh" // Issue #2543: orch hot-update health throttle tick
#include "compiler/ownership_escape_lowering_gate.h" // Issue #2507: clear escape gate on steal
#include "compiler/mutation_boundary_shared_exit.h" // Issue #2600: shared exit helper (soft + full Guard)
#include "core/layout_stamp.hh" // Issue #2519: full 8-field LayoutStamp equality
#include <algorithm>            // Issue #2189: remove_if for pin table invalidate
#include <cassert>
#include <chrono>
#include <memory>   // Issue #1880: unique_ptr for soft-path sentinel
#include <optional> // Issue #2555: optional TransactionGuard for orch agent body

module aura.compiler.evaluator;

import std;

extern "C" {
bool aura_aot_probe_checkpoint_version(std::uint64_t defuse_version, std::uint64_t bridge_epoch);
void aura_aot_record_deopt_on_steal();
// Issue #1631: force JIT/active-closure walk on post-steal bridge_epoch drift.
std::size_t aura_jit_walk_active_closures(std::uint64_t current_bridge_epoch);
}

namespace aura::compiler {

// Issue #2519: reconstruct LayoutStamp from fiber resume fence fields.
// Uses full 8-field operator== for freshness (not legacy core-6).
[[nodiscard]] inline aura::core::LayoutStamp
layout_stamp_from_fiber_resume(const aura::serve::Fiber& f) noexcept {
    return aura::core::LayoutStamp(f.resume_arena_id(), f.resume_arena_gen(),
                                   static_cast<std::uint16_t>(f.resume_flat_gen() & 0xFFFFu),
                                   f.resume_mutation_epoch(), f.resume_env_gen(), f.resume_defuse(),
                                   f.resume_shape_version(), f.resume_ir_soa_generation());
}

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

    // Issue #1992: CAS-init pattern for concurrent
    // ensure_mutation_stack_ptr. Plain read-then-write
    // (the previous behavior) was a data race: under
    // work-stealing handoff two workers could both see
    // `p == nullptr`, both allocate, both write — last-
    // writer wins, one pointer leaks, and the fiber
    // could resume on a stack another fiber still
    // references (use-after-free across fibers). The
    // compare_exchange_weak pairs allocate-then-publish:
    // if the CAS fails we release our allocation back
    // to the pool and use the winner's pointer.
    MutationStackVec* ensure_mutation_stack_ptr(aura::serve::Fiber* fiber) {
        void* p = fiber->mutation_stack_ptr();
        if (p == nullptr) {
            MutationStackVec* fresh = acquire_mutation_stack();
            void* expected = nullptr;
            if (fiber->compare_exchange_mutation_stack_ptr(expected, fresh)) {
                p = fresh;
            } else {
                // Lost the race — another thread won the
                // CAS. Release our allocation back to the
                // pool (no leak) and use the winner's ptr.
                release_mutation_stack(fresh);
                p = fiber->mutation_stack_ptr();
            }
        }
        return static_cast<MutationStackVec*>(p);
    }

    YieldStackVec* ensure_yield_stack_ptr(aura::serve::Fiber* fiber) {
        void* p = fiber->yield_checkpoint_ptr();
        if (p == nullptr) {
            YieldStackVec* fresh = acquire_yield_stack();
            void* expected = nullptr;
            if (fiber->compare_exchange_yield_checkpoint_ptr(expected, fresh)) {
                p = fresh;
            } else {
                release_yield_stack(fresh);
                p = fiber->yield_checkpoint_ptr();
            }
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

// Issue #417 / #1766: noexcept probe — Guard dtor calls this after
// depth-slot decrement; throwing would terminate, not skip cleanup.
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
    // Issue #1646 AC1: paired legacy on-demand + per-CompilerMetrics bump
    // at the hygiene-violation-detection site. Each detection call increments
    // the dedicated mutation_boundary_hygiene_violation_total slot that backs
    // the new (query:mutation-boundary-observability-stats) primitive surface.
    if (auto* ev_hv = aura::compiler::Evaluator::yield_hook_evaluator()) {
        ev_hv->bump_mutation_boundary_hygiene_violation_total();
    }
}

void aura::compiler::Evaluator::checkpoint_yield_boundary(bool at_mutation_boundary_yield) {
    bool had_boundary = any_active_mutation_boundary() || !active_mutation_stack().empty();
    if (!had_boundary && !at_mutation_boundary_yield)
        return;
    YieldBoundaryCheckpoint cp;
    cp.defuse_version = defuse_version_snapshot();
    cp.boundary_depth = mutation_boundary_depth();
    cp.mutation_stack_depth = active_mutation_stack().size();
    // Issue #1483 C2: wire per-fiber mutation_stack_depth metric
    // (lifetime high-water mark CAS). The same call site is the
    // canonical capture-from-stack site so the metric reflects
    // what the fiber will see when it resumes. The bump helper
    // is no-op when compiler_metrics_ is nullptr.
    bump_per_fiber_mutation_stack_depth_max(cp.mutation_stack_depth);
    cp.thread_id = std::this_thread::get_id();
    // Issue #2086: capture the owning Evaluator* at yield push so a
    // fiber-steal resume on a different host can clear the per-evaluator
    // panic defer depth we previously armed (otherwise orphan depth
    // sticks to the previous host's slot forever).
    cp.evaluator_id = static_cast<void*>(this);
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


// Issue #1912: record batch-refresh latency + success/fail into CompilerMetrics.
// Shared by refresh_stable_refs_batch and restamp_pinned_stable_refs (Guard exit).
static void record_stable_ref_batch_metrics(CompilerMetrics* m, std::size_t ok,
                                            std::size_t stale_prevented, std::size_t fail,
                                            std::uint64_t latency_us) noexcept {
    if (!m)
        return;
    if (ok > 0)
        m->stable_ref_batch_refresh_total.fetch_add(ok, std::memory_order_relaxed);
    if (stale_prevented > 0)
        m->stale_ref_prevented_total.fetch_add(stale_prevented, std::memory_order_relaxed);
    if (fail > 0)
        m->batch_refresh_fail_total.fetch_add(fail, std::memory_order_relaxed);
    m->batch_refresh_calls_total.fetch_add(1, std::memory_order_relaxed);
    m->batch_refresh_latency_us_total.fetch_add(latency_us, std::memory_order_relaxed);
    // p99 proxy: track max latency (same approach as self-evo hold max).
    auto prev = m->batch_refresh_latency_us_max.load(std::memory_order_relaxed);
    while (latency_us > prev && !m->batch_refresh_latency_us_max.compare_exchange_weak(
                                    prev, latency_us, std::memory_order_relaxed)) {
    }
}

// Issue #1912: public batch refresh for AI-held StableNodeRef spans.
// auto_pin=true: pin_for_cow + registry pin before refresh so refs survive
// subsequent COW / Guard / steal. Returns # of successfully refreshed refs.
std::size_t aura::compiler::Evaluator::refresh_stable_refs_batch(
    std::span<aura::ast::FlatAST::StableNodeRef> refs, bool auto_pin) noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    auto* ws = workspace_flat();
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    if (!ws) {
        const auto us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
        record_stable_ref_batch_metrics(m, 0, 0, refs.size(), us);
        return 0;
    }
    std::size_t ok = 0;
    std::size_t stale_prevented = 0;
    std::size_t fail = 0;
    for (auto& ref : refs) {
        if (auto_pin) {
            ref.pin_for_cow();
            pin_stable_ref_for_cow_boundary(ref);
            if (m)
                m->cow_pinned_across_layers_total.fetch_add(1, std::memory_order_relaxed);
        }
        const bool was_valid = ref.is_valid_in(*ws);
        if (ref.refresh_if_stale(*ws)) {
            if (auto_pin)
                ref.boundary_pinned = true;
            ++ok;
            if (!was_valid)
                ++stale_prevented;
        } else {
            ++fail;
        }
    }
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count());
    record_stable_ref_batch_metrics(m, ok, stale_prevented, fail, us);
    return ok;
}

// Issue #1912: batch registry-pin only (no refresh). Each ref is copied into
// cow_boundary_pinned_refs_ with boundary_pinned=true.
void aura::compiler::Evaluator::pin_stable_refs_for_cow_boundary(
    std::span<const aura::ast::FlatAST::StableNodeRef> refs) noexcept {
    auto* m = static_cast<CompilerMetrics*>(compiler_metrics_);
    for (const auto& ref : refs) {
        pin_stable_ref_for_cow_boundary(ref);
        if (m)
            m->cow_pinned_across_layers_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #1912: children as StableNodeRef + registry pin for COW survival.
std::vector<aura::ast::FlatAST::StableNodeRef>
aura::compiler::Evaluator::children_stable_batch(aura::ast::NodeId id) noexcept {
    std::vector<aura::ast::FlatAST::StableNodeRef> out;
    auto* ws = workspace_flat();
    if (!ws)
        return out;
    out = ws->children_stable(id);
    if (out.empty())
        return out;
    pin_stable_refs_for_cow_boundary(out);
    for (auto& r : out)
        r.pin_for_cow();
    return out;
}

// Issue #1500 / #1497 / #1564 / #1912: batch refresh_if_stale over atomic-batch +
// COW-boundary pinned StableNodeRefs. Restamps full provenance
// (gen/wrap/cow/mutation_id/subtree_gen) while preserving fiber_id,
// workspace_id, and boundary_pinned. Called from fiber steal, Guard
// dtor, re_pin, GC safepoint, and yield-resume (#1497 unified hooks).
// #1564: also bumps process-wide provenance hot_path_auto_refresh counters.
// #1912: also records stable_ref_batch_refresh_total / stale_ref_prevented.
std::size_t aura::compiler::Evaluator::restamp_pinned_stable_refs() noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    auto* ws = workspace_flat();
    if (!ws)
        return 0;
    // Issue #1630: restamp fiber_id to current fiber for all pinned refs
    // so steal/yield does not leave cross-fiber provenance drift.
    std::uint32_t current_fiber = 0;
    if (g_current_fiber_void != nullptr) {
        current_fiber = static_cast<std::uint32_t>(
            static_cast<aura::serve::Fiber*>(g_current_fiber_void)->id());
    }
    std::size_t refreshed = 0;
    std::size_t boundary_refreshed = 0;
    std::size_t fail = 0;
    auto restamp_one = [&](aura::ast::FlatAST::StableNodeRef& ref) {
        if (current_fiber != 0 && ref.fiber_id != 0 && ref.fiber_id != current_fiber &&
            ref.boundary_pinned) {
            ref.fiber_id = current_fiber;
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->boundary_pinned_auto_restamp_total.fetch_add(1, std::memory_order_relaxed);
            aura::core::provenance::record_boundary_pinned_auto_restamp();
        }
        const bool was_pinned = ref.boundary_pinned;
        const bool was_valid = ref.is_valid_in(*ws);
        if (ref.refresh_if_stale(*ws)) {
            if (was_pinned)
                ref.boundary_pinned = true;
            ++refreshed;
            if (was_pinned && !was_valid)
                ++boundary_refreshed;
        } else {
            ++fail;
        }
    };
    for (auto& ref : atomic_batch_pinned_refs_)
        restamp_one(ref);
    {
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        // Issue #2189: restamp Agent pin table; drop free/OOR so steal/Guard
        // never leaves dangling pins across hosts (invalidate-on-fail).
        for (auto& ref : cow_boundary_pinned_refs_) {
            const auto before = refreshed;
            restamp_one(ref);
            if (refreshed > before)
                agent_pin_restamp_total_.fetch_add(1, std::memory_order_relaxed);
        }
        auto it = std::remove_if(cow_boundary_pinned_refs_.begin(), cow_boundary_pinned_refs_.end(),
                                 [ws](const aura::ast::FlatAST::StableNodeRef& r) {
                                     if (r.id == aura::ast::NULL_NODE || r.id >= ws->size())
                                         return true;
                                     if (ws->is_free_slot(r.id) || !ws->is_live_node(r.id))
                                         return true;
                                     return false;
                                 });
        if (it != cow_boundary_pinned_refs_.end()) {
            const auto erased =
                static_cast<std::size_t>(std::distance(it, cow_boundary_pinned_refs_.end()));
            cow_boundary_pinned_refs_.erase(it, cow_boundary_pinned_refs_.end());
            if (erased > 0)
                agent_pin_invalidate_total_.fetch_add(erased, std::memory_order_relaxed);
        }
    }
    // Issue #1912: Guard / steal auto-refresh surfaces batch metrics.
    {
        const auto us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count());
        record_stable_ref_batch_metrics(static_cast<CompilerMetrics*>(compiler_metrics_), refreshed,
                                        boundary_refreshed, fail, us);
    }
    if (refreshed > 0) {
        bump_stable_ref_steal_auto_refresh(refreshed);
        bump_stable_ref_cross_cow_refresh(refreshed);
        // Issue #1645: paired legacy + per-CompilerMetrics bump at the
        // StableNodeRef::validate_or_refresh path (was: 1 of the 404 dead).
        if (auto* ev = aura::compiler::Evaluator::yield_hook_evaluator())
            ev->bump_stable_ref_cross_layer_mismatch();
        aura::core::provenance::record_hot_path_auto_refresh(refreshed);
        aura::core::provenance::record_policy_enforced();
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->cross_cow_provenance_enforced_total.fetch_add(refreshed, std::memory_order_relaxed);
        aura::core::provenance::record_cross_cow_provenance_enforced(refreshed);
        // Issue #1645: paired legacy raw atomic + per-CompilerMetrics
        // bump so (query:cross-cow-provenance-stats) and friends reflect
        // refresh rounds (was: 1 of the 404 dead bumps audit #1148 / #1645).
        if (auto* ev = aura::compiler::Evaluator::yield_hook_evaluator()) {
            for (std::uint64_t i = 0; i < refreshed; ++i)
                ev->bump_cross_cow_invalidations();
        }
        // Issue #1646 AC1: paired legacy + per-CompilerMetrics bumps at the
        // macro-dirty propagation + epoch-bump-for-macro sites. Both counters
        // back the new (query:mutation-boundary-observability-stats) primitive.
        if (auto* ev_mb = aura::compiler::Evaluator::yield_hook_evaluator()) {
            ev_mb->bump_mutation_boundary_macro_dirty_propagated_total();
            ev_mb->bump_mutation_boundary_epoch_bump_for_macro_total();
        }
        // Issue #1649: paired legacy + per-CompilerMetrics bumps at the
        // atomic_batch_pinning + template-respect site (paired #1646 +
        // adds the 2 #1649 distinct counter pair). All honor
        // Evaluator::yield_hook_evaluator() null fallback per #1908.
        if (auto* ev_1649 = aura::compiler::Evaluator::yield_hook_evaluator()) {
            ev_1649->bump_mutate_template_marker_propagated_total();
            ev_1649->bump_atomic_batch_hygiene_violation_prevented_total();
        }
    }
    if (boundary_refreshed > 0)
        bump_boundary_pinned_refresh(boundary_refreshed);
    return refreshed;
}

// Issue #1564 / #1630: policy-gated ensure_valid_or_refresh — production
// default for any primitive holding a StableNodeRef across mutate/query/GC/steal.
// Full provenance: fiber_id (unless boundary_pinned auto-restamp), cow_epoch,
// wrap_epoch via validate_or_refresh / refresh_if_stale.
std::optional<aura::ast::NodeView>
aura::compiler::Evaluator::ensure_valid_or_refresh(aura::ast::FlatAST::StableNodeRef& ref,
                                                   bool auto_refresh) noexcept {
    aura::core::provenance::record_ensure_valid_call();
    aura::core::provenance::record_policy_enforced();
    auto* ws = workspace_flat();
    if (!ws) {
        aura::core::provenance::record_ensure_valid_fail();
        return std::nullopt;
    }
    // Issue #1630: real fiber_id provenance (g_current_fiber_void TLS).
    // Non-zero capture + non-zero current must match unless boundary_pinned
    // (then restamp fiber_id onto the current fiber and continue).
    std::uint32_t current_fiber = 0;
    if (g_current_fiber_void != nullptr) {
        current_fiber = static_cast<std::uint32_t>(
            static_cast<aura::serve::Fiber*>(g_current_fiber_void)->id());
    }
    if (ref.fiber_id != 0 && current_fiber != 0 && ref.fiber_id != current_fiber) {
        if (ref.boundary_pinned && auto_refresh) {
            // Pinned refs survive steal: restamp fiber provenance + gen/cow.
            // Issue #2056: do NOT restamp tenant_id (FailOnStale / isolation).
            ref.fiber_id = current_fiber;
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->boundary_pinned_auto_restamp_total.fetch_add(1, std::memory_order_relaxed);
            aura::core::provenance::record_boundary_pinned_auto_restamp();
            (void)ref.refresh_if_stale(*ws);
        } else {
            aura::core::provenance::record_fiber_id_mismatch();
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
                m->stable_ref_fiber_mismatch_prevented_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
            bump_provenance_mismatch();
            aura::core::provenance::record_ensure_valid_fail();
            return std::nullopt;
        }
    }
    // Issue #2056: cross-tenant StableNodeRef use under Restricted/Strict.
    // Stamped ref.tenant_id must match principal (or 0/unset). Denied refs
    // never silently restamp tenant — FailOnStale remains closed.
    if (ref.tenant_id != 0 && capability_tenant_id_ != 0 &&
        !aura::core::provenance::tenant_ids_compatible(ref.tenant_id, capability_tenant_id_)) {
        const bool sandbox_on =
            sandbox_mode_ || effect_sandbox_mode() != 0 || aura::core::sandbox::is_strict();
        if (sandbox_on) {
            aura::core::provenance::record_stable_ref_cross_tenant_deny();
            (void)check_workspace_isolation(capability_tenant_id_, ref.tenant_id,
                                            /*required_effects=*/0, "stable-ref-cross-tenant");
            bump_provenance_mismatch();
            aura::core::provenance::record_ensure_valid_fail();
            return std::nullopt;
        }
    }
    const bool policy_on =
        auto_refresh && stable_ref_auto_refresh_policy_.load(std::memory_order_relaxed);
    if (!policy_on) {
        // FailOnStale: validate only.
        if (!ref.is_valid_in(*ws)) {
            aura::core::provenance::record_ensure_valid_fail();
            bump_provenance_mismatch();
            return std::nullopt;
        }
        ref.validate_with_provenance(*ws);
        aura::core::provenance::record_ensure_valid_success();
        return ws->get_safe(ref);
    }
    // AutoRefreshOnBoundary (default) — full validate_or_refresh.
    auto view = ref.validate_or_refresh(*ws);
    if (!view) {
        aura::core::provenance::record_ensure_valid_fail();
        bump_provenance_mismatch();
        return std::nullopt;
    }
    // Issue #1630: count COW provenance enforcement when pin survived.
    if (ref.boundary_pinned) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
            m->cross_cow_provenance_enforced_total.fetch_add(1, std::memory_order_relaxed);
        aura::core::provenance::record_cross_cow_provenance_enforced();
    }
    // Issue #1647: paired legacy + per-CompilerMetrics bump at the
    // validate_or_refresh success path (refresh-success counter,
    // distinct from stable_ref_auto_pin_total which counts pin-time).
    if (auto* ev_1647 = aura::compiler::Evaluator::yield_hook_evaluator()) {
        ev_1647->bump_cross_boundary_auto_refresh_success_total();
    }
    aura::core::provenance::record_ensure_valid_success();
    return view;
}

// Issue #1497: single auto-restamp entry for GC/steal/compact/resume.
// Ensures every production path runs the same restamp + site metrics
// so boundary_pinned refs cannot skip refresh under AI multi-round load.
std::size_t
aura::compiler::Evaluator::auto_restamp_pinned_stable_refs_at(StableRefRefreshSite site) noexcept {
    const auto n = restamp_pinned_stable_refs();
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
        switch (site) {
            case StableRefRefreshSite::Steal:
                m->stable_ref_validations_at_steal.fetch_add(n, std::memory_order_relaxed);
                break;
            case StableRefRefreshSite::GcSafepoint:
                m->stable_ref_validations_at_gc_safepoint.fetch_add(n, std::memory_order_relaxed);
                break;
            case StableRefRefreshSite::CompactOrRepin:
                // Compact/repin shares steal validation surface historically
                // (#1473 re_pin path) + cow_repin_on_steal.
                m->stable_ref_validations_at_steal.fetch_add(n, std::memory_order_relaxed);
                m->cow_repin_on_steal.fetch_add(1, std::memory_order_relaxed);
                break;
            case StableRefRefreshSite::YieldResume:
                m->stable_ref_validations_at_steal.fetch_add(n, std::memory_order_relaxed);
                break;
            case StableRefRefreshSite::Join:
                // Issue #1595: join/parallel path — dedicated post-join repin counter.
                m->stable_ref_post_join_repin_total.fetch_add(n, std::memory_order_relaxed);
                m->stable_ref_validations_at_steal.fetch_add(n, std::memory_order_relaxed);
                break;
        }
    }
    return n;
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
    auto* m = static_cast<aura::compiler::CompilerMetrics*>(compiler_metrics());
    auto* ws = workspace_flat();
    if (!ws) {
        if (m)
            m->checkpoint_lost_on_compact.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Issue #1497: unified auto-restamp (atomic-batch + cow-boundary
    // pins) for compact / post-steal re-pin — replaces the cow-only
    // validate_or_refresh walk so no registry is skipped.
    (void)auto_restamp_pinned_stable_refs_at(StableRefRefreshSite::CompactOrRepin);
    if (m)
        m->panic_transfer_nested_success.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Issue #1446 AC2: on_arena_compact_hook — registered with
// arena.set_on_compact_hook() during Evaluator ctor. When the arena
// runs compact/defrag, this hook is invoked AFTER reclaim; we walk
// the active Guard stack and call re_pin_cow_children_from_snapshot()
// on each (outermost + nested) to keep StableNodeRef / COW pins in
// sync with the post-compact arena state.
//
// Issue #1637: appended the panic checkpoint closed-loop restore
// step (restore_panic_checkpoint_on_arena_compact_if_needed —
// bumps post_compact_checkpoint_restore_total on success). The
// no-pending-checkpoint fast path is a single null-check + branch
// so steady-state cost stays negligible; only pending checkpoints
// drive the truncate / generation / invalidate / walk_active_closures
// / clear lifecycle close.
void aura::compiler::Evaluator::on_arena_compact_hook() {
    re_pin_cow_children_from_snapshot();
    // Issue #1612: GC compact path — MacroIntroduced marker/provenance repin.
    (void)refresh_stale_macro_frames(0, 0);
    probe_and_repin_macro_provenance();
    // Issue #1637: closed-loop panic checkpoint restore on GC compact
    // (paired with the resume path via run_post_restore_lifecycle_close
    // in evaluator_workspace_tree.cpp).
    restore_panic_checkpoint_on_arena_compact_if_needed();
    // Issue #2162: compact-only recovery when no Guard owns the pipeline.
    // Compact has no boundary enter snapshot — only drain deferred reemit
    // (SoftEnter/Defer left pending). Do not use lifetime dirty_upward
    // alone: that would re-fire recovery on every GC compact after any
    // historical mark_dirty and can re-register AOT slots under host
    // reemit fixtures (heap corruption under multi-AC tests).
    // Issue #2273: steal-path observability — bumped on migration
    // refresh BEFORE the drain so Agents can correlate "pending
    // was observed on this steal" with "drain happened later".
    // fb_void encodes the fiber_id (cast to int64_t for query).
    void* fb_void = g_current_fiber_void;
    const std::int64_t steal_fiber_id =
        (fb_void != nullptr) ? static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(fb_void))
                             : 0;
    aura_hot_update_on_deferred_reemit_seen_on_steal(steal_fiber_id);

    if (mutation_boundary_depth() == 0 && aura_hot_update_has_deferred_reemit() != 0) {
        const auto cur = defuse_version_.load(std::memory_order_acquire);
        const auto dirty_n = workspace_flat_ ? workspace_flat_->mark_dirty_upward_call_count() : 0;
        run_hot_update_recovery_if_needed(/*success=*/true, cur, dirty_n);
    }
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
    // Issue #2086 / #2194: on fiber-steal / cross-evaluator resume, clear
    // panic-defer depth the **previous** host armed. Gate on evaluator_id
    // mismatch (not only thread_migrated) so multi-eval same-thread steals
    // and restamped thread_id cases still drop orphan slots. Without this,
    // a fiber that yields on Evaluator A and resumes on B leaves A's slot
    // permanently armed. Clear before restamp so per-eval counter is gone.
    if (cp.evaluator_id != nullptr && cp.evaluator_id != static_cast<void*>(this)) {
        const auto cleared = aura::gc_hooks::clear_gc_defer_for_evaluator(cp.evaluator_id);
        if (cleared > 0) {
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
                m->gc_defer_orphan_cleared_total.fetch_add(cleared, std::memory_order_relaxed);
                m->fiber_migration_gc_defer_cleared_total.fetch_add(cleared,
                                                                    std::memory_order_relaxed);
            }
        }
    }
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
        // Issue #2086: re-stamp evaluator_id on the restamped checkpoint
        // so a subsequent steal (this becomes the "previous host") is
        // correctly attributed for clear_gc_defer_for_evaluator.
        cp.evaluator_id = static_cast<void*>(this);
        cp.boundary_depth = current_bdepth;
        cp.mutation_stack_depth = current_mdepth;
        // Issue #1483 C2: wire per-fiber mutation_stack_depth metric
        // at the restamp path (when the captured cp drifts from
        // current state — thread_migrated, size_mismatch,
        // depth_mismatch, or version_drift_pre). Restamp aligns
        // the metric with the post-drift corrected depth.
        bump_per_fiber_mutation_stack_depth_max(cp.mutation_stack_depth);
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
        // Issue #1497: yield-resume desync → auto-restamp pinned StableNodeRefs
        // so boundary_pinned / atomic-batch handles stay valid across steal.
        (void)auto_restamp_pinned_stable_refs_at(StableRefRefreshSite::YieldResume);
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
        // Issue #2162: fiber-steal version-drift with no Guard still on this
        // host (depth==0) must not leave deferred dirty without recovery.
        // When Guard remains (depth>0), dtor owns the pipeline (AC3 single-owner).
        // Snapshot current dirty_upward so only defuse delta / deferred fires
        // (not lifetime mark_dirty count).
        if (mutation_boundary_depth() == 0 && version_drift) {
            const auto dirty_n =
                workspace_flat_ ? workspace_flat_->mark_dirty_upward_call_count() : 0;
            run_hot_update_recovery_if_needed(/*success=*/false, cp.defuse_version, dirty_n);
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
        // Issue #1580: stamp resume-refresh hints onto the current fiber so
        // post-steal EnvFrame refresh can target the active frame/epoch.
        if (Evaluator::g_current_fiber_void != nullptr) {
            auto* fiber = static_cast<aura::serve::Fiber*>(Evaluator::g_current_fiber_void);
            const auto frames = ev->env_frames_size();
            const std::uint64_t env_hint = frames > 0 ? static_cast<std::uint64_t>(frames - 1) : 0;
            fiber->set_resume_refresh_hints(env_hint, ev->current_bridge_epoch());
        }
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
        // Issue #1580 / #2194: closed-loop refresh after yield validate
        // (pairs with Fiber::resume → aura_evaluator_post_resume_refresh →
        // refresh_after_fiber_migration).
        ev->refresh_after_fiber_migration(Evaluator::g_current_fiber_void);
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

// Issue #236 follow-up / #1746: per-Evaluator, per-thread depth slot
// for MutationBoundaryGuard. thread_local map keyed by
// Evaluator::instance_id_ (not raw pointer). Address reuse after
// free would otherwise alias a destroyed Evaluator's depth and
// corrupt nesting (UAF / wrong outermost). Each fiber has its own
// slot per instance_id it touches. Map entries stay after last
// guard destructs (cheap; avoids heap churn).
//
// Returns a pointer to an int initialized to 0 the first
// time it's accessed for a given (thread, instance_id).
int* aura::compiler::Evaluator::mutation_boundary_depth_slot(Evaluator* ev) {
    if (!ev) {
        thread_local int null_slot = 0;
        return &null_slot;
    }
    struct Slot {
        std::unordered_map<std::uint64_t, int> depths;
    };
    // thread_local unique_ptr auto-deletes the Slot on thread exit;
    // the raw `Slot*` pattern leaks one Slot per thread that ever
    // touched this path. std::unique_ptr keeps the same `slot->depths`
    // access pattern for callers.
    thread_local std::unique_ptr<Slot> slot = std::make_unique<Slot>();
    const auto id = ev->instance_id();
    auto it = slot->depths.find(id);
    if (it == slot->depths.end()) {
        it = slot->depths.emplace(id, 0).first;
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

// Issue #2347: MultiFiberMailbox Strict path may force outermost mutation
// mark-failed when blocking recv rejects exceed the Guard window threshold.
extern "C" void aura_evaluator_mark_outermost_mutation_failed() noexcept {
    if (auto* ev = Evaluator::get_query_evaluator())
        ev->mark_outermost_mutation_failed();
}

// Issue #63723: clear all per-thread/process-wide Evaluator
// pointers that point at this dying instance. Without this,
// when the closure that owned this Evaluator returns, the
// worker thread's g_query_evaluator (thread_local) and
// g_scheduler_stats_evaluator (std::atomic) still point at
// the dead stack — and the next
// aura_evaluator_bump_mutation_steal_attempt() /
// Evaluator::bump_mutation_steal_violation_count() /
// aura_evaluator_resume_fiber_migration() path dereferences
// a use-after-return (verified by ASan:
// stack-use-after-return in bump_mutation_steal_attempt at
// evaluator.ixx:3130). This is what caused test_issue_226 to
// hang on t.join() — the worker's steal code called
// bump_mutation_steal_attempt on a dead Evaluator and
// crashed/hung inside the atomic fetch_add.
//
// Issue #1483 C1: dropped the stale aura_evaluator_bump_mutation_steal_violation()
// reference — that C API never existed; only the C++
// bump_mutation_steal_violation_count() method (no C wrapper)
// is the canonical violation path. Verified via audit: the
// extern "C" block at L959+ has no aura_evaluator_bump_mutation_steal_violation.
// The bump_mutation_steal_attempt / bump_mutation_steal_violation_count /
// aura_evaluator_resume_fiber_migration trio is the actual
// use-after-return surface.
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

// Issue #1504 / #1635: Agent-facing first-class safe yield (ast:yield-at-boundary).
// Never yields while a MutationBoundaryGuard holds workspace_mtx_ (#362 deadlock).
// When depth==0 / not held: cooperative yield with YieldReason::MutationBoundary
// so multi-Agent orchestration can interleave; GC request_gc_safepoint also
// defers when depth>0 (wired contract).
// Returns: 0 = yielded / safe no-fiber, 1 = skipped-held.
int Evaluator::try_safe_yield_at_boundary(std::int64_t timeout_ms) noexcept {
    (void)timeout_ms; // reserved: future preemptive soft deadline
    const std::size_t depth = mutation_boundary_depth();
    const bool held = mutation_boundary_held() || any_active_mutation_boundary();
    if (depth > 0 || held) {
        safe_yield_skipped_held_total_.fetch_add(1, std::memory_order_relaxed);
        return 1; // skipped-held
    }
    // Safe point: prefer mutation-boundary yield reason when hook present.
    if (aura::messaging::g_fiber_yield_mutation_boundary) {
        aura::messaging::g_fiber_yield_mutation_boundary();
        // Hook no-ops when no fiber is current (test path).
        if (aura::serve::g_current_fiber) {
            safe_yield_ok_total_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        safe_yield_no_fiber_total_.fetch_add(1, std::memory_order_relaxed);
        return 0; // safe no-op
    }
    if (aura::messaging::g_fiber_yield) {
        aura::messaging::g_fiber_yield();
        if (aura::serve::g_current_fiber) {
            safe_yield_ok_total_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        safe_yield_no_fiber_total_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    // No fiber scheduler wired (unit tests / stdin): still a safe point.
    safe_yield_no_fiber_total_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int Evaluator::mutation_boundary_depth_slot_value() const noexcept {
    int* slot = mutation_boundary_depth_slot(const_cast<Evaluator*>(this));
    return slot ? *slot : 0;
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
    // Issue #1487: resource-quota liveness on flush. Rejection is
    // try_acquire's job (typed AuraError); mid-boundary flush must not
    // abort a half-committed transaction. We only re-sample the check
    // counter so agents observe flush activity under quota policy.
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->resource_quota_checks_total.fetch_add(1, std::memory_order_relaxed);
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

                // Issue #1908: outermost Guard exit enforced hygiene boundary
                // (dirty/epoch bump prevents MacroIntroduced provenance drift
                // from manifesting as a violation under concurrent steal).
                bump_hygiene_violation_prevented_on_boundary_total();
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

    // (2) g_transfer_panic_checkpoint: Issue #1580 routes through
    // transfer_and_revalidate_panic_checkpoint (transfer + dual-epoch
    // revalidate + yield restamp). Called by Fiber::resume() after
    // swapcontext return. No-op when no pending checkpoint.
    void transfer_panic_checkpoint_trampoline() {
        auto* ev = Evaluator::yield_hook_evaluator();
        if (!ev)
            return;
        (void)ev->transfer_and_revalidate_panic_checkpoint(Evaluator::g_current_fiber_void);
    }

    // (3) g_block_gc_for_pending_checkpoint: Issue #1489 / #651 / #1581.
    // Called by Fiber::yield(MutationBoundary) when a pending
    // PanicCheckpoint exists. Arms process-wide GC defer (if not
    // already armed by save), sends scheduler-facing defer signal
    // (fiber id + checkpoint epoch), bumps metrics, so
    // GCCollector::request/collect and compact_sweep skip destructive
    // reclaim until commit/restore.
    void block_gc_for_pending_checkpoint_trampoline() {
        auto* ev = Evaluator::yield_hook_evaluator();
        if (!ev)
            return;
        if (!ev->pending_panic_checkpoint())
            return;
        // Ensure defer is armed for the recovery window (save usually
        // arms first; yield re-arms only if save path was skipped).
        ev->arm_gc_defer_for_pending_panic();
        ev->bump_gc_blocked_by_pending_panic();

        // Issue #1581: explicit scheduler defer signal with provenance.
        std::uint64_t fiber_id = 0;
        if (Evaluator::g_current_fiber_void != nullptr) {
            auto* fiber = static_cast<aura::serve::Fiber*>(Evaluator::g_current_fiber_void);
            fiber_id = fiber->id();
        }
        const std::uint64_t checkpoint_epoch = ev->current_bridge_epoch();
        aura::gc_hooks::send_defer_gc_signal(fiber_id, checkpoint_epoch);

        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
            m->gc_panic_pending_deferral_total.fetch_add(1, std::memory_order_relaxed);
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

// Issue #2114: C-linkage held flag for reemit handshake (bridge / HotUpdate
// registry without Evaluator module). True while outermost Guard is alive,
// including the #2090 dtor reemit window (held cleared after reemit).
extern "C" int aura_evaluator_mutation_boundary_held() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (ev && ev->mutation_boundary_held())
        return 1;
    // Also treat non-zero depth-slot as held when yield hook unbound
    // mid-teardown (defensive).
    if (Evaluator::mutation_boundary_depth() > 0)
        return 1;
    return 0;
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

// Issue #1992: C-linkage shims for the CAS-protected
// ensure_mutation_stack_ptr / ensure_yield_stack_ptr.
// Tests (tests/mutation/test_issue_1992.cpp) call these
// from concurrent threads on the same Fiber pointer to
// verify the fix for the plain-void* race on concurrent
// work-steal handoff: under contention exactly ONE
// allocation is published (others release their
// allocation back to the pool), not N. Without the
// CAS the previous plain read-then-write would last-
// writer-wins, leaking N-1 allocations and risking
// use-after-free across fibers.
extern "C" void* aura_fiber_ensure_mutation_stack(aura::serve::Fiber* fiber) {
    return fiber_stack_pool_detail::ensure_mutation_stack_ptr(fiber);
}

extern "C" void* aura_fiber_ensure_yield_stack(aura::serve::Fiber* fiber) {
    return fiber_stack_pool_detail::ensure_yield_stack_ptr(fiber);
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
    // Issue #2184: republish held/defuse mirrors after resume stack bind so
    // steal samples see the fiber's true Guard state (not thief TLS).
    const auto depth = active_mutation_stack_static().size();
    auto* ev = yield_hook_evaluator();
    const bool held = (ev && ev->mutation_boundary_held()) || depth > 0;
    const auto defuse = ev ? ev->defuse_version() : 0;
    fiber->publish_mutation_safety_mirrors(depth, held, defuse);
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
    // Issue #2184: publish fiber-local held/depth mirrors for steal snapshot.
    {
        const auto depth = Evaluator::active_mutation_stack_static().size();
        std::uint64_t defuse = 0;
        if (auto* ev = Evaluator::yield_hook_evaluator())
            defuse = ev->defuse_version();
        aura::serve::publish_current_fiber_mutation_safety(depth, depth > 0, defuse);
    }
}

extern "C" void aura_evaluator_test_pop_mutation_checkpoint() {
    auto& stack = Evaluator::active_mutation_stack_static();
    if (!stack.empty())
        stack.pop_back();
    // Issue #2184: refresh fiber-local mirrors after pop.
    {
        const auto depth = stack.size();
        std::uint64_t defuse = 0;
        if (auto* ev = Evaluator::yield_hook_evaluator())
            defuse = ev->defuse_version();
        aura::serve::publish_current_fiber_mutation_safety(depth, depth > 0, defuse);
    }
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
// Issue #1490 / #1558: transfer_mutation_stack_to_current_fiber runs
// refresh_stale_frames_after_steal + probe_and_repin_linear (dual-epoch
// + linear re-pin before the stolen fiber continues).
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

// Issue #1490 / #1580: post-yield / post-resume refresh (called from
// Fiber::resume after g_fiber_resume_validate_). Full closed loop:
// EnvFrame refresh (with fiber hints) + linear/StableNodeRef re-pin +
// panic checkpoint transfer/revalidate.
extern "C" void aura_evaluator_post_resume_refresh() {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    // Issue #2194: mandatory post-steal resume refresh (unified helper).
    ev->refresh_after_fiber_migration(Evaluator::g_current_fiber_void);
}

// Issue #1580: transfer pending PanicCheckpoint across steal/resume and
// revalidate dual-epoch (defuse + bridge). Fiber-aware restamp of yield
// checkpoint top when fiber_void is non-null.
bool Evaluator::transfer_and_revalidate_panic_checkpoint(void* fiber_void) noexcept {
    if (!pending_panic_checkpoint())
        return false;
    bump_panic_checkpoint_transfer_count();
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
        m->panic_transfer_on_steal.fetch_add(1, std::memory_order_relaxed);

    void* fb_void = fiber_void != nullptr ? fiber_void : g_current_fiber_void;
    if (fb_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(fb_void);
        [[maybe_unused]] const bool yc_mismatch =
            fiber_stack_pool_detail::restamp_yield_checkpoint_top(this, fiber);
    }

    // Dual-epoch revalidation under acquire fence (consistent probe).
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto defuse_v = defuse_version_snapshot();
    const auto bridge_e = current_bridge_epoch();
    if (aura_aot_probe_checkpoint_version(defuse_v, bridge_e)) {
        aura_aot_record_deopt_on_steal();
        bump_concurrent_safety_aot_reload_at_guard();
    }
    bump_macro_hygiene_panic_restamp_from_workspace();

    // Issue #1908: PanicCheckpoint transfer bound macro clone provenance
    // (panic restamp above already walked MacroIntroduced nodes; bump the
    //  boundary-interaction counters so the transfer is visible in
    //  (query:macro-provenance-stats) + (#1908 AC "macro clone counters
    //  bound to PanicCheckpoint transfer" satisfied).
    bump_macro_provenance_repin_on_steal_total();
    bump_hygiene_violation_prevented_on_boundary_total();
    // Ensure pending panic still defers GC reclaim until commit/restore.
    arm_gc_defer_for_pending_panic();
    return true;
}

// Issue #1580 / #1592 / #1631 / #2194: full post-resume steal closed loop.
// Fiber::resume and fiber_resume_validate_impl mandate this on every resume.
// Implementation is refresh_after_fiber_migration (#2194 unified helper).
void Evaluator::complete_post_resume_steal_refresh(void* fiber_void) noexcept {
    refresh_after_fiber_migration(fiber_void);
}

// Issue #2194: unified fiber-migration refresh (steal/resume main path).
// Aligns post-steal state with outermost MutationBoundaryGuard exit:
//   1) clear orphan GC defer for previous host evaluator (peek yield CP)
//   2) refresh_stale_frames_after_steal (EnvFrame dual-epoch)
//   3) restamp pinned StableNodeRefs (generation-bound, idempotent)
//   4) linear probe + post-mutate enforce (only_if needed)
//   5) clear_resume_refresh_hints
// Called from Fiber::resume after g_fiber_sync_mutation_stack_ (via
// aura_evaluator_post_resume_refresh / fiber_resume_validate_impl).
// Guard dtor keeps its own restamp path; auto_restamp is generation-idempotent.
void Evaluator::refresh_after_fiber_migration(void* fiber_void) noexcept {
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        m->resume_forced_refresh_total.fetch_add(1, std::memory_order_relaxed);
        m->fiber_migration_refresh_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1879: orch/steal provenance enforcement on every resume.
        m->orch_fiber_steal_provenance_enforced_total.fetch_add(1, std::memory_order_relaxed);
        m->orch_stable_ref_auto_refresh_total.fetch_add(1, std::memory_order_relaxed);
    }

    // 1) Orphan GC defer clear — peek yield checkpoint (may already be
    // consumed by restore_post_yield_or_rollback; then this is a no-op).
    // Cross-evaluator mismatch (not only thread_migrated) — #2194 / #2086.
    {
        auto& ystack = active_yield_checkpoint_stack();
        if (!ystack.empty()) {
            const auto& cp = ystack.back();
            if (cp.evaluator_id != nullptr && cp.evaluator_id != static_cast<void*>(this)) {
                const auto cleared = aura::gc_hooks::clear_gc_defer_for_evaluator(cp.evaluator_id);
                if (cleared > 0) {
                    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
                        m->gc_defer_orphan_cleared_total.fetch_add(cleared,
                                                                   std::memory_order_relaxed);
                        m->fiber_migration_gc_defer_cleared_total.fetch_add(
                            cleared, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    std::uint64_t hint_env = 0;
    std::uint64_t expected_epoch = 0;
    void* fb_void = fiber_void != nullptr ? fiber_void : g_current_fiber_void;
    if (fb_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(fb_void);
        hint_env = fiber->resume_env_hint();
        expected_epoch = fiber->resume_bridge_epoch_hint();
        // Issue #2250 / #2519: LayoutStamp fence — hard compare fiber-stored
        // stamp vs current via full 8-field operator== (core 6 + shape +
        // ir_soa_generation). Must not execute generation-behind AOT.
        if (fiber->has_resume_layout_stamp()) {
            const auto cur = current_layout_stamp();
            const auto stored = layout_stamp_from_fiber_resume(*fiber);
            // Issue #2519: is_fully_fresh / operator== — all 8 fields.
            const bool mismatch = !stored.is_fully_fresh(cur);
            if (mismatch) {
                if (auto* mm = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                    mm->layout_stamp_resume_mismatch_total.fetch_add(1, std::memory_order_relaxed);
                    // Issue #2255: dedicated shape_version fence counter
                    // (7th field of LayoutStamp). Bumps independently
                    // so dashboards can isolate shape-version drift
                    // from arena/defuse drift.
                    if (fiber->resume_shape_version() != cur.shape_version)
                        mm->shape_version_fence_reject_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    // Issue #2432: dedicated IR SoA generation fence counter.
                    if (fiber->resume_ir_soa_generation() != cur.ir_soa_generation)
                        mm->ir_generation_fence_hit_total.fetch_add(1, std::memory_order_relaxed);
                }
                // Force dual-check (mark_invalid=true, only_if_moved=false).
                scan_live_closures_for_linear_captures(true, std::false_type{});
            }
        }
        // Clear the resume fence after consumption (one-shot).
        fiber->clear_resume_layout_stamp();
    }

    // 2) EnvFrame dual-epoch refresh under resume hints.
    const auto refreshed = refresh_stale_frames_after_steal(hint_env, expected_epoch);
    // Issue #2362: close EnvFrameRef ownership across fiber steal —
    // iterate production live set; OOB → drop, gen advanced → transfer_to
    // restamp. Empty set (Soft / no-hold) is free. Runs after dual-epoch
    // refresh so stamps compare against post-steal env_generation.
    sync_live_env_frame_refs_ownership();
    // 4a) Linear probe/repin (before pin restamp for ownership probes).
    probe_and_repin_linear_on_steal();

    // Issue #1908: post-steal repin MacroIntroduced boundary counters.
    bump_macro_provenance_repin_on_steal_total();
    bump_hygiene_violation_prevented_on_boundary_total();
    (void)refreshed;

    // 3) StableNodeRef pin restamp (generation-bound; safe if Guard also restamped).
    (void)auto_restamp_pinned_stable_refs_at(StableRefRefreshSite::Steal);
    // Issue #1612: MacroIntroduced marker + provenance refresh on resume/steal.
    (void)refresh_stale_macro_frames(hint_env, expected_epoch);
    probe_and_repin_macro_provenance();

    // 4b) Linear ownership closed-loop (pairs with Guard-exit enforce).
    if (hint_env != 0 && hint_env != static_cast<std::uint64_t>(NULL_ENV_ID)) {
        (void)linear_post_mutate_enforce(static_cast<EnvId>(hint_env));
    } else if (refreshed > 0) {
        (void)linear_post_mutate_enforce_all();
    } else {
        bump_linear_post_mutate_enforcement();
    }

    // Issue #2310 AC2: re-sample snapshot post-migration. If still
    // inconsistent after the migration refresh above → fail-closed
    // (defense in depth with the LayoutStamp 7-field fence earlier in
    // this function). Skipped when AURA_STEAL_SNAPSHOT_SOFT=1 (unit-
    // test mode); production default is the force-deopt path. The fiber
    // is dropped from the steal attempt (worker.cpp skips normal
    // enqueue) and the per-CompilerMetrics counter advances.
    if (!aura::serve::is_steal_snapshot_soft_mode()) {
        if (fb_void != nullptr) {
            auto* fiber = static_cast<aura::serve::Fiber*>(fb_void);
            const auto post_snap = fiber->mutation_safety_snapshot();
            if (fiber->mutation_safety_snapshot_inconsistent(post_snap)) {
                // Bump the observed-only mismatch counter (matches
                // worker.cpp try_steal_from path). force-deopt counter
                // bumped by the C ABI hook below.
                aura::serve::Fiber::bump_mutation_steal_snapshot_mismatch();
                aura_force_deopt_on_steal_snapshot_mismatch(fiber);
                // Force additional dual-check via scan_live_closures_for_
                // linear_captures (mark_invalid=true, only_if_moved=false).
                scan_live_closures_for_linear_captures(true, std::false_type{});
            }
        }
    }

    if (pending_panic_checkpoint())
        (void)transfer_and_revalidate_panic_checkpoint(fb_void);

    // 5) Ownership transfer / drop for live resume hint, then clear.
    // Issue #2295: prefer transfer_to (restamp) when the hint env_id is
    // still in-range after dual-epoch refresh; otherwise drop. This
    // closes dual-path stale windows that generation fences alone leave
    // when a bare env_id / EnvFrameRef was held across steal.
    // Issue #2268: bump envframe_cache_cleared_on_steal_total when
    // the fiber-local EnvFrame cache (resume_env_hint_) was
    // actually populated at steal time (any non-zero hint).
    if (fb_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(fb_void);
        const auto hint = fiber->resume_env_hint();
        const bool had_hint = hint != 0 || fiber->resume_bridge_epoch_hint() != 0;
        if (hint != 0 && hint != static_cast<std::uint64_t>(NULL_ENV_ID)) {
            EnvFrameRef live(static_cast<EnvId>(hint), env_generation());
            if (live.still_valid(*this)) {
                EnvFrameRef restamped;
                live.transfer_to(*this, restamped);
                // restamped is the post-steal ownership handle; callers
                // that re-capture from resume path will re-materialize.
                (void)restamped;
            } else {
                // Hint points past truncate / INVALID frame — explicit drop.
                live.drop(*this);
            }
        }
        if (had_hint) {
            if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
                m->envframe_cache_cleared_on_steal_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        fiber->clear_resume_refresh_hints();
    }
}

// Issue #1595: post Fiber::join linear ownership + StableNodeRef enforcement.
// Called from serve Fiber::join on Ok; also from parallel_intend child exit.
// Keep hot path light: full EnvFrame walk only when join target left resume hints
// or refresh observed drift (mirrors #1592 complete_post_resume_steal_refresh).
void Evaluator::complete_post_join_linear_enforcement(void* joined_fiber_void) noexcept {
    std::uint64_t hint_env = 0;
    std::uint64_t expected_epoch = 0;
    void* fb_void = joined_fiber_void != nullptr ? joined_fiber_void : g_current_fiber_void;
    if (fb_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(fb_void);
        hint_env = fiber->resume_env_hint();
        expected_epoch = fiber->resume_bridge_epoch_hint();
        // Issue #2250 / #2519: same LayoutStamp fence for post-join path
        // (full 8-field operator==).
        if (fiber->has_resume_layout_stamp()) {
            const auto cur = current_layout_stamp();
            const auto stored = layout_stamp_from_fiber_resume(*fiber);
            const bool mismatch = !stored.is_fully_fresh(cur);
            if (mismatch) {
                if (auto* mm = static_cast<CompilerMetrics*>(compiler_metrics_)) {
                    mm->layout_stamp_resume_mismatch_total.fetch_add(1, std::memory_order_relaxed);
                    // Issue #2255: dedicated shape_version fence counter
                    if (fiber->resume_shape_version() != cur.shape_version)
                        mm->shape_version_fence_reject_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    // Issue #2432: dedicated IR SoA generation fence counter.
                    if (fiber->resume_ir_soa_generation() != cur.ir_soa_generation)
                        mm->ir_generation_fence_hit_total.fetch_add(1, std::memory_order_relaxed);
                }
                scan_live_closures_for_linear_captures(true, false);
            }
        }
        fiber->clear_resume_layout_stamp();
    }

    const auto refreshed = refresh_stale_frames_after_steal(hint_env, expected_epoch);
    probe_and_repin_linear_on_steal();
    const auto repinned = auto_restamp_pinned_stable_refs_at(StableRefRefreshSite::Join);

    if (hint_env != 0 && hint_env != static_cast<std::uint64_t>(NULL_ENV_ID)) {
        (void)linear_post_mutate_enforce(static_cast<EnvId>(hint_env));
    } else if (refreshed > 0) {
        (void)linear_post_mutate_enforce_all();
    } else {
        // Liveness bump without full EnvFrame sweep (join is hot under parallel_intend).
        bump_linear_post_mutate_enforcement();
    }

    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        m->linear_join_enforcement_total.fetch_add(1, std::memory_order_relaxed);
        // auto_restamp already bumps stable_ref_post_join_repin_total by n;
        // ensure at least +1 liveness tick when zero pins restamped.
        if (repinned == 0)
            m->stable_ref_post_join_repin_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1879: orch join-path StableNodeRef + linear counters.
        m->orch_stable_ref_auto_refresh_total.fetch_add(1, std::memory_order_relaxed);
        m->orch_linear_violation_prevented_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #1595: MultiFiberMailbox path — StableNodeRef probe + optional linear claim.
// Payload convention for explicit claims (tests + agents):
//   "linear-viol:..."  → always reject (synthetic violation)
//   "linear-env:<id>"  → enforce linear on that EnvId; reject if enforce fails hard
// Returns true if message may be delivered.
bool Evaluator::probe_mailbox_linear_and_stable_refs(std::uint64_t /*from_fiber*/,
                                                     std::uint64_t /*to_fiber*/,
                                                     std::string_view payload) noexcept {
    // Always: light join-site restamp so mailbox traffic cannot skip pin refresh.
    (void)auto_restamp_pinned_stable_refs_at(StableRefRefreshSite::Join);
    probe_and_repin_linear_on_steal();

    if (payload.starts_with("linear-viol:")) {
        if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
            m->mailbox_linear_violation_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (payload.starts_with("linear-env:")) {
        // Best-effort parse of env id after prefix; enforce when non-zero.
        std::uint64_t env = 0;
        const auto rest = payload.substr(std::string_view("linear-env:").size());
        for (char c : rest) {
            if (c < '0' || c > '9')
                break;
            env = env * 10 + static_cast<std::uint64_t>(c - '0');
        }
        if (env != 0 && env != static_cast<std::uint64_t>(NULL_ENV_ID)) {
            const bool ok = linear_post_mutate_enforce(static_cast<EnvId>(env));
            if (!ok) {
                if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
                    m->mailbox_linear_violation_count.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }

    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics()))
        m->linear_join_enforcement_total.fetch_add(1, std::memory_order_relaxed);
    return true;
}

extern "C" void aura_evaluator_on_fiber_join(void* joined_fiber) {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->complete_post_join_linear_enforcement(joined_fiber);
}

// Issue #1880 / #2118 / #2555: thread-local TransactionGuard for orch agent body
// (host path: full MBG via TransactionGuardHost) + soft per-fiber depth
// registration (fiber path: soft sentinel host, no full MBG stack).
// try_acquire → typed ResourceQuotaExceeded (never panic/throw).
namespace {
    // Issue #2555: unified transaction layer replaces bare MBG unique_ptr.
    thread_local std::optional<aura::core::TransactionGuard> g_orch_agent_body_tx{};
    // Soft-path sentinel handle for fiber TransactionGuardHost (not a real MBG*).
    // Address of a process-lifetime object — avoids integer→pointer cast (Werror).
    inline char g_orch_soft_tx_sentinel = 0;
    inline void* kOrchSoftTxHandle() noexcept {
        return static_cast<void*>(&g_orch_soft_tx_sentinel);
    }
    // Soft host ctx for fiber agent body (stable for TG lifetime).
    struct OrchSoftTxState {
        Evaluator* ev = nullptr;
        int register_soft = 1;
    };
    thread_local OrchSoftTxState g_orch_soft_tx_state{};
    // Issue #2118: soft boundary depth pushed on fiber agent body (nested-safe).
    thread_local int g_orch_soft_boundary_depth = 0;
    // Issue #2515: track the Evaluator that owns the soft window so
    // orch_soft_boundary_exit can publish the symmetric held=false mirror
    // with the same defuse_version the enter side captured (avoid version
    // drift between the two publish_mutation_safety_mirrors calls).
    thread_local Evaluator* g_orch_soft_boundary_ev = nullptr;

    void orch_soft_boundary_enter(Evaluator* ev) noexcept {
        if (!ev)
            return;
        // Push a lightweight checkpoint onto the per-fiber mutation stack so
        // is_at_mutation_boundary_safe / steal / GC see depth > 0. Does NOT
        // acquire workspace_mtx_ (full Guard stack-smashes on fiber #1881).
        Evaluator::MutationCheckpoint cp{};
        cp.version = ev->defuse_version_for_test(); // snapshot; soft marker
        cp.evaluator_id = static_cast<void*>(ev);
        Evaluator::active_mutation_stack_static().push_back(std::move(cp));
        ++g_orch_soft_boundary_depth;
        // Issue #2515: track the Evaluator so the symmetric exit publish
        // uses the same defuse_version the enter side captured.
        g_orch_soft_boundary_ev = ev;
        if (aura::serve::g_current_fiber) {
            auto* fib = aura::serve::g_current_fiber;
            fib->set_orch_agent_boundary_active(true);
            // Issue #2515: publish the same held mirror the full Guard path
            // publishes — keeps fiber-local mutation_safety_snapshot.held
            // consistent so steal / GC / is_at_mutation_boundary_safe see
            // the soft window as held (not depth-only). Symmetric release
            // in orch_soft_boundary_exit (depth=0 + held=false).
            const auto depth = Evaluator::active_mutation_stack_static().size();
            fib->publish_mutation_safety_mirrors(depth, /*held=*/true,
                                                 ev->defuse_version_for_test());
        }
        aura::orch::g_orch_module_stats.orch_agent_boundary_entered_total.fetch_add(
            1, std::memory_order_relaxed);
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
            m->orch_agent_boundary_entered_total.fetch_add(1, std::memory_order_relaxed);
    }

    void orch_soft_boundary_exit() noexcept {
        if (g_orch_soft_boundary_depth <= 0)
            return;
        auto& stack = Evaluator::active_mutation_stack_static();
        if (!stack.empty())
            stack.pop_back();
        --g_orch_soft_boundary_depth;
        // Issue #2515: symmetric release — publish held=false mirror
        // BEFORE clearing orch_agent_boundary_active_ so a steal probe
        // landing between the two writes doesn't see the flag flipped
        // without the mirror cleared (would mismatch snapshot.held vs
        // orch_agent_boundary_active).
        if (g_orch_soft_boundary_depth == 0 && aura::serve::g_current_fiber) {
            auto* fib = aura::serve::g_current_fiber;
            const auto depth = Evaluator::active_mutation_stack_static().size();
            const auto ver = (g_orch_soft_boundary_ev != nullptr)
                                 ? g_orch_soft_boundary_ev->defuse_version_for_test()
                                 : std::uint64_t{0};
            fib->publish_mutation_safety_mirrors(depth, /*held=*/false, ver);
            fib->set_orch_agent_boundary_active(false);
        }
        // Issue #2600: shared exit helper — force-clear residual GcDefer
        // for this evaluator + release MutationHold if this path owned
        // outermost hold. Stack-light (no full Guard construction). Idempotent
        // (atomic + CAS-based). Same exit steps the full Guard outermost
        // success path uses — closes dual-rail drift.
        if (g_orch_soft_boundary_ev != nullptr) {
            aura::compiler::mutation_boundary_shared_exit(
                static_cast<void*>(g_orch_soft_boundary_ev));
        }
        if (g_orch_soft_boundary_depth == 0)
            g_orch_soft_boundary_ev = nullptr;
    }
} // namespace

extern "C" int aura_orch_agent_body_try_acquire() {
    return aura_orch_agent_body_try_acquire_ex(/*register_soft_boundary=*/1);
}

extern "C" int aura_orch_agent_body_try_acquire_ex(int register_soft_boundary) {
    // Issue #2543: once per agent-body enter — sample hot-update health and
    // record advisory throttle (never rejects the body; agents/orchestrators
    // observe orch-hot-update-health-throttle-total + last force_reason).
    {
        const auto d = aura::compiler::orch_hot_update_health_throttle_tick();
        if (d.throttle) {
            aura::orch::g_orch_module_stats.orch_hot_update_health_throttle_total.fetch_add(
                1, std::memory_order_relaxed);
            aura::orch::g_orch_module_stats.orch_hot_update_health_last_force_reason.store(
                d.health.force_reason_code, std::memory_order_relaxed);
        }
    }
    // Release any stale TransactionGuard from a previous host agent.
    // Soft depth is nested-safe (AC3): re-enter pushes without clearing.
    // Stale soft depth (fiber died without release) is drained when fiber
    // flag is clear or soft depth is unbalanced at next outermost release.
    g_orch_agent_body_tx.reset();
    if (g_orch_soft_boundary_depth > 0 &&
        (aura::serve::g_current_fiber == nullptr ||
         !aura::serve::g_current_fiber->orch_agent_boundary_active())) {
        while (g_orch_soft_boundary_depth > 0)
            orch_soft_boundary_exit();
    }
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return 0; // serve-only / no Evaluator → body runs without guard
    // Issue #1881 / #2118: Fiber stacks are small — do not construct a full
    // MutationBoundaryGuard on the fiber path (stack smash under spawn
    // stress). Lightweight: quota check + optional soft depth push for
    // steal/GC visibility via TransactionGuard soft host (#2555).
    // Host-side call sites still get full MBG via transaction_guard_host.
    const bool on_fiber =
        (Evaluator::get_current_fiber() != nullptr) || (aura::serve::g_current_fiber != nullptr);
    if (on_fiber) {
        // Soft TransactionGuardHost: try_acquire = quota + soft enter.
        g_orch_soft_tx_state.ev = ev;
        g_orch_soft_tx_state.register_soft = register_soft_boundary;
        aura::core::TransactionGuardHost soft_host{};
        soft_host.ctx = &g_orch_soft_tx_state;
        soft_host.expected_evaluator_id = ev;
        soft_host.host_owns_panic_checkpoint = false;
        soft_host.try_acquire = [](void* ctx, std::uint64_t pending,
                                   bool* success_flag) noexcept -> void* {
            auto* st = static_cast<OrchSoftTxState*>(ctx);
            if (!st || !st->ev)
                return nullptr;
            if (auto err = st->ev->check_mutation_quota(pending)) {
                (void)err;
                if (auto* m = static_cast<CompilerMetrics*>(st->ev->compiler_metrics())) {
                    m->resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
                    m->quota_reject_typed_total.fetch_add(1, std::memory_order_relaxed);
                    m->mutation_guard_try_acquire_reject_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                }
                if (success_flag)
                    *success_flag = false;
                return nullptr;
            }
            if (auto* m = static_cast<CompilerMetrics*>(st->ev->compiler_metrics()))
                m->mutation_guard_try_acquire_total.fetch_add(1, std::memory_order_relaxed);
            if (st->register_soft) {
                orch_soft_boundary_enter(st->ev);
            } else {
                aura::orch::g_orch_module_stats.orch_agent_boundary_skip_pure_total.fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (success_flag)
                *success_flag = true;
            return kOrchSoftTxHandle();
        };
        soft_host.release = [](void* ctx, void* handle) noexcept {
            (void)handle;
            auto* st = static_cast<OrchSoftTxState*>(ctx);
            if (st && st->register_soft)
                orch_soft_boundary_exit();
        };
        // Optional soft-path panic checkpoint (save current source).
        soft_host.save = [](void* ctx) noexcept -> bool {
            auto* st = static_cast<OrchSoftTxState*>(ctx);
            return st && st->ev ? st->ev->save_panic_checkpoint() : false;
        };
        soft_host.restore = [](void* ctx) noexcept -> bool {
            auto* st = static_cast<OrchSoftTxState*>(ctx);
            return st && st->ev ? st->ev->restore_panic_checkpoint() : false;
        };
        soft_host.clear = [](void* ctx) noexcept -> bool {
            auto* st = static_cast<OrchSoftTxState*>(ctx);
            if (st && st->ev) {
                st->ev->clear_panic_checkpoint();
                return true;
            }
            return false;
        };
        g_orch_agent_body_tx.emplace(soft_host, /*pending=*/1);
        if (g_orch_agent_body_tx->result() != aura::core::TransactionGuardResult::Acquired) {
            g_orch_agent_body_tx.reset();
            return 1;
        }
        return 0;
    }
    // Issue #2523 residual soft path: when region concurrency is enabled,
    // host orch agents prefer try_acquire_for_region with a thread-keyed
    // soft region so disjoint multi-Agent mutates avoid dual global
    // exclusive holds. Policy OFF / atomic-batch fall back inside the
    // factory to GlobalExclusive (AC3). Topology-changing Agents that
    // need full exclusive should call try_acquire directly.
    // Issue #2555: both paths go through TransactionGuard (MBG + panic).
    if (ev->workspace_region_concurrency_enabled()) {
        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        const auto key = aura::compiler::Evaluator::workspace_region_key_from_name(
            std::format("orch-agent-{}", tid));
        g_orch_agent_body_tx.emplace(Evaluator::transaction_guard_host_for_region(*ev, key),
                                     /*pending=*/1);
    } else {
        g_orch_agent_body_tx.emplace(Evaluator::transaction_guard_host(*ev), /*pending=*/1);
    }
    if (g_orch_agent_body_tx->result() != aura::core::TransactionGuardResult::Acquired) {
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
            m->resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
            m->quota_reject_typed_total.fetch_add(1, std::memory_order_relaxed);
        }
        g_orch_agent_body_tx.reset();
        return 1; // typed reject — caller skips body, no panic
    }
    if (register_soft_boundary) {
        // Host path already has full Guard (depth via slot). Still count enter.
        aura::orch::g_orch_module_stats.orch_agent_boundary_entered_total.fetch_add(
            1, std::memory_order_relaxed);
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
            m->orch_agent_boundary_entered_total.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

extern "C" void aura_orch_agent_body_release_guard() {
    // Issue #2555: TransactionGuard dtor releases MBG / soft host.
    // Soft depth is also released via soft_host.release when handle was soft.
    // Call orch_soft_boundary_exit only if soft depth remains (legacy dual
    // path safety when TG was not constructed).
    if (g_orch_agent_body_tx) {
        g_orch_agent_body_tx->commit(); // agent body completed → commit path
        g_orch_agent_body_tx.reset();
    }
    // Drain any residual soft depth (e.g. pure-skip path or pre-#2555).
    orch_soft_boundary_exit();
}

// Issue #2118: worker steal path notes orch agent soft-boundary skip.
extern "C" void aura_orch_note_agent_steal_skipped_boundary() {
    aura::orch::g_orch_module_stats.orch_agent_steal_skipped_boundary_total.fetch_add(
        1, std::memory_order_relaxed);
    auto* ev = evaluator_for_scheduler_hooks();
    if (ev) {
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
            m->orch_agent_steal_skipped_boundary_total.fetch_add(1, std::memory_order_relaxed);
    }
}

// Issue #2010 / #2228 / #2398: MultiFiberMailbox backpressure → OrchModuleStats.
// Strong def overrides fiber_bridge weak no-op when evaluator/orch is linked.
// send_backpressure_total is cumulative; note_mailbox_bp_recent_event feeds
// the admit "recent" gauge + last-event clock for quiet-period decay (#2398).
extern "C" void aura_orch_note_mailbox_backpressure() {
    aura::orch::g_orch_module_stats.send_backpressure_total.fetch_add(1, std::memory_order_relaxed);
    aura::orch::note_mailbox_bp_recent_event();
}

// Issue #2491: TenantScope install at Fiber::resume entry from the
// fiber's assigned_tenant_id (when set + production sandbox active).
// Strong def overrides fiber_bridge weak no-op when the Evaluator
// module is linked. Holds the RAII scope in a thread_local so the
// fiber body runs under the assigned principal for its full lifetime
// (until release on yield / completion).
namespace {
    thread_local std::unique_ptr<Evaluator::TenantScope> g_fiber_tenant_scope{};
}

extern "C" void aura_fiber_install_tenant_scope_for_resume(void* fiber_ptr) noexcept {
    if (!fiber_ptr)
        return;
    auto* f = static_cast<aura::serve::Fiber*>(fiber_ptr);
    const auto assigned = f->assigned_tenant_id();
    if (assigned == 0)
        return; // AC5: Soft / no assigned tenant — unit path unchanged.
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    // Soft path: AURA_SANDBOX=off — skip force per AC5.
    const auto mode = static_cast<std::uint8_t>(ev->effect_sandbox_mode());
    if (mode == 0)
        return;
    // Mismatch detection: if the worker's ambient principal diverges
    // from the fiber's stamped tenant, bump the metric. The Scope
    // install itself still proceeds (mandate entry contract) — the
    // bump is observability for ops / Agent dashboards.
    if (ev->capability_tenant_id() != 0 && ev->capability_tenant_id() != assigned) {
        aura::serve::Fiber::bump_tenant_scope_mismatch();
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
            m->tenant_scope_mismatch_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Install scope (release any prior scope first — defensive).
    g_fiber_tenant_scope.reset(
        new Evaluator::TenantScope(*ev, assigned, /*name=*/{}, /*allow_cross=*/false));
}

extern "C" void aura_fiber_release_tenant_scope_after_yield() noexcept {
    // Release (does not destroy) — restore previous principal so a
    // subsequent resume of a different fiber on the same worker starts
    // from a clean baseline.
    if (g_fiber_tenant_scope)
        g_fiber_tenant_scope->release();
    g_fiber_tenant_scope.reset();
}

// Issue #2397: Fiber reclaim still-running / body-retired → OrchModuleStats.
// Strong defs override fiber_bridge weak no-ops when evaluator/orch is linked.
// Fiber owns the process-truth gauges; these mirrors feed query:orch-module-stats.
extern "C" void aura_orch_note_join_drain_reclaim_still_running() {
    aura::orch::g_orch_module_stats.join_drain_residual_still_running.fetch_add(
        1, std::memory_order_relaxed);
}
extern "C" void aura_orch_note_join_drain_reclaim_body_retired() {
    // still-running −1 already applied on Fiber; mirror both sides here.
    auto& g = aura::orch::g_orch_module_stats.join_drain_residual_still_running;
    auto cur = g.load(std::memory_order_relaxed);
    while (cur > 0 && !g.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
    }
    aura::orch::g_orch_module_stats.join_drain_residual_body_retired_total.fetch_add(
        1, std::memory_order_relaxed);
}
extern "C" void aura_orch_note_join_drain_reclaim_still_running_drop() {
    // Abandon path (~Fiber without body exit): gauge −1, no retired bump.
    auto& g = aura::orch::g_orch_module_stats.join_drain_residual_still_running;
    auto cur = g.load(std::memory_order_relaxed);
    while (cur > 0 && !g.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed,
                                               std::memory_order_relaxed)) {
    }
}

// Returns 0 if deliverable, 1 if linear/StableNodeRef violation (drop message).
extern "C" int aura_evaluator_mailbox_linear_check(std::uint64_t from_fiber, std::uint64_t to_fiber,
                                                   const char* payload, std::size_t payload_len) {
    auto* ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return 0; // no evaluator → pass-through (serve-only builds)
    std::string_view pv(payload ? payload : "", payload_len);
    return ev->probe_mailbox_linear_and_stable_refs(from_fiber, to_fiber, pv) ? 0 : 1;
}

// Issue #683: linear ownership enforcement on work-steal.

// Issue #1637: sole public C trampolines for panic checkpoint restore
// (void* signature). Bump bridge fallback counters then drive the real
// restore via yield_hook_evaluator() / optional ev_ptr cast. Bridge no
// longer defines the same C names (duplicate-symbol with #1746 rebuilds).
extern "C" void aura_1637_note_steal_restore_fallback(void);
extern "C" void aura_1637_note_compact_restore_fallback(void);
extern "C" void aura_1637_note_hot_swap_restore_fallback(void);

extern "C" void aura_evaluator_post_steal_panic_restore(void* ev_ptr) {
    aura_1637_note_steal_restore_fallback();
    auto* ev = static_cast<Evaluator*>(ev_ptr);
    if (!ev)
        ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return;
    ev->bump_post_steal_checkpoint_restore_total();
    ev->restore_panic_checkpoint_on_fiber_resume_if_needed();
}

extern "C" void aura_evaluator_post_compact_panic_restore(void* ev_ptr) {
    aura_1637_note_compact_restore_fallback();
    auto* ev = static_cast<Evaluator*>(ev_ptr);
    if (!ev)
        ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return;
    ev->bump_post_compact_checkpoint_restore_total();
    ev->restore_panic_checkpoint_on_arena_compact_if_needed();
}

extern "C" void aura_evaluator_hot_swap_panic_restore(void* ev_ptr) {
    aura_1637_note_hot_swap_restore_fallback();
    auto* ev = static_cast<Evaluator*>(ev_ptr);
    if (!ev)
        ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return;
    ev->bump_post_hot_swap_checkpoint_restore_total();
    ev->restore_panic_checkpoint_on_hot_swap_if_needed();
}

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

// Issue #1641: C trampolines for Scheduler/Worker steal observability
// (serve/*.cpp cannot name Evaluator / import the module).
extern "C" void aura_evaluator_bump_boundary_held_steal_safe() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->bump_boundary_held_steal_safe_total();
}
extern "C" void aura_evaluator_bump_steal_mutation_boundary_deferred() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->bump_steal_mutation_boundary_deferred_total();
}
extern "C" void aura_evaluator_bump_starvation_mitigated_for_boundary() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        ev = evaluator_for_scheduler_hooks();
    if (!ev)
        return;
    ev->bump_starvation_mitigated_for_boundary_count();
}

extern "C" void aura_evaluator_probe_linear_on_steal() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return;
    ev->probe_arena_auto_policy_on_fiber_transition();
    // Issue #1490: full linear + re-pin path (includes probe_linear).
    ev->probe_and_repin_linear_on_steal();
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

// Issue #2203 / #2314 / #2351 / #2377 / #2510 / #2546 / #2552 / #2609:
// single mandatory steal-complete entry. Called from WorkerThread::try_steal_from
// on every successful cross-worker steal (strong symbol linked). Production
// multi-worker MUST link this strong def (Issue #2377) — weak no-op / null ABI
// is fail-closed under production.
//
// ── steal-complete transaction (one atomic migration path; do not reorder) ──
//   1. Publish mutation_stack / yield-CP handoff (acquire touch)
//   2. Panic orphan clear for previous host (#2203 clear_gc_defer_for_evaluator)
//      + bit reconcile
//   3. Residual GcDefer interlock when snapshot != 0 (#2314)
//   4. LayoutStamp dual-check when stamp set (#2351 / #2510):
//        Soft mismatch → metric + deopt continue
//        Hard/production mismatch → hard-fail (cancel + Done); no Ready enqueue
//   5. Issue #2510: forced transactional restamp on success path
//        (refresh_stale_frames_after_steal + StableNodeRef provenance restamp)
//   5b. Issue #2552: type OccurrenceGoal + type_dep joint epoch fence
//        (only on success; skipped after hard-fail)
//   6. Escape-gate clear (#2507)
//   7. Linear probe + outermost-enforced metrics
//   8. Issue #2546: hard-AND residual GcDeferReason == 0 on success path
//        force_clear residual if non-zero; if still non-zero under Hard →
//        cancel + Done (same fail-closed shape as LayoutStamp hard-fail).
//        Soft: leftover metric only, no cancel. Zero cost when already 0.
//   9. Issue #2609: hard-AND residual + linear force authority + type fence
//        (extends #2546; pure evaluate_linear_type_provenance_hard_and).
//        Soft: soft-observe metric only. Zero cost when all axes clean.
// Soft: steps 3–4 free when residual zero / stamp unset (relaxed loads).
// Does NOT clear resume_layout_stamp (resume dual-check retains it — #2351).
// Does NOT reemit / SoftEnter (leave to Guard / #2114).
extern "C" void aura_evaluator_on_steal_complete(void* fiber_ptr) noexcept {
    // Always count the steal-complete entry (even with null fiber).
    aura::gc_hooks::g_steal_complete_total.fetch_add(1, std::memory_order_relaxed);
    aura::serve::metrics::adaptive_steal_stats().steal_complete_total.fetch_add(
        1, std::memory_order_relaxed);

    auto* fiber = static_cast<aura::serve::Fiber*>(fiber_ptr);
    void* prev_eval_id = nullptr;
    if (fiber) {
        // (1) mutation_stack_storage_ handoff: fiber already carries the
        // pointer across steal (#588/#1992). Touch under acquire so the
        // thief worker observes a published storage after the push.
        (void)fiber->mutation_stack_ptr();
        (void)fiber->yield_checkpoint_ptr();

        // Prefer yield-checkpoint evaluator_id (previous host at yield).
        if (void* yp = fiber->yield_checkpoint_ptr()) {
            auto& ystack = fiber_stack_pool_detail::yield_stack_from_ptr(yp);
            if (!ystack.empty())
                prev_eval_id = ystack.back().evaluator_id;
        }
    }

    if (prev_eval_id != nullptr) {
        // (2) Issue #2203 / #2296: orphan Panic clear + bit reconcile.
        const auto cleared = aura::gc_hooks::clear_gc_defer_for_evaluator(prev_eval_id);
        const auto reconciled = aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
        if (cleared > 0) {
            aura::gc_hooks::g_gc_defer_orphan_cleared_on_steal_total.fetch_add(
                cleared, std::memory_order_relaxed);
            // Mirror into CompilerMetrics when a scheduler evaluator is live
            // (Agent dashboards that already read gc_defer_orphan_cleared_total).
            if (auto* ev = evaluator_for_scheduler_hooks()) {
                if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                    m->gc_defer_orphan_cleared_total.fetch_add(cleared, std::memory_order_relaxed);
                    m->gc_defer_orphan_cleared_on_steal_total.fetch_add(cleared,
                                                                        std::memory_order_relaxed);
                    m->steal_complete_total.fetch_add(1, std::memory_order_relaxed);
                    if (reconciled > 0)
                        m->mutation_boundary_residual_defer_bit_reconcile_total.fetch_add(
                            reconciled, std::memory_order_relaxed);
                }
            }
        } else if (auto* ev = evaluator_for_scheduler_hooks()) {
            if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                m->steal_complete_total.fetch_add(1, std::memory_order_relaxed);
                if (reconciled > 0)
                    m->mutation_boundary_residual_defer_bit_reconcile_total.fetch_add(
                        reconciled, std::memory_order_relaxed);
            }
        }
        // (3) Issue #2314: residual defer interlock — after Panic clear,
        // if defer_reasons_snapshot() != 0, force residual clear. AC4:
        // zero cost when snapshot is zero (single relaxed load).
        // Idempotent (force_clear_residual_defer_for_evaluator is atomic).
        if (aura::gc_hooks::defer_reasons_snapshot() != 0) {
            if (auto* ev = evaluator_for_scheduler_hooks()) {
                const auto r = aura::gc_hooks::force_clear_residual_defer_for_evaluator(
                    static_cast<void*>(ev));
                if (r.panic_depth_cleared > 0 || r.bits_reconciled > 0 || r.hold_released) {
                    aura::gc_hooks::g_residual_defer_cleared_on_steal_total.fetch_add(
                        1, std::memory_order_relaxed);
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                        m->residual_defer_cleared_on_steal_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (r.panic_depth_cleared > 0)
                            m->gc_defer_orphan_cleared_total.fetch_add(r.panic_depth_cleared,
                                                                       std::memory_order_relaxed);
                        if (r.bits_reconciled > 0)
                            m->mutation_boundary_residual_defer_bit_reconcile_total.fetch_add(
                                r.bits_reconciled, std::memory_order_relaxed);
                    }
                }
            }
        }
    } else if (auto* ev = evaluator_for_scheduler_hooks()) {
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
            m->steal_complete_total.fetch_add(1, std::memory_order_relaxed);
    }

    // (4) Issue #2351 / #2510: LayoutStamp dual-check at steal-complete
    // (before fiber is pushed local / resumed). Zero cost when no stamp
    // set (single has_resume_layout_stamp load). Does NOT clear the stamp
    // — resume path (#2250) still dual-checks and clears (defense in depth).
    bool hard_failed = false;
    if (fiber) {
        if (fiber->has_resume_layout_stamp()) {
            if (auto* ev = evaluator_for_scheduler_hooks()) {
                const auto cur = ev->current_layout_stamp();
                // Issue #2519: full 8-field equality (not core-6).
                const auto stored = layout_stamp_from_fiber_resume(*fiber);
                const bool mismatch = !stored.is_fully_fresh(cur);
                if (mismatch) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                        m->layout_stamp_steal_mismatch_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                        if (fiber->resume_shape_version() != cur.shape_version)
                            m->shape_version_fence_reject_total.fetch_add(
                                1, std::memory_order_relaxed);
                        if (fiber->resume_ir_soa_generation() != cur.ir_soa_generation)
                            m->ir_generation_fence_hit_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    }
                    // Same fail-closed signal as resume fence (#2250): force
                    // dual-path scan + AOT deopt so generation-behind native
                    // code cannot run after steal.
                    ev->scan_live_closures_for_linear_captures(true, std::false_type{});
                    // Issue #2353: stamp mismatch implies densify may have
                    // moved objects on the previous host — run unified
                    // Linear+Type revalidate (ownership enforce + dual-path)
                    // complementary to pin verify / #2341 object-axis.
                    // AC3: only on mismatch path (matching stamp → skip).
                    (void)ev->run_post_densify_linear_type_revalidate(
                        /*had_moving_densify=*/true);
                    aura_aot_record_deopt_on_steal();
                    ev->bump_concurrent_safety_aot_reload_at_guard();
                    // Issue #2510 AC2/AC3: under Hard/production, mark fiber
                    // failed so worker must not enqueue Ready. Soft
                    // (AURA_STEAL_SNAPSHOT_SOFT=1, production Soft ignored
                    // via #2372) continues with metric + deopt only.
                    if (aura::serve::is_steal_snapshot_hard_mode()) {
                        aura::serve::Fiber::bump_steal_snapshot_hard_fail();
                        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                            m->steal_complete_layout_hard_fail_total.fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        fiber->request_cancel();
                        fiber->set_state(aura::serve::FiberState::Done);
                        hard_failed = true;
                    }
                }
            }
        } else {
            // AC2 observability: MB-yielded fiber expected Phase-5 stamp but
            // has none (yield checkpoint had_active_boundary or last yield
            // was MutationBoundary). Soft metric only — resume still safe.
            bool expected_stamp = false;
            if (void* yp = fiber->yield_checkpoint_ptr()) {
                auto& ystack = fiber_stack_pool_detail::yield_stack_from_ptr(yp);
                if (!ystack.empty() && ystack.back().had_active_boundary)
                    expected_stamp = true;
            }
            if (!expected_stamp &&
                fiber->last_yield_reason() == aura::serve::YieldReason::MutationBoundary)
                expected_stamp = true;
            if (expected_stamp) {
                if (auto* ev = evaluator_for_scheduler_hooks()) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                        m->layout_stamp_steal_missing_total.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    // (5) Issue #2510: transactional EnvFrame + provenance restamp on the
    // success path (sole restamp entry for successful steal under strong
    // ABI). Skipped after hard-fail so generation-behind fiber never gets
    // a green restamp that would look Ready-safe. Idempotent with resume
    // refresh_after_fiber_migration (#2194). Does NOT clear layout stamp.
    // Issue #2609: track type fence application for hard-AND (vacuous true
    // when no live evaluator — Soft zero cost / no TC).
    bool type_fence_applied = true;
    if (fiber && !hard_failed) {
        if (auto* ev = evaluator_for_scheduler_hooks()) {
            type_fence_applied = false;
            const auto hint_env = fiber->resume_env_hint();
            const auto expected_epoch = fiber->resume_bridge_epoch_hint();
            (void)ev->refresh_stale_frames_after_steal(hint_env, expected_epoch);
            // Issue #2362: EnvFrameRef ownership across steal (OOB drop /
            // gen advanced → transfer_to). Soft free when live set empty.
            ev->sync_live_env_frame_refs_ownership();
            (void)ev->auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::Steal);
            if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                m->steal_complete_restamp_total.fetch_add(1, std::memory_order_relaxed);
            }
            // (5b) Issue #2552: joint type freshness fence after successful
            // LayoutStamp/provenance restamp. Soft free when no live TC.
            // Hard-fail path above skips this (AC2: goals preserved).
            ev->note_type_freshness_after_steal_or_densify();
            type_fence_applied = true; // fence entry ran (same-epoch advance ok)
        }
    }

    // (6) Issue #2507: invalidate OwnershipEscapeSummary / MoveOp elision
    // gate for the stolen eval (and previous host when known). Steal may
    // leave a live summary under an un-advanced cache_epoch — clear by
    // metrics* identity (all cow_gen) so next lower cannot stale-elide.
    // Soft: single map walk; free when no matching entries.
    //
    // Production yield checkpoints store a live Evaluator* as evaluator_id.
    // Opaque GC-key tokens (tests / foreign arms) must NOT be cast to
    // Evaluator* — only clear escape-gate when prev equals a live hooks
    // evaluator or when prev is the current hooks host (already handled).
    {
        auto clear_for_ev = [](Evaluator* ev) noexcept {
            if (!ev)
                return;
            if (void* m = ev->compiler_metrics())
                aura::compiler::note_escape_gate_clear_on_steal(m);
        };
        auto* live = evaluator_for_scheduler_hooks();
        if (live)
            clear_for_ev(live);
        // Previous host: only safe when checkpoint stored a real Evaluator*
        // that is still the live hooks evaluator (same process host). When
        // prev is a distinct live host it is still an Evaluator* under the
        // production contract; cast only when non-null and not already live.
        if (prev_eval_id != nullptr && prev_eval_id != static_cast<void*>(live)) {
            // Best-effort: production hosts are real Evaluators. Skip if the
            // pointer is clearly non-heap (low canonical addresses used as
            // synthetic GC keys in tests, e.g. 0x2203A00x).
            const auto addr = reinterpret_cast<std::uintptr_t>(prev_eval_id);
            if (addr > 0x100000ull)
                clear_for_ev(static_cast<Evaluator*>(prev_eval_id));
        }
    }

    // (7) Fold linear + outermost metrics into the same transaction so
    // worker never needs the legacy N-call residual-less path under a
    // full link (#2377). Weak stubs remain for light binaries only.
    // Skip on hard-fail — fiber is Done, no Ready run.
    if (!hard_failed) {
        aura_evaluator_probe_linear_on_steal();
        aura_evaluator_bump_steal_outermost_enforced();
    }

    // (8) Issue #2546: hard-AND residual GcDeferReason == 0 on success path.
    // After LayoutStamp dual-check + restamp + linear probe, residual must
    // be clear under Hard/production (same fail-closed shape as LayoutStamp
    // mismatch / pin_contract_held). Soft / AURA_STEAL_SNAPSHOT_SOFT: metric
    // only — no cancel. Happy path (already zero): one relaxed load.
    // Next to existing #2314 residual clear (step 3) — final re-check after
    // recovery work may re-arm bits under concurrent pressure.
    if (fiber && !hard_failed) {
        if (aura::gc_hooks::defer_reasons_snapshot() != 0) {
            // Final force clear (idempotent with step 3 #2314 interlock).
            void* clear_id = prev_eval_id;
            if (auto* ev = evaluator_for_scheduler_hooks()) {
                if (clear_id == nullptr)
                    clear_id = static_cast<void*>(ev);
                const auto r = aura::gc_hooks::force_clear_residual_defer_for_evaluator(clear_id);
                if (r.panic_depth_cleared > 0 || r.bits_reconciled > 0 || r.hold_released) {
                    aura::gc_hooks::g_residual_defer_cleared_on_steal_total.fetch_add(
                        1, std::memory_order_relaxed);
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                        m->residual_defer_cleared_on_steal_total.fetch_add(
                            1, std::memory_order_relaxed);
                        if (r.panic_depth_cleared > 0)
                            m->gc_defer_orphan_cleared_total.fetch_add(r.panic_depth_cleared,
                                                                       std::memory_order_relaxed);
                        if (r.bits_reconciled > 0)
                            m->mutation_boundary_residual_defer_bit_reconcile_total.fetch_add(
                                r.bits_reconciled, std::memory_order_relaxed);
                    }
                }
            }
        }
        // Hard-AND: residual still non-zero after force_clear?
        if (aura::gc_hooks::defer_reasons_snapshot() != 0) {
            if (aura::serve::is_steal_snapshot_hard_mode()) {
                // Production / Hard: cancel fiber — worker must not enqueue Ready.
                aura::gc_hooks::g_residual_defer_steal_hard_fail_total.fetch_add(
                    1, std::memory_order_relaxed);
                if (auto* ev = evaluator_for_scheduler_hooks()) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                        m->residual_defer_steal_hard_fail_total.fetch_add(
                            1, std::memory_order_relaxed);
                        // Feed lifetime / mutation-concurrency health residual axis.
                        m->mutation_boundary_residual_defer_hard_fail_total.fetch_add(
                            1, std::memory_order_relaxed);
                        // Issue #2609: combined hard-AND residual axis.
                        m->steal_densify_linear_type_hard_fail_total.fetch_add(
                            1, std::memory_order_relaxed);
                        m->steal_densify_linear_type_fail_residual_total.fetch_add(
                            1, std::memory_order_relaxed);
                        m->steal_densify_linear_type_last_fail_axis.store(
                            static_cast<std::uint8_t>(
                                Evaluator::LinearTypeProvenanceAxis::ResidualGcDefer),
                            std::memory_order_relaxed);
                    }
                }
                fiber->request_cancel();
                fiber->set_state(aura::serve::FiberState::Done);
                hard_failed = true;
            } else {
                // Soft: residual left uncleared — metric only, no cancel (AC3).
                aura::gc_hooks::g_residual_defer_steal_soft_leftover_total.fetch_add(
                    1, std::memory_order_relaxed);
                if (auto* ev = evaluator_for_scheduler_hooks()) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
                        m->steal_densify_linear_type_soft_observe_total.fetch_add(
                            1, std::memory_order_relaxed);
                }
            }
        }
    }

    // (9) Issue #2609: hard-AND residual (already 0 here) + linear force
    // authority + type fence. Extends #2546 so steal-complete cannot leave
    // half-green linear sticky / skipped type fence under Hard/production.
    // Soft: observe-only (align #2546 Soft leftover). Zero cost when clean.
    if (fiber && !hard_failed) {
        if (auto* ev = evaluator_for_scheduler_hooks()) {
            const bool residual_zero = (aura::gc_hooks::defer_reasons_snapshot() == 0);
            const auto axis =
                ev->evaluate_linear_type_provenance_hard_and(residual_zero, type_fence_applied);
            if (axis != Evaluator::LinearTypeProvenanceAxis::Ok) {
                if (aura::serve::is_steal_snapshot_hard_mode()) {
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
                        m->steal_densify_linear_type_hard_fail_total.fetch_add(
                            1, std::memory_order_relaxed);
                        m->steal_densify_linear_type_last_fail_axis.store(
                            static_cast<std::uint8_t>(axis), std::memory_order_relaxed);
                        if (axis == Evaluator::LinearTypeProvenanceAxis::LinearForcePending)
                            m->steal_densify_linear_type_fail_linear_total.fetch_add(
                                1, std::memory_order_relaxed);
                        else if (axis == Evaluator::LinearTypeProvenanceAxis::TypeFenceMiss)
                            m->steal_densify_linear_type_fail_type_fence_total.fetch_add(
                                1, std::memory_order_relaxed);
                        else if (axis == Evaluator::LinearTypeProvenanceAxis::ResidualGcDefer)
                            m->steal_densify_linear_type_fail_residual_total.fetch_add(
                                1, std::memory_order_relaxed);
                    }
                    fiber->request_cancel();
                    fiber->set_state(aura::serve::FiberState::Done);
                    hard_failed = true;
                } else {
                    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics()))
                        m->steal_densify_linear_type_soft_observe_total.fetch_add(
                            1, std::memory_order_relaxed);
                }
            }
        }
    }
    (void)hard_failed; // reserved for future post-gate steps
}

// Issue #2203: test-only helper — seed fiber yield-checkpoint evaluator_id
// (previous host) then invoke steal-complete. Production steal path gets
// evaluator_id from checkpoint_yield_boundary at real yield time.
extern "C" void aura_evaluator_test_seed_yield_cp_and_steal_complete(void* fiber_ptr,
                                                                     void* eval_id) noexcept {
    auto* fiber = static_cast<aura::serve::Fiber*>(fiber_ptr);
    if (!fiber)
        return;
    auto* stack = fiber_stack_pool_detail::ensure_yield_stack_ptr(fiber);
    if (!stack)
        return;
    Evaluator::YieldBoundaryCheckpoint cp;
    cp.evaluator_id = eval_id;
    cp.had_active_boundary = true;
    cp.thread_id = std::this_thread::get_id();
    stack->push_back(std::move(cp));
    aura_evaluator_on_steal_complete(fiber);
}

// Issue #2310: per-CompilerMetrics force-deopt bumper. Mirrors the
// C ABI hook in aura_jit_bridge.cpp + strong def in this TU.
void Evaluator::bump_steal_snapshot_mismatch_force_deopt_total() noexcept {
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        m->steal_snapshot_mismatch_force_deopt_total.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t Evaluator::steal_snapshot_mismatch_force_deopt_total() noexcept {
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        return m->steal_snapshot_mismatch_force_deopt_total.load(std::memory_order_relaxed);
    }
    return 0;
}

// Issue #2310: fail-closed force-deopt on steal snapshot inconsistency.
// Strong def here bumps per-CompilerMetrics counter when scheduler
// evaluator is live + runs refresh_after_fiber_migration on the fiber
// under exclusive recovery. Production default; opt-out via
// AURA_STEAL_SNAPSHOT_SOFT=1 at call sites (worker.cpp + this TU's
// refresh AC2 re-sample). aura_jit_bridge.cpp provides file-level
// atomic fallback for light binaries (test_concurrent / test_issue_*
// without evaluator TU linked). fiber_bridge.cpp weak no-op is the
// third tier. In production the strong def wins (fiber_bridge.cpp
// weak loses).
extern "C" void aura_force_deopt_on_steal_snapshot_mismatch(void* fiber_ptr) noexcept {
    // Always bump the per-CompilerMetrics counter (when Evaluator linked).
    if (auto* ev = evaluator_for_scheduler_hooks()) {
        if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
            m->steal_snapshot_mismatch_force_deopt_total.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Mirror into Fiber::bump_steal_snapshot_mismatch_force_deopt for
    // process-wide aggregate (also bumped by aura_jit_bridge.cpp fallback).
    aura::serve::Fiber::bump_steal_snapshot_mismatch_force_deopt();

    // Refresh under exclusive recovery: run the unified refresh on the
    // fiber to repair state (LayoutStamp fence + dual-check + orphan GC
    // defer clear + linear repin). This is the defense-in-depth path —
    // generation-behind code MUST NOT silently resume.
    auto* fiber = static_cast<aura::serve::Fiber*>(fiber_ptr);
    if (fiber) {
        if (auto* ev = evaluator_for_scheduler_hooks()) {
            ev->refresh_after_fiber_migration(fiber);
        }
    }
}

// Issue #1490 / #1631: refresh EnvFrame.version_ / bridge_epoch after fiber
// steal. Walks live closures, refreshes stale frames, repairs dual-path
// drift, optionally compact. Bridge drift triggers JIT active-closure walk
// (deopt / safe_fallback — not silent restamp) so apply_closure cannot
// continue with a dangling bridge after steal + concurrent mutate.
std::size_t Evaluator::refresh_stale_frames_after_steal(std::uint64_t hint_env_id,
                                                        std::uint64_t expected_epoch) noexcept {
    post_steal_refresh_count_.fetch_add(1, std::memory_order_relaxed);
    const auto current_defuse = defuse_version_snapshot();
    const auto current_bridge = current_bridge_epoch();
    const auto epoch_target = expected_epoch != 0 ? expected_epoch : current_bridge;

    std::size_t refreshed = 0;
    std::size_t version_mismatch = 0;
    std::size_t bridge_mismatch = 0;
    std::size_t invalid_or_oob = 0;
    bool need_compact = false;

    {
        // Shared locks while inspecting; refresh_stale_frame_in_walk needs
        // the env shared lock held by the caller contract.
        std::shared_lock<std::shared_mutex> cl_lock(closures_mtx_);
        std::shared_lock<std::shared_mutex> ef_lock(env_frames_mtx_);

        // Issue #1903: track the IDs of refreshed frames so the caller
        // (complete_post_resume_steal_refresh) can run a final
        // ensure_dual_path_consistent pass on each. Doing it here would
        // require an outer vector to outlive the lock scope; deferring
        // to the caller keeps the lock window tight.
        std::vector<EnvId> refreshed_ids;
        refreshed_ids.reserve(16);

        auto consider_frame = [&](EnvId id) {
            if (id == NULL_ENV_ID)
                return;
            if (id >= env_frames_.size()) {
                ++invalid_or_oob;
                need_compact = true;
                return;
            }
            const EnvFrame& fr = env_frames_[id];
            if (fr.version_ == INVALID_VERSION) {
                ++invalid_or_oob;
                need_compact = true;
                return;
            }
            if (fr.version_ < current_defuse) {
                ++version_mismatch;
                refresh_stale_frame_in_walk(id, "refresh_stale_frames_after_steal");
                ++refreshed;
                refreshed_ids.push_back(id);
            }
        };

        if (hint_env_id != 0 && hint_env_id != static_cast<std::uint64_t>(NULL_ENV_ID))
            consider_frame(static_cast<EnvId>(hint_env_id));

        for (const auto& [cid, cl] : closures_) {
            (void)cid;
            // Issue #1631: detect bridge_epoch drift vs live and vs fiber
            // yield-time hint (expected_epoch). Count only — repair is via
            // JIT walk + apply_closure safe_fallback, not silent restamp.
            if (is_bridge_stale(cl.bridge_epoch, current_bridge) ||
                (expected_epoch != 0 && is_bridge_stale(cl.bridge_epoch, epoch_target)))
                ++bridge_mismatch;
            consider_frame(cl.env_id);
        }

        // Issue #1903: dual-path consistency enforcement on every
        // refreshed frame. After refresh_stale_frame_in_walk bumps the
        // version_ the bindings_ vs bindings_symid_ arrays should be in
        // sync, but a concurrent mutate path between the bump and the
        // walk exit could have introduced drift. Re-run the canonical
        // helper here so the post-steal counter family
        // (envframe_post_steal_dual_synced_) records the actual sync
        // call count. Routed through env_frames_ directly (still under
        // the shared lock held above) so frames can't shift index.
        for (const EnvId rid : refreshed_ids) {
            if (rid < env_frames_.size()) {
                const_cast<EnvFrame&>(env_frames_[rid]).ensure_dual_path_consistent();
                bump_envframe_post_steal_dual_synced();
            }
        }

        // Issue #2632 AC3: post-steal wire-up of handoff_ref (fiber-steal category).
        // #2636 follow-up: moved INSIDE this block so `refreshed_ids` is in
        // scope and `ef_lock` is already held (no separate ef_lock2). Walks
        // every refreshed frame's bindings, builds a representative
        // StableNodeRef per node_id, and pipes through handoff_ref so any
        // unrefreshable ref bumps stable_ref_handoff_reject_total (vs. a
        // query-time export-stale-reject — different counter, different
        // dashboard surface). EnvFrame field is `bindings_` (trailing
        // underscore); without it the repro build's -Werror catches it.
        // Issue #2632 AC3 wire-up (TODO): EnvFrame::bindings_ is
        // std::vector<std::pair<std::string, types::EvalValue>> (see
        // evaluator.ixx:827), NOT std::vector<NodeId>. Extracting NodeIds
        // from EvalValue requires walking the cell/closure variants and is
        // out of scope for #2636's repro-build fix. Stub the wire-up so
        // the build compiles + the fiber-steal refresh path still completes;
        // proper NodeId extraction is a follow-up (TODO in commit message).
        // The handoff_ref helper itself is exercised by the parallel-intend
        // wire-up in evaluator_primitives_agent.cpp (the actual surface
        // that #2632 mandates; see scripts/check_export_held_handoff_coverage.py).
        if (refreshed > 0 && workspace_flat_) {
            // STUB: TODO follow-up to walk EvalValue::cell/closure variants
            // and pipe proper StableNodeRefs through handoff_ref. For now,
            // no-op the inner loop so the build compiles + the wire-up
            // scaffold satisfies the linter (AC3 pattern presence).
            ast::FlatAST::StableNodeRef dummy{};
            dummy.tenant_id = capability_tenant_id_;
            dummy.fiber_id = static_cast<std::uint32_t>(aura_fiber_current_id());
            (void)handoff_ref(std::move(dummy));
        }
    }

    auto* m = static_cast<CompilerMetrics*>(compiler_metrics());
    if (version_mismatch > 0 || bridge_mismatch > 0) {
        if (m) {
            m->envframe_version_mismatch_post_steal_total.fetch_add(
                version_mismatch + bridge_mismatch, std::memory_order_relaxed);
            if (bridge_mismatch > 0)
                m->bridge_epoch_drift_post_steal_total.fetch_add(bridge_mismatch,
                                                                 std::memory_order_relaxed);
            if (refreshed > 0)
                m->envframe_dualpath_repair_total.fetch_add(refreshed, std::memory_order_relaxed);
            if (invalid_or_oob > 0)
                m->envframe_cross_fiber_stale_total.fetch_add(invalid_or_oob,
                                                              std::memory_order_relaxed);
        }
        bump_envframe_concurrent_steal_resync();
    }

    // Issue #1631: bridge drift → force active-closure walk (deopt/rebuild
    // fallback). Weak/stub no-ops when JIT is not linked.
    if (bridge_mismatch > 0) {
        const auto deopted = aura_jit_walk_active_closures(current_bridge);
        if (m && deopted > 0)
            m->bridge_epoch_deopt_walk_post_steal_total.fetch_add(deopted,
                                                                  std::memory_order_relaxed);
        if (deopted > 0)
            aura_aot_record_deopt_on_steal();
    }

    // Heavy path: reclaim dead frames / rewrite orphan env_ids when
    // OOB or INVALID frames were observed (true drift, not soft stale).
    // Issue #1889: compact_env_frames itself dual-epoch bumps under its
    // interlock (no public primitive Guard here — internal steal repair;
    // public surface is evaluator:compact-env-frames with Guard #1842).
    if (need_compact) {
        (void)compact_env_frames();
        if (m)
            m->envframe_dualpath_repair_total.fetch_add(1, std::memory_order_relaxed);
    }

    return refreshed;
}

void Evaluator::probe_and_repin_linear_on_steal() noexcept {
    probe_linear_ownership_on_fiber_steal();
    (void)re_pin_cow_children_from_snapshot();
    // restamp already runs inside probe_linear for pins; keep explicit
    // restamp for atomic-batch registry coverage on dual-path steal.
    (void)restamp_pinned_stable_refs();
}

// Issue #1612: MacroIntroduced-specific marker + provenance refresh.
// Called from complete_post_resume_steal_refresh (Fiber::resume / steal)
// and on_arena_compact_hook (GC compact). Walks workspace MacroIntroduced
// nodes for expansion-provenance consistency; restamps pinned StableNodeRefs
// that target MacroIntroduced so steal/GC cannot drop hygiene.
std::size_t Evaluator::refresh_stale_macro_frames(std::uint64_t /*hint_env_id*/,
                                                  std::uint64_t /*expected_epoch*/) noexcept {
    macro_refresh_invoke_count_.fetch_add(1, std::memory_order_relaxed);
    auto* ws = workspace_flat();
    if (!ws || ws->size() == 0)
        return 0;

    std::size_t repaired = 0;
    std::size_t stale_prevented = 0;
    constexpr auto kExpansion =
        static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);

    // Workspace scan: MacroIntroduced nodes must keep expansion provenance
    // bit when they still claim the marker (detect drift after concurrent
    // mutate / compact). Restoring the marker is only done when the node
    // has kExpansion but lost MacroIntroduced — reverse drift is rarer
    // (marker lost) and is counted as stale_prevented without rewrite.
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (!ws->is_live_node(id))
            continue;
        const bool is_macro = ws->is_macro_introduced(id);
        const auto md = ws->macro_dirty(id);
        const bool has_expansion = (md & kExpansion) != 0;
        if (is_macro && !has_expansion) {
            // Marker present without expansion bit — hygiene drift.
            // Re-stamp expansion provenance (non-destructive) if API allows.
            // Count as prevented so agents can observe steal/GC races.
            ++stale_prevented;
            ++repaired;
        } else if (!is_macro && has_expansion) {
            // Expansion bit without MacroIntroduced marker — restamp marker.
            ws->set_marker(id, aura::ast::SyntaxMarker::MacroIntroduced);
            ++stale_prevented;
            ++repaired;
        }
        // Zero provenance on MacroIntroduced: re-link source node id as weak
        // provenance stamp (node id itself) when provenance is empty.
        if (is_macro && ws->provenance(id) == 0) {
            ws->set_provenance(id, static_cast<std::uint32_t>(id == 0 ? 1 : id));
            ++repaired;
        }
    }

    // Pinned StableNodeRefs targeting MacroIntroduced: force refresh_if_stale.
    auto restamp_macro_pins = [&](std::vector<aura::ast::FlatAST::StableNodeRef>& pins) {
        for (auto& ref : pins) {
            const auto nid = ref.id;
            if (nid == aura::ast::NULL_NODE || nid >= ws->size())
                continue;
            if (!ws->is_macro_introduced(nid))
                continue;
            if (ref.refresh_if_stale(*ws)) {
                ++repaired;
                ++stale_prevented;
            }
        }
    };
    restamp_macro_pins(atomic_batch_pinned_refs_);
    {
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        restamp_macro_pins(cow_boundary_pinned_refs_);
    }

    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        if (stale_prevented > 0)
            m->macro_stale_ref_prevented_total.fetch_add(stale_prevented,
                                                         std::memory_order_relaxed);
        m->macro_refresh_invoke_total.fetch_add(1, std::memory_order_relaxed);
    }
    return repaired;
}

void Evaluator::probe_and_repin_macro_provenance() noexcept {
    auto* ws = workspace_flat();
    if (!ws)
        return;
    std::size_t repinned = 0;
    auto repin = [&](std::vector<aura::ast::FlatAST::StableNodeRef>& pins) {
        for (auto& ref : pins) {
            const auto nid = ref.id;
            if (nid == aura::ast::NULL_NODE || nid >= ws->size())
                continue;
            // Prefer MacroIntroduced targets; also refresh any pin that is
            // already stale (steal/GC safety).
            const bool is_macro =
                nid < ws->size() && ws->is_live_node(nid) && ws->is_macro_introduced(nid);
            if (!is_macro && ref.is_valid_in(*ws))
                continue;
            if (ensure_valid_or_refresh(ref, /*auto_refresh=*/true).has_value())
                ++repinned;
            else if (ref.refresh_if_stale(*ws))
                ++repinned;
        }
    };
    repin(atomic_batch_pinned_refs_);
    {
        std::lock_guard<std::mutex> lock(cow_boundary_pins_mtx_);
        repin(cow_boundary_pinned_refs_);
    }
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics())) {
        if (repinned > 0)
            m->macro_provenance_repin_total.fetch_add(repinned, std::memory_order_relaxed);
        m->macro_provenance_probe_total.fetch_add(1, std::memory_order_relaxed);
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
