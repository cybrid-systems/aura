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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
    // Issue #4717 (P4 keystone of #4707): per-body evaluator execution
    // state. These were Evaluator members, so two fiber bodies driving one
    // Evaluator clobbered each other's self-TCO frame-reuse stamp and
    // counted each other's eval depth. They describe the RUNNING BODY, not
    // the shared code/IR/heap, so they belong to the fiber Context.
    std::size_t eval_depth = 0;
    std::size_t last_gc_temp_eval_depth = 0;
    std::size_t last_auto_gc_eval_depth = 0;
    // Issue #4599 self-TCO frame reuse, one stamp per running body.
    struct TcoReuseState {
        bool live = false;
        std::uint64_t cid = 0;
        const void* env = nullptr;
        std::uint32_t n = 0;
        std::uint64_t bridge = 0;
        std::uint64_t defuse = 0;
        std::uint64_t env_gen = 0;
        std::uint32_t syms[8] = {};
    };
    TcoReuseState tco;
    // Issue #4717: file-mode outermost-eval bookkeeping (#4465/#4471) and the
    // nested try-body depth (#4456). All three are per-body execution state;
    // they used to be shared Evaluator members.
    const void* file_diag_root_flat = nullptr;
    std::uint32_t file_diag_root_id = 0;
    std::uint32_t file_resume_line = 0;
    std::uint32_t file_resume_column = 0;
    std::uint32_t file_resume_file_id = 0;
    int try_body_depth = 0;
    // Issue #4717: the pool/flat the body's intern lookups are bound to.
    // The intern TABLES are shared read-mostly data (#4719); which pool they
    // are currently keyed for is per-body execution state.
    const void* sym_intern_pool = nullptr;
    std::uint64_t intern_pool_gen = 0;
    const void* float_intern_flat = nullptr;
    // Issue #4717: the #4599 name-resolution memo (#NameResEntry, keyed by
    // node id) is a per-body cache: two bodies writing/reading one vector
    // raced. Type-erased so this core header stays compiler-type free; the
    // evaluator installs a typed deleter.
    std::shared_ptr<void> name_res_cache;
    // Issue #4717: #4595 let-binding kind memo, also node-id keyed and
    // per-body for the same reason.
    std::shared_ptr<void> let_bind_kind_cache;
    // Issue #4717 step 2: the one-line Soft rebind bracket
    // (Evaluator::arm/disarm_rebind_light_path) describes the running body.
    // As Evaluator members, two bodies clobbered each other's arm/disarm
    // and focus; they live here now (fiber-local, plain-thread fallback
    // preserves the old per-thread semantics).
    bool rebind_light_path = false;
    bool rebind_light_cascaded = false;
    bool rebind_light_stamped = false;
    std::string rebind_light_focus;
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
