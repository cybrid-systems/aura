// evaluator_fiber_mutation.cpp — P1-l: per-fiber mutation stack + boundary hooks
// aura.compiler.evaluator module partition.

module;

#include "messaging_bridge.h"
#include "serve/fiber.h"

module aura.compiler.evaluator;

import std;

namespace aura::compiler {

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
// Issue #264: Evaluator instance for yield/resume hooks (set by
// outermost MutationBoundaryGuard; cleared on guard exit).
thread_local aura::compiler::Evaluator* g_yield_hook_evaluator = nullptr;
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

// Implementation of active_mutation_stack() — the
// header has the declaration only (to avoid pulling
// fiber.h into evaluator.ixx). Here we have full access
// to the Fiber class definition.
std::vector<aura::compiler::Evaluator::MutationCheckpoint>&
aura::compiler::Evaluator::active_mutation_stack() {
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        void* p = fiber->mutation_stack_ptr();
        if (p == nullptr) {
            // Lazy allocation: first enter on this fiber.
            p = new std::vector<MutationCheckpoint>();
            fiber->set_mutation_stack_ptr(p);
        }
        return *static_cast<std::vector<MutationCheckpoint>*>(p);
    }
    return g_main_thread_stack;
}

std::vector<aura::compiler::Evaluator::YieldBoundaryCheckpoint>&
aura::compiler::Evaluator::active_yield_checkpoint_stack() {
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        void* p = fiber->yield_checkpoint_ptr();
        if (p == nullptr) {
            p = new std::vector<YieldBoundaryCheckpoint>();
            fiber->set_yield_checkpoint_ptr(p);
        }
        return *static_cast<std::vector<YieldBoundaryCheckpoint>*>(p);
    }
    return g_main_thread_yield_checkpoints;
}

std::vector<aura::compiler::Evaluator::YieldBoundaryCheckpoint>&
aura::compiler::Evaluator::active_yield_checkpoint_stack_static() {
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        void* p = fiber->yield_checkpoint_ptr();
        if (p == nullptr) {
            p = new std::vector<YieldBoundaryCheckpoint>();
            fiber->set_yield_checkpoint_ptr(p);
        }
        return *static_cast<std::vector<YieldBoundaryCheckpoint>*>(p);
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
    bool had_boundary =
        any_active_mutation_boundary() || !active_mutation_stack().empty();
    if (!had_boundary && !at_mutation_boundary_yield)
        return;
    YieldBoundaryCheckpoint cp;
    cp.defuse_version = defuse_version_snapshot();
    cp.boundary_depth = mutation_boundary_depth();
    cp.mutation_stack_depth = active_mutation_stack().size();
    cp.thread_id = std::this_thread::get_id();
    cp.had_active_boundary = had_boundary || at_mutation_boundary_yield;
    active_yield_checkpoint_stack().push_back(cp);
    mutation_yield_count_.fetch_add(1, std::memory_order_relaxed);
}

