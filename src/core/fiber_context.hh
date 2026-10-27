// fiber_context.hh — Issue #4716: fiber-local evaluator execution context.
//
// Several evaluator/TLS items are not per-thread caches: they describe the
// execution context of the body that is currently running (an active
// IRInterpreter, the outermost Guard's evaluator identity, the macro-hygiene
// depth/limits of the running expand, the recursive TypeRegistry lock stack,
// the resource-quota tenant, the CLI body lock, pipeline cascade roots and
// the PCV checkpoint depth). A fiber that yields inside one of those regions
// parks its ucontext and may be resumed on another worker thread, so a
// thread_local strands (or corrupts) the value.
//
// The fix mirrors #4713's tree-walker root segment: ONE Context per fiber,
// created at spawn, and a thread_local CACHED POINTER to the segment the
// thread is running (swapped in Fiber::resume, out when control returns to
// the worker). Every access goes through active(), which falls back to a
// per-thread plain Context when no fiber is attached, so plain (non-fiber)
// threads keep exact thread_local semantics.
//
// Only the pointer is thread_local; the values never move between threads,
// so a push/pop pair can never straddle two workers (#4716 stress AC).

#ifndef AURA_CORE_FIBER_CONTEXT_HH
#define AURA_CORE_FIBER_CONTEXT_HH

#include <cstdint>
#include <vector>

namespace aura::core::fiber_ctx {

// Recursive TypeRegistry lock stack (#4404). Moved here from type_impl.cpp
// so a fiber's frames live in its own Context.
inline constexpr int kTregLockFrameCap = 8;

struct TregLockFrame {
    const void* lock = nullptr;
    int unique_depth = 0;
    int shared_skip = 0;
};

// One execution context per fiber (or per plain thread via the fallback).
// The field comments name the thread_local each one replaced.
struct Context {
    // IRInterpreter::t_active_ — the interpreter that owns the running frame
    // set (#4600 / #4617 deopt resume).
    void* ir_active = nullptr;
    // gc_hooks::g_current_eval_identity — outermost Guard evaluator identity
    // (#3617; arena per-eval last-proof slots).
    void* eval_identity = nullptr;
    // macro_expansion.cpp: inner expand checkpoint head + hygiene depth and
    // the per-expand MacroSelfEvo policy mirrors (#2023 / #2243 / #2806).
    void* macro_inner_ckpt = nullptr;
    int macro_depth = 0;
    int macro_effective_max_depth = 0;
    bool macro_allow_rest_hygiene = true;
    bool macro_force_hygienic = false;
    std::uint32_t macro_max_gensym_map = 0;
    std::uint32_t macro_max_violations = 0;
    // type_impl.cpp: recursive TypeRegistry lock stack (#4404).
    TregLockFrame treg_frames[kTregLockFrameCap];
    int treg_top = 0;
    // resource_quota::t_quota_tenant (#3049 / #3668).
    std::uint64_t quota_tenant = 0;
    // evaluator_primitives_messaging.cpp: scoped CLI body lock (#4054).
    void* cli_body_lock = nullptr;
    // dirty_propagation.ixx: pipeline cascade roots for this drain.
    std::vector<std::uint32_t> cascade_roots;
    // persistent_child_vector.hh: live MutationCheckpoint depth (#3233).
    std::uint32_t pcv_checkpoint_live = 0;
    // lowering_impl.cpp: #4681 call-slot scratch (lower-call scoped).
    const void* lower_call_slot_cells = nullptr;
    void* lower_call_slot_hits = nullptr;
    // Observability: owning fiber id (0 = plain thread fallback).
    std::uint64_t owner_fiber = 0;
};

// Issue #4716: the running fiber's context, or nullptr on a plain thread.
// Defined in fiber_context.cpp — ONE definition process-wide (a header
// thread_local would be duplicated per module TU under GCC module linkage,
// which is exactly the split that would make the swap invisible).
extern thread_local Context* t_ctx_cur;

// Cold path: this thread's plain (non-fiber) fallback context.
Context* plain_context() noexcept;

// Issue #4716: hot-path accessor. The cached-pointer load is the whole cost
// on a fiber thread; the fallback call only happens off-fiber.
inline Context& active() noexcept {
    Context* c = t_ctx_cur;
    return c ? *c : *plain_context();
}

extern "C" {
// Create/destroy a fiber's context. create() returns nullptr on OOM (the
// fiber then runs on the plain fallback — degraded, never crashes).
Context* aura_fiber_ctx_create(std::uint64_t owner_fiber) noexcept;
void aura_fiber_ctx_destroy(Context* ctx) noexcept;
// Install the fiber's context on the running thread (Fiber::resume) and
// drop it when control returns to the worker. Nested attaches restore the
// previous pointer, mirroring aura_tw_roots_attach/detach.
void aura_fiber_ctx_attach(Context* ctx) noexcept;
Context* aura_fiber_ctx_detach() noexcept;
// Issue #4716 observability: attaches + fiber switches, and how many plain
// threads ever needed the fallback (proves the fallback path still runs).
std::uint64_t aura_fiber_ctx_attach_total() noexcept;
std::uint64_t aura_fiber_ctx_detach_total() noexcept;
std::uint64_t aura_fiber_ctx_plain_total() noexcept;
// Issue #4716 stress/AC probes. These operate on the SAME storage the
// production paths use (treg_frames/treg_top is exactly what
// TypeRegistry::TypeRegistryLock::lock/unlock push and pop), without taking
// the registry mutex, so a test can hold a "lock frame" across a simulated
// yield and prove a sibling context cannot see it.
void aura_fiber_ctx_treg_push_probe(const void* tag) noexcept;
void aura_fiber_ctx_treg_pop_probe(const void* tag) noexcept;
int aura_fiber_ctx_treg_depth_probe() noexcept;
}

} // namespace aura::core::fiber_ctx

#endif // AURA_CORE_FIBER_CONTEXT_HH
