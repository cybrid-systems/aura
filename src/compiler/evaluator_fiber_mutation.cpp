// evaluator_fiber_mutation.cpp — P1-l: per-fiber mutation stack + boundary hooks
// aura.compiler.evaluator module partition.

module;

#include <thread>
#include <unordered_map>
#include <vector>
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
} // namespace

// Register the function pointers at static-init time. The
// fiber side calls them; we don't need the Evaluator to
// know about Fiber (one-way dependency).
struct FiberHookRegistrar {
    FiberHookRegistrar() {
        aura::serve::g_fiber_setter_ = fiber_setter_impl;
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

void Evaluator::unbind_yield_hook_evaluator() {
    if (g_yield_hook_evaluator == this)
        g_yield_hook_evaluator = nullptr;
}

void Evaluator::yield_mutation_boundary() {
    if (aura::messaging::g_fiber_yield_mutation_boundary)
        aura::messaging::g_fiber_yield_mutation_boundary();
}

} // namespace aura::compiler