bool aura::compiler::Evaluator::restore_post_yield_or_rollback() {
    auto& stack = active_yield_checkpoint_stack();
    if (stack.empty())
        return true;
    auto cp = stack.back();
    stack.pop_back();
    if (!cp.had_active_boundary)
        return true;
    bool thread_migrated = cp.thread_id != std::this_thread::get_id();
    bool version_drift = !is_version_current(cp.defuse_version);
    bool depth_mismatch = mutation_boundary_depth() != cp.boundary_depth;
    if (thread_migrated || version_drift || depth_mismatch) {
        cross_fiber_rollback_count_.fetch_add(1, std::memory_order_relaxed);
        if (outermost_mutation_success_flag_)
            *outermost_mutation_success_flag_ = false;
        return false;
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
        using C = aura::compiler::Evaluator::MutationCheckpoint;
        delete static_cast<std::vector<C>*>(p);
    }
    void fiber_yield_checkpoint_deleter_impl(void* p) {
        using C = aura::compiler::Evaluator::YieldBoundaryCheckpoint;
        delete static_cast<std::vector<C>*>(p);
    }
    void fiber_yield_checkpoint_impl(uint8_t reason) {
        if (!g_yield_hook_evaluator)
            return;
        bool at_boundary =
            reason == static_cast<uint8_t>(aura::serve::YieldReason::MutationBoundary);
        g_yield_hook_evaluator->checkpoint_yield_boundary(at_boundary);
    }
    void fiber_resume_validate_impl() {
        if (!g_yield_hook_evaluator)
            return;
        (void)g_yield_hook_evaluator->restore_post_yield_or_rollback();
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
    // The static version uses the same per-fiber / main-thread
    // routing logic. We could call active_mutation_stack() but
    // it's a non-static member, so we inline the routing here.
    if (g_current_fiber_void != nullptr) {
        auto* fiber = static_cast<aura::serve::Fiber*>(g_current_fiber_void);
        void* p = fiber->mutation_stack_ptr();
        if (p == nullptr) {
            p = new std::vector<MutationCheckpoint>();
            fiber->set_mutation_stack_ptr(p);
        }
        return *static_cast<std::vector<MutationCheckpoint>*>(p);
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
void Evaluator::bind_yield_hook_evaluator() { g_yield_hook_evaluator = this; }

// Issue #456: per-thread query-evaluator accessors.
void Evaluator::set_query_evaluator(Evaluator* ev) noexcept {
    g_query_evaluator = ev;
}
Evaluator* Evaluator::get_query_evaluator() noexcept {
    return g_query_evaluator;
}

void Evaluator::unbind_yield_hook_evaluator() {
    if (g_yield_hook_evaluator == this)
        g_yield_hook_evaluator = nullptr;
}

// Issue #285: yield_hook_evaluator() getter.
Evaluator* Evaluator::yield_hook_evaluator() noexcept {
    return g_yield_hook_evaluator;
}

void Evaluator::yield_mutation_boundary() {
    if (aura::messaging::g_fiber_yield_mutation_boundary)
        aura::messaging::g_fiber_yield_mutation_boundary();
}

// ═════════════════════════════════════════════════════════════════════════
// Issue #285: install the flush_mutation_boundary hook into the
// messaging bridge. The hook runs on the fiber thread (set/cleared
// at the outermost guard). The static-init pattern matches
// g_fiber_yield_mutation_boundary above.
//
// Bridge-callable trampoline (file-local): wraps the thread-local
// `g_yield_hook_evaluator` so Fiber::yield can invoke the flush
// without needing the module include.
namespace {
void flush_mutation_boundary_trampoline() {
    if (aura::compiler::Evaluator::yield_hook_evaluator())
        aura::compiler::Evaluator::yield_hook_evaluator()
            ->flush_mutation_boundary();
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
        aura::messaging::g_flush_mutation_boundary =
            &flush_mutation_boundary_trampoline;
        // Issue #354: register the
        // mutation-boundary-held check trampoline.
        aura::messaging::g_mutation_boundary_held =
            &mutation_boundary_held_trampoline;
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
}

// ═════════════════════════════════════════════════════════════════════════
// Issue #453: Panic Checkpoint lifecycle hooks across fiber migration.
//
// Three bridge trampolines. All three are file-local and use the
// thread-local `g_yield_hook_evaluator` (set by the outermost
// active MutationBoundaryGuard via bind_yield_hook_evaluator) to
// find the active evaluator. When no guard is active, the
// evaluator is null and each trampoline is a no-op (the bridge
// hooks are nullable; Fiber::yield/resume treat null as "skip").
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
    if (!ev) return;
    if (!ev->pending_panic_checkpoint()) return;
    ev->bump_panic_checkpoint_transfer_count();
    // The actual "re-stamp" is a follow-up to #453: a full
    // implementation would walk the per-fiber storage and
    // bump a generation counter so stale observers can detect
    // the transfer. For the scope-limited P0 ship, the metric
    // bump is enough to make the transfer observable.
}

// (3) g_block_gc_for_pending_checkpoint: bumps the GC-block
// counter. Called by Fiber::yield(MutationBoundary) when a
// pending checkpoint exists. The actual GC defer is a
// follow-up (requires scheduler.cpp + gc_coordinator.cpp
// integration; out of scope for the P0 ship).
void block_gc_for_pending_checkpoint_trampoline() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev) return;
    if (!ev->pending_panic_checkpoint()) return;
    ev->bump_gc_blocked_by_pending_panic();
}

struct PanicCheckpointRegistrar {
    PanicCheckpointRegistrar() {
        aura::messaging::g_pending_panic_checkpoint =
            &pending_panic_checkpoint_trampoline;
        aura::messaging::g_transfer_panic_checkpoint =
            &transfer_panic_checkpoint_trampoline;
        aura::messaging::g_block_gc_for_pending_checkpoint =
            &block_gc_for_pending_checkpoint_trampoline;
    }
};
const PanicCheckpointRegistrar g_panic_checkpoint_registrar{};

} // anonymous namespace

// Evaluator::pending_panic_checkpoint: returns true if the
// outermost active guard (tracked via thread-local
// `g_yield_hook_evaluator`) has a captured checkpoint. Returns
// false when no guard is active.
bool Evaluator::pending_panic_checkpoint() const noexcept {
    // The bridge trampoline checks the same path; this is
    // the in-module entry point used by tests + future
    // scheduler hooks. The thread-local slot is set by
    // bind_yield_hook_evaluator (called by the guard ctor)
    // and cleared by unbind_yield_hook_evaluator (dtor).
    auto* active = Evaluator::yield_hook_evaluator();
    if (active != this) return false; // not the active evaluator
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
extern "C" std::size_t
aura_evaluator_mutation_boundary_depth() {
    return Evaluator::mutation_boundary_depth();
}

// Issue #588: depth from a fiber's opaque mutation_stack_storage_.
extern "C" std::size_t
aura_evaluator_mutation_stack_depth_from_ptr(void* mutation_stack_storage) {
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
    void* p = per_fiber_stack != nullptr ? per_fiber_stack : fiber->mutation_stack_ptr();
    if (p == nullptr) {
        p = new std::vector<MutationCheckpoint>();
        fiber->set_mutation_stack_ptr(p);
    }
}

// Test seam (#588): push/pop a synthetic checkpoint on the
// active (per-fiber) mutation stack for steal-safety tests.
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
    if (!ev) return 0; // no evaluator → no guard
    return ev->request_gc_safepoint();
}

extern "C" void
aura_evaluator_wait_for_safepoint(std::uint64_t timeout_ms) {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev) return; // no evaluator → no wait
    ev->wait_for_safepoint(timeout_ms);
}

// Issue #683: linear ownership enforcement on work-steal.
extern "C" void aura_evaluator_probe_linear_on_steal() {
    auto* ev = Evaluator::yield_hook_evaluator();
    if (!ev)
        return;
    ev->probe_linear_ownership_on_fiber_steal();
}

} // namespace aura::compiler
