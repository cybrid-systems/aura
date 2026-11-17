module;
#include "core/ownership.hh"                // Issue #4936: immortal<T>
#include "rebind_phase_stats.h"             // per-phase rebind timing (opt-in)
#include "compiler/security_capabilities.h" // Issue #4876: macro deny text
#include <algorithm>
#include "core/atomic_fence_port.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "aura_jit.h"
#include "aura_jit_bridge.h"
#include "aot_reload_consistency_proof.h" // Issue #4510: hook demotion stamps would_allow_native=false
#include "lock_order_audit.h"
#include "gc_coord_scope.h" // Issue #2131: pin → cascade → audit state machine
#include "runtime_shared.h"
#include "value_tags.h" // Issue #181 Cycle 2: v2 string encoding helpers
#include "observability_metrics.h"
#include "observability_snapshot.h"
#include "per_defuse_index.h"
#include <atomic>
#include "messaging_bridge.h"
#include "core/arena_auto_policy_stats.h"
#include "core/gc_hooks.h"
#include "core/post_compact_lifecycle.hh" // Issue #2436: post-compact IR dirty sync step
#include "core/workspace_epoch.hh" // Issue #1964 cycle 2b: current_mutation_epoch / bump_mutation_epoch
#include "core/workspace_isolation.hh" // Issue #4134: existing tenant-isolation counter (fail-closed deny)
#include "jit_typed_mutation_stats.h"
#include "typed_mutation_audit.h" // Issue #1884: TypeProp ↔ invariant correlation
#include "linear_occurrence_mutate_stats.h"
#include "shape_jit_pass_closedloop_stats.h"
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef AURA_BUILD_GIT_SHA
#define AURA_BUILD_GIT_SHA "unknown"
#endif
#include <poll.h>
#include "serve/fiber.h"
#include "serve/gc_coordinator.h"
#include "shape.h"
#include "shape_profiler.h"
#include "compiler/type_feedback.h" // Issue #4568: per-arg feedback, no profiler lock
#include "spec_jit_controller.h"
#include "hash_meta.h"                     // FNV constants (#901)
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>
#include "hot_update_registry.hh"       // #2127: deopt-window signals for adaptive thr
#include "pipeline_policy.hh"           // Issue #2213: tree-walker fallback production gate
#include "ir_cache_boundary_restamp.hh" // Issue #4377: boundary-exit defuse restamp
#include "jit_assumptions.hh"           // Issue #4569: assumption-based JIT invalidation

#include "core/runtime_config.hh"
export module aura.compiler.service;
import std;
import aura.core.consts; // module-owned numeric constants (#901/#905)
import aura.core;
import aura.compiler.ir_cache_pure;
import aura.compiler.ast_walkers;
import aura.core.type;
import aura.parser.parser;
import aura.core.mutation;
import aura.core.lifetime_pin; // Issue #4683: linear_root_snapshot gate
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_soa;
import aura.compiler.ir_executor;
import aura.compiler.pass_manager;
import aura.compiler.optimization_passes; // Issue #3007: Production hot residual CastOp sweep
import aura.compiler.dirty_propagation;   // Issue #2106: cascade_skip metrics sink
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.compiler.cache;
import aura.diag;
import aura.core.error; // Issue #807/#808: AuraResult bridge

// Issue #3680: weak probe — JIT-less links may not resolve the strong
// definition (typed_mutation_audit_hooks.cpp); null ⇒ fail open to the
// joint bump (Soft/Off semantics). Purview placement AFTER the contiguous
// import block (post-module-declaration imports must be contiguous), not
// the GMF (only preprocessor inclusions allowed there).
extern "C" int aura_production_defaults_active_probe() noexcept __attribute__((weak));

// Issue #4078: production pass/depth/gensym/steal/cap must not typecheck
// or eval the flat macro_expand_all just returned. Strong def in
// macro_expansion.cpp. Soft/Off returns 0 (historical half-expand).
extern "C" int aura_hygiene_expand_deny_blocks_eval(void) noexcept;

// Issue #4083: partial-relower success drops installed ScalarFn rows
// after ORC prefix removal. Strong def in aura_jit_runtime.cpp.
extern "C" void aura_drop_jit_fn_native_for_define(const char* name);

// Issue #4388: cache-hit republish samples epoch + define-drop generation.
// Strong defs in aura_jit_runtime.cpp.
extern "C" void aura_jit_cache_hit_republish_arm(std::uint64_t epoch, std::uint64_t drop_gen);
extern "C" void aura_jit_set_defuse_word(const std::atomic<std::uint64_t>* word); // #4600
extern "C" void aura_jit_cache_hit_republish_disarm(void);
extern "C" int aura_jit_cache_hit_republish_refused(void);
extern "C" std::uint64_t aura_jit_define_drop_gen(void);
extern "C" std::int64_t aura_jit_fn_ptr_for_test(std::int64_t func_id);
extern "C" void aura_register_fn_named(const char* name, std::int64_t func_id,
                                       std::int64_t (*fn_ptr)(std::int64_t*, std::uint32_t),
                                       std::int32_t local_count, std::int32_t arg_count,
                                       std::int32_t env_count);

// Issue #4084: prologue deopt returns fixnum 0. Take-and-clear the
// thread-local sentinel so try_jit_execute does not publish that 0.
// Strong def in aura_jit_bridge.cpp; weak 0 in the light stub.
extern "C" int aura_jit_take_prologue_deopt_sentinel(void) noexcept;
// Issue #5174: native-frame ceiling. Distinct from the prologue sentinel
// so exec_jit can refuse instead of falling through to eval_ir.
extern "C" const char* aura_jit_take_depth_message(void) noexcept;
// Issue #4682 (6b): native tier for hot workspace defines. Strong defs in
// aura_jit_bridge.cpp / aura_jit_runtime.cpp; weak "armed" / -1 in the
// light stub (no tier there).
extern "C" int aura_jit_deopt_frame_host_armed(void) noexcept;
extern "C" std::int64_t aura_jit_exchange_native_self_id(std::int64_t self_id) noexcept;
extern "C" void aura_jit_set_linear_env_none(void);
extern "C" void aura_jit_tier_self_cell_swap_4682(std::int64_t* idx, std::int64_t* word) noexcept;
extern "C" void aura_jit_suppress_runtime_reset_this_thread(int enable) noexcept;

[[nodiscard]] inline aura::diag::Diagnostic
hygiene_expand_deny_diag_4078(const aura::ast::FlatAST* flat = nullptr) {
    // Issue #4876: the sentinel code is not a user-facing explanation.
    const char* why = aura_macro_hygiene_last_limit_reason_string();
    std::string reason =
        aura::compiler::security::macro_limit_user_reason(why, "hygiene-pass-limit");
    // Issue #5241: keyed on the sentinel reason code, not the rendered text.
    const auto kind = (why != nullptr && std::string_view(why) == "capability-deny")
                          ? aura::diag::ErrorKind::EffectDenied
                          : aura::diag::ErrorKind::InternalError;
    // #5235: the refusal is a source-level diagnostic — point it at the macro
    // definition that triggered the expand gate instead of a bare message.
    aura::diag::SourceLocation loc{};
    std::uint32_t node_id = ~0u;
    if (flat != nullptr) {
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            const auto& node = flat->get(id);
            if (node.tag != aura::ast::NodeTag::MacroDef || node.line == 0)
                continue;
            loc = aura::diag::SourceLocation{node.line, node.col == 0 ? 1u : node.col,
                                             aura::diag::source_file_for_flat(flat)};
            node_id = static_cast<std::uint32_t>(id);
            break;
        }
    }
    return aura::diag::Diagnostic{kind, std::move(reason), loc, node_id};
}

// ── Issue #4711: eval-mutex (M1) release across a blocking wait ───────
//
// Single lock order, never reversed: body mutex M0 (the CLI/denseness
// unique_lock published in the messaging TLS slot, #4356/#4674) then eval
// mutex M1 (eval_mutex_, #1431). CompilerService::eval takes M0 then M1;
// a fiber body takes M0 and may enter eval (eval-current / the JIT
// recompile hook) and then needs M1.
//
// A blocking wait inside eval (fiber:join, read-line, http-post, channel
// park) drops M0 so a sibling body can run. Re-taking M0 on resume while
// M1 is still held is an M1 → M0 acquisition: TSan
// lock-order-inversion #4711, and a real deadlock once the sibling body
// holds M0 and waits for M1. The wait guards therefore drop M1 too
// (EvalMutexWaitLockGuard in messaging_bridge.h, declared before the
// body-lock guard) and re-take it AFTER M0, so every path keeps the same
// M0 → M1 order. Release order across the wait is the reverse.
struct EvalMutexWaitTls {
    std::recursive_mutex* mtx = nullptr;
    // Recursive locks this thread holds through the published scope.
    int depth = 0;
    // Issue #4717 follow-up: interpreter execution mutexes (IRDefineEnvBinding::exec_mtx,
    // taken by run_pinned_ir_closure_) this thread holds. Dispatch pins the
    // binding, drops the cache gate and then takes exec_mtx for the run, so
    // the order is M0 body lock -> M1 eval mutex -> M2 exec mutex and a
    // blocking wait must release them in the exact reverse order. Without M2
    // a body spawned from inside the interpreter (orch:parallel) deadlocks:
    // the joiner parks in IRInterpreter::call_closure holding exec_mtx while
    // the spawned body needs that same mutex to dispatch its own define.
    struct ExecHold {
        std::recursive_mutex* m = nullptr;
        int depth = 0;
    };
    std::vector<ExecHold> exec_holds;
    // True between the release hook and its reacquire. A wait nested inside a
    // released region must NOT unlock the same recursive mutex twice (that is
    // UB), so the inner release is a no-op until the outer reacquire runs.
    bool exec_released = false;
};
static thread_local EvalMutexWaitTls t_eval_mutex_wait_tls;

static void* eval_mutex_wait_release_impl() {
    EvalMutexWaitTls& t = t_eval_mutex_wait_tls;
    if (t.exec_released)
        return nullptr; // an enclosing wait already dropped them
    const bool has_eval = t.mtx != nullptr && t.depth > 0;
    if (!has_eval && t.exec_holds.empty())
        return nullptr; // this thread holds nothing — nothing to drop
    // Reverse acquisition order: exec mutexes (M2) topmost first, THEN the
    // eval mutex (M1). M0 is released by the wait's body-lock guard after
    // this hook returns.
    for (auto it = t.exec_holds.rbegin(); it != t.exec_holds.rend(); ++it) {
        for (int i = 0; i < it->depth; ++i)
            it->m->unlock();
    }
    const int n = has_eval ? t.depth : 0;
    // Full-depth unwind: a re-entrant eval (Path B) holds the recursive
    // mutex more than once, and a partial unlock would leave it owned by
    // this (parked) thread — the inversion would just rotate.
    for (int i = 0; i < n; ++i)
        t.mtx->unlock();
    t.depth = 0;
    t.exec_released = true;
    // Non-zero whenever anything was dropped (n can be 0 with only M2 held).
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(n) + 1);
}

static void eval_mutex_wait_reacquire_impl(void* token) {
    EvalMutexWaitTls& t = t_eval_mutex_wait_tls;
    const int raw = static_cast<int>(reinterpret_cast<std::intptr_t>(token)) - 1;
    if (raw < 0)
        return;
    const int n = raw;
    t.exec_released = false;
    // Acquisition order: M1 first (the M0 body lock is re-taken by the wait
    // guard before this hook), then the exec mutexes (M2) bottom-to-top.
    if (t.mtx != nullptr && n > 0) {
        for (int i = 0; i < n; ++i)
            t.mtx->lock();
        t.depth = n;
    }
    for (auto& h : t.exec_holds) {
        for (int i = 0; i < h.depth; ++i)
            h.m->lock();
    }
}

// Publish the eval mutex this scope holds in the TLS slot the wait hooks
// read. Nested eval (Path B) stacks: the enclosing scope is restored on
// exit, and a wait/re-acquire inside the inner scope unwinds the full
// recursive depth.
struct EvalMutexWaitPublishScope {
    std::recursive_mutex* prev_mtx = nullptr;
    int prev_depth = 0;
    std::recursive_mutex& mtx;
    EvalMutexWaitPublishScope(const EvalMutexWaitPublishScope&) = delete;
    EvalMutexWaitPublishScope& operator=(const EvalMutexWaitPublishScope&) = delete;
    EvalMutexWaitPublishScope(EvalMutexWaitPublishScope&&) = delete;
    EvalMutexWaitPublishScope& operator=(EvalMutexWaitPublishScope&&) = delete;
    explicit EvalMutexWaitPublishScope(std::recursive_mutex& m)
        : mtx(m) {
        prev_mtx = t_eval_mutex_wait_tls.mtx;
        prev_depth = (prev_mtx == &m) ? t_eval_mutex_wait_tls.depth : 0;
        t_eval_mutex_wait_tls.mtx = &m;
        t_eval_mutex_wait_tls.depth = prev_depth + 1;
    }
    ~EvalMutexWaitPublishScope() {
        t_eval_mutex_wait_tls.mtx = prev_mtx;
        t_eval_mutex_wait_tls.depth = prev_depth;
    }
};

// Issue #4717 follow-up: publish an interpreter execution mutex (M2) for the duration of
// the run so a blocking wait inside the body drops it too (see
// EvalMutexWaitTls::exec_holds for the order).
struct ExecMutexWaitPublishScope {
    std::recursive_mutex& mtx;
    ExecMutexWaitPublishScope(const ExecMutexWaitPublishScope&) = delete;
    ExecMutexWaitPublishScope& operator=(const ExecMutexWaitPublishScope&) = delete;
    ExecMutexWaitPublishScope(ExecMutexWaitPublishScope&&) = delete;
    ExecMutexWaitPublishScope& operator=(ExecMutexWaitPublishScope&&) = delete;
    explicit ExecMutexWaitPublishScope(std::recursive_mutex& m)
        : mtx(m) {
        t_eval_mutex_wait_tls.exec_holds.push_back({&m, 1});
    }
    ~ExecMutexWaitPublishScope() {
        auto& holds = t_eval_mutex_wait_tls.exec_holds;
        if (!holds.empty())
            holds.pop_back();
    }
};

// Issue #4765: the registry walks (expire_stale_live_closures_, restamp,
// poison, GC-root collect, env-id remap) hold the IR cache gate and take each
// define's exec_mtx. A body running on that define holds exec_mtx and, when
// it calls another define, re-enters dispatch and waits for the gate: an ABBA
// deadlock against a sibling fiber's mutate:rebind (gdb on the aura-harness
// swarm run: body in run_pinned_ir_closure_ -> dispatch_ir_define_closure ->
// gate; rebind in mark_define_dirty -> expire_stale_live_closures_ ->
// exec_mtx). Dropping exec_mtx for the gate wait is NOT safe (a sibling body
// then re-enters the same non-reentrant interpreter: ~ExecFrame SIGSEGV).
//
// Instead a thread that must BLOCK (on the gate, or on another define's
// exec_mtx) while holding exec mutexes PARKS them: it records them here,
// blocks, and on wake-up unparks and waits for any walker still inside one
// of its interpreters. A walker that holds the gate and finds a parked
// exec_mtx borrows that interpreter for one binding's walk.
static std::mutex g_exec_parked_mtx_4765;
static std::condition_variable g_exec_parked_cv_4765;
struct ExecParkState4765 {
    int parked = 0;  // owner threads blocked with this exec_mtx held
    int walking = 0; // walkers currently inside the interpreter unlocked
};
static std::unordered_map<const std::recursive_mutex*, ExecParkState4765> g_exec_parked_4765;

// Park every exec_mtx this thread holds (it is about to block).
static bool park_exec_holds_4765() {
    EvalMutexWaitTls& t = t_eval_mutex_wait_tls;
    if (t.exec_released || t.exec_holds.empty())
        return false;
    std::scoped_lock<std::mutex> lk(g_exec_parked_mtx_4765);
    for (auto& h : t.exec_holds)
        ++g_exec_parked_4765[h.m].parked;
    return true;
}

// Unpark after the wait: no new walker may start, and the thread does not
// resume the body until every walker already inside one of its
// interpreters has left.
static void unpark_exec_holds_4765() {
    EvalMutexWaitTls& t = t_eval_mutex_wait_tls;
    std::unique_lock<std::mutex> lk(g_exec_parked_mtx_4765);
    for (auto& h : t.exec_holds) {
        auto it = g_exec_parked_4765.find(h.m);
        if (it != g_exec_parked_4765.end())
            --it->second.parked;
    }
    g_exec_parked_cv_4765.wait(lk, [&] {
        for (auto& h : t.exec_holds) {
            auto it = g_exec_parked_4765.find(h.m);
            if (it != g_exec_parked_4765.end() && it->second.walking > 0)
                return false;
        }
        return true;
    });
    for (auto& h : t.exec_holds) {
        auto it = g_exec_parked_4765.find(h.m);
        if (it != g_exec_parked_4765.end() && it->second.parked <= 0 && it->second.walking <= 0)
            g_exec_parked_4765.erase(it);
    }
}

// Blocking acquisition from a thread that may hold exec mutexes (the IR
// cache gate, or another define's exec_mtx): park while blocked.
static void lock_park_aware_4765(std::recursive_mutex& m) {
    if (m.try_lock())
        return;
    if (!park_exec_holds_4765()) {
        m.lock();
        return;
    }
    m.lock();
    unpark_exec_holds_4765();
}

static void lock_ir_gate_body_safe_4765(std::recursive_mutex& gate) {
    lock_park_aware_4765(gate);
}

// Issue #4765: exec_mtx taken by a walker that HOLDS the IR cache gate.
// Owns the lock when it is free (or held by this thread); otherwise, once
// the owner is parked (blocked, see above), borrows the interpreter for the
// scope of this guard - the owner cannot resume until the guard is gone.
struct ExecUnderGate4765 {
    std::recursive_mutex& m;
    bool owned = false;
    bool borrowed = false;
    explicit ExecUnderGate4765(std::recursive_mutex& gate_mutex)
        : m(gate_mutex) {
        for (;;) {
            if (m.try_lock()) {
                owned = true;
                return;
            }
            {
                std::scoped_lock<std::mutex> lk(g_exec_parked_mtx_4765);
                auto it = g_exec_parked_4765.find(&m);
                if (it != g_exec_parked_4765.end() && it->second.parked > 0) {
                    ++it->second.walking;
                    borrowed = true;
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    }
    ExecUnderGate4765(const ExecUnderGate4765&) = delete;
    ExecUnderGate4765& operator=(const ExecUnderGate4765&) = delete;
    ExecUnderGate4765(ExecUnderGate4765&&) = delete;
    ExecUnderGate4765& operator=(ExecUnderGate4765&&) = delete;
    ~ExecUnderGate4765() {
        if (owned)
            m.unlock();
        if (borrowed) {
            {
                std::scoped_lock<std::mutex> lk(g_exec_parked_mtx_4765);
                auto it = g_exec_parked_4765.find(&m);
                if (it != g_exec_parked_4765.end()) {
                    --it->second.walking;
                    if (it->second.parked <= 0 && it->second.walking <= 0)
                        g_exec_parked_4765.erase(it);
                }
            }
            g_exec_parked_cv_4765.notify_all();
        }
    }
};

// lock_guard-shaped exec_mtx acquisition for a body run (parks while blocked).
struct ExecRunLock4765 {
    std::recursive_mutex& m;
    explicit ExecRunLock4765(std::recursive_mutex& run_mutex)
        : m(run_mutex) {
        lock_park_aware_4765(run_mutex);
    }
    ExecRunLock4765(const ExecRunLock4765&) = delete;
    ExecRunLock4765& operator=(const ExecRunLock4765&) = delete;
    ExecRunLock4765(ExecRunLock4765&&) = delete;
    ExecRunLock4765& operator=(ExecRunLock4765&&) = delete;
    ~ExecRunLock4765() { m.unlock(); }
};

// #5242: eval lives in its own TU now; the hook wiring stays here with its
// TU-local statics and eval reaches it through this member.
static void wire_eval_mutex_wait_hooks() {
    if (aura::messaging::g_eval_mutex_wait_release == nullptr) {
        aura::messaging::g_eval_mutex_wait_release = &eval_mutex_wait_release_impl;
        aura::messaging::g_eval_mutex_wait_reacquire = &eval_mutex_wait_reacquire_impl;
    }
}

// Issue #1885: CompilerService is the primary Compiler-layer entry.
// Dependency direction: Compiler → Core + Parser (not reverse).
// Layering authority: src/core/module_boundary.ixx — update it when
// adding new cross-layer edges (e.g. serve/fiber bridges below).
static_assert(aura::core::boundary::AllowedDependency<aura::core::boundary::ModuleLayer::Compiler,
                                                      aura::core::boundary::ModuleLayer::Core>);
static_assert(aura::core::boundary::AllowedDependency<aura::core::boundary::ModuleLayer::Compiler,
                                                      aura::core::boundary::ModuleLayer::Parser>);
static_assert(!aura::core::boundary::layer_may_depend_on(
    aura::core::boundary::ModuleLayer::Core, aura::core::boundary::ModuleLayer::Compiler));
#if defined(AURA_PRODUCTION_PACK)
static_assert(aura::compiler::opt_registry::kDefaultOptPipelineAosDeleted,
              "Issue #4007: production TUs delete the AoS named factory");
#endif

// ── JIT primitive call dispatcher ────────────────────────
// Bridges OpPrimCall/OpPrimitive from JIT code to evaluator PrimFn table.
// The PrimId enum value is used as an index into kPrimNames to find
// the primitive name, then looked up in the evaluator's primitives table.
// Uses a global primitives pointer set by CompilerService::register_jit_primitives().

// PrimId name table (mirrors ir.ixx kPrimNames — must stay in sync)
static constexpr const char* kPrimNameTable[] = {
    "hash",
    "hash-length",
    "hash-has-key?",
    "hash-keys",
    "hash-values",
    "string-append",
    "string-length",
    "string-ref",
    "substring",
    "string=?",
    "string<?",
    "number->string",
    "string->number",
    "display",
    "write",
    "newline",
    "error",
    "assert",
    "read",
    "read-file",
    "write-file",
    "file-exists?",
    "gensym",
    "apply",
    "vector",
    "vector-ref",
    "vector-set!",
    "vector-length",
    "vector?",
    "make-vector",
    "import",
    "char=?",
    "char<?",
    "char->integer",
    "integer->char",
    "quotient",
    "remainder",
    "length",
    "list-ref",
    "reverse",
    "raise",
    "error?",
    "pair?",
    "null?",
    // Issue #4175: R7RS string comparisons — keep in sync with ir.ixx kPrimNames.
    "string>?",
    "string>=?",
    "string<=?",
};

static std::atomic<const aura::compiler::Primitives*> g_jit_prim_ctx{nullptr};

// Issue #3593: the Evaluator that owns the wired prim table — the same
// owner the IR executor uses for invoke_prim_with_telemetry
// (ir_executor_impl.cpp context_.evaluator). Null when the JIT link is
// stubbed / unwired -> aura_jit_prim_dispatch fails closed (no silent
// exec), same shape as the #3275 weak-abort rule.
static std::atomic<aura::compiler::Evaluator*> g_jit_prim_owner{nullptr};

// Issue #3593: resolve the owner Evaluator for the wired table. The ctx
// must still point at prims (stale prims pointer -> no exec).
static aura::compiler::Evaluator*
owner_evaluator(const aura::compiler::Primitives* prims) noexcept {
    if (prims != nullptr && g_jit_prim_ctx.load(std::memory_order_acquire) == prims)
        return g_jit_prim_owner.load(std::memory_order_acquire);
    return nullptr;
}

// Nested CompilerService last-writer-wins: inner ctor overwrites process-wide
// linear / lock / top-cell wires with &evaluator_. A thread_local stack
// restores the previous Evaluator* on ~inner so ~outer is not left with
// lock_write / linear_scan on a destroyed workspace_mtx_ (glibc
// "corrupted double-linked list" / SIGBUS in aura_alloc_closure).
inline thread_local std::vector<aura::compiler::Evaluator*> g_linear_hook_eval_stack;
static thread_local std::vector<aura::compiler::Evaluator*> g_lock_hook_eval_stack;

inline void install_linear_hooks_for_eval(aura::compiler::Evaluator* ev) {
    aura_set_linear_post_mutate_enforce_fn(
        [](void* user, std::uint32_t env_id) -> int {
            if (!user)
                return 0;
            auto* e = static_cast<aura::compiler::Evaluator*>(user);
            return e->linear_post_mutate_enforce(env_id) ? 0 : 1;
        },
        ev);
    aura_set_linear_live_closure_scan_fn(
        [](void* user) {
            if (!user)
                return;
            static_cast<aura::compiler::Evaluator*>(user)->scan_live_closures_for_linear_captures(
                true);
        },
        ev);
}

static void pop_linear_hooks_for_eval(aura::compiler::Evaluator* ev) {
    if (!g_linear_hook_eval_stack.empty() && g_linear_hook_eval_stack.back() == ev)
        g_linear_hook_eval_stack.pop_back();
    if (g_linear_hook_eval_stack.empty()) {
        aura_set_linear_post_mutate_enforce_fn(nullptr, nullptr);
        aura_set_linear_live_closure_scan_fn(nullptr, nullptr);
    } else {
        install_linear_hooks_for_eval(g_linear_hook_eval_stack.back());
    }
}

static void install_lock_and_topcell_hooks_for_eval(aura::compiler::Evaluator* ev) {
    aura_set_lock_hooks(
        [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->lock_workspace_shared(); },
        [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->unlock_workspace_shared(); },
        [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->lock_workspace_unique(); },
        [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->unlock_workspace_unique(); },
        [](void* e) -> std::uint64_t {
            return static_cast<aura::compiler::Evaluator*>(e)->get_defuse_version();
        },
        [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->yield_mutation_boundary(); },
        static_cast<void*>(ev));
    // Issue #4600 §5: publish the defuse atomic's address so JIT car/cdr
    // guards acquire-load the word directly (no per-access hook call).
    aura_jit_set_defuse_word(ev->defuse_version_word());
    aura_set_top_cell_getter(
        [](void* e, int64_t idx) -> int64_t {
            auto* evp = static_cast<aura::compiler::Evaluator*>(e);
            if (idx < 0 || static_cast<std::size_t>(idx) >= evp->cells().size())
                return 0;
            return evp->cells()[static_cast<std::size_t>(idx)].val;
        },
        static_cast<void*>(ev));
    // Issue #4321: JIT keyword literals intern into this evaluator.
    aura_set_keyword_hooks(
        [](void* e, const char* name) -> int64_t {
            auto* evp = static_cast<aura::compiler::Evaluator*>(e);
            if (!evp)
                return 0;
            return evp->intern_keyword(name ? std::string_view(name) : std::string_view{}).val;
        },
        [](void* e, int64_t idx) -> const char* {
            auto* evp = static_cast<aura::compiler::Evaluator*>(e);
            if (!evp || idx < 0)
                return nullptr;
            const auto& kt = evp->keyword_table();
            const auto u = static_cast<std::size_t>(idx);
            if (u >= kt.size())
                return nullptr;
            return kt[u].c_str();
        },
        static_cast<void*>(ev));
    // Issue #4727: JIT quoted-symbol atoms intern into the same evaluator-
    // global symbol table the tree walker uses, so eq?/symbol? agree across
    // the tier face and any workspace COW layer.
    aura_set_symbol_hooks(
        [](void* e, const char* name) -> int64_t {
            auto* evp = static_cast<aura::compiler::Evaluator*>(e);
            if (!evp)
                return 0;
            return evp->intern_symbol(name ? std::string_view(name) : std::string_view{}).val;
        },
        [](void* e, int64_t idx) -> const char* {
            auto* evp = static_cast<aura::compiler::Evaluator*>(e);
            if (!evp || idx < 0)
                return nullptr;
            const auto& st = evp->symbol_table();
            const auto u = static_cast<std::size_t>(idx);
            if (u >= st.size())
                return nullptr;
            return st[u].c_str();
        },
        static_cast<void*>(ev));
}

static void pop_lock_hooks_for_eval(aura::compiler::Evaluator* ev) {
    if (!g_lock_hook_eval_stack.empty() && g_lock_hook_eval_stack.back() == ev)
        g_lock_hook_eval_stack.pop_back();
    if (g_lock_hook_eval_stack.empty()) {
        aura_clear_evaluator_runtime_hooks(static_cast<void*>(ev));
    } else {
        install_lock_and_topcell_hooks_for_eval(g_lock_hook_eval_stack.back());
    }
}

// ── Dual string heaps (Issue #2575 / #181) ───────────────────
// JIT ConstString → g_string_pool via aura_alloc_string.
// Evaluator prims → Primitives::string_heap / Evaluator::string_heap_.
// Same v2 encoding; PrimCall must re-intern at the boundary.

// Issue #181 Cycle 2: STRING_BIAS_VAL_2 is the v2 string range upper bound.

inline bool is_str_val(std::int64_t val) {
    return aura::compiler::types::is_string_raw_v2(val);
}

// JIT string pool access (defined in aura_jit_runtime.cpp)
extern "C" std::size_t aura_jit_pool_size();
extern "C" const char* aura_jit_pool_string(std::size_t idx);
extern "C" std::int64_t aura_alloc_string(const char* s);
extern "C" void aura_set_hash_str_eq_callback(std::int64_t (*fn)(std::int64_t, std::int64_t));
extern "C" std::int64_t aura_hash_string_cmp_fn(std::int64_t, std::int64_t);
extern "C" void aura_set_hash_str_convert_callback(std::int64_t (*fn)(std::int64_t));
extern "C" std::int64_t aura_hash_string_convert_fn(std::int64_t);

// Issue #2100: MacroIntroduced deopt → AST restore hook (aura_jit_runtime.cpp).
extern "C" void aura_jit_set_macro_deopt_restore_fn(std::uint64_t (*fn)() noexcept);
extern "C" void aura_jit_macro_introduced_preserved_inc(std::uint64_t n);
extern "C" void aura_jit_macro_introduced_lost_inc(std::uint64_t n);
extern "C" void aura_multi_eval_macro_marker_preserved_inc(std::uint64_t n); // #2764

// Issue #2577: JIT-idx → last eval-heap index (validated by content).
// Avoids O(iterations) push_back when the same ConstString is re-converted
// on every PrimCall. Invalid entries are refreshed on content mismatch.
inline std::unordered_map<std::uint64_t, std::size_t> g_jit_idx_to_eval_idx;

// JIT-pool string → evaluator heap (PrimCall args).
// OOB of JIT pool → pass through (already evaluator-indexed).
// Issue #2577: content intern + jit-idx cache (no O(N) growth for fixed args).
inline std::int64_t convert_str_for_eval(std::int64_t val,
                                         const aura::compiler::Primitives* prims) {
    if (!is_str_val(val) || !prims)
        return val;
    std::uint64_t idx = aura::compiler::types::string_idx_raw_v2(val);
    const char* content = aura_jit_pool_string(static_cast<std::size_t>(idx));
    if (!content)
        return val;
    auto& eval_heap = const_cast<aura::compiler::Primitives*>(prims)->string_heap();
    // Fast path: cached JIT idx still points at same content.
    if (auto cit = g_jit_idx_to_eval_idx.find(idx); cit != g_jit_idx_to_eval_idx.end()) {
        auto eidx = cit->second;
        if (eidx < eval_heap.size() && eval_heap[eidx] == content)
            return aura::compiler::types::make_string_raw_v2(static_cast<std::uint64_t>(eidx));
    }
    // Content intern: reuse an existing eval-heap slot with the same text.
    // Issue #5146: lookup, not a walk of the high-water mark. npos is
    // size_t(-1); this module does not name LockedStringHeap.
    if (const auto hit = eval_heap.find_content(content); hit != static_cast<std::size_t>(-1)) {
        g_jit_idx_to_eval_idx[idx] = hit;
        return aura::compiler::types::make_string_raw_v2(static_cast<std::uint64_t>(hit));
    }
    auto new_idx = eval_heap.mint(content);
    g_jit_idx_to_eval_idx[idx] = new_idx;
    return aura::compiler::types::make_string_raw_v2(static_cast<std::uint64_t>(new_idx));
}

// Evaluator-heap string → JIT g_string_pool (PrimCall results).
// Issue #2577: aura_alloc_string content-interns, so identical results share a slot.
static std::int64_t convert_str_for_jit(std::int64_t val, const aura::compiler::Primitives* prims) {
    if (!is_str_val(val) || !prims)
        return val;
    std::uint64_t idx = aura::compiler::types::string_idx_raw_v2(val);
    auto& eval_heap = const_cast<aura::compiler::Primitives*>(prims)->string_heap();
    if (idx >= eval_heap.size())
        return aura_alloc_string("");
    return aura_alloc_string(eval_heap[static_cast<std::size_t>(idx)].c_str());
}

// Key equality callback for JIT hash scan loop (Phase 4b)
// Compares two string-encoded values, handling both JIT and evaluator string encoding.
extern "C" std::int64_t aura_hash_callback_key_eq(std::int64_t stored_key,
                                                  std::int64_t search_key) {
    using aura::compiler::types::string_idx_raw_v2;
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    // Fast path: raw value equality
    if (stored_key == search_key)
        return 1;
    // String comparison (Issue #181 Cycle 2: v2 encoding)
    if (is_str_val(stored_key) && is_str_val(search_key) && prims) {
        auto& sh = const_cast<aura::compiler::Primitives*>(prims)->string_heap();
        std::uint64_t eval_idx = string_idx_raw_v2(stored_key);
        if (static_cast<std::size_t>(eval_idx) >= sh.size())
            return 0;
        std::uint64_t s_idx = string_idx_raw_v2(search_key);
        if (static_cast<std::size_t>(s_idx) < sh.size()) {
            const char* jit_s = aura_jit_pool_string(static_cast<std::size_t>(s_idx));
            if (jit_s)
                return (sh[static_cast<std::size_t>(eval_idx)] == jit_s) ? 1 : 0;
            return (sh[static_cast<std::size_t>(eval_idx)] == sh[static_cast<std::size_t>(s_idx)])
                       ? 1
                       : 0;
        }
    }
    return 0;
}

// Issue #3593: internal dispatch body — anonymous namespace gives internal
// linkage, so the compile-time call binding below cannot be interposed by
// the weak aura_jit_prim_dispatch stub in aura_jit_light_test_objects
// (.so link-order shadow intercepts even intra-TU extern "C" calls).
namespace aura::compiler {
namespace {

    std::int64_t jit_prim_dispatch_impl(std::int64_t prim_id, std::int64_t* args,
                                        std::int32_t argc) {
        auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
        if (!prims)
            return 0;

        // Look up primitive by PrimId → kPrimNameTable → evaluator name
        std::string_view pname;
        if (prim_id >= 0 && static_cast<std::size_t>(prim_id) < std::size(kPrimNameTable))
            pname = kPrimNameTable[static_cast<std::size_t>(prim_id)];
        if (pname.empty())
            return 0;

        auto pfn = prims->lookup(std::string(pname));
        if (!pfn)
            return 0;

        // Issue #2575: convert string args JIT→eval; re-intern results eval→JIT.
        std::vector<aura::compiler::types::EvalValue> eval_args;
        eval_args.reserve(static_cast<std::size_t>(argc));
        for (std::int32_t i = 0; i < argc; ++i) {
            std::int64_t a = args[i];
            if (is_str_val(a))
                a = convert_str_for_eval(a, prims);
            eval_args.emplace_back(a);
        }

        // Issue #3593: route through the same Evaluator choke point the
        // interpreter + IR use (#2152/#2583 dispatch require_effect, #3235
        // heap-mutate Guard, #2490/#3526 isolation auto-gate). The JIT C ABI
        // previously called (*pfn) bare — a PrimMeta / name-inferred effectful
        // prim could exec with zero cap + isolation check. Fail-closed when no
        // owner is wired (stub link / unwired JIT): no silent exec.
        auto* owner = owner_evaluator(prims);
        if (!owner)
            return 0;
        auto result =
            owner->invoke_prim_with_telemetry(pname, eval_args, [&] { return (*pfn)(eval_args); });

        if (is_str_val(result.val))
            return convert_str_for_jit(result.val, prims);
        return result.val;
    }


} // namespace
} // namespace aura::compiler

extern "C" std::int64_t aura_jit_prim_dispatch(std::int64_t prim_id, std::int64_t* args,
                                               std::int32_t argc) {
    // Issue #3593: thin C ABI entry — the body lives in
    // jit_prim_dispatch_impl (internal linkage, interposition-proof).
    return aura::compiler::jit_prim_dispatch_impl(prim_id, args, argc);
}

// Issue #5175: OpHashRef's inline scan only accepts the JIT 0x80 marker.
// An evaluator `(hash ...)` table uses fingerprint metas, so the scan
// misses and used to raise "key not found" for a key that is present.
// On that miss the JIT calls this, which is the same primitive the IR
// executor uses. The inline splitmix scan stays for JIT-layout tables.
// Unwired owner keeps the #4855 missing-key error. Weak stub returns
// the void sentinel for light-link binaries that do not compile this TU.
extern "C" std::int64_t aura_hash_ref_miss_error(std::int64_t key_val);
extern "C" std::int64_t aura_jit_eval_hash_ref(std::int64_t hash_val, std::int64_t key_val) {
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    auto* owner = owner_evaluator(prims);
    if (!prims || !owner)
        return aura_hash_ref_miss_error(key_val);
    auto pfn = prims->lookup("hash-ref");
    if (!pfn)
        return aura_hash_ref_miss_error(key_val);
    if (is_str_val(key_val))
        key_val = convert_str_for_eval(key_val, prims);
    auto result = owner->invoke_prim_with_telemetry("hash-ref", [&] {
        return (*pfn)({aura::compiler::types::EvalValue{hash_val},
                       aura::compiler::types::EvalValue{key_val}});
    });
    if (is_str_val(result.val))
        return convert_str_for_jit(result.val, prims);
    return result.val;
}

// Issue #3720 / #4018: JIT hash-set! / hash-remove! / cell-set! C ABI
// (aura_hash_set / aura_hash_remove / aura_cell_set) must share
// require_effect with interpreter + IR telemetry. Same owner as
// aura_jit_prim_dispatch. Unwired owner → 0 (no silent write). Soft/Off:
// require_effect is a no-op when owner is wired. Weak stub in
// aura_jit_prim_dispatch_stub.cpp.
extern "C" int aura_jit_owner_require_effect(std::uint16_t bits, const char* op) noexcept {
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    auto* owner = owner_evaluator(prims);
    if (!owner)
        return 0;
    return owner->require_effect(bits, op ? std::string_view(op) : "hash-set!") ? 1 : 0;
}

// Issue #4036: owner-Evaluator principal for closure slot tenant stamps —
// the same capability_tenant_id_ require_effect records. Same owner wiring
// as aura_jit_owner_require_effect; unwired owner → 0 (unstamped). Weak
// stub in aura_jit_prim_dispatch_stub.cpp.
extern "C" std::uint64_t aura_jit_owner_capability_tenant(void) noexcept {
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    auto* owner = owner_evaluator(prims);
    if (!owner)
        return 0;
    return owner->capability_tenant_id();
}

// Issue #4093: owner sandbox mode for the JIT cell/hash/pair tenant gate —
// the same owner wiring as aura_jit_owner_require_effect. Unwired owner → 0
// (production face off; Soft/Off zero-cost contract keeps the tenant arrays
// unread). Weak stub in aura_jit_prim_dispatch_stub.cpp.
extern "C" int aura_jit_owner_sandbox_mode(void) noexcept {
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    auto* owner = owner_evaluator(prims);
    if (!owner)
        return 0;
    return owner->effect_sandbox_mode();
}

// Issue #4093: emit the IsolationDeny through the owner's
// check_workspace_isolation (fiber id + Mutation epoch, #2388 single
// record_audit path — no second audit writer). Returns 1 on allow,
// 0 on deny (the JIT access is skipped by the caller).
// Issue #4134: fail-closed — an unwired owner must NOT false-allow. The
// #4093/#4094 JIT gates decide the access skip locally on stamp mismatch
// and never consult this return, but any caller that honors it now gets
// deny (same shape as the fail-closed siblings aura_jit_owner_require_effect
// / aura_jit_owner_sandbox_mode). The IsolationDeny SE needs the owner's
// record_audit path, so emission is best-effort: unwired, the existing
// tenant-isolation counter tenant_boundary_violation_prevented_total is the
// observable (no new mid-metrics key).
extern "C" int aura_jit_owner_check_isolation(std::uint64_t target_tenant, std::uint64_t ref_tenant,
                                              std::uint16_t bits, const char* op) noexcept {
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    auto* owner = owner_evaluator(prims);
    if (!owner) {
        // Issue #4134: fail-closed — unwired owner denies (see above).
        using ::aura::core::workspace_isolation::g_tenant_isolation_metrics;
        g_tenant_isolation_metrics().tenant_boundary_violation_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
        return 0;
    }
    return owner->check_workspace_isolation(target_tenant, ref_tenant, bits,
                                            op ? std::string_view(op) : "jit-tenant")
               ? 1
               : 0;
}

// Issue #3593: test hook — drive the JIT C ABI dispatch entry directly.
// Light-test links can bind a bare aura_jit_prim_dispatch reference to the
// weak stub in aura_jit_light_test_objects (.so), silently bypassing the
// strong body; this hook and the C ABI entry both delegate to the same
// internal impl, so tests always exercise the real routing (telemetry
// choke point).
extern "C" std::int64_t aura_test_jit_prim_dispatch(std::int64_t prim_id, std::int64_t* args,
                                                    std::int32_t argc) {
    return aura::compiler::jit_prim_dispatch_impl(prim_id, args, argc);
}

// ── Hash operation dispatch ──────────────────────────────
// Bridges OpHashRef/OpHashSet/OpHashRemove from JIT code to evaluator's hash primitives.
// These are separate from kPrimNameTable (which covers PrimCall-level primitives)
// because hash ops have dedicated IROpcodes for inline dispatch.

namespace aura::compiler {


// Convert FlatParseResult to Diagnostic with structured location.
// Falls back to a generic parse error if no structured errors exist.
inline aura::diag::Diagnostic parse_error_diag(const aura::parser::FlatParseResult& pr) {
    if (!pr.errors.empty()) {
        return {aura::diag::ErrorKind::ParseError, pr.errors[0].message, pr.errors[0].location};
    }
    return {aura::diag::ErrorKind::ParseError, pr.error.empty() ? "parse error" : pr.error};
}

// Issue #3795: SoA dirty entry declared before Wrap so run_on_dirty_blocks_only
// can delegate to the columnar helper (defined below).
export inline std::size_t run_dirty_escape_on_soa(IRModuleV2& soa_mod,
                                                  std::vector<std::vector<std::uint8_t>>& maps);

// ── EscapeAnalysisWrap — IR pass that computes per-function escape info ───
// Runs after lowering, before JIT codegen. Stores escape maps per function.
// JIT and IR interpreter read the results to select arena vs heap allocation.
//
// Issue #143: this was already in the tree but had no test coverage and
// was not part of pass_manager. This PR keeps the existing implementation
// (which uses aura::jit::run_escape_analysis on FlatInstruction) and
// adds tests/test_issue_143.cpp to verify it end-to-end.
export struct EscapeAnalysisWrap {
    // Per-function escape maps (indexed by IR function id).
    // Each entry: 0=NON_ESCAPING, 1=ESCAPED.
    std::vector<std::vector<std::uint8_t>> maps;
    BlockDirtyPred block_dirty_pred_;

    void set_block_dirty_pred(BlockDirtyPred pred) noexcept { block_dirty_pred_ = pred; }
    // Issue #3042: test-only function-pointer setter (non-capturing).
    void set_block_dirty_fn(bool (*fn)(std::uint32_t)) noexcept {
        block_dirty_pred_ = BlockDirtyPred{.fn = fn};
    }

    // Issue #684: DirtyAwarePass hook for incremental escape analysis.
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        return block_dirty_pred_(block_id);
    }

    // Issue #1574: IncrementalPass entry points for dirty pipeline.
    void run(aura::ir::IRFunction& func) {
        if (!g_use_arena)
            return;
        if (func.id >= maps.size())
            maps.resize(func.id + 1);
        std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs(func.blocks.size());
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            if (block_dirty_pred_ && !is_block_dirty(static_cast<std::uint32_t>(bi)))
                continue;
            for (auto& instr : func.blocks[bi].instructions) {
                if (instr.linear_ownership_state != 0 && instr.narrow_evidence != 0)
                    linear_occurrence_mutate::record_escape_violation_prevented();
                flat_instrs[bi].push_back(
                    {static_cast<std::uint32_t>(instr.opcode),
                     {instr.operands[0], instr.operands[1], instr.operands[2], instr.operands[3]},
                     0,
                     instr.narrow_evidence,
                     instr.type_id,
                     instr.linear_ownership_state,
                     0,
                     // Issue #1610 / #1616: IR→JIT marker + provenance
                     instr.source_marker,
                     instr.provenance});
            }
        }
        aura::jit::run_escape_analysis(flat_instrs, func.local_count, maps[func.id]);
    }
    void run(aura::ir::BasicBlock& /*block*/) {
        // Escape analysis is whole-function; per-block is a no-op.
    }

    void run(aura::ir::IRModule& module) {
        if (!g_use_arena) {
            maps.clear();
            return;
        }
        maps.resize(module.functions.size());
        for (auto& func : module.functions)
            run(func);
    }

    // Issue #3795: ProductionPureWrapPass SoA dirty peel (columnar).
    // Soft/unit keep AoS run(IRFunction&) / DirtySoAEntryPass grandfather.
    void run_on_dirty_blocks_only(IRModuleV2& mod) {
        aura::compiler::ir_soa_migration::record_consumer_pass();
        (void)run_dirty_escape_on_soa(mod, maps);
    }

    bool has_error() const { return false; }
    std::string_view name() const { return "escape-analysis"; }
    // Issue #2258: HotPassDodCompliant for run_incremental_dirty_pipeline.
    [[nodiscard]] constexpr bool uses_soa_view() const noexcept { return true; }
    static constexpr bool kPureWrap = true;

    // Get escape map for a function. Returns nullptr if unavailable.
    const std::uint8_t* get_map(std::uint32_t func_id) const {
        if (func_id < maps.size() && !maps[func_id].empty())
            return maps[func_id].data();
        return nullptr;
    }

    std::size_t get_map_size(std::uint32_t func_id) const {
        if (func_id < maps.size())
            return maps[func_id].size();
        return 0;
    }

    // Get the full storage (for passing to JIT/IRInterpreter)
    const std::vector<std::vector<std::uint8_t>>& all_maps() const { return maps; }
    std::vector<std::vector<std::uint8_t>> take_maps() { return std::move(maps); }
};

static_assert(DirtyAwarePass<EscapeAnalysisWrap>,
              "EscapeAnalysisWrap exposes is_block_dirty for IRSoA wiring");
static_assert(IncrementalPass<EscapeAnalysisWrap>,
              "EscapeAnalysisWrap is IncrementalPass for dirty pipeline (#1574)");
static_assert(HotPassDodCompliant<EscapeAnalysisWrap>,
              "EscapeAnalysisWrap HotPassDodCompliant (#2258)");
static_assert(PureWrapPass<EscapeAnalysisWrap>, "EscapeAnalysisWrap PureWrapPass (#2258)");
static_assert(DirtySoAEntryPass<EscapeAnalysisWrap>,
              "Issue #3454/#3795: Soft/unit AoS DirtySoAEntryPass grandfather retained");
static_assert(ProductionPureWrapPass<EscapeAnalysisWrap>,
              "Issue #3795: EscapeAnalysisWrap SoA dirty entry satisfies ProductionPureWrapPass");
// Issue #3701 / #3795: Production + soa_mod skips EscapeAnalysisWrap AoS run in
// the incremental suite; columnar dirty-block escape via run_on_dirty_blocks_only /
// run_dirty_escape_on_soa. Soft keeps the AoS grandfather.
// Issue #4262: run_dirty_escape_on_soa peels SoA columns in place — no
// FlatInstruction AoS vector materialize on the production dirty path.

// Issue #4262: columnar escape on dirty SoA blocks — zero AoS instr
// materialize. Soft EscapeAnalysisWrap::run(IRFunction&) grandfather
// still uses aura::jit::run_escape_analysis on FlatInstruction vectors.
export inline constexpr int kEscapeSoaNoFlatMaterializeIssue = 4262;

namespace detail_escape_soa {

    inline void mark_escape(std::vector<std::uint8_t>& escape_map, std::uint32_t slot,
                            std::uint32_t local_count) noexcept {
        if (slot < local_count)
            escape_map[slot] = 1;
    }

    // First pass + backward prop over dirty-block SoA column spans (no AoS copy).
    inline void run_escape_on_soa_fn(IRFunctionSoA& fn, std::vector<std::uint8_t>& escape_map) {
        using aura::ir::IROpcode;
        const std::uint32_t local_count = fn.local_count;
        escape_map.assign(local_count, 0);

        auto op0 = [&](std::uint32_t i) -> std::uint32_t {
            return i < fn.operand0_.size() ? fn.operand0_[i] : 0u;
        };
        auto op1 = [&](std::uint32_t i) -> std::uint32_t {
            return i < fn.operand1_.size() ? fn.operand1_[i] : 0u;
        };
        auto op2 = [&](std::uint32_t i) -> std::uint32_t {
            return i < fn.operand2_.size() ? fn.operand2_[i] : 0u;
        };
        auto lin_at = [&](std::uint32_t i) -> std::uint8_t {
            return i < fn.linear_ownership_states_.size() ? fn.linear_ownership_states_[i]
                                                          : std::uint8_t{0};
        };
        auto ne_at = [&](std::uint32_t i) -> std::uint32_t {
            return i < fn.narrow_evidence_.size() ? fn.narrow_evidence_[i] : 0u;
        };

        // Pass 1: mark escape points on dirty blocks only.
        (void)fn.for_each_block(
            [&](std::uint32_t /*bid*/, BasicBlockSoA& block) {
                for (std::uint32_t i = block.start_idx; i < block.end_idx && i < fn.opcodes_.size();
                     ++i) {
                    const auto opc = fn.opcodes_[i];
                    const std::uint8_t lin = lin_at(i);
                    const std::uint32_t ne = ne_at(i);
                    if (lin != 0 && ne != 0)
                        linear_occurrence_mutate::record_escape_violation_prevented();
                    switch (opc) {
                        case IROpcode::Return:
                            mark_escape(escape_map, op0(i), local_count);
                            break;
                        case IROpcode::Call: {
                            mark_escape(escape_map, op0(i), local_count);
                            const std::uint32_t arg_base = op1(i);
                            const std::uint32_t arg_count = op2(i);
                            for (std::uint32_t a = 0; a < arg_count && (arg_base + a) < local_count;
                                 ++a)
                                escape_map[arg_base + a] = 1;
                            break;
                        }
                        case IROpcode::Apply: {
                            const std::uint32_t closure_slot = op0(i);
                            const std::uint32_t arg_count = op1(i);
                            mark_escape(escape_map, closure_slot, local_count);
                            for (std::uint32_t a = 0;
                                 a < arg_count && (closure_slot + 1 + a) < local_count; ++a)
                                escape_map[closure_slot + 1 + a] = 1;
                            break;
                        }
                        case IROpcode::Capture:
                        case IROpcode::CaptureRef:
                            mark_escape(escape_map, op2(i), local_count);
                            if (lin != 0) {
                                mark_escape(escape_map, op0(i), local_count);
                                mark_escape(escape_map, op2(i), local_count);
                            }
                            break;
                        case IROpcode::CellSet:
                            mark_escape(escape_map, op1(i), local_count);
                            break;
                        case IROpcode::HashSet:
                            mark_escape(escape_map, op2(i), local_count);
                            break;
                        case IROpcode::PrimCall: {
                            const std::uint32_t arg_base = op1(i);
                            const std::uint32_t arg_count = op2(i);
                            for (std::uint32_t a = 0; a < arg_count && (arg_base + a) < local_count;
                                 ++a)
                                escape_map[arg_base + a] = 1;
                            break;
                        }
                        default:
                            break;
                    }
                }
            },
            /*dirty_only=*/true);

        // Pass 2: backward propagation (dirty blocks; same rules as jit path).
        bool changed = true;
        while (changed) {
            changed = false;
            (void)fn.for_each_block(
                [&](std::uint32_t /*bid*/, BasicBlockSoA& block) {
                    for (std::uint32_t i = block.start_idx;
                         i < block.end_idx && i < fn.opcodes_.size(); ++i) {
                        const std::uint32_t result = op0(i);
                        if (result >= local_count || !escape_map[result])
                            continue;
                        switch (fn.opcodes_[i]) {
                            case IROpcode::Local:
                                if (op1(i) < local_count && !escape_map[op1(i)]) {
                                    escape_map[op1(i)] = 1;
                                    changed = true;
                                }
                                break;
                            case IROpcode::MakePair:
                                if (op1(i) < local_count && !escape_map[op1(i)]) {
                                    escape_map[op1(i)] = 1;
                                    changed = true;
                                }
                                if (op2(i) < local_count && !escape_map[op2(i)]) {
                                    escape_map[op2(i)] = 1;
                                    changed = true;
                                }
                                break;
                            default:
                                break;
                        }
                    }
                },
                /*dirty_only=*/true);
        }
    }

} // namespace detail_escape_soa

export inline std::size_t run_dirty_escape_on_soa(IRModuleV2& soa_mod,
                                                  std::vector<std::vector<std::uint8_t>>& maps) {
    if (!g_use_arena) {
        maps.clear();
        return 0;
    }
    if (maps.size() < soa_mod.functions.size())
        maps.resize(soa_mod.functions.size());
    std::size_t updated = 0;
    for (std::size_t fi = 0; fi < soa_mod.functions.size(); ++fi) {
        auto& fn = soa_mod.functions[fi];
        if (fn.dirty_block_count() == 0) {
            aura::compiler::ir_soa_migration::record_dirty_block_skip(fn.blocks_.size());
            continue;
        }
        if (g_fn_shape_stable_probe && g_fn_shape_stable_probe(fn.name)) {
            aura::compiler::ir_soa_migration::record_dirty_block_skip(fn.blocks_.size());
            continue;
        }
        // Issue #4262: peel dirty blocks in place — no AoS instr materialize.
        auto [runs, skips] = fn.for_each_block(
            [&](std::uint32_t /*bid*/, BasicBlockSoA& /*block*/) {
                // Counting-only walk; analysis is in run_escape_on_soa_fn.
            },
            /*dirty_only=*/true);
        if (skips)
            aura::compiler::ir_soa_migration::record_dirty_block_skip(skips);
        if (runs)
            aura::compiler::ir_soa_migration::record_dirty_block_run(runs);
        detail_escape_soa::run_escape_on_soa_fn(fn, maps[fi]);
        ++updated;
    }
    return updated;
}

// CompilerService — owns a full compilation session's lifecycle.
//
// Each request creates a fresh AST in the arena; after eval, arena
// is reset for the next request. Evaluator state (closures, defines)
// persists across resets.
//
// For multi-module scenarios, use module_arena() to get an isolated
// arena that can be independently reset.
//

// Issue #147: post-mutation invariant check mode. Controls how
// `typed_mutate` reacts to OwnershipNotes emitted by
// `post_mutation_invariant_check`:
//   - Disabled      — check is not run (status stays NotChecked).
//   - WarningsOnly  — check runs, notes are surfaced via
//                     MutationResult::invariant_diagnostics, but
//                     execution is NOT blocked. Default.
//   - Strict        — check runs, any note (use-after-move,
//                     double-borrow, leaked-linear, invalidated
//                     occurrence narrowing) causes typed_mutate
//                     to return success=false with the diagnostic.
export enum class InvariantCheckMode : std::uint8_t {
    Disabled = 0,
    WarningsOnly = 1,
    Strict = 2,
};

// Issue #169 Phase 1: incremental-strictness config flag.
// Three modes:
//   - Conservative: invalidate MORE than strictly necessary
//     (the safest, slowest path; minimal precision needed)
//   - Balanced:     default; use the existing forward BFS on
//                   dep_graph_ (no behavior change vs pre-#169)
//   - Aggressive:   invalidate LESS; trust the new precise
//                   impact analysis (Goals 1-2 of #169). The
//                   Aggressive path will only become available
//                   once those goals land.
//
// This flag is currently read by the future Goals 1-4
// implementations. In Balanced mode (the default), behavior
// is identical to pre-#169. Conservative mode invalidates MORE
// (a strictness bump for users who want maximum safety).
// Aggressive mode currently behaves the same as Balanced
// until Goals 1-2 land; the enum value is reserved.
export enum class IncrementalStrictness : std::uint8_t {
    Conservative = 0,
    Balanced = 1,
    Aggressive = 2,
};

// Issue #411: post-mutation auto-incremental typecheck
// mode. Three modes control whether typed_mutate auto-
// invokes TypeChecker::infer_flat_partial on the most
// recent MutationRecord after a successful mutation:
//
//   - Eager     (default) — auto-invoke on every successful
//                            typed_mutate. (query:type <name>)
//                            and (get-inferred-type <node-id>)
//                            return up-to-date types
//                            immediately, with no manual
//                            (typecheck-incremental) call.
//                            Adds ~O(dirty-subtree) work to
//                            every mutation.
//   - Lazy                 — do not auto-invoke. Callers
//                            must explicitly call
//                            (typecheck-incremental) after
//                            a batch of mutations. Useful
//                            for batch-mutation pipelines
//                            where many mutations happen
//                            before a type query.
//   - Disabled             — never invoke infer_flat_partial
//                            from the typed_mutate path.
//                            Equivalent to the pre-#411
//                            behavior. The (typecheck-current)
//                            full pass still works.
//
// All three modes preserve the existing (typecheck-current)
// and (typecheck-incremental) primitive paths; the mode
// only affects the auto-invocation that happens at the
// end of a successful typed_mutate.
export enum class IncrementalTypecheckMode : std::uint8_t {
    Eager = 0,
    Lazy = 1,
    Disabled = 2,
};

// Issue #4930: define-root consult observability. The parent-link fast path
// answers the common "cur is a direct child of par" case in O(1); the
// child-list scan stays as a torn-state fallback. The counters let the
// regression test prove the warm hit path never re-scans root's children.
export inline std::atomic<std::uint64_t> g_define_root_parent_link_hits{0};
export inline std::atomic<std::uint64_t> g_define_root_child_list_scans{0};

export class CompilerService {
public:
    // Issue #531: closure / EnvFrame / bridge_epoch /
    // linear_ownership_state observability accessors.
    // Read directly from the shared CompilerMetrics struct
    // (also bumped by invalidate_function for stale_refresh,
    // and by the follow-up apply_closure for bridge_epoch
    // hit + linear_check_pass). Exposed via
    // (query:closure-env-safety-stats) primitive.
    [[nodiscard]] std::uint64_t get_closure_stale_refresh_count() const noexcept {
        return metrics_.closure_stale_refresh_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_bridge_epoch_hit_count() const noexcept {
        return metrics_.bridge_epoch_hit_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_linear_check_pass_count() const noexcept {
        return metrics_.linear_check_pass_count_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_gc_envframe_stale_skipped() const noexcept {
        return metrics_.gc_envframe_stale_skipped_.load(std::memory_order_relaxed);
    }
    // Issue #305: TypeId/TypeScheme propagation observability
    // accessors (EDA hardware optimization / synthesis track).
    // Read directly from the shared CompilerMetrics struct
    // (also bumped by the follow-up TypePropagationPass wire-up
    // and the bit-width-inference pass in the EDA backend).
    // Exposed via (compile:type-propagation-stats) primitive.
    [[nodiscard]] std::uint64_t get_type_propagation_runs() const noexcept {
        return metrics_.type_propagation_runs_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_type_propagation_total() const noexcept {
        return metrics_.type_propagation_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_type_propagation_unknown() const noexcept {
        return metrics_.type_propagation_unknown_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_type_propagation_int_width() const noexcept {
        return metrics_.type_propagation_int_width_.load(std::memory_order_relaxed);
    }
    // Bump helpers (for the follow-up TypePropagationPass +
    // bit-width-inference wire-up in the EDA backend).
    // Public so the test file can verify the counter wiring
    // is reachable from C++.
    void bump_type_propagation_runs() noexcept {
        metrics_.type_propagation_runs_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_type_propagation_total(std::uint64_t delta) noexcept {
        metrics_.type_propagation_total_.fetch_add(delta, std::memory_order_relaxed);
    }
    void bump_type_propagation_unknown() noexcept {
        metrics_.type_propagation_unknown_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_type_propagation_int_width() noexcept {
        metrics_.type_propagation_int_width_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #612: re-sync TypeRegistry ADT constructors from the
    // active workspace FlatAST (post-mutate validation hook).
    void refresh_adt_constructors_from_workspace() {
        auto* flat = evaluator_.workspace_flat();
        if (!flat || !current_pool_ || flat->root == aura::ast::NULL_NODE)
            return;
        register_adt_from_define_types(*flat, *current_pool_, flat->root);
    }
    // Issue #306: hardware resource linear-ownership
    // accessors (EDA track — wire/reg/mem/port borrow +
    // double-drive detection). Read directly from the shared
    // CompilerMetrics struct (also bumped by the follow-up
    // OwnershipEnv wire-up in lowering_linear_types). Exposed
    // via (query:linear-ownership-stats) primitive.
    [[nodiscard]] std::uint64_t get_hw_resource_wire_borrows() const noexcept {
        return metrics_.hw_resource_wire_borrows_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_hw_resource_reg_writes() const noexcept {
        return metrics_.hw_resource_reg_writes_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_hw_resource_mem_access() const noexcept {
        return metrics_.hw_resource_mem_access_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_hw_resource_double_drive() const noexcept {
        return metrics_.hw_resource_double_drive_.load(std::memory_order_relaxed);
    }
    // Bump helpers (for the follow-up OwnershipEnv + linear
    // lowering wire-up in the EDA backend). Public so the
    // test file can verify the counter wiring is reachable
    // from C++.
    void bump_hw_resource_wire_borrows() noexcept {
        metrics_.hw_resource_wire_borrows_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_hw_resource_reg_writes() noexcept {
        metrics_.hw_resource_reg_writes_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_hw_resource_mem_access() noexcept {
        metrics_.hw_resource_mem_access_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_hw_resource_double_drive() noexcept {
        metrics_.hw_resource_double_drive_.fetch_add(1, std::memory_order_relaxed);
    }
    // Bump helpers (for follow-up IRClosure::invalidate_if_stale +
    // GCEnvWalkFn integration). Public so the test file can
    // verify the counter wiring is reachable from C++.
    void bump_bridge_epoch_hit_count() noexcept {
        metrics_.bridge_epoch_hit_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_linear_check_pass_count() noexcept {
        metrics_.linear_check_pass_count_.fetch_add(1, std::memory_order_relaxed);
    }
    void bump_gc_envframe_stale_skipped() noexcept {
        metrics_.gc_envframe_stale_skipped_.fetch_add(1, std::memory_order_relaxed);
    }
    // Issue #681: compiler closure invalidation observability.
    [[nodiscard]] std::uint64_t get_compiler_inval_bridge_epoch_total() const noexcept {
        return metrics_.compiler_inval_bridge_epoch_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_compiler_closure_epoch_mismatch_hits() const noexcept {
        return metrics_.compiler_closure_epoch_mismatch_hits.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_compiler_closure_safe_fallbacks() const noexcept {
        return metrics_.compiler_closure_safe_fallbacks.load(std::memory_order_relaxed);
    }
    // Issue #682: compiler GC root coordination observability.
    [[nodiscard]] std::uint64_t get_ir_closure_roots_registered() const noexcept {
        return metrics_.ir_closure_roots_registered.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_hotswap_root_miss() const noexcept {
        return metrics_.hotswap_root_miss.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_compiler_gc_safepoint_defer_count() const noexcept {
        return metrics_.compiler_gc_safepoint_defer_count.load(std::memory_order_relaxed);
    }

    CompilerService();

    // ── Hot-swap implementation (Issue #97 Action 1) ───────────
    // Parse the new source as a single function definition, lower to IR,
    // and replace the existing function (same name) in the current module
    // while preserving its id. Returns true on success.
    bool hot_swap_function_impl(const std::string& name, const std::string& new_source) {
        // Currently only supports the IR-execution path (JIT/AOT).
        // For the tree-walker path, function defs live in the workspace's
        // FlatAST, which requires a different hot-swap path (TODO).
        if (!last_ir_mod_ || last_ir_mod_->functions.empty())
            return false;
        // Issue #4936: one checked deref after the guard above.
        auto& live_ir_mod = *last_ir_mod_;

        // Issue #1262/#1264: acquire mutation epoch with fence before
        // hot-swap so concurrent invalidate_function / mutate cannot
        // tear bridge_epoch vs mutation_epoch observations.
        const std::uint64_t epoch_at_entry = aura::core::current_mutation_epoch();
        aura::util::thread_fence(std::memory_order_acq_rel);
        // Issue #1262: stamp AOT emit defuse_version for versioned mangle
        // so subsequent AOT reloads enforce current epoch in symbol names.
        aura_set_aot_defuse_version(evaluator_.defuse_version());

        // Find existing function by name
        std::uint32_t existing_id = 0xFFFFFFFF;
        for (std::uint32_t i = 0; i < live_ir_mod.functions.size(); ++i) {
            if (live_ir_mod.functions[i].name == name) {
                existing_id = i;
                break;
            }
        }
        if (existing_id == 0xFFFFFFFF)
            return false;

        // Parse the new source in the temp arena (avoids clobbering workspace)
        auto alloc = temp_arena_.allocator();
        aura::ast::StringPool* new_pool = nullptr;
        aura::ast::FlatAST* new_flat = nullptr;
        // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
        new_pool = temp_arena_.create_with_cover<aura::ast::StringPool>(
            nullptr, "stack-cover-dies-at-return", alloc);
        new_flat = temp_arena_.create_with_cover<aura::ast::FlatAST>(
            nullptr, "stack-cover-dies-at-return", alloc);
        if (!new_pool || !new_flat)
            return false;
        auto pr = aura::parser::parse_to_flat(new_source, *new_flat, *new_pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return false;

        // Lower to IR using temp_arena_ — the resulting IRFunction shares
        // no memory with the workspace pool, so it can be moved into the
        // module's existing functions[] without aliasing issues.
        aura::ir::IRModule new_mod = aura::compiler::lower_to_ir(
            *new_flat, *new_pool, temp_arena_, &evaluator_.primitives(), &type_registry_);
        if (new_mod.functions.empty())
            return false;

        // The lowered module has at least one function. Use the first one
        // (or, if multiple, the one with matching name).
        std::uint32_t new_id = 0;
        for (std::uint32_t i = 0; i < new_mod.functions.size(); ++i) {
            if (new_mod.functions[i].name == name) {
                new_id = i;
                break;
            }
        }
        if (new_id >= new_mod.functions.size() || new_mod.functions[new_id].name != name)
            return false;

        // Issue #1264: re-check epoch after lower; concurrent mutation
        // advanced epoch → race detected, refuse hot-swap (caller retries).
        const std::uint64_t epoch_now = aura::core::current_mutation_epoch();
        if (epoch_now != epoch_at_entry) {
            metrics_.hot_update_race_detected.fetch_add(1, std::memory_order_relaxed);
            aura::util::thread_fence(std::memory_order_acq_rel);
            return false;
        }

        // Issue #225 cycle 3: invalidate the bridge data for
        // the hot-swapped function. Closures that captured
        // the old bindings will see the new epoch_ and
        // re-parse on next use.
        invalidate_bridge_for(name);
        on_compiler_invalidate_gc_coordination(name);

        // Bump epoch after successful invalidation so AOT mangle + JIT
        // caches observe the swap (#1262 versioned symbols).
        aura::core::bump_mutation_epoch();
        metrics_.hot_swap_versioned_mangle_enforced.fetch_add(1, std::memory_order_relaxed);

        // Hot-swap: replace in last_ir_mod_ at existing_id, preserving the id
        return live_ir_mod.hot_swap_function(existing_id, std::move(new_mod.functions[new_id]));
    }

    // ── Inter-agent messaging (P0) ──────────────────────────
    void set_session_id(const std::string& id) {
        session_id_ = id;
        evaluator_.set_session_id(id);
    }
    // Issue #4460: file-mode oneshot only. cs.eval stays an error value.
    void set_require_failure_is_diagnostic(bool on) noexcept {
        evaluator_.set_require_failure_is_diagnostic(on);
    }
    // Issue #5140: --serve-async-bench runs its script as the top-level
    // program on the bench fiber; that body arms file-mode diagnostics just
    // like --load, so an uncaught error keeps its location and snippet.
    void set_file_diag_in_fiber_body(bool on) noexcept {
        evaluator_.set_file_diag_in_fiber_body(on);
    }
    std::string session_id() const { return session_id_; }
    void set_wake_eventfd(int fd) { wake_eventfd_ = fd; }

    void push_message(const std::string& sender, const std::string& msg) {
        mailbox_.push_back({sender, msg});
        if (wake_eventfd_ >= 0) {
            uint64_t _val = 1;
            ::write(wake_eventfd_, &_val, sizeof(_val));
        }
    }

    std::optional<std::string> pop_message(int timeout_ms = -1) {
        if (!mailbox_.empty()) {
            last_sender_ = mailbox_.front().first;
            auto msg = std::move(mailbox_.front().second);
            mailbox_.erase(mailbox_.begin());
            return msg;
        }
        if (timeout_ms == 0)
            return std::nullopt;
        // Use poll() on wake_eventfd_ for timeout-capable wait
        if (wake_eventfd_ >= 0 && timeout_ms != 0) {
            struct pollfd pfd;
            pfd.fd = wake_eventfd_;
            pfd.events = POLLIN;
            int pret = ::poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : -1);
            if (pret > 0) {
                // Drain the eventfd
                uint64_t val = 0;
                ::read(wake_eventfd_, &val, sizeof(val));
            } else if (pret == 0) {
                return std::nullopt; // timeout
            }
            // (pret < 0) = error, fall through to yield
        }
        // Fallback: try fiber yield (scheduler-based wake)
        if (!mailbox_.empty()) {
            last_sender_ = mailbox_.front().first;
            auto msg = std::move(mailbox_.front().second);
            mailbox_.erase(mailbox_.begin());
            return msg;
        }
        if (aura::serve::g_current_fiber && timeout_ms != 0) {
            aura::serve::g_current_fiber->set_state(aura::serve::FiberState::Waiting);
            aura::serve::Fiber::yield();
            if (!mailbox_.empty()) {
                last_sender_ = mailbox_.front().first;
                auto msg = std::move(mailbox_.front().second);
                mailbox_.erase(mailbox_.begin());
                return msg;
            }
        }
        return std::nullopt;
    }

    std::string last_sender() const { return last_sender_; }
    std::size_t mailbox_size() const { return mailbox_.size(); }

    static void register_session(const std::string& id, CompilerService* svc) {
        std::scoped_lock lk(registry_mtx());
        registry()[id] = svc;
    }

    static void unregister_session(const std::string& id) {
        std::scoped_lock lk(registry_mtx());
        registry().erase(id);
    }

    static CompilerService* lookup(const std::string& id) {
        std::scoped_lock lk(registry_mtx());
        auto it = registry().find(id);
        return it != registry().end() ? it->second : nullptr;
    }

    void reset() {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        // Issue #984: clear thread_local lowering hooks on reset so a
        // subsequent session cannot see this service's trampolines.
        clear_lowering_compiler_core_hooks();
        arena_.reset();
        // IR cache references arena-allocated FlatAST data;
        // must be cleared after arena reset to avoid dangling pointers.
        ir_cache_.clear();
        ir_cache_strings_.clear();
        // Issue #1258: force full dirty on ir_cache_v2_ SoA entries
        // before clear so any concurrent reader sees dirty (not stale
        // clean) state across arena reset / hot-swap.
        for (auto& [_, entry] : ir_cache_v2_) {
            entry.dirty = true;
            entry.mark_all_blocks_dirty();
        }
        ir_cache_v2_.clear();
        // Issue #4600 §8: the lowering memo's IR carries no-op-deleter
        // FlatAST/StringPool views from this arena — drop it with the
        // arena reset (same rule as ir_cache_bridge_ below).
        jit_program_memo_.reset();
        // Issue #225 cycle 3: explicitly clear all bridge data.
        // The shared_ptr-based bridges (Issue #224) hold a
        // non-owning view of the FlatAST, but the FlatAST's
        // internal arrays (child_data_, etc.) live in the
        // arena and are invalidated by arena_.reset() above.
        // The bridges themselves must be cleared to avoid
        // referencing the now-freed arena memory.
        ir_cache_bridge_.clear();
        ir_define_env_bindings_.clear();
        ir_define_closure_owner_.clear();
        ir_value_cell_bindings_.clear();
        ir_call_slots_4681_.clear(); // Issue #4681
        ir_disk_snapshots_.clear();
        // Issue #223 + #1258 + #1263: bump mutation_epoch_ so any stale
        // ClosureBridgeData that captured the old epoch is detected
        // by the bridge callback / apply_closure. The bridge_epoch_
        // field on ClosureBridgeData captures this at construction
        // time; a mismatch indicates the bridge's flat*/pool* are
        // dangling (the arena was reset). The bridge falls back
        // to re-parse from body_source (or invalidates the closure).
        // Force-dirty was applied above before clear — no false-clean.
        aura::core::bump_mutation_epoch();
        metrics_.ir_soa_cache_reset_epoch_bumps.fetch_add(1, std::memory_order_relaxed);
        metrics_.arena_reset_dirty_forced.fetch_add(1, std::memory_order_relaxed);
        // Re-install lowering hooks after reset so the live service
        // continues to observe bridge epoch / linear metadata.
        install_lowering_compiler_core_hooks();
    }

    // ---- Strict mode (type errors → rejected) ------------------------
    void set_strict_mode(bool s) { strict_mode_ = s; }
    bool strict_mode() const { return strict_mode_; }

    // ---- Issue #283 follow-up #5: bidirectional_mode flag -----------
    // When enabled (default), check_flat's If branch applies
    // Occurrence Typing narrowing the same way synthesize_flat_if
    // does, and synthesize_flat_if's last_if_narrowing_ capture
    // handles both 'or' and 'and' predicates. When disabled,
    // check_flat falls back to the original uniform check
    // (no narrowing) and synthesize_flat_if keeps the single-
    // predicate capture path. Opt-out is useful for workspaces
    // with many nested ifs where the bidirectional path is
    // measurably slower than the legacy path.
    void set_bidirectional_mode(bool b) { bidirectional_mode_ = b; }
    // Issue #1420 AC3: accessor for (compile:bidirectional-stats)
    // :mode field. The persistent CompilerService flag is the
    // authoritative source — per-call TypeChecker instances are
    // short-lived and pick up the flag via
    // tc.set_bidirectional_mode(bidirectional_mode_) at
    // construction (see service.ixx:3291 for one example).
    bool bidirectional_mode() const { return bidirectional_mode_; }

    // Issue #2992: AURA_GRADUAL_PERMISSIVENESS=permissive|balanced|strict
    // (default balanced). set_strict_mode(true) still wins at the engine.
    void set_gradual_permissiveness(GradualPermissiveness p) {
        gradual_permissiveness_ = p;
        metrics_.gradual_permissiveness_mode.store(static_cast<std::uint64_t>(p),
                                                   std::memory_order_relaxed);
    }
    [[nodiscard]] GradualPermissiveness gradual_permissiveness() const noexcept {
        return gradual_permissiveness_;
    }

    // Issue #2993: AURA_TYPECHECK_METRICS=minimal|full (default minimal).
    void set_typecheck_metrics_tier(TypecheckMetricsTier t) {
        aura::compiler::set_typecheck_metrics_tier(t);
        metrics_.typecheck_metrics_tier.store(static_cast<std::uint64_t>(t),
                                              std::memory_order_relaxed);
    }
    [[nodiscard]] TypecheckMetricsTier typecheck_metrics_tier() const noexcept {
        return aura::compiler::typecheck_metrics_tier();
    }

    // ---- Unified evaluation (IR-first with fallback) -----------------

    // Issue #2213: production gate on silent tree-walker fallback.
    // When needs_tree_walker_fallback is true:
    //   Allow     → TakeWalker (legacy; unit default)
    //   Forbidden → HardError  (production preferred)
    //   ForceSoa  → ContinueIr + forced_soa metric
    // Happy path (needs=false) is a single bool check + no metric write.
    [[nodiscard]] TreeWalkerFallbackDisposition
    consult_tree_walker_fallback_gate_(const aura::ast::FlatAST& flat,
                                       const aura::ast::StringPool& pool, aura::ast::NodeId root) {
        const bool needs = needs_tree_walker_fallback(flat, pool, root);
        if (!needs)
            return TreeWalkerFallbackDisposition::ContinueIr;
        // #2213 (2026-10-06 face, restored): the ROOT form is a Call whose
        // head resolves to a REGISTERED primitive — eval_flat dispatches
        // that prim by contract, so this is not SoA abandonment. Take the
        // walker for registered-prim roots; every other needs=true source
        // (unknown names, query:/mutate: DSL, while/fiber calls) keeps the
        // policy dispositions. Fixing this at the needs/membership layer
        // instead routes these calls into the IR pipeline — the exact
        // mishandling the tree_walker_only set exists for (verified
        // 2026-10-06: exempting membership broke module_query ×3 and
        // occurrence_coercion ×2).
        // Forbidden is the fail-closed production face: it vetoes even the
        // #2213 registered-prim-root walker take — test_tree_walker_fallback_strict
        // AC1 pins Forbidden + set-code → HardError. The consult-layer restore
        // (9198f5070) ordered this branch before the policy switch, so under
        // Forbidden a registered-prim root silently walked. Check the face first.
        if (production_pipeline_strict()) {
            metrics_.tree_walker_fallback_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.tree_walker_fallback_forbidden_total.fetch_add(1, std::memory_order_relaxed);
            return TreeWalkerFallbackDisposition::HardError;
        }
        if (root != aura::ast::NULL_NODE && root < flat.size()) {
            const auto rv = flat.get(root);
            if (rv.tag == aura::ast::NodeTag::Call && !rv.children.empty()) {
                const auto cal = flat.get(rv.child(0));
                if (cal.tag == aura::ast::NodeTag::Variable) {
                    const auto head = pool.resolve(cal.sym_id);
                    // Issue #5334: for-each / map must be able to run a
                    // deep callback on the IR heap. A callback that hits
                    // the walker's 512-frame C-stack guard aborts after its
                    // side effects; the file face then re-runs the failing
                    // form (effects twice). Route these two prim roots to
                    // IR, where the heap frames carry the recursion.
                    // Issue #5334: only an INLINE lambda callback can be
                    // lowered to IR here. A named callback that set!s a
                    // top-level value is refused by the #4513/#4618 rule,
                    // and lowering the unknown name would ConstVoid the
                    // call and drop the walk. Keep that form on the walker.
                    bool hof_lambda_5334 = false;
                    if ((head == "for-each" || head == "map") && rv.children.size() >= 2) {
                        const auto arg0 = flat.get(rv.child(1));
                        hof_lambda_5334 = arg0.tag == aura::ast::NodeTag::Lambda;
                    }
                    if (!hof_lambda_5334 && !head.empty() &&
                        evaluator_.primitives().slot_for_name(std::string(head)) <
                            evaluator_.primitives().slot_count()) {
                        metrics_.tree_walker_fallback_total.fetch_add(1, std::memory_order_relaxed);
                        return TreeWalkerFallbackDisposition::TakeWalker;
                    }
                }
            }
        }
        metrics_.tree_walker_fallback_total.fetch_add(1, std::memory_order_relaxed);
        const auto disp = tree_walker_fallback_disposition(/*needs_fallback=*/true);
        if (disp == TreeWalkerFallbackDisposition::HardError)
            metrics_.tree_walker_fallback_forbidden_total.fetch_add(1, std::memory_order_relaxed);
        else if (disp == TreeWalkerFallbackDisposition::ContinueIr)
            metrics_.tree_walker_fallback_forced_soa_total.fetch_add(1, std::memory_order_relaxed);
        // TakeWalker: no extra counter (tree_walker_fallback_total is enough).
        return disp;
    }

    // Issue #4478: when the strict #2213 gate refuses the tree-walker
    // fallback for a Call root, the env may already prove the callee is not
    // a procedure. The tree walker would have raised "cannot call" at this
    // site — raise it here instead of an internal pipeline error. A callable
    // callee (closure / primitive / cell holding one) keeps the existing
    // refusal: the fallback has a different cause there.
    [[nodiscard]] std::optional<aura::diag::Diagnostic>
    noncallable_call_diag_4478(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                               aura::ast::NodeId root) const {
        if (root == aura::ast::NULL_NODE || root >= flat.size())
            return std::nullopt;
        const auto rv = flat.get(root);
        if (rv.tag != aura::ast::NodeTag::Call || rv.children.empty())
            return std::nullopt;
        const auto cal = flat.get(rv.child(0));
        if (cal.tag != aura::ast::NodeTag::Variable)
            return std::nullopt;
        auto name = std::string(pool.resolve(cal.sym_id));
        if (name.empty())
            return std::nullopt;
        auto bound = evaluator_.top_env().lookup(name);
        if (!bound)
            return std::nullopt;
        auto fn = *bound;
        if (types::is_cell(fn)) {
            auto ci = types::as_cell_id(fn);
            if (ci < evaluator_.cells().size())
                fn = evaluator_.cells()[ci];
        }
        if (types::is_closure(fn) || types::is_primitive(fn))
            return std::nullopt;
        const auto line = rv.line == 0 ? 1u : rv.line;
        const auto col = rv.col == 0 ? 1u : rv.col;
        return aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError, "cannot call: " + name,
                                      aura::diag::SourceLocation{line, col, 0}};
    }

    // Check if an expression needs the tree-walker evaluator.
    // IR pipeline cannot handle: EDSL primitives, quoted pairs, special forms,
    // macro definitions, error handling, or non-primitive variable references
    // (which may come from runtime imports).
    bool needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                    const aura::ast::StringPool& pool,
                                    aura::ast::NodeId root) const;

    // Issue #402: subtree-walked fallback check. Used by
    // the fast path in needs_tree_walker_fallback when
    // summary_flags_ == 0 (no MacroDef/DefineType/etc.
    // anywhere). Walks only the root subtree (typically
    // a handful of nodes for arithmetic / primitive-call
    // expressions) instead of the entire flat. Applies the
    // SAME per-node logic as the slow-path scan — Variable
    // keyword check, Call query:/mutate: prefix, primitive
    // slot lookup, etc. — but bounded by root size.
    bool subtree_needs_tree_walker_fallback(
        const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool, aura::ast::NodeId root,
        const std::unordered_set<std::string>* extra_known = nullptr) const;

    // Issue #402: public counter accessor for tests.
    [[nodiscard]] std::uint64_t get_needs_tree_walker_fallback_calls() const noexcept {
        return metrics_.needs_tree_walker_fallback_calls.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_needs_tree_walker_fast_path_hits() const noexcept {
        return metrics_.needs_tree_walker_fast_path_hits.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_needs_tree_walker_slow_path_hits() const noexcept {
        return metrics_.needs_tree_walker_slow_path_hits.load(std::memory_order_relaxed);
    }

    // Issue #1284: name is IR-resolvable if present in v1 cache, value-cell
    // bindings, OR ir_cache_v2_ (post-mutate define cache). Counting v2 hits
    // measures how often define-cache avoids tree-walker fallback.
    [[nodiscard]] bool name_in_ir_define_cache(const std::string& name) const {
        if (ir_cache_.count(name) || ir_value_cell_bindings_.count(name))
            return true;
        if (ir_cache_v2_.count(name)) {
            metrics_.tree_walker_define_cache_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Issue #5241: the one place a diagnostic kind becomes a core
    // AuraErrorKind. A structured switch on the enum — never the display
    // phrase, which is wording and may change.
    [[nodiscard]] static aura::core::AuraErrorKind core_kind_of(aura::diag::ErrorKind k) noexcept {
        using aura::diag::ErrorKind;
        switch (k) {
            case ErrorKind::ParseError:
                return aura::core::AuraErrorKind::ParseError;
            case ErrorKind::UnexpectedToken:
                return aura::core::AuraErrorKind::UnexpectedToken;
            case ErrorKind::UnterminatedSExpr:
                return aura::core::AuraErrorKind::UnterminatedSExpr;
            case ErrorKind::UnboundVariable:
                return aura::core::AuraErrorKind::UnboundVariable;
            case ErrorKind::DivisionByZero:
                return aura::core::AuraErrorKind::DivisionByZero;
            case ErrorKind::InvalidClosure:
                return aura::core::AuraErrorKind::InvalidClosure;
            case ErrorKind::ArityMismatch:
                return aura::core::AuraErrorKind::ArityMismatch;
            case ErrorKind::TypeError:
                return aura::core::AuraErrorKind::TypeError;
            case ErrorKind::OutOfMemory:
                return aura::core::AuraErrorKind::ArenaOutOfMemory;
            case ErrorKind::IRCorruption:
            case ErrorKind::IRNoReturn:
            case ErrorKind::InternalError:
                return aura::core::AuraErrorKind::InternalInvariantViolation;
            // The core enum has no separate category for these faces; they
            // stay EvalError rather than being guessed from text.
            case ErrorKind::UncaughtException:
            case ErrorKind::Note:
            case ErrorKind::Warning:
            case ErrorKind::StaleWorkspace:
            case ErrorKind::EffectDenied:
            case ErrorKind::UserError:
            case ErrorKind::ModuleError:
            case ErrorKind::IoError:
            case ErrorKind::MutationRefused:
                return aura::core::AuraErrorKind::EvalError;
        }
        return aura::core::AuraErrorKind::EvalError;
    }

    // Issue #808 Phase 1: AuraResult-facing eval entry (wraps EvalResult).
    // Hot-path eval_flat still returns EvalResult during migration; this
    // surface makes failure explicit as aura::core::AuraResult without
    // throwing. Prefer this for new Agent control-loop call sites.
    [[nodiscard]] aura::core::AuraResult<types::EvalValue>
    eval_as_aura_result(std::string_view input) {
        auto r = eval(input);
        // Issue #809: count Diagnostic→AuraError interop at Evaluator boundary.
        evaluator_.bump_error_policy_interop_conversion();
        if (r)
            return *r;
        const auto& d = r.error();
        return aura::core::make_unexpected(core_kind_of(d.kind), d.message);
    }

    // Issue #807/#808/#809: convert an existing EvalResult without re-running eval.
    [[nodiscard]] static aura::core::AuraResult<types::EvalValue>
    eval_result_to_aura(const EvalResult& r) {
        if (r)
            return *r;
        const auto& d = r.error();
        return aura::core::make_unexpected(core_kind_of(d.kind), d.message);
    }

    // Issue #4899: file mode (oneshot) turns a top-level statement-position
    // error VALUE that escaped the entry function into a located diagnostic,
    // the shape the tree walker already produces (#4465). Without this the IR
    // entry returned the value: main printed <error> (or nothing when a later
    // form overwrote the last value) and exited 0.
    [[nodiscard]] EvalResult file_mode_statement_error_4899(EvalResult r, ast::FlatAST& flat,
                                                            ast::NodeId root) {
        // Issue #4899 follow-up: always consume the IR entry escape stamp so a
        // later eval never reads a stale one.
        const auto escaped_stmt = evaluator_.take_ir_entry_escape_stmt(&flat);
        if (!r || !evaluator_.require_failure_is_diagnostic())
            return r;
        if (!types::is_error(*r) || types::is_string(*r))
            return r;
        // Issue #4899 follow-up: locate the TOP-LEVEL form the error left
        // through. A non-tail statement stamps its node on the escaping
        // Return; a tail error value (the program's last form) has no escape,
        // so it belongs to the last top-level form. Falling back to the root
        // (always 1:1) made main report the error at the first form and, in
        // direct mode, resume after it: forms 2..N ran a second time.
        const auto loc_of = [&flat](ast::NodeId node_id) {
            const auto node_view = flat.get(node_id);
            return aura::diag::SourceLocation{node_view.line == 0 ? 1u : node_view.line,
                                              node_view.col == 0 ? 1u : node_view.col, 0};
        };
        ast::NodeId top_form = root;
        if (root != ast::NULL_NODE && root < flat.size()) {
            const auto root_view = flat.get(root);
            if (root_view.tag == ast::NodeTag::Begin && !root_view.children.empty()) {
                top_form = root_view.child(root_view.children.size() - 1);
                if (escaped_stmt != 0 && escaped_stmt < flat.size()) {
                    // The top-level child that starts at or before the
                    // escaped statement (children are in source order).
                    const auto stmt_view = flat.get(static_cast<ast::NodeId>(escaped_stmt));
                    const auto before = [](std::uint32_t lhs_line, std::uint32_t lhs_col,
                                           std::uint32_t rhs_line, std::uint32_t rhs_col) {
                        return lhs_line < rhs_line || (lhs_line == rhs_line && lhs_col <= rhs_col);
                    };
                    for (std::size_t child_index = 0; child_index < root_view.children.size();
                         ++child_index) {
                        const auto child_view = flat.get(root_view.child(child_index));
                        if (child_view.line == 0 ||
                            !before(child_view.line, child_view.col, stmt_view.line, stmt_view.col))
                            break;
                        top_form = root_view.child(child_index);
                    }
                }
            }
        }
        if (top_form != ast::NULL_NODE && top_form < flat.size())
            evaluator_.set_file_resume_form_4899(loc_of(top_form));
        auto msg = evaluator_.soft_error_message(*r);
        auto loc = evaluator_.error_location(*r);
        if (!loc) {
            if (escaped_stmt != 0 && escaped_stmt < flat.size())
                loc = loc_of(static_cast<ast::NodeId>(escaped_stmt));
            else if (top_form != ast::NULL_NODE && top_form < flat.size())
                loc = loc_of(top_form);
            else
                loc = loc_of(root);
        }
        // Issue #4909: user (error ...) → "error:", refusals → effect-denied.
        return std::unexpected(
            aura::diag::Diagnostic{evaluator_.error_value_kind(*r), std::move(msg), *loc, root});
    }

    [[nodiscard]] EvalResult eval(std::string_view input);

    // #5242: eval's implementation unit reaches the wait-hook wiring (whose
    // helpers stay TU-local here) through this thin member.
    void wire_eval_mutex_wait_hooks_() { wire_eval_mutex_wait_hooks(); }

    // ---- IR pipeline ------------------------------------------------

    [[nodiscard]] EvalResult eval_ir(std::string_view input);

    // ── --jit: compile via LLVM ORC JIT and execute ──────────────
    // --jit: compile via LLVM ORC JIT and execute
    [[nodiscard]] EvalResult exec_jit(std::string_view input);

    // ---- Type checking (L6.x) ----------------------------------------

    // Run the TypeChecker on a input expression.
    // Returns a string with the inferred type or error messages.
    std::string typecheck(std::string_view input) {
        auto r = typecheck_full(input);
        return r.output;
    }

    // Issue #79: structured typecheck result. The `has_errors` flag is the
    // source of truth for "did typecheck find anything wrong?", and the
    // caller (e.g. main.cpp) should use this to set exit code rather than
    // parsing the output string.
    struct TypecheckResult {
        std::string output;
        bool has_errors = false;
        std::vector<aura::diag::Diagnostic> diagnostics;
    };

    TypecheckResult typecheck_full(std::string_view input);

    // Issue #148 Phase 5: incremental type inference API.
    // Given a MutationRecord (typically obtained from
    // query_mutation_log()), re-infer the affected subtree
    // of the current AST via the partial-reinference path
    // (TypeChecker::infer_flat_partial). Returns the number
    // of nodes that were re-inferred (cache hits don't
    // count). The full infer path is still available via
    // typecheck() — use this when the caller has a specific
    // MutationRecord and wants to skip re-inferring the
    // untouched subtrees.
    //
    // Requires a current AST and pool (set via set_code or
    // the typed_mutate path's (set-code ...) primitive). If
    // either is null, returns 0 (no work to do).
    //
    // The returned stats_ counter is updated as a side effect
    // (cache_hits / cache_misses / stale_cache), accessible
    // via the underlying TypeChecker (not exposed at the
    // CompilerService level yet — Phase 5b).
    std::size_t incremental_infer(const aura::ast::MutationRecord& rec);

    // ---- Multi-module arena support ----------------------------------

    // ---- Multi-module compilation (ArenaGroup) ----------------------

    // Get or create a per-module arena.
    ast::ASTArena& module_arena(const std::string& name,
                                std::size_t initial_size = 8uz * 1024 * 1024) {
        return arena_group_.module_arena(name, initial_size);
    }

    // Reset a specific module's arena.
    void reset_module(const std::string& name) {
        aura::diag::unbind_source_file_flats(name); // Issue #4916
        arena_group_.reset_module(name);
    }

    // ── Module-level state for incremental compilation ──────────────

    // Per-module state: source content + dirty flag + dependency set.
    // When any cached function that this module depends on is redefined,
    // the module is marked dirty and will be recompiled on next access.
    struct ModuleState {
        std::string source;
        std::unordered_set<std::string> deps; // cached functions this module depends on
        bool dirty = true;                    // initially dirty (needs compile)
    };

    // ── Cache helpers ────────────────────────────────────────────

    // Leaf directory must be a real directory owned by this euid with no
    // group/other write. A symlink or a shared /tmp directory is a miss.
    static bool module_cache_dir_trusted(const std::string& dir) {
        std::string leaf = dir;
        while (!leaf.empty() && (leaf.back() == '/' || leaf.back() == '\\'))
            leaf.pop_back();
        if (leaf.empty())
            return false;
        struct stat st;
        if (::lstat(leaf.c_str(), &st) != 0)
            return false;
        if (!S_ISDIR(st.st_mode))
            return false;
        if (st.st_uid != static_cast<uid_t>(::geteuid()))
            return false;
        const auto shared_write = static_cast<mode_t>(S_IWGRP) | static_cast<mode_t>(S_IWOTH);
        if ((st.st_mode & shared_write) != 0)
            return false;
        return true;
    }

    // $XDG_CACHE_HOME/aura/modules, else $HOME/.cache/aura/modules.
    // Both unset: disk cache disabled (no shared /tmp fallback). #5213
    static std::string module_cache_dir() {
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && xdg[0] != '\0')
            return std::string(xdg) + "/aura/modules/";
        if (const char* home = std::getenv("HOME"); home && home[0] != '\0')
            return std::string(home) + "/.cache/aura/modules/";
        return {};
    }

    // Cache file path for a module name + source content hash + build SHA.
    // The hash prevents loading stale cache when source changes. The SHA
    // keeps one binary from executing another's IR (#5213). Empty when the
    // disk cache is disabled.
    static std::string module_cache_path(const std::string& name, const std::string& source = "") {
        const auto dir = module_cache_dir();
        if (dir.empty())
            return {};
        auto sanitized = name;
        if (sanitized.empty())
            sanitized = "__default__";
        for (auto& c : sanitized) {
            if (c == '/' || c == '\\' || c == ':' || c == ' ')
                c = '_';
        }
        // Append a hash of the source to invalidate on source change
        if (!source.empty()) {
            auto h = std::hash<std::string>{}(source);
            sanitized += "_" + std::to_string(h);
        }
        std::string sha = AURA_BUILD_GIT_SHA;
        for (auto& c : sha) {
            const bool hex =
                (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (!hex)
                c = '_';
        }
        if (sha.empty())
            sha = "unknown";
        sanitized += "_";
        sanitized += sha;
        return dir + sanitized + ".abfc";
    }

    // Create the leaf at 0700. An existing untrusted directory is not
    // chmod'd; the caller skips the write.
    static bool ensure_cache_dir() {
        const auto dir = module_cache_dir();
        if (dir.empty())
            return false;
        if (module_cache_dir_trusted(dir))
            return true;
        std::string leaf = dir;
        while (!leaf.empty() && leaf.back() == '/')
            leaf.pop_back();
        const auto slash = leaf.find_last_of('/');
        const std::string parent =
            slash == std::string::npos ? std::string{} : leaf.substr(0, slash);
        std::error_code ec;
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        if (::mkdir(leaf.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
        return module_cache_dir_trusted(dir);
    }

    // Mark a module dirty when one of its IR dependencies changes.
    void mark_module_dirty(const std::string& changed_fn) {
        aura::core::arena_policy::signal_dirty_cascade();
        for (auto& [mname, state] : module_states_) {
            if (state.deps.count(changed_fn)) {
                state.dirty = true;
            }
        }
    }

    // Check if a module is dirty and needs recompilation.
    bool is_module_dirty(const std::string& name) const {
        auto it = module_states_.find(name);
        return it == module_states_.end() || it->second.dirty;
    }

    // Recompile a module only if it's dirty. The dirty-skip
    // optimization (Issue #125) makes this the primary way to
    // reload a module: callers should call reload_module()
    // instead of compile_module() when they just want to
    // re-evaluate. When the module is clean, this is a no-op
    // (the metrics counter `module_dirty_skips` is bumped).
    // When the module is dirty, the full compile_module() path
    // runs (the metrics counter `module_dirty_recompiles` is
    // bumped before the call).
    EvalResult reload_module(const std::string& name) {
        auto it = module_states_.find(name);
        if (it == module_states_.end()) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                          "module not found: " + name});
        }
        if (!it->second.dirty) {
            // Issue #125: already up to date — skip recompilation
            metrics_.module_dirty_skips.fetch_add(1, std::memory_order_relaxed);
            return EvalResult(types::make_void());
        }
        metrics_.module_dirty_recompiles.fetch_add(1, std::memory_order_relaxed);
        return compile_module(name, it->second.source);
    }

    // Compile a module into its own arena. Parses source, finds all
    // top-level (define ...) forms, caches each as IR, and binds env
    // via IRInterpreter when possible (Issue #272).
    //
    // Uses the module's dedicated arena instead of the main arena_.
    // On success marks the module as clean and records its dependencies.
    // Subsequent calls with the same name will detect dirty state
    // and skip recompilation if nothing changed.
    EvalResult compile_module(const std::string& name, const std::string& source);

    // ── EDSL IR cache (Phase 2) ──
    // Per-define IR cache for the (set-code ...) → (eval-current) pipeline.
    // Unlike ir_cache_ above (which is keyed by the script-level entry name),
    // this is keyed by the individual define NAME inside the workspace AST.
    // The dirty flag is set by mutate:rebind / mutate:set-body / etc. on the
    // workspace, so eval-current only re-lowers what changed.
    //
    // Source is the canonical unparsed form (stable across minor whitespace
    // changes), and source_hash is FNV-1a of that string. Cache hits when
    // source_hash matches AND dirty is false.
    struct IRCacheEntry {
        std::string source;                               // canonical (unparsed) form
        std::size_t source_hash = 0;                      // FNV-1a of source
        std::vector<aura::ir::IRFunction> irs;            // lowered IR functions
        std::vector<aura::ir::ClosureBridgeData> bridges; // parallel to irs
        std::vector<std::string> strings;                 // parallel string pool
        bool dirty = true;                                // needs re-lower
        std::size_t mutation_count = 0;                   // snapshot at lower time (legacy)
        // Issue #166: epoch snapshot at lower time. On every
        // mutation, mutation_epoch_ is incremented atomically.
        // On next access, if the entry's epoch doesn't match
        // the current epoch, it's stale and needs re-lower.
        // The cost is one uint64 compare per cache lookup
        // (~1ns); the benefit is a single global "is anything
        // stale?" check that complements the per-function
        // invalidate_function BFS in dep_graph_.
        std::uint64_t last_seen_epoch_ = 0;
        // Issue #2033: monotonic stamp (mutation + bridge + defuse).
        CacheEntryVersionStamp version_stamp_{};
        // Issue #1042: access stamp for LRU eviction under long-running serve.
        std::uint64_t last_used = 0;

        // Issue #2033 / #2111 / #2183: write stamp after successful lower /
        // store / partial / AOT restamp. Funnel through pure restamp_cache_entry.
        // soa_gen is the live SoA generation fence at store time.
        void stamp_version(std::uint64_t mut_epoch, std::uint64_t bridge, std::uint64_t defuse,
                           std::uint64_t soa_gen = 0) {
            restamp_cache_entry(version_stamp_, mut_epoch, bridge, defuse, soa_gen);
            mutation_count = static_cast<std::size_t>(mut_epoch);
            last_seen_epoch_ = mut_epoch;
        }

        // Issue #2111: bump SoA generation fence (module + functions).
        // Called from mark_*_dirty paths so dirty=false + gen advance
        // is still detected by should_relower.
        void bump_soa_generation() { soa_mod.bump_generation(); }
        [[nodiscard]] std::uint64_t live_soa_generation() const noexcept {
            return soa_mod.max_function_generation();
        }
        // Issue #196: per-block dirty bitmask. One inner
        // vector per IRFunction; each inner vector has 1 byte
        // per basic block (1=dirty, 0=clean). Indexed by
        // [func_idx][block_idx]. The outer vector mirrors
        // irs.size(); the inner vector mirrors
        // irs[func_idx].blocks.size().
        //
        // Why per-block: a typical mutation in a deep dep
        // graph touches 1-2 functions and even fewer blocks.
        // The full-invalidate approach (pre-#196) re-lowers
        // every function in the cascade; the per-block
        // approach (this) can in principle re-lower only
        // the affected blocks. The "smarter re-lower" Phase
        // 5 follow-up will consume is_block_dirty() to skip
        // clean blocks; this PR ships the data structure +
        // observability so the consumer can be wired in
        // without re-touching the cache.
        //
        // Convention: all blocks in a freshly-cached entry
        // are dirty (the entry needs a re-lower before
        // being usable). After store_define_v2 succeeds,
        // all blocks are clean.
        std::vector<std::vector<std::uint8_t>> block_dirty_per_func_;
        // Issue #684: dual-emit SoA module stored alongside AoS irs[].
        IRModuleV2 soa_mod;
        // Issue #684: per-instruction dirty bitmask (parallel to SoA
        // instruction_dirty_ column). Indexed [func_idx][instr_idx].
        std::vector<std::vector<std::uint8_t>> instruction_dirty_per_func_;
        // Issue #4615: cached block→instruction prefix offsets, one entry
        // per irs[] function. offsets[fi][b] is the flat instruction index
        // of block b's first instruction, so offsets[fi].back() is the
        // function's instruction total. The table is rebuilt lazily after
        // an irs layout change (irs_layout_epoch_ moves; every write to irs
        // must call invalidate_irs_layout_() — both halves are linted by
        // scripts/check_block_prefix_offsets_4615.py). Before this cache,
        // cascade_block_to_instructions and is_instruction_dirty recomputed
        // the total and the prefix base per block/query, so mutate:rebind
        // was O(B^2) in a calling define's basic blocks.
        mutable std::vector<std::vector<std::size_t>> block_inst_offsets_;
        mutable std::uint64_t block_inst_offsets_epoch_ = 0;
        std::uint64_t irs_layout_epoch_ = 0;
        // Issue #2045: persisted NodeId → (func, block[, instr]) reverse
        // index for compute_impact_scope. Rebuilt/patched after every
        // store_define_v2 / partial re-lower so selective invalidate
        // never uses a stale map against new IR layout.
        SourceToIrMap source_to_ir_map;
        // Issue #3117: dual-topology abort cleared this map; do not
        // lazy-rebuild from pre-abort IR (NodeIds vs restored AST).
        // Cleared on the next successful store_define_v2.
        bool abort_map_invalid = false;
        // Issue #3481: true only after store_define_v2 / true per-fn AST
        // relower. Facade AOT n>0 and instruction peel (pass-only, no AST
        // re-lower) must not restamp mutation/bridge/defuse/soa to live
        // while this is false — lookup stays 1 after a later dirty-bit
        // clear without a content store.
        bool content_stored_this_epoch = false;
        // Issue #3351: last-acked peer IR soft-stale generation for this
        // define (process-wide name gen from owner-scoped invalidate).
        // lookup_define_v2 must not return clean while live gen > ack.
        std::uint64_t peer_ir_stale_ack_ = 0;

        // Issue #196: per-block dirty bitmask helpers.

        // Mark every block in every function dirty. Used by
        // mark_define_dirty / mark_all_defines_dirty to
        // signal a full re-lower is needed.
        // Issue #2033 / #2034: also force SoA instruction_dirty_
        // sync from block_dirty_ so stamp/dirty columns stay
        // consistent after cascade.
        void mark_all_blocks_dirty() {
            for (auto& func_blocks : block_dirty_per_func_) {
                for (auto& b : func_blocks)
                    b = 1;
            }
            for (auto& soa_fn : soa_mod.functions)
                soa_fn.mark_all_blocks_dirty();
            (void)force_soa_instruction_dirty_sync();
            // Issue #4653: a wholesale block dirty mark must also cascade
            // the AoS instruction-dirty mask.  A fail-closed arm that
            // clears want_partial marks every block dirty, but the mask
            // used to keep the slots that were clean before the mark at
            // idf == 0.  The per-fn peel keeps the OLD instruction of a
            // clean slot, so a same-shape edit (same block count, same
            // per-block instruction count, different constant) promoted
            // the pre-edit IR.  Cascade the mask over the live layout so
            // no slot stays clean behind a wholesale mark.
            mark_all_instruction_dirty_for_current_layout_();
        }

        // Issue #4653: mark every live instruction slot dirty for the
        // CURRENT irs layout.  mark_all_instruction_dirty() only flips the
        // bits the mask already holds, so a layout change (a grown or
        // shrunk function) left the trailing slots clean.  Rebuild the mask
        // against the live layout (one row per irs function, one bit per
        // instruction) and set every bit, so a wholesale dirty mark can
        // never leave an idf == 0 slot behind.
        void mark_all_instruction_dirty_for_current_layout_() {
            if (instruction_dirty_per_func_.size() < irs.size())
                instruction_dirty_per_func_.resize(irs.size());
            for (std::size_t fi = 0; fi < irs.size(); ++fi) {
                std::size_t total = 0;
                for (const auto& b : irs[fi].blocks)
                    total += b.instructions.size();
                auto& idf = instruction_dirty_per_func_[fi];
                idf.assign(total, std::uint8_t{1});
            }
        }

        // Issue #2034 / #2139: mirror AoS block_dirty_per_func_ into SoA
        // block_dirty_, then force instruction_dirty_ cascade for every
        // dirty block via IRModuleV2::finish_dirty_sync() (single entry).
        // Returns # of instruction bits flipped 0→1 (metric accounting).
        // Safe to call after any cascade path that set AoS bits directly.
        // Issue #2522: batch mark_blocks_dirty per function (one generation
        // bump per function, not per block). Issue #2681 linter
        // (check_batch_dirty_discipline_2615.py) enforces batch discipline
        // across all production TUs in src/compiler/.
        std::size_t force_soa_instruction_dirty_sync() {
            for (std::size_t fi = 0; fi < block_dirty_per_func_.size(); ++fi) {
                if (fi >= soa_mod.functions.size())
                    break;
                auto& soa_fn = soa_mod.functions[fi];
                const auto& aos = block_dirty_per_func_[fi];
                std::vector<std::uint32_t> dirty_ids;
                dirty_ids.reserve(aos.size());
                for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(aos.size()); ++bi) {
                    if (aos[bi] != 0)
                        dirty_ids.push_back(bi);
                }
                if (!dirty_ids.empty())
                    soa_fn.mark_blocks_dirty(dirty_ids);
            }
            // Issue #2139: only production call path into module dirty sync.
            return soa_mod.finish_dirty_sync();
        }

        // Mark a single block dirty. Resizes the bitmask
        // for that function if needed (e.g. caller doesn't
        // know the block count up front).
        // Issue #2034: dual-emits SoA block+instr dirty so
        // partial re-lower / walk_soa_function_hotpath stay
        // trustworthy. Batch cascade paths should still call
        // force_soa_instruction_dirty_sync() once at the end.
        void mark_block_dirty(std::size_t func_idx, std::uint32_t block_idx) {
            mark_block_dirty_impl(func_idx, block_idx, /*cascade_instrs=*/true);
        }

        // Issue #2522: batch mark multiple blocks dirty on one function.
        // AoS bits + instr cascade per block; SoA uses mark_blocks_dirty
        // (single generation / fence advance). Empty span is a no-op.
        void mark_blocks_dirty(std::size_t func_idx, std::span<const std::uint32_t> block_idxs) {
            if (block_idxs.empty())
                return;
            if (func_idx >= block_dirty_per_func_.size())
                block_dirty_per_func_.resize(func_idx + 1);
            auto& fb = block_dirty_per_func_[func_idx];
            std::uint32_t max_id = 0;
            for (const auto bi : block_idxs)
                max_id = std::max(max_id, bi);
            if (max_id >= fb.size())
                fb.resize(max_id + 1, 0);
            for (const auto bi : block_idxs) {
                fb[bi] = 1;
                cascade_block_to_instructions(func_idx, bi);
            }
            if (func_idx < soa_mod.functions.size())
                soa_mod.functions[func_idx].mark_blocks_dirty(block_idxs);
        }

        // Issue #2109: mark block dirty without cascading every instruction
        // dirty — used when ImpactScope has precise affected_instrs so clean
        // instructions inside the block can be skipped on re-emit.
        // Prefer mark_blocks_dirty_bit_only for multi-block precise scopes (#2615).
        void mark_block_dirty_bit_only(std::size_t func_idx, std::uint32_t block_idx) {
            mark_block_dirty_impl(func_idx, block_idx, /*cascade_instrs=*/false);
        }

        // Issue #2615: multi-block bit-only + one SoA generation fence.
        // AoS block bits set; SoA uses mark_blocks_dirty_bits_only (no instr cascade).
        void mark_blocks_dirty_bit_only(std::size_t func_idx,
                                        std::span<const std::uint32_t> block_idxs) {
            if (block_idxs.empty())
                return;
            if (func_idx >= block_dirty_per_func_.size())
                block_dirty_per_func_.resize(func_idx + 1);
            auto& fb = block_dirty_per_func_[func_idx];
            std::uint32_t max_id = 0;
            for (const auto bi : block_idxs)
                max_id = std::max(max_id, bi);
            if (max_id >= fb.size())
                fb.resize(max_id + 1, 0);
            for (const auto bi : block_idxs)
                fb[bi] = 1;
            // No cascade_block_to_instructions — precise instr marks follow.
            if (func_idx < soa_mod.functions.size())
                soa_mod.functions[func_idx].mark_blocks_dirty_bits_only(block_idxs);
        }

        void mark_block_dirty_impl(std::size_t func_idx, std::uint32_t block_idx,
                                   bool cascade_instrs) {
            if (func_idx >= block_dirty_per_func_.size()) {
                block_dirty_per_func_.resize(func_idx + 1);
            }
            auto& fb = block_dirty_per_func_[func_idx];
            if (block_idx >= fb.size())
                fb.resize(block_idx + 1, 1);
            else
                fb[block_idx] = 1;
            if (cascade_instrs)
                cascade_block_to_instructions(func_idx, block_idx);
            if (func_idx < soa_mod.functions.size()) {
                // Issue #3355: production dual-emit uses batch-of-1 so residual
                // SoA single-mark is unreachable in this TU (same face as
                // #3293 abort). Soft/test keep IRFunctionSoA::mark_block_dirty.
                const std::uint32_t one[] = {block_idx};
                if (cascade_instrs)
                    soa_mod.functions[func_idx].mark_blocks_dirty(one);
                else
                    soa_mod.functions[func_idx].mark_blocks_dirty_bits_only(one);
            }
        }

        // Issue #4615: irs layout changed — drop the cached prefix offsets.
        // Call after every write to irs[] (store, clear, per-function
        // replace, __top__ prepend, poison sweep, test plant).
        void invalidate_irs_layout_() noexcept { ++irs_layout_epoch_; }

        // Issue #4615: O(1) amortized block→instruction prefix lookup.
        // The whole per-function table is rebuilt once per layout epoch;
        // every cascade / query after that is one indexed read.
        const std::vector<std::size_t>& block_inst_offsets_at_(std::size_t func_idx) const {
            if (block_inst_offsets_epoch_ != irs_layout_epoch_ ||
                block_inst_offsets_.size() != irs.size()) {
                block_inst_offsets_.clear();
                block_inst_offsets_.resize(irs.size());
                for (std::size_t fi = 0; fi < irs.size(); ++fi) {
                    const auto& blocks = irs[fi].blocks;
                    auto& offs = block_inst_offsets_[fi];
                    offs.assign(blocks.size() + 1, 0);
                    std::size_t acc = 0;
                    for (std::size_t bi = 0; bi < blocks.size(); ++bi) {
                        acc += blocks[bi].instructions.size();
                        offs[bi + 1] = acc;
                    }
                }
                block_inst_offsets_epoch_ = irs_layout_epoch_;
            }
            static const std::vector<std::size_t> kNoOffsets;
            if (func_idx >= block_inst_offsets_.size())
                return kNoOffsets;
            return block_inst_offsets_[func_idx];
        }

        // Issue #684: mark every instruction in a block dirty (SoA cascade).
        // Issue #4615: base/total come from the cached prefix — no per-call
        // sum over every block.
        void cascade_block_to_instructions(std::size_t func_idx, std::uint32_t block_idx) {
            if (func_idx >= irs.size())
                return;
            if (func_idx >= instruction_dirty_per_func_.size())
                instruction_dirty_per_func_.resize(func_idx + 1);
            const auto& func = irs[func_idx];
            const auto& offs = block_inst_offsets_at_(func_idx);
            const std::size_t total = offs.empty() ? 0 : offs.back();
            auto& idf = instruction_dirty_per_func_[func_idx];
            if (idf.size() < total)
                idf.resize(total, 0);
            if (block_idx >= func.blocks.size() || block_idx + 1 >= offs.size())
                return;
            const std::size_t base = offs[block_idx];
            const auto count = func.blocks[block_idx].instructions.size();
            for (std::size_t i = base; i < base + count; ++i)
                idf[i] = 1;
            for (const auto& instr : func.blocks[block_idx].instructions) {
                if (instr.narrow_evidence != 0 || instr.type_id != 0 ||
                    instr.linear_ownership_state != 0)
                    jit_typed_mutation::record_type_propagation_stamp();
            }
        }

        // Issue #4615: batch body mark — one resize + one contiguous fill
        // instead of a per-block cascade whose total/prefix scans were
        // O(B) each (O(B^2) per dirty mark). block_limit is the caller's
        // body bitmask width (block_dirty_per_func_[body_idx].size()); only
        // blocks below it were cascaded pre-fix, so the fill stops there
        // while the dirty column is still sized to the whole function
        // (matching the old per-call resize). Type-propagation stamps stay
        // per instruction so the #746 counters are unchanged.
        void mark_body_instructions_dirty_(std::size_t func_idx, std::size_t block_limit) {
            if (func_idx >= irs.size())
                return;
            if (func_idx >= instruction_dirty_per_func_.size())
                instruction_dirty_per_func_.resize(func_idx + 1);
            const auto& func = irs[func_idx];
            const auto& offs = block_inst_offsets_at_(func_idx);
            const std::size_t total = offs.empty() ? 0 : offs.back();
            auto& idf = instruction_dirty_per_func_[func_idx];
            if (idf.size() < total)
                idf.resize(total, 0);
            const std::size_t blocks_done = std::min(block_limit, func.blocks.size());
            const std::size_t end = offs.empty() ? 0 : offs[blocks_done];
            std::fill(idf.begin(), idf.begin() + static_cast<std::ptrdiff_t>(end), std::uint8_t{1});
            for (std::size_t bi = 0; bi < blocks_done; ++bi) {
                for (const auto& instr : func.blocks[bi].instructions) {
                    if (instr.narrow_evidence != 0 || instr.type_id != 0 ||
                        instr.linear_ownership_state != 0)
                        jit_typed_mutation::record_type_propagation_stamp();
                }
            }
        }

        void init_instruction_dirty_from_irs() {
            instruction_dirty_per_func_.clear();
            instruction_dirty_per_func_.reserve(irs.size());
            for (const auto& fn : irs) {
                std::size_t n = 0;
                for (const auto& b : fn.blocks)
                    n += b.instructions.size();
                instruction_dirty_per_func_.emplace_back(n, std::uint8_t{0});
            }
        }

        void clear_all_instruction_dirty() {
            for (auto& idf : instruction_dirty_per_func_)
                for (auto& b : idf)
                    b = 0;
        }

        // Issue #946/#950: full instruction-dirty cascade for define-level mutate.
        void mark_all_instruction_dirty() {
            if (instruction_dirty_per_func_.empty() && !irs.empty())
                init_instruction_dirty_from_irs();
            for (auto& idf : instruction_dirty_per_func_)
                for (auto& b : idf)
                    b = 1;
        }

        bool is_instruction_dirty(std::size_t func_idx, std::uint32_t block_idx,
                                  std::uint32_t inst_in_block) const {
            if (func_idx >= irs.size() || func_idx >= instruction_dirty_per_func_.size())
                return false;
            const auto& func = irs[func_idx];
            if (block_idx >= func.blocks.size())
                return false;
            // Issue #4615: prefix base from the cached offset table (O(1)).
            const auto& offs = block_inst_offsets_at_(func_idx);
            if (block_idx + 1 >= offs.size())
                return false;
            const auto idx = offs[block_idx] + inst_in_block;
            const auto& idf = instruction_dirty_per_func_[func_idx];
            if (idx >= idf.size())
                return false;
            return idf[idx] != 0;
        }

        void mark_instruction_dirty(std::size_t func_idx, std::uint32_t block_idx,
                                    std::uint32_t inst_in_block) {
            if (func_idx >= irs.size())
                return;
            if (func_idx >= instruction_dirty_per_func_.size())
                instruction_dirty_per_func_.resize(func_idx + 1);
            const auto& func = irs[func_idx];
            // Issue #4615: total + base from the cached prefix (O(1)).
            const auto& offs = block_inst_offsets_at_(func_idx);
            const std::size_t total = offs.empty() ? 0 : offs.back();
            auto& idf = instruction_dirty_per_func_[func_idx];
            if (idf.size() < total)
                idf.resize(total, 0);
            if (block_idx >= func.blocks.size() || block_idx + 1 >= offs.size())
                return;
            const auto idx = offs[block_idx] + inst_in_block;
            if (idx < idf.size())
                idf[idx] = 1;
        }

        void clear_instruction_dirty(std::size_t func_idx, std::uint32_t block_idx,
                                     std::uint32_t inst_in_block) {
            if (func_idx >= instruction_dirty_per_func_.size())
                return;
            if (func_idx >= irs.size())
                return;
            const auto& func = irs[func_idx];
            if (block_idx >= func.blocks.size())
                return;
            // Issue #4615: prefix base from the cached offset table (O(1)).
            const auto& offs = block_inst_offsets_at_(func_idx);
            if (block_idx + 1 >= offs.size())
                return;
            const auto idx = offs[block_idx] + inst_in_block;
            auto& idf = instruction_dirty_per_func_[func_idx];
            if (idx < idf.size())
                idf[idx] = 0;
        }

        // Clear a single block's dirty flag (called by the
        // smarter-re-lower after re-lowering the block).
        // No-op if indices out of range.
        void clear_block_dirty(std::size_t func_idx, std::uint32_t block_idx) {
            if (func_idx >= block_dirty_per_func_.size())
                return;
            auto& fb = block_dirty_per_func_[func_idx];
            if (block_idx >= fb.size())
                return;
            fb[block_idx] = 0;
        }

        // Query: is this (function, block) dirty? Returns
        // false (clean) for out-of-range indices.
        bool is_block_dirty(std::size_t func_idx, std::uint32_t block_idx) const {
            if (func_idx >= block_dirty_per_func_.size())
                return false;
            const auto& fb = block_dirty_per_func_[func_idx];
            if (block_idx >= fb.size())
                return false;
            return fb[block_idx] != 0;
        }

        // Query: total dirty block count across all
        // functions. Used by the observability primitive.
        std::size_t dirty_block_count() const {
            std::size_t n = 0;
            for (const auto& fb : block_dirty_per_func_) {
                for (auto b : fb)
                    if (b)
                        ++n;
            }
            return n;
        }

        // Query: dirty block count for a single function.
        // Returns 0 for out-of-range func_idx.
        std::size_t func_dirty_block_count(std::size_t func_idx) const {
            if (func_idx >= block_dirty_per_func_.size())
                return 0;
            std::size_t n = 0;
            for (auto b : block_dirty_per_func_[func_idx])
                if (b)
                    ++n;
            return n;
        }

        // Issue #1915: true if any block dirty bit is set.
        [[nodiscard]] bool any_block_dirty() const noexcept {
            for (const auto& fb : block_dirty_per_func_) {
                for (auto b : fb) {
                    if (b)
                        return true;
                }
            }
            return dirty;
        }

        // Issue #2035: map AoS block_dirty_per_func_ / SoA block_dirty_
        // columns into an AOT emit region mask for selective reemit.
        // Partial dirty → Performance bit (1<<1); full dirty adds (1<<3).
        // Evolution bit (1<<2) is never set (permanently excluded by #2016).
        [[nodiscard]] std::uint64_t compute_region_mask_from_dirty() const noexcept {
            bool any = false;
            bool full = true;
            bool saw = false;
            for (const auto& fb : block_dirty_per_func_) {
                if (fb.empty()) {
                    full = false;
                    continue;
                }
                for (auto b : fb) {
                    saw = true;
                    if (b)
                        any = true;
                    else
                        full = false;
                }
            }
            if (!any) {
                for (const auto& fn : soa_mod.functions) {
                    for (auto b : fn.block_dirty_) {
                        saw = true;
                        if (b)
                            any = true;
                        else
                            full = false;
                    }
                }
            }
            if (!any) {
                if (dirty)
                    return (1ULL << 1); // body-only dirty flag, no bit layout
                return 0;
            }
            std::uint64_t mask = (1ULL << 1); // Performance / body region
            if (full && saw)
                mask |= (1ULL << 3); // broader reemit for full-function dirty
            return mask;
        }

        // Issue #4311: production store_define_v2 / cache_define drop
        // __top__. size>=2 is then [body, nested], not [__top__, body].
        // Only a real leading __top__ shifts the body to index 1.
        [[nodiscard]] std::size_t body_idx() const noexcept {
            if (irs.size() >= 2 && irs[0].name == "__top__")
                return 1;
            return 0;
        }

        // Issue #1915: body-only dirty stamp (prefer partial re-lower).
        // Dual-shape: irs[0]=__top__ (kept clean), irs[1]=body (all blocks dirty).
        // Single-fn and dropped-__top__ production store: body at irs[0].
        // Returns # of blocks marked.
        std::size_t mark_body_only_dirty() {
            if (irs.empty()) {
                mark_all_blocks_dirty();
                return dirty_block_count();
            }
            if (block_dirty_per_func_.size() < irs.size())
                block_dirty_per_func_.resize(irs.size());
            const std::size_t body_idx = this->body_idx();
            if (body_idx >= block_dirty_per_func_.size() ||
                block_dirty_per_func_[body_idx].empty()) {
                // Resize body bitmask from irs layout.
                if (body_idx < irs.size()) {
                    if (block_dirty_per_func_.size() <= body_idx)
                        block_dirty_per_func_.resize(body_idx + 1);
                    block_dirty_per_func_[body_idx].assign(irs[body_idx].blocks.size(),
                                                           std::uint8_t{0});
                }
            }
            if (body_idx >= block_dirty_per_func_.size() ||
                block_dirty_per_func_[body_idx].empty()) {
                mark_all_blocks_dirty();
                return dirty_block_count();
            }
            // Keep __top__ clean only when it is actually the prefix.
            // Issue #4311: do not clear irs[0] on a dropped-__top__ bundle.
            if (body_idx == 1 && !irs.empty() && irs[0].name == "__top__" &&
                !block_dirty_per_func_[0].empty()) {
                for (auto& b : block_dirty_per_func_[0])
                    b = 0;
            }
            // Issue #2522: batch body blocks — one SoA generation bump,
            // not N× mark_block_dirty fence advances.
            auto& fb = block_dirty_per_func_[body_idx];
            std::vector<std::uint32_t> body_ids;
            body_ids.reserve(fb.size());
            for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(fb.size()); ++bi) {
                fb[bi] = 1;
                body_ids.push_back(bi);
            }
            // Issue #4615: one batch instruction cascade for the whole body.
            // The old loop called cascade_block_to_instructions per block,
            // and each call rescanned every block for the total and blocks
            // 0..bi for the prefix base — O(B^2) per body-dirty mark.
            mark_body_instructions_dirty_(body_idx, fb.size());
            if (body_idx < soa_mod.functions.size() && !body_ids.empty())
                soa_mod.functions[body_idx].mark_blocks_dirty(body_ids);
            const std::size_t n = body_ids.size();
            // Issue #2034 / #2139: finish_dirty_sync validates block→instr
            // (batch already cascaded; repair any residual desync without
            // re-bumping per block).
            (void)soa_mod.finish_dirty_sync();
            dirty = true;
            // Issue #3481: body-dirty means cached irs is pre-mutate again.
            content_stored_this_epoch = false;
            return n;
        }

        // Issue #1915: mark only the entry/body function of a *caller*
        // (dependent) dirty — nested lambdas stay clean for partial re-lower.
        // Returns # of blocks marked (0 → fallback full mark).
        std::size_t mark_caller_body_dirty() { return mark_body_only_dirty(); }

        // Query: total instruction count for a single
        // function. Sum of all instructions across all
        // basic blocks in irs[func_idx]. Returns 0 for
        // out-of-range func_idx. Used by
        // (query:soa-dirty-stats) to compute the
        // dirty-instruction percentage.
        std::size_t func_instruction_count(std::size_t func_idx) const {
            if (func_idx >= irs.size())
                return 0;
            std::size_t n = 0;
            for (const auto& b : irs[func_idx].blocks)
                n += b.instructions.size();
            return n;
        }

        // Issue #293: count of functions that have at least
        // one dirty block. Used by the (query:compiler-cache-
        // stats) 3-tuple primitive.
        std::size_t dirty_func_count() const {
            std::size_t n = 0;
            for (const auto& fb : block_dirty_per_func_) {
                for (auto b : fb) {
                    if (b) {
                        ++n;
                        break;
                    }
                }
            }
            return n;
        }

        // Issue #293 / #2032: count of functions whose dirty block
        // count falls in the "incremental re-lower" range
        // (1..(T-1) dirty blocks, per estimate_relower_blocks /
        // get_partial_relower_threshold). Functions at/above
        // threshold are "full re-lower" candidates.
        std::size_t incremental_candidates_count() const {
            const auto thr = get_partial_relower_threshold();
            std::size_t n = 0;
            for (const auto& fb : block_dirty_per_func_) {
                std::size_t dirty = 0;
                for (auto b : fb)
                    if (b)
                        ++dirty;
                if (dirty > 0 && dirty < thr)
                    ++n;
            }
            return n;
        }

        // Initialize the per-func bitmask to match the
        // current irs[] layout. All blocks start dirty
        // (the entry needs re-lower before use). Called
        // by store_define_v2 after irs is populated.
        void init_block_dirty_from_irs() {
            block_dirty_per_func_.clear();
            block_dirty_per_func_.reserve(irs.size());
            for (const auto& fn : irs) {
                block_dirty_per_func_.emplace_back(fn.blocks.size(), std::uint8_t{1});
            }
        }

        // Clear all per-block dirty bits. Called by
        // store_define_v2 after init — the just-stored
        // entry is clean.
        void clear_all_block_dirty() {
            for (auto& fb : block_dirty_per_func_) {
                for (auto& b : fb)
                    b = 0;
            }
        }

        // Issue #196: public read-only view of the per-block
        // dirty bitmask for the observability layer.
        [[nodiscard]] const std::vector<std::vector<std::uint8_t>>&
        block_dirty_view() const noexcept {
            return block_dirty_per_func_;
        }
    };
    std::unordered_map<std::string, IRCacheEntry, aura::core::TransparentStringHash,
                       std::equal_to<>>
        ir_cache_v2_;
    // Issue #4091: FnKey → define-name side index for the shape dirty hook.
    // mark_shape_dirty_for_fn_key resolves the cache entry in O(1) instead
    // of hashing session_id_ + name for every cached define on every
    // result-shape flip. Maintained at the ir_cache_v2_ insert/erase sites.
    std::unordered_map<shape::FnKey, std::string> shape_fnkey_names_;
    // Issue #3069: abort-force generation fence. Bumped (release) *before*
    // walking ir_cache_v2_ so concurrent lookup_define_v2 can observe
    // abort-in-progress without reading a half-forced entry. 0 = never
    // aborted (Soft/Off lookup pays one acquire of 0).
    std::atomic<std::uint64_t> abort_force_generation_{0};
    std::atomic<std::uint8_t> abort_force_in_progress_{0};
    std::atomic<std::uint8_t> abort_force_hold_{0}; // test-only mid-loop window
    // Issue #959 / #1042: hard cap for long-running --serve.
    // Evicts dirty-first, then oldest last_used (LRU).
    static constexpr std::size_t kIRCacheV2MaxEntries = 2048;
    std::uint64_t ir_cache_v2_access_clock_ = 0;

    void maybe_evict_ir_cache_v2() {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (ir_cache_v2_.size() <= kIRCacheV2MaxEntries)
            return;
        // Prefer dropping dirty entries (will re-lower on demand).
        for (auto it = ir_cache_v2_.begin();
             it != ir_cache_v2_.end() && ir_cache_v2_.size() > kIRCacheV2MaxEntries;) {
            if (it->second.dirty) {
                // Issue #4091: drop the FnKey → name side index row.
                shape_fnkey_names_.erase(shape::make_fn_key(session_id_, it->first));
                // Issue #4528: same-generation invalidate — drop the
                // victim's v1 / bridge / strings rows in the same erase.
                // A surviving v1 row keeps the pre-mutate body lowerable:
                // lower_to_ir_with_cache_tracked copies it into the next
                // caller and store_define_v2 republishes those bytes as a
                // clean generation (the freshness consults only refused a
                // v2 row that is PRESENT and dirty, not one that is gone).
                ir_cache_.erase(it->first);
                ir_cache_bridge_.erase(it->first);
                ir_cache_strings_.erase(it->first);
                it = ir_cache_v2_.erase(it);
                metrics_.ir_cache_v2_evictions_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
        // Issue #1042: still over → LRU by last_used (oldest first).
        const std::size_t target = kIRCacheV2MaxEntries * 3 / 4;
        while (ir_cache_v2_.size() > target && !ir_cache_v2_.empty()) {
            auto victim = ir_cache_v2_.begin();
            std::uint64_t oldest = victim->second.last_used;
            for (auto it = ir_cache_v2_.begin(); it != ir_cache_v2_.end(); ++it) {
                if (it->second.last_used < oldest) {
                    oldest = it->second.last_used;
                    victim = it;
                }
            }
            // Issue #4091: drop the FnKey → name side index row.
            shape_fnkey_names_.erase(shape::make_fn_key(session_id_, victim->first));
            // Issue #4528: same-generation invalidate (see the dirty arm).
            ir_cache_.erase(victim->first);
            ir_cache_bridge_.erase(victim->first);
            ir_cache_strings_.erase(victim->first);
            ir_cache_v2_.erase(victim);
            metrics_.ir_cache_v2_evictions_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.ir_cache_v2_lru_evictions_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ── EDSL IR cache V2 (Phase 2) public API ──
    // Called from evaluator_primitives_eval.cpp's (eval-current) primitive.
    // See dual-workspace design (archived: docs-archive-pre-2026-06).

    // Look up a define in the cache. Returns:
    //   0 = hit, source matches, not dirty → reuse cached IR
    //   1 = source changed OR dirty → needs re-lower
    //   2 = not in cache → first time
    // If entry exists but is dirty, the caller should re-lower and call
    // store_define_v2 to update the entry.
    int lookup_define_v2(const std::string& name, std::size_t source_hash) {
        // Issue #3069 / #3258 / #4717: the abort fence must be observable
        // WHILE force_ir_cache_dirty_after_abort walks the cache. The walker
        // holds ir_cache_gate_ for the whole walk, so a read path that takes
        // the same gate first never sees the mid-walk window (it would answer
        // from a half-forced entry) and deadlocks against the #3159 AC4
        // mid-walk hold (walker waits for the hold, the probe waits for the
        // gate). Answer the in-progress face lock-free, before the gate:
        // fail-closed needs-relower, no entry read.
        if (abort_force_in_progress_.load(std::memory_order_acquire) != 0) {
            metrics_.should_relower_total.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return 2;
        // Issue #1042: touch LRU stamp on every lookup.
        it->second.last_used = ++ir_cache_v2_access_clock_;
        // Issue #126 / #2033: pure should_relower with CacheEntryVersionStamp
        // (mutation_epoch + bridge_epoch + defuse_version). Fixes the pre-#2033
        // bug that passed mutation_count twice (never detecting epoch drift).
        // Issue #1555: also treat body-only bitmasks (dirty_block_count>0)
        // as needs-relower even if entry.dirty was cleared inconsistently.
        const auto cur_mut = aura::core::current_mutation_epoch();
        const auto cur_bridge = bridge_epoch();
        const auto cur_defuse = evaluator_.defuse_version();
        const auto cur_soa_gen = it->second.live_soa_generation();
        // Issue #3069 / Issue #3258: abort-force fence. gen==0: one acquire,
        // never aborted (Soft/Off zero extra). Else: in_progress OR this
        // entry's abort_force_generation lagging live gen → needs-relower.
        // Do not serve pre-abort IR or consult source_to_ir_map.
        const auto abort_gen0 = abort_force_generation_.load(std::memory_order_acquire);
        if (abort_force_rejects_clean_hit_(it->second, abort_gen0)) {
            metrics_.should_relower_total.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        // Issue #3324: abort dual-topology restore cleared the map and
        // marked abort_map_invalid. lookup must not treat a later
        // dirty-cleared / live-restamped entry as a clean hit while the
        // map/IR still belong to pre-abort identity. Soft abort already
        // set the flag (force-dirty is abort-path, not production-gated).
        // Issue #3551: count as should_relower (not silent serve).
        if (it->second.abort_map_invalid) {
            metrics_.should_relower_total.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        // Issue #3481: content latch. Facade AOT / instr peel may ack
        // peer + abort-force gen without a content store. lookup stays
        // needs-relower until store_define_v2 or a peel that rewrote AST.
        if (!it->second.content_stored_this_epoch)
            return 1;
        std::uint32_t reasons = 0;
        const bool need = should_relower(source_hash, it->second.source_hash, it->second.dirty,
                                         it->second.version_stamp_, cur_mut, cur_bridge, cur_defuse,
                                         &reasons, cur_soa_gen, abort_gen0) ||
                          it->second.dirty_block_count() > 0;
        if (need) {
            metrics_.should_relower_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2033 / #2183: stamp-domain observability for silent-stale
            // forensics. Never serve IR when stamp domains disagree with live.
            if (reasons & kRelowerBridgeEpoch)
                metrics_.should_relower_bridge_epoch_mismatch_total.fetch_add(
                    1, std::memory_order_relaxed);
            const std::uint32_t stamp_bits =
                reasons & (kRelowerMutationDrift | kRelowerBridgeEpoch | kRelowerDefuseVersion |
                           kRelowerSoaGeneration);
            if (stamp_bits != 0) {
                metrics_.should_relower_stamp_mismatch_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                // Issue #2183 AC2: dedicated force-relower counter + reason OR.
                metrics_.cache_stamp_mismatch_force_relower_total.fetch_add(
                    1, std::memory_order_relaxed);
                metrics_.cache_stamp_mismatch_reasons_bits.fetch_or(
                    static_cast<std::uint64_t>(stamp_bits), std::memory_order_relaxed);
            }
            // Issue #2111: generation-only stale (dirty may be false).
            if (reasons & kRelowerSoaGeneration) {
                metrics_.soa_generation_stale_prevented_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            }
            return 1;
        }
        // Issue #3069 / Issue #3258: abort may have started after the first check.
        const auto abort_gen1 = abort_force_generation_.load(std::memory_order_acquire);
        if (abort_force_rejects_clean_hit_(it->second, abort_gen1) ||
            (abort_gen1 != 0 && abort_gen0 != abort_gen1))
            return 1;
        // Issue #3351: owner-scoped invalidate does not bump
        // g_aot_table_epoch (#2841/#2951). Peer IR-cache can still look
        // clean (dirty + stamps match). Last-look: name-level peer IR
        // soft-stale generation. Empty table: one acquire (Soft /
        // single-eval zero extra). Reuses should_relower_total.
        const auto ir_stale_gen = aura_aot_peer_ir_name_stale_gen(name.c_str());
        if (ir_stale_gen > it->second.peer_ir_stale_ack_) {
            metrics_.should_relower_total.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        // Issue #1915: clean hit with matching source_hash — no re-lower.
        metrics_.invalidate_early_exit_clean_total.fetch_add(1, std::memory_order_relaxed);
        return 0; // hit
    }

    // Store (or replace) a define's IR cache entry. Called after re-lower.
    // Also recomputes the source_hash from the canonical unparse.
    // Issue #196: also rebuilds the per-block dirty bitmask from the
    // freshly-stored irs[] (all blocks clean) so the entry is ready
    // for incremental re-lower on subsequent mutations.
    void store_define_v2(const std::string& name, std::string source,
                         std::vector<aura::ir::IRFunction> irs,
                         std::vector<aura::ir::ClosureBridgeData> bridges,
                         std::vector<std::string> strings, std::uint64_t sampled_mut_epoch) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        // Issue #4341: sampled_mut_epoch is the mutation epoch the stored
        // IR was sampled under — captured before the lower (#4338
        // semantics), NOT read live at this store.
        auto hash = fnv1a_64(source);
        auto& entry = ir_cache_v2_[name];
        // Issue #4091: keep the FnKey → name side index in sync.
        shape_fnkey_names_.insert_or_assign(shape::make_fn_key(session_id_, name), name);
        entry.source = std::move(source);
        entry.source_hash = hash;
        entry.irs = std::move(irs);
        entry.invalidate_irs_layout_();
        entry.bridges = std::move(bridges);
        entry.strings = std::move(strings);
        entry.dirty = false;
        entry.last_used = ++ir_cache_v2_access_clock_; // #1042
        // Issue #3117 / #3481: stored IR is the content epoch — clear abort
        // latch and mark content stored BEFORE restamp so #3481 does not
        // skip the live mutation/bridge/defuse/soa write.
        entry.abort_map_invalid = false;
        entry.content_stored_this_epoch = true;
        // Issue #4157: stored content is a new AST/IR epoch — residual
        // CastOp persist noted before this store holds NodeIds the
        // rebuilt source_to_ir_map can mis-attribute. Invalidate persist
        // freshness so the #3986 Shape-flip fail-closed treats it as
        // unproven. Production/Full only (Soft/Off never consult the
        // flip gate).
        if (aura::compiler::dirty::residual_castop_persist_active())
            aura::compiler::dirty::bump_residual_castop_persist_content_epoch();
        // Issue #2033 / #2111 / #2183: unified restamp after successful store.
        // Issue #4377: name is recorded when this store sits inside a
        // mutation boundary so the exit defuse bump can be acknowledged.
        restamp_cache_entry_live_(entry, sampled_mut_epoch, name);
        ack_peer_ir_stale_on_restamp_(entry, name);
        // Issue #3136: residual-force bitmap. Soft / Off skips this probe.
        if (aura_production_defaults_active_probe() != 0) {
            hot_update_registry().note_relower_success_coverage(1ULL << (fnv1a_64(name) & 63));
            // Issue #3229: define-id side set (6-bit collision).
            hot_update_registry().note_relower_success_define(relower_success_define_id(name));
            // Issue #3513: clear the native-untrusted latch before reemit.
            hot_update_registry().note_ir_content_stored_for_native();
            // Issue #4577: full relower stores new IR before reemit.
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                erase_jit_cache_bundle_rows_for_define_(name);
            }
            aura_drop_jit_fn_native_for_define(name.c_str());
            // Issue #3751: decide_and_reemit after the dirty-ring push.
            struct ReemitEvalOwnerGuard {
                void* prev_reemit;
                void* prev_reg;
                explicit ReemitEvalOwnerGuard(void* e) noexcept
                    : prev_reemit(aura_aot_get_reemit_owner_eval())
                    , prev_reg(aura_aot_get_register_owner_eval()) {
                    aura_aot_set_reemit_owner_eval(e);
                    aura_aot_set_register_owner_eval(e);
                }
                ~ReemitEvalOwnerGuard() noexcept {
                    aura_aot_set_reemit_owner_eval(prev_reemit);
                    aura_aot_set_register_owner_eval(prev_reg);
                }
                ReemitEvalOwnerGuard(const ReemitEvalOwnerGuard&) = delete;
                ReemitEvalOwnerGuard& operator=(const ReemitEvalOwnerGuard&) = delete;
                ReemitEvalOwnerGuard(ReemitEvalOwnerGuard&&) = delete;
                ReemitEvalOwnerGuard& operator=(ReemitEvalOwnerGuard&&) = delete;
            } owner_guard(static_cast<void*>(&evaluator_));
            aura_production_dirty_ring_push(name.c_str(), 0, 0);
            (void)hot_update_registry().decide_and_reemit(aura_get_aot_defuse_version(),
                                                          HotUpdateRegistry::ReemitReason::Cascade);
        }
        // Issue #196: rebuild the per-block dirty bitmask to
        // match the new irs layout, then mark all blocks clean.
        // init_block_dirty_from_irs() sizes to irs[].blocks.size()
        // and marks all dirty; clear_all_block_dirty() flips
        // them to clean. Net effect: bitmask mirrors irs shape
        // and reports "no dirty blocks".
        entry.init_block_dirty_from_irs();
        entry.init_instruction_dirty_from_irs();
        entry.clear_all_block_dirty();
        entry.clear_all_instruction_dirty();
        // Issue #684: attach dual-emit SoA module from last lower.
        if (pending_soa_snapshot_) {
            entry.soa_mod = std::move(pending_soa_snapshot_->module);
            pending_soa_snapshot_.reset();
        }
        // Issue #2033 / #2034: ensure SoA instr dirty stays in sync
        // after store shape rebuild (AoS bits may be clean; still
        // force-sync for dual-emit parity).
        (void)entry.force_soa_instruction_dirty_sync();
        // Issue #2045: full rebuild of source_to_ir_map after store
        // (new IR layout + dual-emit SoA); consistency check wired.
        rebuild_or_patch_source_to_ir_map_(entry, /*only_func_idx=*/std::nullopt);
        // Issue #959: enforce max-size policy after store.
        maybe_evict_ir_cache_v2();
    }

    // Mark a single define dirty. Called by mutate:rebind, mutate:set-body, etc.
    // When dirty is set, the next (eval-current) will re-lower the define.
    //
    // Phase 3: also cascade via dep_graph_[name].called_by (BFS) so all
    // transitively dependent defines are marked dirty too. A mutation to
    // f must re-lower every g that references f (the IR for g embeds a
    // closure capture of f's lowered function). When pre_cache_workspace_defines
    // is enabled (Phase 3 full), the depends_on scan populates dep_graph_.
    //
    // Issue #196: also marks every block in every function of the
    // affected entries as dirty. This is a conservative
    // whole-function invalidation; the smarter per-block
    // invalidation will be added in a follow-up that consults
    // the dep_graph_ to find which block(s) of a dependent
    // actually reference the mutated define.
    // Issue #741: detect quote/lambda in a FlatAST (agent macro/self-mod).
    static bool flat_has_quote_or_lambda(const aura::ast::FlatAST& flat) noexcept {
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            const auto tag = flat.get(id).tag;
            if (tag == aura::ast::NodeTag::Quote || tag == aura::ast::NodeTag::Lambda)
                return true;
        }
        return false;
    }

    // Issue #741: selective bridge refresh — only expire shared_ptr views
    // for func indices named in impact_scope; bump epoch on the rest.
    //
    // Issue #1523 lock order: NO mutex acquired here (pure bridge table).
    // Caller must already hold mutate_mtx_ when racing with concurrent
    // invalidate_function (invalidate_function does). Public call sites
    // that may race should take OrderedUniqueLock(mutate) first.
    void selective_invalidate_bridge_for_impact(const std::string& name, const ImpactScope& scope) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        std::vector<aura::ir::ClosureBridgeData>* bridges = nullptr;
        if (auto bit = ir_cache_bridge_.find(name); bit != ir_cache_bridge_.end())
            bridges = &bit->second;
        else if (auto vit = ir_cache_v2_.find(name);
                 vit != ir_cache_v2_.end() && !vit->second.bridges.empty())
            bridges = &vit->second.bridges;
        if (!bridges || bridges->empty())
            return;

        std::unordered_set<std::size_t> affected_funcs;
        for (const auto& b : scope.affected_blocks)
            affected_funcs.insert(b.function_index);
        // Issue #2031: also collect funcs touched only via precise instr refs.
        for (const auto& i : scope.affected_instrs)
            affected_funcs.insert(i.function_index);

        pending_impact_func_indices_ = affected_funcs;

        const auto current_epoch = bridge_epoch();
        std::uint64_t impacted = 0;
        for (std::size_t fi = 0; fi < bridges->size(); ++fi) {
            auto& bridge = (*bridges)[fi];
            bridge.bridge_epoch = current_epoch;
            if (!affected_funcs.empty() && !affected_funcs.count(fi))
                continue;
            bridge.flat.reset();
            bridge.pool.reset();
            bridge.body_id = aura::ast::NULL_NODE;
            ++impacted;
        }
        if (impacted > 0) {
            metrics_.bridge_invalidations_count.fetch_add(1, std::memory_order_relaxed);
            metrics_.compiler_inval_bridge_epoch_total.fetch_add(impacted,
                                                                 std::memory_order_relaxed);
            const std::uint64_t block_count =
                scope.affected_blocks.empty() ? 1u : scope.affected_blocks.size();
            metrics_.incremental_closure_bridge_impact_blocks_total.fetch_add(
                block_count, std::memory_order_relaxed);
            evaluator_.bump_incremental_closure_bridge_impact_blocks(block_count);
            // Issue #2031: instr-level impact observability on bridge path.
            if (scope.instr_level_hits > 0)
                metrics_.instr_level_impact_hits_total.fetch_add(scope.instr_level_hits,
                                                                 std::memory_order_relaxed);
            if (!scope.affected_instrs.empty())
                metrics_.instr_level_dirty_marks_total.fetch_add(scope.affected_instrs.size(),
                                                                 std::memory_order_relaxed);
        }
    }

    // Issue #2031 / #2045: build NodeId → (func, block, instr) from
    // lowered IR source_ast_node_id stamps. Thin wrapper over pure helper
    // so call sites (dirty cascade, impact scope) keep a stable name.
    static void populate_source_to_ir_from_irs(const std::vector<aura::ir::IRFunction>& irs,
                                               SourceToIrMap& out) {
        populate_source_to_ir_map_from_irs(irs, out);
    }

    // Issue #2045: after every (partial or full) re-lower that updates
    // ir_cache_v2_ / SoA, rebuild or incrementally patch source_to_ir_map
    // and run a cheap consistency check (debug/fuzz + metrics).
    // only_func_idx = nullopt → full rebuild; set → patch that function.
    void rebuild_or_patch_source_to_ir_map_(IRCacheEntry& entry,
                                            std::optional<std::size_t> only_func_idx) {
        if (only_func_idx.has_value()) {
            const auto fi = *only_func_idx;
            if (fi < entry.irs.size()) {
                patch_source_to_ir_map_for_function(entry.irs[fi], fi, entry.source_to_ir_map);
            } else {
                // Out-of-range patch request → full rebuild (safe).
                rebuild_source_to_ir_map_from_irs(entry.irs, entry.source_to_ir_map);
            }
            metrics_.source_to_ir_map_patch_total.fetch_add(1, std::memory_order_relaxed);
        } else {
            rebuild_source_to_ir_map_from_irs(entry.irs, entry.source_to_ir_map);
            metrics_.source_to_ir_map_rebuild_total.fetch_add(1, std::memory_order_relaxed);
        }
        // SoA dual-emit: when SoA columns exist, verify map indices
        // resolve to live blocks there too (under-mapping risk if SoA
        // and AoS diverge after partial replace).
        if (!entry.soa_mod.functions.empty()) {
            std::size_t soa_bad = 0;
            for (const auto& [nid, loc] : entry.source_to_ir_map) {
                if (loc.function_index >= entry.soa_mod.functions.size()) {
                    ++soa_bad;
                    continue;
                }
                const auto& soa_fn = entry.soa_mod.functions[loc.function_index];
                if (loc.block_index >= soa_fn.blocks_.size()) {
                    ++soa_bad;
                    continue;
                }
                if (loc.has_instr()) {
                    const auto& blk = soa_fn.blocks_[loc.block_index];
                    const auto abs = blk.start_idx + loc.instr_index;
                    if (abs >= blk.end_idx || abs >= soa_fn.source_node_ids_.size()) {
                        ++soa_bad;
                        continue;
                    }
                    const auto sn = soa_fn.source_node_ids_[abs];
                    if (sn != 0 && static_cast<aura::ast::NodeId>(sn) != nid)
                        ++soa_bad;
                }
            }
            if (soa_bad > 0) {
                metrics_.source_to_ir_map_soa_desync_total.fetch_add(soa_bad,
                                                                     std::memory_order_relaxed);
                // Repair: full rebuild from AoS (authoritative) so next
                // impact_scope cannot under-invalidate. SoA layout drift
                // is still recorded for observability.
                rebuild_source_to_ir_map_from_irs(entry.irs, entry.source_to_ir_map);
                metrics_.source_to_ir_map_rebuild_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const auto bad = count_source_to_ir_map_inconsistencies(entry.irs, entry.source_to_ir_map);
        if (bad > 0) {
            metrics_.source_to_ir_map_inconsistency_total.fetch_add(bad, std::memory_order_relaxed);
            // Self-heal: full rebuild once more from current irs.
            rebuild_source_to_ir_map_from_irs(entry.irs, entry.source_to_ir_map);
            metrics_.source_to_ir_map_rebuild_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Debug/fuzz consistency assert (always cheap; soft return).
        (void)assert_source_to_ir_map_consistent(entry.irs, entry.source_to_ir_map);
        metrics_.source_to_ir_map_consistent_checks_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Ensure entry has a usable map (lazy rebuild if empty after load).
    // Issue #3068: emptiness-only. Callers that need a *complete*
    // map before an impact snapshot use prepare_source_to_ir_map_for_partial_.
    void ensure_source_to_ir_map_(IRCacheEntry& entry) {
        if (entry.abort_map_invalid)
            return;
        if (!entry.source_to_ir_map.empty() || entry.irs.empty())
            return;
        rebuild_or_patch_source_to_ir_map_(entry, std::nullopt);
    }

    // Issue #3068: fail-closed map prepare for the partial-relower
    // decision. Same critical section as the threshold consult:
    //   1. ensure (empty → rebuild)
    //   2. if desynced → recover_source_to_ir_map_desync (patch, then rebuild)
    //   3. if IR stamps still lack instr locs → rebuild once more
    // Returns false when the map is still unusable → caller MUST
    // upgrade to full (no silent partial). Already-consistent +
    // complete maps pay only the consistency / missing-loc walks
    // (no rebuild — Soft/Off zero extra on clean windows).
    [[nodiscard]] bool prepare_source_to_ir_map_for_partial_(IRCacheEntry& entry) {
        if (entry.irs.empty())
            return true;
        // Issue #3258: never consult a mix of cleared / uncleared maps
        // while the abort force-dirty walk is in progress, or if this
        // entry has not acked the live abort gen.
        if (abort_force_rejects_clean_hit_(entry))
            return false;
        if (entry.abort_map_invalid)
            return false;
        ensure_source_to_ir_map_(entry);
        if (entry.source_to_ir_map.empty())
            return false;
        if (!source_to_ir_map_is_consistent(entry.irs, entry.source_to_ir_map)) {
            const auto bad =
                count_source_to_ir_map_inconsistencies(entry.irs, entry.source_to_ir_map);
            if (bad > 0)
                metrics_.source_to_ir_map_inconsistency_total.fetch_add(bad,
                                                                        std::memory_order_relaxed);
            std::vector<std::size_t> preferred;
            preferred.reserve(entry.block_dirty_per_func_.size());
            for (std::size_t fi = 0; fi < entry.block_dirty_per_func_.size(); ++fi) {
                if (entry.func_dirty_block_count(fi) > 0)
                    preferred.push_back(fi);
            }
            // Issue #3324: production/Strict abort-stale map must not
            // partial-patch pre-abort NodeIds (shallow consistency can
            // hide leftover locs). Soft / never-aborted: force_full false.
            const bool abort_stale_map =
                abort_force_generation_.load(std::memory_order_acquire) != 0 &&
                (aura::compiler::typed_audit::production_defaults_active() ||
                 aura::compiler::typed_audit::get_strategy() ==
                     aura::compiler::typed_audit::AuditStrategy::Full) &&
                (entry.abort_map_invalid ||
                 entry.version_stamp_.abort_force_generation <
                     abort_force_generation_.load(std::memory_order_relaxed));
            auto rec = recover_source_to_ir_map_desync(entry.irs, entry.source_to_ir_map, preferred,
                                                       abort_stale_map);
            if (rec.funcs_patched > 0) {
                metrics_.source_to_ir_desync_funcs_patched.fetch_add(
                    static_cast<std::uint64_t>(rec.funcs_patched), std::memory_order_relaxed);
                metrics_.source_to_ir_map_patch_total.fetch_add(
                    static_cast<std::uint64_t>(rec.funcs_patched), std::memory_order_relaxed);
            }
            if (rec.used_full_rebuild)
                metrics_.source_to_ir_map_rebuild_total.fetch_add(1, std::memory_order_relaxed);
            if (rec.recovered) {
                metrics_.source_to_ir_desync_recovered_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                metrics_.source_to_ir_map_consistent_checks_total.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                return false;
            }
        }
        if (source_to_ir_map_missing_instr_loc(entry.irs, entry.source_to_ir_map)) {
            // Restore from current IR so CastOp / type cone cannot
            // silent-partial on a block-only or dropped loc.
            rebuild_or_patch_source_to_ir_map_(entry, std::nullopt);
            if (source_to_ir_map_missing_instr_loc(entry.irs, entry.source_to_ir_map) ||
                entry.source_to_ir_map.empty())
                return false;
        }
        return source_to_ir_map_is_consistent(entry.irs, entry.source_to_ir_map) &&
               !entry.source_to_ir_map.empty();
    }

    // Issue #2034 / #2139: canonical cascade-exit SoA dirty finish.
    // All production mark_define_dirty / invalidate / body-only / impact
    // cascade paths must call this once — no ad-hoc
    // sync_instruction_dirty_from_block_dirty loops at call sites.
    //   1. force_soa_instruction_dirty_sync → finish_dirty_sync (block→instr)
    //   2. bump soa_dirty_sync_total (flipped bits, min +1 for observability)
    //   3. bump soa_dirty_finish_cascade_total once per finish (#2139)
    //   4. SoA generation fence (#2111)
    // Postcondition: entry.soa_mod.instruction_dirty_synced_with_blocks().
    void finish_cascade_soa_dirty_sync_(IRCacheEntry& entry) {
        const auto flipped = entry.force_soa_instruction_dirty_sync();
        const auto n = flipped > 0 ? static_cast<std::uint64_t>(flipped) : 1u;
        metrics_.soa_dirty_sync_total.fetch_add(n, std::memory_order_relaxed);
        // Issue #2139: once-per-finish counter (distinct from flipped bits).
        metrics_.soa_dirty_finish_cascade_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.soa_dirty_finish_wired.store(1, std::memory_order_relaxed);
        entry.bump_soa_generation();
        metrics_.soa_generation_bump_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #2183: unified restamp for IR cache entries — every successful
    // store / true per-fn AST relower / cascade store of stored content
    // calls this so CacheEntryVersionStamp matches live mutation/bridge/
    // defuse/soa. Issue #3481: AOT reemit / instr peel must NOT call this
    // while dirty || abort_map_invalid || !content_stored_this_epoch.
    // Issue #4341: the mutation stamp is the epoch the stored IR was
    // SAMPLED under (#4338 semantics), not the epoch observed at store —
    // a Workspace-rank EDSL mutate that lands between sample and store
    // must leave the stamp behind the live epoch, so the next
    // lookup_define_v2 / should_relower reads needs-relower (fail-closed)
    // instead of reviving pre-mutate IR as a clean hit. Bridge / defuse /
    // soa keep their live reads (own invalidation axes). Cascade-reemit
    // restamps re-assert the entry's already-stamped mutation_count
    // (re-assert, never advance) so a concurrent bump survives them too.
    void restamp_cache_entry_live_(IRCacheEntry& entry, std::uint64_t sampled_mut,
                                   std::string_view stored_name = {}) {
        const auto mut = sampled_mut;
        const auto bridge = bridge_epoch();
        const auto defuse = evaluator_.defuse_version();
        const auto soa = entry.live_soa_generation();
        entry.stamp_version(mut, bridge, defuse, soa);
        // Issue #3069: success-path restamp acks the live abort-force gen
        // so a post-abort store is a clean hit (not stuck behind the fence).
        entry.version_stamp_.abort_force_generation =
            abort_force_generation_.load(std::memory_order_acquire);
        metrics_.cache_entry_version_stamp_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.cache_stamp_restamp_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #4377: the boundary exit bumps defuse after this stamp.
        if (!stored_name.empty() && evaluator_.any_active_mutation_boundary())
            note_ir_cache_stored_inside_boundary(stored_name);
    }

    // Issue #4377: outermost boundary success. The exit bump is the legacy
    // second tick of the same boundary, not a later edit of the stored IR.
    // Write that defuse. Leave the sampled mutation epoch (#4341), bridge,
    // and soa as the store wrote them.
    void restamp_defines_after_boundary_exit_() noexcept {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        const auto live_defuse = evaluator_.defuse_version();
        if (live_defuse == 0)
            return;
        for (const auto& note : ir_cache_boundary_stored_names()) {
            auto it = ir_cache_v2_.find(note);
            if (it == ir_cache_v2_.end())
                continue;
            auto& entry = it->second;
            if (entry.dirty || entry.abort_map_invalid || !entry.content_stored_this_epoch)
                continue;
            if (entry.version_stamp_.defuse_version == 0 ||
                entry.version_stamp_.defuse_version == live_defuse)
                continue;
            entry.version_stamp_.defuse_version = live_defuse;
        }
    }

    static void restamp_boundary_stored_trampoline(void* ctx) noexcept {
        static_cast<CompilerService*>(ctx)->restamp_defines_after_boundary_exit_();
    }

    // Issue #3481: ack peer IR stale + abort-force gen without writing
    // mutation/bridge/defuse/soa to live (content still pre-mutate).
    void ack_cache_entry_fences_live_(IRCacheEntry& entry, const std::string& name) noexcept {
        ack_peer_ir_stale_on_restamp_(entry, name);
        entry.version_stamp_.abort_force_generation =
            abort_force_generation_.load(std::memory_order_acquire);
    }

    // Issue #3481: split ack from content restamp. Returns true iff
    // mutation/bridge/defuse/soa were restamped to live.
    bool maybe_restamp_cache_entry_content_live_(IRCacheEntry& entry, const std::string& name,
                                                 bool content_stored_this_epoch,
                                                 std::uint64_t sampled_mut) {
        // Issue #3481: ack always; content restamp only when irs is stored.
        // Issue #4341: sampled mutation epoch threaded through.
        ack_cache_entry_fences_live_(entry, name);
        if (entry.dirty || entry.abort_map_invalid || !content_stored_this_epoch)
            return false;
        restamp_cache_entry_live_(entry, sampled_mut, name);
        return true;
    }

    // Issue #3351: ack process-wide peer IR name gen on local restamp so
    // this eval can clean-hit again without clearing other peers.
    void ack_peer_ir_stale_on_restamp_(IRCacheEntry& entry, const std::string& name) noexcept {
        entry.peer_ir_stale_ack_ = aura_aot_peer_ir_name_stale_gen(name.c_str());
    }

    // Issue #3258: gen==0 never-aborted lookup is one acquire. After the
    // first abort, in_progress OR abort_force_generation lag rejects a
    // clean hit (do not serve pre-abort IR / map).
    [[nodiscard]] bool abort_force_rejects_clean_hit_(const IRCacheEntry& entry) const noexcept {
        return abort_force_rejects_clean_hit_(
            entry, abort_force_generation_.load(std::memory_order_acquire));
    }
    [[nodiscard]] bool abort_force_rejects_clean_hit_(const IRCacheEntry& entry,
                                                      std::uint64_t gen) const noexcept {
        if (gen == 0)
            return false;
        if (abort_force_in_progress_.load(std::memory_order_acquire) != 0)
            return true;
        return entry.version_stamp_.abort_force_generation < gen;
    }

    // Issue #3117 / Issue #3258: publish abort-force fence *before* dual-topology
    // restore. Bump generation first so a concurrent lookup that observes
    // gen!=0 also applies the lag check even if in_progress is not yet
    // visible (release/acquire). Then set in_progress for the walk.
    void begin_abort_ir_cache_force_fence() {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        // Issue #3258: bump gen first so lag check is visible.
        abort_force_generation_.fetch_add(1, std::memory_order_release);
        abort_force_in_progress_.store(1, std::memory_order_release);
        // Issue #4256: fence dep_graph_generation_ at abort-begin so
        // concurrent record_dependency during dual-topology restore
        // stale-rejects (force_dirty also bumps after restore).
        {
            lock_order::OrderedUniqueLock<std::shared_mutex> dep_write(dep_graph_mtx_,
                                                                       lock_order::Level::DepGraph);
            dep_graph_generation_.fetch_add(1, std::memory_order_release);
            metrics_.dep_graph_generation_total.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock<std::mutex> cascade_guard(cascade_decision_mtx_);
            deferred_hybrid_armed_.store(1, std::memory_order_release);
            deferred_hybrid_gen_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Issue #3551: drop pre-abort irs + source_to_ir_map for one define
    // so lookup cannot serve restored-hash / pre-abort IR. Soft/Off:
    // observe only. Production/Full: hard drop. abort_define_set is the
    // ir_cache_v2_ key set walked by force_ir_cache_dirty_after_abort.
    void clear_cache_v2_for_define(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (!(aura::compiler::typed_audit::production_defaults_active() ||
              aura::compiler::typed_audit::get_strategy() ==
                  aura::compiler::typed_audit::AuditStrategy::Full)) {
            g_phase5_abort_cache_clear_observe_total.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return;
        it->second.irs.clear();
        it->second.invalidate_irs_layout_(); // Issue #4615
        it->second.source_to_ir_map.clear();
        g_phase5_abort_cache_clear_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #3033 / #3069 / #3117 / #3324: dual-topology abort force-dirty.
    // abort_restore_dual_topology restores AST topology but IR cache
    // stamps / source_to_ir_map can still point at pre-abort IR.
    // Force-dirty + zero-restamp every cached entry, then *clear*
    // (do not rebuild from) source_to_ir_map so ImpactScope cannot
    // under-invalidate on aborted NodeIds. Abort path only.
    // Issue #3551: also drop entry.irs (clear_cache_v2_for_define).
    // Issue #3852: production also force-bumps AOT table epoch so probe
    // cannot return mid-abort native (Soft: AOT untouched).
    void force_ir_cache_dirty_after_abort();

    // Issue #3985: Phase-5 Moving densify success relocates FlatAST NodeIds
    // after store_define_v2. source_to_ir_map / content_stored_this_epoch
    // stay pre-densify unless we refuse clean-hit and partial peel.
    // Reuse abort_map_invalid so prepare_source_to_ir_map_for_partial_
    // fail-closes to full. Do not drop irs / do not bump abort fence /
    // do not bump AOT table (abort densify still uses
    // force_ir_cache_dirty_after_abort). Soft/Off: one production/Full
    // load, no map walk. Distinguisher: should_relower_total.
    static constexpr int kDensifyIrCacheMapInvalidIssue = 3985;
    void force_ir_cache_map_invalid_after_densify() {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (!(aura::compiler::typed_audit::production_defaults_active() ||
              aura::compiler::typed_audit::get_strategy() ==
                  aura::compiler::typed_audit::AuditStrategy::Full))
            return;
        std::size_t n = 0;
        for (auto& [name, entry] : ir_cache_v2_) {
            if (!entry.content_stored_this_epoch)
                continue;
            entry.source_to_ir_map.clear();
            entry.abort_map_invalid = true;
            entry.content_stored_this_epoch = false;
            entry.dirty = true;
            entry.mark_all_blocks_dirty();
            finish_cascade_soa_dirty_sync_(entry);
            ++n;
        }
        if (n > 0)
            metrics_.should_relower_total.fetch_add(static_cast<std::uint64_t>(n),
                                                    std::memory_order_relaxed);
        // Issue #4155: densify relocated FlatAST NodeIds — the residual
        // CastOp persist TLS still holds pre-densify Ids that a later
        // store_define_v2 map rebuild can alias to the WRONG block
        // (mark_entry_from_dead_coercion_persist_ false-attribute; #3618
        // fail-closes on attribution failure only). Drop the stale persist
        // + bump the decision-invalidate gen on the same production/Full
        // face. Soft / Off returned above (zero cost). The #4154 remirror
        // latch is untouched: empty persist → remirror returns 0 early and
        // the next production sweep re-notes the live sites.
        aura::compiler::dirty::reset_residual_castop_persist_after_densify();
    }

    // Issue #2181: hard-require SoA block↔instr dirty sync before any
    // partial peel (relower_only_dirty_blocks / instr-level / DirtyAware entry
    // via this path). Returns true if partial is safe (desync==0 after sync
    // and no pre-existing desync). Returns false → caller MUST take full
    // re-lower for this define (no under-invalidate).
    //
    //   1. count desync; if >0 bump soa_dirty_desync_detected_total
    //   2. force_soa_instruction_dirty_sync (AoS→SoA + finish_dirty_sync)
    //   3. if still desync OR had pre-sync desync → force_full metric, false
    //   4. else true (happy path already synced: O(dirty) scan only)
    [[nodiscard]] bool gate_partial_soa_dirty_sync_(IRCacheEntry& entry) {
        metrics_.soa_dirty_desync_gate_wired.store(1, std::memory_order_relaxed);
        const auto desync0 = entry.soa_mod.count_block_instr_dirty_desync();
        if (desync0 > 0) {
            metrics_.soa_dirty_desync_detected_total.fetch_add(static_cast<std::uint64_t>(desync0),
                                                               std::memory_order_relaxed);
        }
        // Always sync at partial entry (AC1) — O(dirty blocks).
        const auto flipped = entry.force_soa_instruction_dirty_sync();
        if (flipped > 0) {
            metrics_.soa_dirty_desync_synced_bits_total.fetch_add(
                static_cast<std::uint64_t>(flipped), std::memory_order_relaxed);
            metrics_.soa_dirty_sync_total.fetch_add(static_cast<std::uint64_t>(flipped),
                                                    std::memory_order_relaxed);
        } else if (desync0 > 0) {
            // Sync was a no-op but desync was reported — still account.
            metrics_.soa_dirty_sync_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (desync0 > 0) {
            metrics_.soa_dirty_finish_cascade_total.fetch_add(1, std::memory_order_relaxed);
            entry.bump_soa_generation();
            metrics_.soa_generation_bump_total.fetch_add(1, std::memory_order_relaxed);
        }
        const auto desync1 = entry.soa_mod.count_block_instr_dirty_desync();
        // Residual desync after sync → hard force full.
        // Pre-sync desync → also force full (AC4: no partial peel under
        // uncertain bookkeeping; sync still ran so bits are repaired for
        // the subsequent full path).
        if (desync1 > 0 || desync0 > 0) {
            metrics_.soa_dirty_desync_force_full_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #2193: last-reason + per-reason counter for Agents.
            note_relower_fallback(metrics_, RelowerFallbackReason::DesyncForceFull);
            return false;
        }
        // AC2: successful partial gate — desync must be 0.
        contract_assert(entry.soa_mod.instruction_dirty_synced_with_blocks());
        return true;
    }

    // Issue #2127: consult deopt-storm + dirty density + base thr (#2112)
    // for partial/full decision. Records effective thr on CompilerMetrics.
    // Issue #2212: Shape bit widens partial window (prefer partial under
    // shape churn) — separate from deopt thr↓. Global soft throttle still
    // feeds #2127 thr adaptation; shape_storm no longer lowers thr.
    // Issue #2190: after Shape preference, apply StormLevel Global gate
    // (prefer full under Global/Both; Shape-only does not force full).
    // Issue #4156: the #2248 AdaptiveThrPolicy risk clamp (inverted
    // polarity — MapInconsistent / DesyncForceFull narrow partial) is
    // applied inside decide_workload_adaptive_partial_relower, so this
    // consult and every peel entry share the wired threshold.
    AdaptiveRelowerDecision
    consult_workload_adaptive_partial_(std::size_t dirty_n, std::size_t total_blocks = 0) noexcept {
        auto& reg = hot_update_registry();
        // #2127 thr adaptation: Global soft throttle only (not Shape —
        // Shape prefers partial via #2212 widen, not thr↓).
        const bool deopt_storm = reg.should_throttle_reemit();
        auto d = decide_workload_adaptive_partial_relower(dirty_n, total_blocks,
                                                          reg.deopt_window_count(),
                                                          reg.deopt_storm_threshold(), deopt_storm);
        // #2212: Shape bit → widen thr / force partial for dirty < 2× thr.
        apply_shape_storm_partial_preference(d, dirty_n);
        // Issue #3986: Shape preference flipped adaptive-full → partial.
        // Empty DeadCoercion persist is unknown cone (same unit as #3310
        // impact_ub==0). Production fail-closed full; Soft/Off keep Shape
        // widen (zero extra persist consult when not production).
        if (d.shape_flipped_full_to_partial) {
            const bool production = aura::compiler::typed_audit::production_hard_face_active();
            if (production) {
                // Issue #3986: persist empty = unknown cone. Issue #4157:
                // nonempty alone is not proof — persist noted before the
                // latest densify/store holds NodeIds the rebuilt
                // source_to_ir_map can mis-attribute (the #4155
                // false-positive shape: #3618 fail-closes an attribution
                // FAILURE, not a stale-alias SUCCESS). Fail-closed full
                // unless the persist is FRESH (write epoch == content
                // epoch — noted after the latest densify/store); the
                // peel's #3618 same-define attribution stays the final
                // proof. Soft/Off keep Shape widen (zero persist consult).
                if (aura::compiler::dirty::residual_castop_persist_size() == 0 ||
                    !aura::compiler::dirty::residual_castop_persist_fresh()) {
                    d.want_partial = false;
                    d.reason_bits |= kAdaptiveReasonFull;
                    d.reason_bits &= ~kAdaptiveReasonPartial;
                } else {
                    d.shape_flipped_full_to_partial = false;
                }
            } else {
                d.shape_flipped_full_to_partial = false;
            }
        }
        // #2190: Global bit of unified StormLevel → force full (additional
        // gate; set_partial_relower_threshold still controls base thr).
        d.want_partial = apply_partial_relower_storm_gate(d.want_partial);
        metrics_.partial_relower_threshold_used.store(d.effective_threshold,
                                                      std::memory_order_relaxed);
        return d;
    }

    // Issue #2126: prefer compute_impact_scope instr/block dirty under
    // get_partial_relower_threshold() instead of mark_all_blocks_dirty.
    // Returns true when impact dirty was applied (caller skips full mark).
    // Free-var nested targeting remains a separate backup after this.
    // Issue #4598: rebind resolved the define root with a full-workspace
    // DFS (find_define_by_name) on every impact consult — O(W) per
    // rebind. Cache name -> NodeId per FlatAST; a hit is trusted only if
    // it is still a live Define of that symbol whose parent chain reaches
    // flat.root with child membership at each hop. Otherwise fall back
    // to the DFS and refill.
    std::optional<aura::ast::NodeId> cached_define_root_(const aura::ast::FlatAST& flat,
                                                         const aura::ast::StringPool& pool,
                                                         const std::string& name) {
        if (define_root_cache_owner_ != &flat) {
            define_root_cache_.clear();
            define_root_cache_owner_ = &flat;
        }
        if (auto it = define_root_cache_.find(name); it != define_root_cache_.end()) {
            const auto id = it->second;
            const auto sym = pool.find_by_name(name);
            bool ok = sym && id < flat.size() && !flat.is_free_slot(id) &&
                      flat.tag(id) == aura::ast::NodeTag::Define && flat.sym_id(id) == *sym;
            if (ok) {
                auto cur = id;
                ok = false;
                for (int hop = 0; hop < 64; ++hop) {
                    if (cur == flat.root) {
                        ok = true;
                        break;
                    }
                    const auto par = flat.parent_of(cur);
                    if (par == aura::ast::NULL_NODE || par == cur || par >= flat.size() ||
                        flat.is_free_slot(par))
                        break;
                    // #4682: borrowed span; get(par) copied root's full
                    // child list (O(W)) on every hop.
                    if (!define_root_is_child_(flat, par, cur))
                        break;
                    cur = par;
                }
            }
            if (ok) {
                metrics_define_root_cache_hits_.fetch_add(1, std::memory_order_relaxed);
                return id;
            }
            define_root_cache_.erase(it);
        }
        // Issue #4682: a rebind replaces the Define node, so the first
        // consult after every rebind missed and paid a full-workspace DFS
        // (find_first_node_with, ~18% of eval-current at W=1000/D=1).
        // Resolve through the maintained Define tag index instead: if
        // exactly one live Define of this symbol is reachable from root it
        // is what the DFS would return. 0 or >1 candidates fall back to the
        // DFS (pre-order tie-break).
        {
            if (const auto sym = pool.find_by_name(name)) {
                aura::ast::NodeId only = aura::ast::NULL_NODE;
                int n = 0;
                for (auto id :
                     flat.collect_by_tag(static_cast<std::uint32_t>(aura::ast::NodeTag::Define))) {
                    if (id >= flat.size() || flat.is_free_slot(id) ||
                        flat.tag(id) != aura::ast::NodeTag::Define || flat.sym_id(id) != *sym)
                        continue;
                    if (!define_root_reachable_(flat, id))
                        continue;
                    if (++n > 1)
                        break;
                    only = id;
                }
                if (n == 1) {
                    metrics_define_root_tag_index_hits_.fetch_add(1, std::memory_order_relaxed);
                    define_root_cache_[name] = only;
                    return only;
                }
            }
        }
        auto found = flat.find_define_by_name(pool, name);
        if (found)
            define_root_cache_[name] = *found;
        return found;
    }
    static bool define_root_is_child_(const aura::ast::FlatAST& flat, aura::ast::NodeId par,
                                      aura::ast::NodeId cur) {
        // Issue #4930: the maintained parent link answers the direct-child
        // case in O(1). Without it every consult of a root-level define
        // walked flat.root's whole child list (O(W)) on the cache-hit and
        // tag-index paths alike; the list scan stays as a torn-state
        // fallback for a parent link that disagrees with the child list.
        if (flat.parent_of(cur) == par) {
            g_define_root_parent_link_hits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_define_root_child_list_scans.fetch_add(1, std::memory_order_relaxed);
        for (auto c : flat.children(par))
            if (c == cur)
                return true;
        return false;
    }
    static bool define_root_reachable_(const aura::ast::FlatAST& flat, aura::ast::NodeId id) {
        auto cur = id;
        for (int hop = 0; hop < 64; ++hop) {
            if (cur == flat.root)
                return true;
            const auto par = flat.parent_of(cur);
            if (par == aura::ast::NULL_NODE || par == cur || par >= flat.size() ||
                flat.is_free_slot(par))
                return false;
            if (!define_root_is_child_(flat, par, cur))
                return false;
            cur = par;
        }
        return false;
    }
    std::atomic<std::uint64_t> metrics_define_root_tag_index_hits_{0};
    std::uint64_t linear_window_scans_skipped_4683_ = 0;
    std::unordered_map<std::string, aura::ast::NodeId> define_root_cache_;
    const aura::ast::FlatAST* define_root_cache_owner_ = nullptr;
    std::atomic<std::uint64_t> metrics_define_root_cache_hits_{0};

    bool try_apply_impact_minimal_dirty_(IRCacheEntry& entry, const std::string& name,
                                         aura::ast::NodeId root_hint = aura::ast::NULL_NODE) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto* flat = evaluator_.workspace_flat();
        auto* pool = evaluator_.workspace_pool();
        if (!flat || !pool)
            return false;
        aura::ast::NodeId root = root_hint;
        if (root == aura::ast::NULL_NODE || root >= flat->size()) {
            auto found = cached_define_root_(*flat, *pool, name);
            if (!found)
                return false;
            root = *found;
        }
        ensure_source_to_ir_map_(entry);
        if (entry.source_to_ir_map.empty()) {
            metrics_.instr_level_impact_prefer_fallback_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            return false;
        }
        std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                           std::equal_to<>>
            ir_cache_index;
        for (std::size_t fi = 0; fi < entry.irs.size(); ++fi)
            ir_cache_index[entry.irs[fi].name] = fi;
        auto scope = compute_impact_scope(*flat, root, entry.source_to_ir_map, ir_cache_index);
        const auto thr = get_partial_relower_threshold();
        const bool instr_ok = !scope.affected_instrs.empty() && scope.affected_instrs.size() < thr;
        const bool block_ok = !scope.affected_blocks.empty() && scope.affected_blocks.size() < thr;
        if (!instr_ok && !block_ok) {
            metrics_.instr_level_impact_prefer_fallback_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            if (scope.unmapped_ast_nodes > 0)
                metrics_.instr_level_impact_misses_total.fetch_add(scope.unmapped_ast_nodes,
                                                                   std::memory_order_relaxed);
            return false;
        }
        // Prefer instr-granularity when present; else block-only impact.
        // apply_impact_scope_dirty already handles precise vs block cascade.
        // Issue #2133: when instr-eligible, record unmapped ratio for Agents.
        if (scope.has_instr_precision())
            metrics_.instr_level_unmapped_ratio_bp.store(scope.unmapped_ratio_bp(),
                                                         std::memory_order_relaxed);
        entry.dirty = true;
        (void)apply_impact_scope_dirty(entry, scope);
        metrics_.instr_level_impact_prefer_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.incremental_impact_blocks_hit_total.fetch_add(1, std::memory_order_relaxed);
        evaluator_.bump_impact_scope_calls(
            scope.affected_blocks.empty() ? 1u : scope.affected_blocks.size());
        // Credit nested/__top__ kept clean when impact did not touch them.
        const auto dirty_fns = entry.dirty_func_count();
        const auto total_fns = entry.irs.size();
        if (total_fns > dirty_fns) {
            metrics_.minimal_recompile_clean_funcs_saved.fetch_add(total_fns - dirty_fns,
                                                                   std::memory_order_relaxed);
            metrics_.minimal_recompile_scope_samples.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    // Issue #3349: after remirror, mark this entry's IR blocks that
    // DeadCoercion persist AST / encoded blocks map to. Soft / empty
    // persist: caller skips (size 0). Returns true when at least one
    // block was attributed (CastOp site now dirty — AC mark path).
    bool mark_entry_from_dead_coercion_persist_(IRCacheEntry& entry) {
        // Collect (func, block) then one mark_blocks_dirty per function
        // (#2615 / #2774 — no N× mark_block_dirty fence loop).
        std::unordered_map<std::size_t, std::vector<std::uint32_t>> per_fn;
        auto note = [&](std::size_t fi, std::uint32_t bi) {
            if (fi >= entry.irs.size())
                return;
            const auto nblocks = fi < entry.block_dirty_per_func_.size()
                                     ? entry.block_dirty_per_func_[fi].size()
                                     : entry.irs[fi].blocks.size();
            if (bi >= nblocks)
                return;
            auto& v = per_fn[fi];
            for (auto x : v) {
                if (x == bi)
                    return;
            }
            v.push_back(bi);
        };
        for (auto ast : aura::compiler::dirty::residual_castop_persist_ast()) {
            auto mit = entry.source_to_ir_map.find(ast);
            if (mit == entry.source_to_ir_map.end())
                continue;
            note(mit->second.function_index, mit->second.block_index);
        }
        for (auto enc : aura::compiler::dirty::residual_castop_persist_blocks()) {
            if (enc == 0 || aura::compiler::dirty::is_fn_node(enc) ||
                aura::compiler::dirty::is_ast_dep_node(enc))
                continue;
            std::size_t fi = 0;
            std::uint32_t bi = 0;
            if (aura::compiler::dirty::is_block_dep_node(enc)) {
                const auto d = aura::compiler::dirty::decode_block_dep_node(enc);
                fi = d.func_idx;
                bi = d.block_idx;
            } else {
                const auto [f, b] = aura::compiler::dirty::decode_block_node(enc);
                fi = f;
                bi = b;
            }
            note(fi, bi);
        }
        if (per_fn.empty())
            return false;
        for (auto& [fi, blocks] : per_fn)
            entry.mark_blocks_dirty(fi, blocks);
        return true;
    }

    // Issue #3034: upper bound for the partial-relower decision from
    // ImpactScope / hybrid cascade. Reuses the workspace flat +
    // source_to_ir_map + compute_impact_scope walk (same shape as
    // try_apply_impact_minimal_dirty_). Returns 0 when impact cannot be
    // computed (no flat / no root / empty source map) so Soft/Off stays
    // zero-cost on clean or empty-impact windows. The returned bound
    // counts affected blocks plus cross-fn indirect / unresolved callee
    // hits — signals that the local block_dirty mask may under-count.
    std::size_t impact_upper_bound_for_entry_(const std::string& name, IRCacheEntry& entry) {
        auto* flat = evaluator_.workspace_flat();
        auto* pool = evaluator_.workspace_pool();
        if (!flat || !pool)
            return 0;
        aura::ast::NodeId root = aura::ast::NULL_NODE;
        {
            auto found = cached_define_root_(*flat, *pool, name);
            if (!found)
                return 0;
            root = *found;
        }
        ensure_source_to_ir_map_(entry);
        // Issue #3068: empty / desynced / missing-instr-loc map is
        // unknown impact, not empty impact. Sentinel forces
        // should_partial_relower_impact_checked to upgrade to full.
        // no-flat / no-root above still return 0 (Soft zero-cost).
        if (entry.source_to_ir_map.empty() ||
            !source_to_ir_map_is_consistent(entry.irs, entry.source_to_ir_map) ||
            source_to_ir_map_missing_instr_loc(entry.irs, entry.source_to_ir_map))
            return static_cast<std::size_t>(-1);
        std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                           std::equal_to<>>
            ir_cache_index;
        for (std::size_t fi = 0; fi < entry.irs.size(); ++fi)
            ir_cache_index[entry.irs[fi].name] = fi;
        auto scope = compute_impact_scope(*flat, root, entry.source_to_ir_map, ir_cache_index);
        // Upper bound = affected blocks NOT yet covered by the local dirty
        // mask (under-estimate signal) + cross-fn indirect / unresolved
        // callee hits (dual-DepGraph edges not reflected in the mask).
        // Blocks already dirty are not under-estimates; __top__ synthetic
        // blocks with no AST mapping are excluded automatically (not in
        // affected_blocks).
        std::size_t uncovered = 0;
        for (const auto& br : scope.affected_blocks) {
            const auto fi = br.function_index;
            const auto bi = br.block_index;
            if (fi < entry.block_dirty_per_func_.size() &&
                bi < entry.block_dirty_per_func_[fi].size() &&
                entry.block_dirty_per_func_[fi][bi] != 0)
                continue; // already dirty — no under-estimate here
            ++uncovered;
        }
        return uncovered + scope.cross_fn_indirect_hits + scope.unresolved_callee_hits +
               deferred_hybrid_pending_upper_bound_(name);
    }
    // Issue #3097: production-only bounded upper-bound contribution from
    // pending deferred hybrid edges that target this define (caller ==
    // name OR callee == name). Closes the dual-DepGraph race lag where
    // NodeId edges lag the string graph under concurrent mutate / fiber
    // / lockless batch — record_dependency rejects and queues, drain
    // happens at cascade end, but the partial consult can observe a
    // stale impact_upper_bound_for_entry_ between reject and drain.
    // Soft / Off: zero-cost early exit on armed == 0 (cheap load); when
    // armed but Soft / Off, also early-exit so Soft stays zero-cost.
    std::size_t deferred_hybrid_pending_upper_bound_(const std::string& name) const noexcept {
        if (deferred_hybrid_armed_.load(std::memory_order_acquire) == 0)
            return 0;
        if (!aura::compiler::typed_audit::production_defaults_active())
            return 0;
        // Issue #3067: deferred_hybrid_edges_ is mutated under
        // dep_graph_mtx_ write (in record_dependency) — read under shared.
        // Cheap walk: O(pending) string compares. Pending edges are bounded
        // by the reject window, not the workspace size.
        lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                              lock_order::Level::DepGraph);
        std::size_t targeted = 0;
        for (const auto& [caller_name, callee_name] : deferred_hybrid_edges_) {
            if (caller_name == name || callee_name == name)
                ++targeted;
        }
        return targeted;
    }

    // Issue #2133: build [func][block] instruction counts for DefineDirtyMaskView.
    static std::vector<std::vector<std::uint32_t>>
    build_block_instr_counts_(const IRCacheEntry& entry) {
        std::vector<std::vector<std::uint32_t>> counts;
        counts.resize(entry.irs.size());
        for (std::size_t fi = 0; fi < entry.irs.size(); ++fi) {
            counts[fi].resize(entry.irs[fi].blocks.size());
            for (std::size_t bi = 0; bi < entry.irs[fi].blocks.size(); ++bi)
                counts[fi][bi] =
                    static_cast<std::uint32_t>(entry.irs[fi].blocks[bi].instructions.size());
        }
        return counts;
    }

    // Issue #2133: when ImpactScope has precise affected_instrs under the
    // partial threshold, run DirtyAware passes on those instructions only
    // (no full-function re-lower from AST). Keeps clean instrs in dirty
    // blocks. Returns true when instr-level path fully handled the entry.
    bool relower_affected_instrs_(const std::string& name, IRCacheEntry& entry,
                                  const ImpactScope& scope) {
        const auto thr = get_partial_relower_threshold();
        if (!scope.instr_level_eligible(thr))
            return false;
        metrics_.instr_level_relower_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.instr_level_unmapped_ratio_bp.store(scope.unmapped_ratio_bp(),
                                                     std::memory_order_relaxed);
        // Stamp precise dirty (idempotent if already applied).
        (void)apply_impact_scope_dirty(entry, scope);

        // Count clean instrs inside dirty blocks (saved work).
        std::size_t clean_in_dirty = 0;
        std::size_t dirty_instrs = 0;
        for (std::size_t fi = 0; fi < entry.irs.size(); ++fi) {
            for (std::size_t bi = 0; bi < entry.irs[fi].blocks.size(); ++bi) {
                const bool bd = fi < entry.block_dirty_per_func_.size() &&
                                bi < entry.block_dirty_per_func_[fi].size() &&
                                entry.block_dirty_per_func_[fi][bi] != 0;
                if (!bd)
                    continue;
                const auto n = entry.irs[fi].blocks[bi].instructions.size();
                for (std::uint32_t ii = 0; ii < n; ++ii) {
                    if (entry.is_instruction_dirty(fi, static_cast<std::uint32_t>(bi), ii))
                        ++dirty_instrs;
                    else
                        ++clean_in_dirty;
                }
            }
        }
        if (clean_in_dirty > 0) {
            metrics_.relower_partial_insts_saved_total.fetch_add(clean_in_dirty,
                                                                 std::memory_order_relaxed);
            metrics_.relower_instruction_skip_total.fetch_add(clean_in_dirty,
                                                              std::memory_order_relaxed);
            metrics_.instr_level_pass_skipped_clean.fetch_add(clean_in_dirty,
                                                              std::memory_order_relaxed);
        }

        // Run DirtyAware suite with instr-precision mask (mutates entry.irs).
        // Issue #2907/#3822: under prod_soa, SoA hot pack + dirty-block sync
        // into AoS writeback SSOT (entry.irs); Soft keeps AoS-only suite.
        {
            aura::ir::IRModule mod;
            mod.functions = entry.irs; // copy for pass mutability then write back
            DefineDirtyMaskView define_mask;
            define_mask.block_dirty_per_func = &entry.block_dirty_per_func_;
            define_mask.instruction_dirty_per_func = &entry.instruction_dirty_per_func_;
            auto counts = build_block_instr_counts_(entry);
            define_mask.block_instr_counts = &counts;
            const DefineDirtyMaskView* mask_ptr = &define_mask;
            const auto skipped = run_incremental_dirty_pass_suite_(mod, mask_ptr, &entry.soa_mod);
            (void)skipped;
            metrics_.instr_level_pass_runs_total.fetch_add(1, std::memory_order_relaxed);
            // Mirror process atomics into CompilerMetrics.
            metrics_.instr_level_pass_skipped_clean.fetch_add(
                aura::compiler::instr_level_pass_skipped_clean_total.exchange(
                    0, std::memory_order_relaxed),
                std::memory_order_relaxed);
            entry.irs = std::move(mod.functions);
            entry.invalidate_irs_layout_(); // Issue #4615: pass writeback may resize blocks.
        }

        // Clear dirty bits after instr-level pass work.
        entry.clear_all_instruction_dirty();
        for (auto& fb : entry.block_dirty_per_func_)
            for (auto& b : fb)
                b = 0;
        entry.dirty = false;
        metrics_.relower_instruction_level_hits.fetch_add(1, std::memory_order_relaxed);
        evaluator_.bump_relower_instruction_level_hit();
        if (dirty_instrs > 0)
            metrics_.relower_block_count.fetch_add(scope.affected_blocks.size(),
                                                   std::memory_order_relaxed);
        (void)name;
        return true;
    }

    // Issue #2031 / #2109: stamp block + instruction dirty bits from
    // ImpactScope. When precise affected_instrs are present, mark block
    // dirty *without* cascading all instructions so clean slots stay
    // skippable on partial re-emit (AC2). Block-only scopes keep cascade.
    // Issue #2522: non-precise multi-block path batches per function
    // (one generation bump per function, not per block).
    // Returns number of instruction dirty marks applied.
    std::size_t apply_impact_scope_dirty(IRCacheEntry& entry, const ImpactScope& scope) {
        const bool precise = !scope.affected_instrs.empty();
        if (precise) {
            // Issue #2615: batch bit-only per function (one fence each), not
            // N× mark_block_dirty_bit_only → N× generation fence advances.
            std::unordered_map<std::size_t, std::vector<std::uint32_t>> by_fn;
            for (const auto& b : scope.affected_blocks)
                by_fn[b.function_index].push_back(b.block_index);
            for (auto& [fi, ids] : by_fn)
                entry.mark_blocks_dirty_bit_only(fi, ids);
        } else if (!scope.affected_blocks.empty()) {
            // Group block ids by function for mark_blocks_dirty batch.
            // Issue #2522 / #2615: one generation bump per function.
            std::unordered_map<std::size_t, std::vector<std::uint32_t>> by_fn;
            for (const auto& b : scope.affected_blocks)
                by_fn[b.function_index].push_back(b.block_index);
            for (auto& [fi, ids] : by_fn)
                entry.mark_blocks_dirty(fi, ids);
        }
        std::size_t marks = 0;
        for (const auto& ir : scope.affected_instrs) {
            entry.mark_instruction_dirty(ir.function_index, ir.block_index, ir.instr_index);
            // Dual-emit SoA column (absolute instruction index).
            if (ir.function_index < entry.soa_mod.functions.size() &&
                ir.function_index < entry.irs.size()) {
                const auto& fn = entry.irs[ir.function_index];
                std::uint32_t abs = 0;
                for (std::uint32_t bi = 0; bi < ir.block_index && bi < fn.blocks.size(); ++bi)
                    abs += static_cast<std::uint32_t>(fn.blocks[bi].instructions.size());
                abs += ir.instr_index;
                entry.soa_mod.functions[ir.function_index].mark_instruction_dirty(abs);
            }
            ++marks;
        }
        // Issue #2034 / #2109: block-only scopes cascade-sync SoA (full
        // block→instr). Precise instr scopes already dual-emitted dirty
        // slots above — do NOT sync_instruction_dirty_from_block_dirty
        // (that would re-dirty every clean instruction in the block).
        if (precise)
            metrics_.soa_dirty_sync_total.fetch_add(1, std::memory_order_relaxed);
        else
            finish_cascade_soa_dirty_sync_(entry);
        if (scope.instr_level_hits > 0)
            metrics_.instr_level_impact_hits_total.fetch_add(scope.instr_level_hits,
                                                             std::memory_order_relaxed);
        if (scope.unmapped_ast_nodes > 0)
            metrics_.instr_level_impact_misses_total.fetch_add(scope.unmapped_ast_nodes,
                                                               std::memory_order_relaxed);
        if (marks > 0) {
            metrics_.instr_level_dirty_marks_total.fetch_add(marks, std::memory_order_relaxed);
            metrics_.relower_instruction_level_hits.fetch_add(1, std::memory_order_relaxed);
            evaluator_.bump_relower_instruction_level_hit();
        }
        return marks;
    }

    // Issue #680 / #2031: compute ir_cache_pure impact_scope for a Define
    // mutation root, stamp instruction-level dirty, bump observability.
    void run_define_impact_scope(aura::ast::NodeId root) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto* flat = evaluator_.workspace_flat();
        if (!flat || root == aura::ast::NULL_NODE || root >= flat->size())
            return;
        SourceToIrMap source_to_ir;
        std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                           std::equal_to<>>
            ir_cache_index;
        // Prefer IR cache reverse index when define name is known.
        // Issue #2045: use persisted entry.source_to_ir_map (rebuilt after
        // re-lower); lazy-rebuild if empty so impact_scope never sees stale
        // ad-hoc maps from pre-relower IR.
        std::string define_name;
        if (flat->get(root).tag == aura::ast::NodeTag::Define) {
            if (auto* pool = evaluator_.workspace_pool())
                define_name = std::string(pool->resolve(flat->get(root).sym_id));
        } else {
            // Walk up to enclosing Define for mutate:rebind body roots.
            auto cur = root;
            std::size_t safety = 0;
            while (cur != aura::ast::NULL_NODE && cur < flat->size() && safety++ < flat->size()) {
                if (flat->get(cur).tag == aura::ast::NodeTag::Define) {
                    if (auto* pool = evaluator_.workspace_pool())
                        define_name = std::string(pool->resolve(flat->get(cur).sym_id));
                    break;
                }
                cur = flat->parent_of(cur);
            }
        }
        if (!define_name.empty()) {
            if (auto it = ir_cache_v2_.find(define_name); it != ir_cache_v2_.end()) {
                ensure_source_to_ir_map_(it->second);
                source_to_ir = it->second.source_to_ir_map;
                for (std::size_t fi = 0; fi < it->second.irs.size(); ++fi)
                    ir_cache_index[it->second.irs[fi].name] = fi;
            }
        }
        auto scope = compute_impact_scope(*flat, root, source_to_ir, ir_cache_index);
        const std::uint64_t blocks =
            scope.affected_blocks.empty() ? 1u : scope.affected_blocks.size();
        evaluator_.bump_impact_scope_calls(blocks);
        evaluator_.bump_edsl_mutate_invalidate_precision();
        // Issue #2031: apply instruction-level dirty to the cached entry.
        if (!define_name.empty()) {
            if (auto it = ir_cache_v2_.find(define_name); it != ir_cache_v2_.end())
                (void)apply_impact_scope_dirty(it->second, scope);
        } else if (scope.instr_level_hits > 0 || scope.unmapped_ast_nodes > 0) {
            // Still record observability when cache miss.
            if (scope.instr_level_hits > 0)
                metrics_.instr_level_impact_hits_total.fetch_add(scope.instr_level_hits,
                                                                 std::memory_order_relaxed);
            if (scope.unmapped_ast_nodes > 0)
                metrics_.instr_level_impact_misses_total.fetch_add(scope.unmapped_ast_nodes,
                                                                   std::memory_order_relaxed);
        }
        if (flat->is_macro_introduced(root)) {
            evaluator_.bump_macro_hygiene_dirty_impact();
            // Issue #1145: wire selfevo hygiene dirty-epoch hit path.
            evaluator_.bump_selfevo_hyg_dirty();
            evaluator_.bump_selfevo_hyg_dirty_hit();
            evaluator_.bump_selfevo_hyg_dirty_savings();
        }

        // Issue #741: quote/lambda defines — selective bridge refresh +
        // live EnvFrame version re-stamp for captured closures.
        if (flat_has_quote_or_lambda(*flat)) {
            if (!define_name.empty()) {
                selective_invalidate_bridge_for_impact(define_name, scope);
                metrics_.incremental_closure_quote_lambda_stale_prevented_total.fetch_add(
                    1, std::memory_order_relaxed);
                evaluator_.bump_incremental_closure_quote_lambda_stale_prevented();
            }
            metrics_.incremental_closure_bridge_impact_blocks_total.fetch_add(
                blocks, std::memory_order_relaxed);
            evaluator_.bump_incremental_closure_bridge_impact_blocks(blocks);
        }
        (void)evaluator_.resync_live_closure_env_versions_on_invalidate();
    }

    // atomic_bump_epochs_and_stamp_bridge: defined later
    // (Issue #1522/#1476/#1524/#1496/#1607 — single soft/hard epoch protocol)
    // authoritative helper with AOT table epoch + JIT batch_deopt).

    // Issue #1625: does nested IR function reference `mutated_name`?
    // free_vars (capture list) + instruction-level ConstString scan
    // against entry.strings (covers quote/name loads when lowered).
    static bool nested_lambda_references_name(const aura::ir::IRFunction& nfn,
                                              const std::string& mutated_name,
                                              const std::vector<std::string>& strings) {
        for (const auto& fv : nfn.free_vars) {
            if (fv == mutated_name)
                return true;
        }
        // Instruction-level: ConstString loads of the name (quoted /
        // symbolic refs that escape free_vars on some lower paths).
        for (const auto& bb : nfn.blocks) {
            for (const auto& instr : bb.instructions) {
                if (instr.opcode != aura::ir::IROpcode::ConstString)
                    continue;
                // Issue #4321: keyword literals are not name refs.
                if (instr.operands[2] != 0)
                    continue;
                const auto idx = instr.operands[1];
                if (idx < strings.size() && strings[idx] == mutated_name)
                    return true;
            }
        }
        return false;
    }

    // Issue #1625: mark only nested-lambda blocks that reference the
    // mutated name (per-block), not the whole nested function.
    // When free_vars/ConstString say "refs" but no block-level evidence
    // (empty/synthetic IR), mark entry_block only — keeps dirty << total
    // on multi-block nested lambdas. Returns blocks marked.
    std::size_t mark_nested_lambda_blocks_targeted(IRCacheEntry& entry, std::size_t fi,
                                                   const std::string& mutated_name) {
        if (fi >= entry.irs.size())
            return 0;
        const auto& nfn = entry.irs[fi];
        if (!nested_lambda_references_name(nfn, mutated_name, entry.strings))
            return 0;
        if (entry.block_dirty_per_func_.size() <= fi)
            entry.block_dirty_per_func_.resize(fi + 1);
        auto& fb = entry.block_dirty_per_func_[fi];
        if (fb.size() < nfn.blocks.size())
            fb.resize(nfn.blocks.size(), std::uint8_t{0});

        // Collect per-block instruction hits (ConstString → name).
        std::vector<std::uint8_t> hit(nfn.blocks.size(), 0);
        bool any_instr_hit = false;
        for (std::size_t bi = 0; bi < nfn.blocks.size(); ++bi) {
            for (const auto& instr : nfn.blocks[bi].instructions) {
                if (instr.opcode != aura::ir::IROpcode::ConstString)
                    continue;
                // Issue #4321: keyword literals are not name refs.
                if (instr.operands[2] != 0)
                    continue;
                const auto idx = instr.operands[1];
                if (idx < entry.strings.size() && entry.strings[idx] == mutated_name) {
                    hit[bi] = 1;
                    any_instr_hit = true;
                    break;
                }
            }
        }

        std::size_t marked = 0;
        if (any_instr_hit) {
            // Issue #2522: collect hits then batch mark (one fence).
            std::vector<std::uint32_t> hit_ids;
            hit_ids.reserve(hit.size());
            for (std::size_t bi = 0; bi < hit.size(); ++bi) {
                if (!hit[bi])
                    continue;
                hit_ids.push_back(static_cast<std::uint32_t>(bi));
            }
            if (!hit_ids.empty()) {
                entry.mark_blocks_dirty(fi, hit_ids);
                marked = hit_ids.size();
            }
        } else {
            // free_vars / capture hit without per-block IR evidence:
            // mark entry block only (not all nested blocks).
            const auto eb = static_cast<std::size_t>(nfn.entry_block);
            const std::uint32_t bi = eb < nfn.blocks.size() ? static_cast<std::uint32_t>(eb) : 0u;
            if (!nfn.blocks.empty()) {
                entry.mark_block_dirty(fi, bi);
                marked = 1;
            }
        }
        const std::size_t total = nfn.blocks.size();
        const std::size_t kept = total > marked ? total - marked : 0;
        if (marked > 0) {
            metrics_.dep_graph_nested_lambda_blocks_targeted_total.fetch_add(
                marked, std::memory_order_relaxed);
            metrics_.dep_graph_nested_lambda_blocks_kept_clean_total.fetch_add(
                kept, std::memory_order_relaxed);
        }
        return marked;
    }

    // Issue #2137: drain deferred non-render cascades after present / budget exit.
    // Body in service_dirty.cpp.
    void flush_frame_budget_deferred_();

    // Issue #2035: after mark_define_dirty / invalidate_function cascade,
    // fan-out HotUpdateRegistry dirty listeners, derive emit region mask
    // from block_dirty_ / SoA columns, and (when reemit provider is wired)
    // trigger aura_reemit_aot_for_dirty for selective AOT re-emit.
    // Body in service_dirty.cpp (HotUpdateRegistry link discipline).
    void notify_hot_update_after_cascade_(const std::string& name,
                                          const std::vector<std::string>& dependents = {});

    // Wave 5: bodies in service_dirty.cpp (first service module partition).
    void mark_define_dirty(const std::string& name);
    // Mark all defines dirty. Called when (set-code ...) re-parses the whole
    // workspace (which can change any define's body). Issue #196: also flips
    // every block in every entry to dirty. Wave2: bulk jit_.invalidate_all().
    void mark_all_defines_dirty();

    // Issue #196: fine-grained per-block dirty marking API.
    // Public hooks so the smarter-re-lower and EDSL code can
    // mark individual blocks dirty without invalidating the
    // whole function. The block is identified by (func_idx,
    // block_idx) within the entry's irs[] / blocks[].

    // Mark a single block dirty. Returns true on success,
    // false if the entry doesn't exist or the indices are
    // out of range (with conservative behavior: if the entry
    // exists but indices are out of range, the whole entry
    // is marked dirty).
    bool mark_block_dirty_v2(const std::string& name, std::size_t func_idx,
                             std::uint32_t block_idx) {
        // Issue #1855: unique vs compile:relower-strategy shared
        // snapshot of dirty_block_count (same jit_cache_mtx_ as
        // invalidate / JIT cache writers).
        std::unique_lock cache_write(jit_cache_mtx_);
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& entry = it->second;
        if (func_idx >= entry.irs.size() || block_idx >= entry.irs[func_idx].blocks.size()) {
            // Out of range — conservatively mark the whole
            // entry dirty so the next lookup_define_v2 will
            // re-lower.
            entry.dirty = true;
            entry.mark_all_blocks_dirty();
            finish_cascade_soa_dirty_sync_(entry);
            return true;
        }
        entry.mark_block_dirty(func_idx, block_idx);
        // Issue #2034: force SoA instruction_dirty_ after single-block mark.
        finish_cascade_soa_dirty_sync_(entry);
        metrics_.ir_soa_block_dirty_hits_total.fetch_add(1, std::memory_order_relaxed);
        if (func_idx < entry.irs.size() && block_idx < entry.irs[func_idx].blocks.size()) {
            metrics_.irsoa_dirty_cascade_savings.fetch_add(
                entry.irs[func_idx].blocks[block_idx].instructions.size(),
                std::memory_order_relaxed);
        }
        // Don't flip entry.dirty — a single dirty block
        // doesn't necessarily mean the whole entry is
        // invalid (consumers can re-lower just that block).
        return true;
    }

    // Clear a single block's dirty bit. Called by the
    // smarter-re-lower after re-lowering a block.
    // Returns true on success, false if the entry doesn't
    // exist.
    bool clear_block_dirty_v2(const std::string& name, std::size_t func_idx,
                              std::uint32_t block_idx) {
        // Issue #1855: unique vs relower-strategy shared read.
        std::unique_lock cache_write(jit_cache_mtx_);
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& entry = it->second;
        entry.clear_block_dirty(func_idx, block_idx);
        return true;
    }

    // Query: total dirty block count for an entry. Returns
    // 0 if the entry doesn't exist.
    std::size_t dirty_block_count_v2(const std::string& name) const {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return 0;
        return it->second.dirty_block_count();
    }

    // Query: dirty block count for a specific function in
    // an entry. Returns 0 if the entry or func_idx is
    // out of range.
    std::size_t func_dirty_block_count_v2(const std::string& name, std::size_t func_idx) const {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return 0;
        return it->second.func_dirty_block_count(func_idx);
    }

    // Query: is a specific block dirty? Returns false if
    // the entry, func_idx, or block_idx is out of range.
    bool is_block_dirty_v2(const std::string& name, std::size_t func_idx,
                           std::uint32_t block_idx) const {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        return it->second.is_block_dirty(func_idx, block_idx);
    }

    // Issue #224 cycle 2: per-block re-lower consumer.
    // The smarter-re-lower foundation was laid in Issue
    // #196 (per-block dirty bitmask + observability
    // primitives). This helper is the first real
    // consumer: it consults the bitmask and decides
    // whether to (a) skip the re-lower entirely (no
    // dirty blocks → return cached IR) or (b) do a
    // full re-lower.
    //
    // Cycle 2 is intentionally scoped: the helper does
    // NOT yet do per-block re-lowering (which would
    // require a new lowering API that can re-emit a
    // single block in isolation). The full re-lower is
    // the existing path via lower_to_ir_with_cache.
    // The honest win is the early-exit: if no blocks
    // are dirty, we save the entire lowering pass.
    //
    // Returns:
    //   - true  if a re-lower was performed OR skipped
    //            (i.e., the entry was either refreshed
    //             or already clean).
    //   - false if the entry doesn't exist in ir_cache_v2_
    //            (caller needs to do a full first-time
    //             lower).
    //
    // Side effects:
    //   - Bumps either relower_skipped_entirely_count
    //     (no work done) or relower_full_called_count
    //     (full re-lower performed).
    //   - On full re-lower: calls store_define_v2 which
    //     rebuilds the bitmask from the new irs[] and
    //     clears all dirty bits.
    //
    // The (source, flat, pool, expanded_root) args are
    // passed in (rather than looked up internally) so
    // the caller can pass either the per-call flat or
    // the workspace_flat, depending on the call site.
    // Issue #1601 AC2: explicit alias for the production partial path —
    // selectively re-lowers only dirty functions/blocks (via
    // relower_define_function per-block copy + limited dirty pipeline)
    // and falls back to full re-lower when shape mismatches.
    bool relower_only_dirty_blocks(const std::string& name, std::string_view source,
                                   aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                   aura::ast::NodeId expanded_root) {
        return relower_define_blocks(name, source, flat, pool, expanded_root);
    }

    bool relower_define_blocks(const std::string& name, std::string_view source,
                               aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                               aura::ast::NodeId expanded_root, bool commit_per_fn_partial = true);

    // Issue #224 cycle 3: per-function re-lower. Re-lowers a
    // single Lambda AST node and replaces that function in the
    // cached entry's irs[] without touching the rest of the
    // bundle.
    //
    // This is the real per-function win: if a cached entry has
    // a body Lambda + N nested lambdas, and only the body's
    // function has dirty blocks, we re-lower ONE function
    // instead of N+1. For typical define bodies (single Lambda,
    // no nested functions), this collapses to "re-lower the
    // one function".
    //
    // The (flat, pool, lambda_node_id) args let the caller
    // pass the FlatAST that contains the Lambda — typically
    // the same flat that was used to populate the entry via
    // store_define_v2 / cache_define, possibly with the source
    // re-parsed after a (set-code ...) mutation.
    //
    // Returns:
    //   - true  if the per-function re-lower succeeded and the
    //            function was replaced in the cache.
    //   - false if the entry doesn't exist, func_idx is out of
    //            range, lambda_node_id is invalid, or the
    //            lowering returned an empty function. Caller
    //            should fall back to relower_define_blocks()
    //            (full re-lower) on false.
    //
    // Side effects:
    //   - Bumps relower_per_function_called_count
    //   - Replaces ir_cache_v2_[name].irs[func_idx] in place
    //   - Clears the per-block dirty bits for that function
    //     (the new IR is presumed correct)
    //   - Mirrors the new function into ir_cache_[name] (v1).
    //     Same dropped-__top__ layout uses func_idx; a __top__ prefix
    //     on only one cache keeps func_idx-1 (#4311).
    //   - Runs per-function passes (compute_kind, constant_fold)
    //     on the new function, matching what cache_define does
    //     for the full-bundle path.
    //
    // The MakeClosure operands in the new function reference
    // func_id 0 (reset by lower_function_at). The caller is
    // responsible for fixing them up; for cycle 3 we accept
    // func_id 0 as a placeholder and document the limitation.
    // A future cycle will add a func_id remap pass for the
    // per-function replacement path.
    bool relower_define_function(const std::string& name, std::size_t func_idx,
                                 aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                 aura::ast::NodeId lambda_node_id);

    // Phase 2: walk workspace_flat_'s top-level defines and pre-populate
    // the v2 IR cache. For each define, compute canonical source (unparsed),
    // hash it, and:
    //   - if hash matches and not dirty → skip (cache hit)
    //   - if hash differs OR dirty OR not in cache → re-lower and store
    // Called by (set-code ...) via the pre_cache_workspace_defines_fn_ hook
    // in Evaluator, after the new flat is parsed.
    // Plan A (Phase 3 follow-up): split the pre-existing
    // pre_cache_workspace_defines into two functions to avoid the
    // cache_define side effect (which calls eval_flat on top_env and
    // pollutes the env).
    //
    // The lightweight populate_dep_graph_from_workspace walks the
    // workspace, finds free variable references, and records caller→callee
    // edges in dep_graph_. NO cache_define, NO eval_flat, NO pollution.
    // Called automatically from (set-code ...).
    //
    // The heavy populate_ir_cache_v2_from_workspace actually does the IR
    // lowering via cache_define. Opt-in only; not called by default to
    // avoid the side-effect issue. Future work.
    //
    // Issue #4621: a workspace define's canonical source is re-parsed and
    // re-lowered (populate_ir_cache_v2_from_workspace -> cache_define,
    // relower_dirty_defines_from_workspace -> relower_define_blocks,
    // specialize_one_lambda_via_ir -> parse_to_flat). It must therefore
    // round-trip the define's parameter list. The legacy unparse_node
    // dropped a lambda's dotted rest marker — `(lambda (a . r) r)`
    // unparsed to `(lambda (a r) r)` and the all-args form
    // `(lambda xs ...)` to `(lambda (xs) ...)` — so the re-parse turned a
    // variadic workspace define into a fixed-arity one and silently
    // changed the define's arity (issue #4621). aura::ast::unparse_to_string
    // is the #2922 SSOT unparser: it keeps the `. rest` marker, so the
    // re-parse restores the same parameter list.
    static std::string workspace_define_canonical_source(const aura::ast::FlatAST& flat,
                                                         const aura::ast::StringPool& pool,
                                                         const std::string& name,
                                                         aura::ast::NodeId body_id) {
        std::string body_src;
        if (body_id != aura::ast::NULL_NODE && body_id < flat.size() && !flat.is_free_slot(body_id))
            body_src = aura::ast::unparse_to_string(flat, pool, body_id, {});
        if (body_src.empty())
            body_src = unparse_node(flat, pool, body_id, 0);
        if (body_src.empty())
            return {};
        return "(define " + name + " " + body_src + ")";
    }

    // Issue #4682 step 2: persistent reverse-dep graph. A full populate
    // records the mutation-log position it is in sync with; a single
    // rebind whose log_before matches (nothing else mutated in between)
    // re-walks only that define: O(|body|) instead of O(N) + W unparses.
    const void* dep_sync_flat_4682_ = nullptr;
    std::uint64_t dep_sync_log_4682_ = ~std::uint64_t{0};
    std::uint64_t dep_sync_last_id_4682_ = ~std::uint64_t{0};
    std::uint64_t dep_graph_one_hits_4682_ = 0;
    static std::uint64_t log_last_id_4682_(const aura::ast::FlatAST& f, std::uint64_t n) {
        const auto& log = f.all_mutations();
        return (n == 0 || n > log.size()) ? 0 : log[n - 1].mutation_id;
    }
    std::uint64_t dep_graph_one_fallbacks_4682_ = 0;
    void populate_dep_graph_one_4682_(const std::string& name, std::uint64_t log_before) {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return;
        const auto sym = ws_pool->find_by_name(name);
        std::vector<aura::ast::NodeId> ids;
        if (sym)
            ids = ws_flat->defines_for_sym(*sym);
        if (dep_sync_flat_4682_ != ws_flat || dep_sync_log_4682_ != log_before ||
            dep_sync_last_id_4682_ != log_last_id_4682_(*ws_flat, log_before) || ids.size() != 1) {
            ++dep_graph_one_fallbacks_4682_;
            populate_dep_graph_from_workspace();
            return;
        }
        ++dep_graph_one_hits_4682_;
        populate_dep_graph_define_4682_(*ws_flat, *ws_pool, ids.front(),
                                        [&](aura::ast::SymId s, const std::string&) {
                                            return !ws_flat->defines_for_sym(s).empty();
                                        });
        dep_sync_log_4682_ = ws_flat->mutation_log_size();
        dep_sync_last_id_4682_ = log_last_id_4682_(*ws_flat, dep_sync_log_4682_);
    }
    // Issue #4793: ast:restore stamp path — the graph is in sync with the live log now.
    [[nodiscard]] bool restore_dep_graph_synced_() {
        auto* ws_flat = evaluator_.workspace_flat();
        if (!ws_flat || dep_sync_flat_4682_ != ws_flat)
            return false;
        const auto n = ws_flat->mutation_log_size();
        return dep_sync_log_4682_ == n && dep_sync_last_id_4682_ == log_last_id_4682_(*ws_flat, n);
    }
    // ast:restore stamp path: re-walk the restored roots whose content
    // changed, plus every Define nested under them (the full rebuild walks
    // those too), then mark the graph in sync. O(changed bodies), not O(W).
    // The caller only takes this path when the graph was in sync before
    // the swap and no define was added; every other define's edges and
    // ir_cache_v2_ shell are then exactly what the (additive) full rebuild
    // would leave.
    std::uint64_t restore_dep_graph_names_walked_ = 0;
    void restore_dep_graph_names_(const std::vector<std::string>& names) {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return;
        auto is_define = [&](aura::ast::SymId s, const std::string&) {
            return !ws_flat->defines_for_sym(s).empty();
        };
        std::vector<aura::ast::NodeId> stack;
        for (const auto& name : names) {
            const auto sym = ws_pool->find_by_name(name);
            if (!sym)
                continue;
            for (auto root : ws_flat->defines_for_sym(*sym)) {
                stack.assign(1, root);
                while (!stack.empty()) {
                    const auto nid = stack.back();
                    stack.pop_back();
                    if (nid >= ws_flat->size() || ws_flat->is_free_slot(nid))
                        continue;
                    auto nv = ws_flat->get(nid);
                    if (nv.tag == aura::ast::NodeTag::Define) {
                        populate_dep_graph_define_4682_(*ws_flat, *ws_pool, nid, is_define);
                        ++restore_dep_graph_names_walked_;
                    }
                    for (auto c : nv.children)
                        stack.push_back(c);
                }
            }
        }
        restore_dep_graph_advance_();
    }
    void restore_dep_graph_advance_() {
        auto* ws_flat = evaluator_.workspace_flat();
        if (!ws_flat)
            return;
        dep_sync_flat_4682_ = ws_flat;
        dep_sync_log_4682_ = ws_flat->mutation_log_size();
        dep_sync_last_id_4682_ = log_last_id_4682_(*ws_flat, dep_sync_log_4682_);
    }
    void populate_dep_graph_from_workspace() {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return;
        // Issue #4557: every Define name in the flat, collected once. The
        // sibling test below used to rescan the whole flat for each
        // Variable reference of each define (O(W^2) per set-code).
        std::unordered_set<std::string> define_names;
        for (aura::ast::NodeId sid = 0; sid < ws_flat->size(); ++sid) {
            auto sv = ws_flat->get(sid);
            if (sv.tag == aura::ast::NodeTag::Define && sv.sym_id != aura::ast::INVALID_SYM)
                define_names.insert(std::string(ws_pool->resolve(sv.sym_id)));
        }
        for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id)
            populate_dep_graph_define_4682_(*ws_flat, *ws_pool, id,
                                            [&](aura::ast::SymId, const std::string& vname) {
                                                return define_names.count(vname) != 0;
                                            });
        dep_sync_flat_4682_ = ws_flat;
        dep_sync_log_4682_ = ws_flat->mutation_log_size();
        dep_sync_last_id_4682_ = log_last_id_4682_(*ws_flat, dep_sync_log_4682_);
    }
    template <class IsDefine>
    void populate_dep_graph_define_4682_(aura::ast::FlatAST& flat_ref,
                                         aura::ast::StringPool& pool_ref, aura::ast::NodeId id,
                                         IsDefine&& is_define) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto* ws_flat = &flat_ref;
        auto* ws_pool = &pool_ref;
        {
            auto v = ws_flat->get(id);
            if (v.tag != aura::ast::NodeTag::Define)
                return;
            if (v.sym_id == aura::ast::INVALID_SYM)
                return;
            auto name = std::string(ws_pool->resolve(v.sym_id));
            if (name.empty() || name[0] == '_')
                return;
            if (v.children.empty())
                return;
            // Build canonical source + hash for the v2 entry (lightweight —
            // just unparse, no cache_define, no eval_flat side effects).
            auto child_id = v.child(0);
            std::string canonical =
                workspace_define_canonical_source(*ws_flat, *ws_pool, name, child_id);
            auto hash = fnv1a_64(canonical);
            // Walk the define body to find Variable references that resolve
            // to other defines in the workspace. Record caller→callee in
            // dep_graph_ for cascade dirty invalidation.
            std::vector<aura::ast::NodeId> stack;
            stack.push_back(child_id);
            std::unordered_set<std::string> seen;
            while (!stack.empty()) {
                auto nid = stack.back();
                stack.pop_back();
                if (nid >= ws_flat->size())
                    continue;
                auto nv = ws_flat->get(nid);
                if (nv.tag == aura::ast::NodeTag::Variable && nv.sym_id != aura::ast::INVALID_SYM) {
                    auto vname = std::string(ws_pool->resolve(nv.sym_id));
                    if (!vname.empty() && vname != name && seen.insert(vname).second) {
                        // Check if vname is a sibling define in this workspace
                        if (is_define(nv.sym_id, vname)) {
                            record_dependency(name, vname);
                        }
                    }
                }
                // Issue #1657: also walk Quote + Lambda free-var + macro
                // subtree references that the basic Variable walk above
                // would miss. Each non-Variable reference is recorded as
                // a dep_graph_ edge miss so callers can fall back to
                // coarse full-dirty for affected defines. Bumps the
                // dep_graph_edge_miss_count metric on miss.
                if (nv.tag == aura::ast::NodeTag::Quote || nv.tag == aura::ast::NodeTag::Lambda ||
                    nv.tag == aura::ast::NodeTag::MacroDef) {
                    // Issue #1657: complex nested forms may miss Variable
                    // edges; count for coarse-fallback observability.
                    metrics_.dep_graph_edge_miss_count.fetch_add(1, std::memory_order_relaxed);
                }
                for (auto c : nv.children)
                    stack.push_back(c);
            }
            // Create/update the v2 entry (lightweight — no cache_define).
            // Only create if not already present OR if the hash changed.
            // (We don't overwrite an existing entry's depends_on, which was
            // computed last time and is still valid if the source hash matches.)
            const bool existed = ir_cache_v2_.find(name) != ir_cache_v2_.end();
            auto& entry = ir_cache_v2_[name];
            // Issue #4091: keep the FnKey → name side index in sync.
            shape_fnkey_names_.insert_or_assign(shape::make_fn_key(session_id_, name), name);
            if (!existed) {
                entry.source = canonical;
                entry.source_hash = hash;
                entry.dirty = false; // source-only shell until heavy populate
            } else if (entry.source_hash != hash) {
                entry.source = canonical;
                entry.source_hash = hash;
                entry.irs.clear();
                entry.invalidate_irs_layout_(); // Issue #4615
                entry.bridges.clear();
                entry.strings.clear();
                entry.block_dirty_per_func_.clear();
                entry.instruction_dirty_per_func_.clear();
                entry.dirty = true; // body changed — keep rebind / cascade mark
            }
        }
    }

    // Heavy (opt-in) populate of the v2 IR cache. Calls cache_define for
    // each top-level define, which has side effects (eval_flat on top_env).
    // NOT called by default. Future work: re-enable when the side-effect
    // issue is resolved.
    // Issue #2579: after eval-current TW, map each workspace value define
    // name → its top_env cell index for IR Variable lowering. Does not
    // re-evaluate or overwrite bindings (TW already did that).
    void sync_workspace_value_cells_from_env() {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (ws_flat && ws_pool) {
            // Issue #4682 (B): prefilter on the tag / sym SoA columns so
            // FlatAST::get (seqlock + full NodeView assemble) runs only for
            // Define nodes. Same ids, same order, same checks as the old
            // full get() scan (free slots included).
            for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                if (ws_flat->tag(id) != aura::ast::NodeTag::Define ||
                    ws_flat->sym_id(id) == aura::ast::INVALID_SYM)
                    continue;
                auto v = ws_flat->get(id);
                if (v.tag != aura::ast::NodeTag::Define || v.sym_id == aura::ast::INVALID_SYM)
                    continue;
                auto name = std::string(ws_pool->resolve(v.sym_id));
                if (name.empty() || name[0] == '_')
                    continue;
                if (v.children.empty())
                    continue;
                // Skip pure function defines (Lambda body) — IR function cache
                // owns those; only value defines need cell indices.
                // Issue #4228: Soft TW Lambda exports are registered below via
                // top_env Soft-cell walk (not workspace Lambda skip alone).
                if (ws_flat->get(v.child(0)).tag == aura::ast::NodeTag::Lambda) {
                    // Issue #4681: the define's env cell is its call slot.
                    if (call_slots_enabled_4681_()) {
                        auto cell = evaluator_.top_env().lookup(name);
                        bool slot_is_closure = false;
                        if (cell && types::is_cell(*cell)) {
                            // Issue #4717: snapshot under the cells storage lock.
                            std::scoped_lock<std::recursive_mutex> cell_lk(
                                evaluator_.cells_storage_mutex());
                            const auto ci = types::as_cell_id(*cell);
                            slot_is_closure = ci < evaluator_.cells().size() &&
                                              types::is_closure(evaluator_.cells()[ci]);
                        }
                        if (slot_is_closure) {
                            ir_call_slots_4681_[name] = types::as_cell_id(*cell);
                            auto lv = ws_flat->get(v.child(0));
                            slot_arity_4681_[name] = static_cast<int>(lv.params.size());
                        }
                    }
                    continue;
                }
                // Issue #4632: lookup_binding is a linear reverse scan over
                // the env's whole binding vector. A workspace :switch into
                // a workspace that has had one mutate:rebind re-binds that
                // workspace's defines on EVERY switch, so the env's binding
                // vector grows with the number of switches - and the scan
                // made every switch O(env size), i.e. the switch loop
                // quadratic. Env::lookup answers the same question through
                // the env's O(1) binding index (same newest-wins answer).
                auto existing = evaluator_.top_env().lookup(name);
                if (!existing || !types::is_cell(*existing))
                    continue;
                ir_value_cell_bindings_[name] = types::as_cell_id(*existing);
            }
        }
        // Issue #4228: Soft oneshot std/math (even?/odd?/positive?/zero?/…)
        // and other require-injected Lambda cells are TW-evaluated into
        // top_env. Prefer those live TW closures via TopCellLoad over IR
        // function-cache MakeClosure (high-bit stubs) — native filter/map
        // apply_closure misses the stub while let/apply/TW Call stay green.
        sync_soft_tw_export_cells_into_ir_value_bindings_();
    }

    // Issue #4632: cursor over the env's append-only binding vector -
    // the Soft export walk registers each binding once, when it appears,
    // instead of re-scanning the whole env on every workspace :switch.
    std::size_t soft_export_bindings_cursor_ = 0;
    std::string soft_export_last_name_;

    void sync_soft_tw_export_cells_into_ir_value_bindings_() {
        // Soft (require … all:) injects via Env::bind into legacy string
        // bindings_ (top_ often has no StringPool, so bindings_symid_ /
        // bindings_with_names() stay empty — #4228). Walk bindings() and
        // also force-lookup Soft std/math preds Soft oneshot auto-loads.
        auto try_register = [&](const std::string& name, const types::EvalValue& val) {
            types::EvalValue actual = val;
            if (types::is_cell(actual)) {
                const auto ci = types::as_cell_id(actual);
                // Issue #4717: snapshot the cell under the storage lock so a
                // concurrent cell append cannot reallocate underneath.
                {
                    std::scoped_lock<std::recursive_mutex> cell_lk(
                        evaluator_.cells_storage_mutex());
                    if (ci >= evaluator_.cells().size())
                        return;
                    actual = evaluator_.cells()[ci];
                }
                if (!types::is_closure(actual)) {
                    // Issue #4587: a host value such as (define n 5) is a
                    // cell whose payload is not a closure. Skipping it made
                    // a workspace lambda lower the name to ConstI64 0.
                    // TopCellLoad reads this cell, including after set!.
                    ir_value_cell_bindings_[name] = ci;
                    return;
                }
                const auto cid = types::as_closure_id(actual);
                if (types::is_ir_closure_id(cid))
                    return;
                ir_value_cell_bindings_[name] = ci;
                return;
            }
            // Soft may inject a bare TW closure (not cell-wrapped). Wrap
            // into a fresh define-cell so TopCellLoad can feed native
            // filter/map apply_closure the live Soft cid.
            if (!types::is_closure(actual))
                return;
            const auto cid = types::as_closure_id(actual);
            if (types::is_ir_closure_id(cid))
                return;
            const auto ci = evaluator_.cells().size();
            evaluator_.cells().push_back(actual);
            // Re-bind as cell so later TW lookups stay consistent with IR.
            evaluator_.top_env().bind(name, types::make_cell(static_cast<std::uint64_t>(ci)));
            ir_value_cell_bindings_[name] = ci;
        };

        // Issue #4632: this walk used to re-scan the WHOLE env on every
        // call. A workspace :switch into a workspace that has had one
        // mutate:rebind re-binds that workspace's defines on EVERY switch,
        // and each re-bind appends to the env's binding vector - so the env
        // grew with the number of switches and the full walk made the
        // switch loop quadratic (one rebind was enough: the growth comes
        // from the switches, not from the rebind count).
        //
        // The env's binding vector is append-only (Env::bind appends), so a
        // cursor over it is sound: every binding is registered exactly
        // once, when it appears. A wholesale replace
        // (Env::replace_string_bindings) or an unbind shrinks the vector -
        // the cursor then restarts and the walk runs again.
        auto& bindings = evaluator_.top_env().bindings();
        if (soft_export_bindings_cursor_ > bindings.size())
            soft_export_bindings_cursor_ = 0; // wholesale replace - rescan
        // A mid-vector erase shifts the tail down by one and would hide the
        // last binding from the cursor. Detect it by re-reading the name of
        // the last binding the cursor consumed.
        if (soft_export_bindings_cursor_ > 0 and
            soft_export_last_name_ != bindings[soft_export_bindings_cursor_ - 1].first) {
            soft_export_bindings_cursor_ = 0;
        }
        while (soft_export_bindings_cursor_ < bindings.size()) {
            // Copy the pair by value: try_register may bind a fresh cell,
            // which appends to the vector (and would invalidate iterators).
            const std::string name = bindings[soft_export_bindings_cursor_].first;
            const types::EvalValue val = bindings[soft_export_bindings_cursor_].second;
            ++soft_export_bindings_cursor_;
            soft_export_last_name_ = name;
            if (name.empty() or name[0] == '_')
                continue;
            try_register(name, val);
        }
        // Soft oneshot std/math preds - ensure even when bindings() is
        // parent-shadowed or only visible via lookup.
        static constexpr const char* kSoftMathPreds[] = {
            "even?", "odd?", "positive?", "negative?", "zero?",
        };
        for (const char* pred : kSoftMathPreds) {
            if (ir_value_cell_bindings_.count(pred))
                continue;
            // Issue #4632: Env::lookup answers the same question through the
            // env's O(1) binding index; lookup_binding is a linear reverse
            // scan over the whole env (quadratic over a switch loop).
            if (auto v = evaluator_.top_env().lookup(pred))
                try_register(pred, *v);
        }
    }

    void populate_ir_cache_v2_from_workspace() {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return;
        // Issue #1495: prefer partial re-lower for already-cached
        // dirty defines before the full cache_define path below.
        (void)relower_dirty_defines_from_workspace();
        for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
            auto v = ws_flat->get(id);
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            if (v.sym_id == aura::ast::INVALID_SYM)
                continue;
            auto name = std::string(ws_pool->resolve(v.sym_id));
            if (name.empty() || name[0] == '_')
                continue;
            if (v.children.empty())
                continue;
            auto child_id = v.child(0);
            std::string canonical =
                workspace_define_canonical_source(*ws_flat, *ws_pool, name, child_id);
            auto hash = fnv1a_64(canonical);
            auto it = ir_cache_v2_.find(name);
            // populate_dep_graph_from_workspace may have created a
            // source-only v2 shell (hash match, empty irs/bridges).
            // Require materialized IR before skipping cache_define.
            const bool ir_materialized = ir_cache_bridge_.count(name) > 0 ||
                                         (it != ir_cache_v2_.end() && !it->second.irs.empty());
            if (it != ir_cache_v2_.end() && !it->second.dirty && it->second.source_hash == hash &&
                ir_materialized) {
                continue;
            }
            // Issue #1495: if partial re-lower already cleaned this
            // entry (dirty cleared), skip full lower even when hash
            // still drifts (source stamp may lag body unparse form).
            if (it != ir_cache_v2_.end() && !it->second.dirty &&
                it->second.dirty_block_count() == 0 && ir_materialized && !it->second.irs.empty()) {
                // Refresh hash to match current workspace.
                it->second.source = canonical;
                it->second.source_hash = hash;
                continue;
            }
            auto alloc = arena_.allocator();
            aura::ast::StringPool* tmp_pool = nullptr;
            aura::ast::FlatAST* tmp_flat = nullptr;
            // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
            tmp_pool = arena_.create_with_cover<aura::ast::StringPool>(
                nullptr, "stack-cover-dies-at-return", alloc);
            tmp_flat = arena_.create_with_cover<aura::ast::FlatAST>(
                nullptr, "stack-cover-dies-at-return", alloc);
            if (!tmp_pool || !tmp_flat)
                continue;
            auto pr = aura::parser::parse_to_flat(canonical, *tmp_flat, *tmp_pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE)
                continue;
            tmp_flat->root = pr.root;
            // Pass bind_in_env=false: don't pollute the workspace's env
            // by calling eval_flat. The define is bound later by
            // eval-current which uses its own env.
            // Issue #4341: sample before cache_define lowers this define —
            // the mirror store_define_v2 stamps the sampled epoch.
            const std::uint64_t ir_sample_epoch = aura::core::current_mutation_epoch();
            (void)cache_define(canonical, *tmp_flat, *tmp_pool, pr.root, name,
                               /*bind_in_env=*/false);
            // Issue #63723 / #2579: do NOT bind_value_define_via_ir here.
            // Eager IR env bind polluted top_ before tree-walker eval-current
            // (H8 multi-define call-init / value defines). Value cells for
            // IR lowering are recorded post-eval via
            // sync_workspace_value_cells_from_env.
            // Mirror cache_define output into v2 (dep_graph populate may have
            // left a source-only shell that previously blocked heavy populate).
            if (auto cit = ir_cache_.find(name); cit != ir_cache_.end()) {
                std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
                if (auto bt = ir_cache_bridge_.find(name); bt != ir_cache_bridge_.end())
                    bridge_bundle = bt->second;
                std::vector<std::string> strings_bundle;
                if (auto st = ir_cache_strings_.find(name); st != ir_cache_strings_.end())
                    strings_bundle = st->second;
                store_define_v2(name, canonical, std::move(cit->second), std::move(bridge_bundle),
                                std::move(strings_bundle), ir_sample_epoch);
                auto vit = ir_cache_v2_.find(name);
                if (vit != ir_cache_v2_.end()) {
                    ir_cache_[name] = vit->second.irs;
                    ir_cache_bridge_[name] = vit->second.bridges;
                    ir_cache_strings_[name] = vit->second.strings;
                }
            } else {
                auto& entry = ir_cache_v2_[name];
                // Issue #4091: keep the FnKey → name side index in sync.
                shape_fnkey_names_.insert_or_assign(shape::make_fn_key(session_id_, name), name);
                entry.source = canonical;
                entry.source_hash = hash;
                entry.dirty = false;
            }
        }
    }

    // Issue #3255: Soft dual-graph parity fail must fail-closed before
    // partial peel. Production/Strict already force-dirties all callers
    // at record_dependency / drain (#3187); Soft only rebuilt. A concurrent
    // fiber / lockless batch that left the graphs divergent can still
    // reach relower_dirty_defines_from_workspace with want_partial=true
    // under an incomplete hybrid cascade cone. Re-check graphs_consistent
    // under shared dep_graph_mtx_ (walk only — no extra exclusive lock
    // when already consistent). On fail: exclusive rebuild, bump
    // dual_dep_graph_parity_fail_total, force-dirty all callers (same
    // walk as #3165/#3187), and set want_partial=false +
    // mark_all_blocks_dirty. Distinguisher: existing
    // partial_forced_full_by_impact_total. Quiet when graphs already
    // consistent (no exclusive lock, no counter bump).
    void fail_closed_soft_dual_graph_parity_before_partial_(IRCacheEntry& entry,
                                                            bool& want_partial) {
        if (!want_partial)
            return;
        bool graphs_ok = true;
        {
            lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                                  lock_order::Level::DepGraph);
            graphs_ok = aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                                 dep_name_to_slot_);
        }
        if (graphs_ok)
            return;
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        if (aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                     dep_name_to_slot_))
            return;
        aura::compiler::dirty::rebuild_node_dep_graph_from_string(node_dep_graph_, dep_graph_,
                                                                  dep_name_to_slot_);
        metrics_.dual_dep_graph_parity_fail_total.fetch_add(1, std::memory_order_relaxed);
        aura::compiler::dirty::g_dual_dep_graph_parity_fail_total_atomic().fetch_add(
            1, std::memory_order_relaxed);
        std::unordered_set<std::string, aura::core::TransparentStringHash, std::equal_to<>>
            affected;
        for (const auto& [callee_name, callee_entry] : dep_graph_) {
            (void)callee_name;
            for (const auto& caller_name : callee_entry.called_by)
                affected.insert(caller_name);
        }
        for (const auto& caller_name : affected) {
            auto cit2 = ir_cache_v2_.find(caller_name);
            if (cit2 != ir_cache_v2_.end()) {
                cit2->second.dirty = true;
                cit2->second.mark_all_blocks_dirty();
                finish_cascade_soa_dirty_sync_(cit2->second);
            }
        }
        want_partial = false;
        entry.mark_all_blocks_dirty();
        entry.dirty = true;
        metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #3255: a Soft NodeId drop can land after the last dirty peel.
    // The helper above returns immediately when want_partial is false, and
    // relower never calls it when the cache is already clean. Dedup
    // record_dependency only remirrors its own edge, so root→leaf stays
    // missing. Restore at the end of every relower. Already-consistent is
    // a shared walk only — no exclusive lock, no counter bump (AC3).
    // Unslotted string edges stay inconsistent under production (#3657);
    // rebuild still skips them.
    void restore_soft_dual_graph_parity_after_relower_() {
        bool graphs_ok = true;
        {
            lock_order::OrderedSharedLock<std::shared_mutex> read(dep_graph_mtx_,
                                                                  lock_order::Level::DepGraph);
            graphs_ok = aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                                 dep_name_to_slot_);
        }
        if (graphs_ok)
            return;
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        if (aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                     dep_name_to_slot_))
            return;
        aura::compiler::dirty::rebuild_node_dep_graph_from_string(node_dep_graph_, dep_graph_,
                                                                  dep_name_to_slot_);
        metrics_.dual_dep_graph_parity_fail_total.fetch_add(1, std::memory_order_relaxed);
        aura::compiler::dirty::g_dual_dep_graph_parity_fail_total_atomic().fetch_add(
            1, std::memory_order_relaxed);
        std::unordered_set<std::string, aura::core::TransparentStringHash, std::equal_to<>>
            affected;
        for (const auto& [callee_name, callee_entry] : dep_graph_) {
            (void)callee_name;
            for (const auto& caller_name : callee_entry.called_by)
                affected.insert(caller_name);
        }
        for (const auto& caller_name : affected) {
            auto cit2 = ir_cache_v2_.find(caller_name);
            if (cit2 != ir_cache_v2_.end()) {
                cit2->second.dirty = true;
                cit2->second.mark_all_blocks_dirty();
                finish_cascade_soa_dirty_sync_(cit2->second);
            }
        }
        metrics_.partial_forced_full_by_impact_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #3615: cone-wide dual-graph parity check. Extends #3486 (which
    // only consulted dirty_names.front()) to walk every name in the cone
    // and detect Soft-erased holes: a root whose dep_graph_[root].called_by
    // is empty but node_dep still has encode_fn_node(callee_slot)
    // dependents — divergent state from a prior Soft invalidate that
    // erased the string edge without rebuilding the node mirror. Same
    // take-full path as fail_closed_soft_dual_graph_parity_before_partial_
    // (rebuild + bump dual_dep_graph_parity_fail_total + force-dirty all
    // callers + partial_forced_full_by_impact_total++ + want_partial=false).
    // Issue #3761: remirror drops node-only callers. Snapshot those
    // dependents into mark_caller_body_dirty + cone_names *before*
    // rebuild_node_dep_graph_from_string (string stays SSOT).
    // Issue #3891: empty-hole arm skipped extra node-only callers when
    // string called_by was nonempty. Peel observer now treats those as
    // Soft-hole too. graphs_consistent stays string→node one-way (string
    // authority; extra node edges do not invert the checker).
    // Caller holds cascade_decision_mtx_ (relower path). Soft / Off skip
    // (zero extra — AC3). No new query key (AC4 — reuses
    // dual_dep_graph_parity_fail_total + partial_forced_full_by_impact_total).
    // String graph stays authority (rebuild_node_dep_graph_from_string).
    // Non-duplicative vs #3188 (facade skipped IR dirty), #3345 (depth-1),
    // #3381 (one-hop union), #3474 (FIFO cone), #3486 (front-only consult),
    // #3165/#3187 (all-callers on record_dependency fail).
    bool
    fail_closed_soft_dual_graph_parity_before_partial_cone_(std::vector<std::string>& cone_names,
                                                            bool& want_partial);

    // Issue #1495: walk dirty ir_cache_v2_ entries and prefer
    // partial re-lower (relower_define_blocks → per-function /
    // per-block) before a full cache_define. Called from
    // populate_ir_cache_v2_from_workspace and (eval-current)
    // so AI mutate:set-body / rebind hot paths actually consume
    // the body-only dirty bitmask instead of always full-lowering.
    //
    // Returns the number of defines successfully partially re-lowered
    // (or skipped-as-clean). Full-fallback count is already on
    // relower_full_called_count.
    std::size_t relower_dirty_defines_from_workspace();

    // Clear the whole EDSL IR cache. Called by --reset-arena / gc.
    void clear_define_cache_v2() { ir_cache_v2_.clear(); }

    // Get cached IR for a define (returns nullptr if not cached).
    // Used by (eval-current) to assemble the IRModule.
    const IRCacheEntry* get_define_v2(const std::string& name) const {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return nullptr;
        return &it->second;
    }

    // Issue #2183 test: inject stale stamp (clean dirty, old mutation/bridge)
    // so lookup_define_v2 must force re-lower without dirty bit.
    bool inject_stale_cache_stamp_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& e = it->second;
        e.dirty = false;
        e.clear_all_block_dirty();
        e.clear_all_instruction_dirty();
        // Force detectable stamp drift even when live mut/bridge are 0:
        //   - mutation_count = 0 (behind any live > 0)
        //   - bridge_epoch = non-zero sentinel != live (always trips
        //     stamp.bridge_epoch != 0 && != current_bridge_epoch)
        constexpr std::uint64_t kStaleBridgeSentinel = 0xDEADBEEFull;
        const auto live_bridge = bridge_epoch();
        const auto stale_bridge = (live_bridge == kStaleBridgeSentinel) ? (kStaleBridgeSentinel - 1)
                                                                        : kStaleBridgeSentinel;
        restamp_cache_entry(e.version_stamp_, /*mut=*/0, stale_bridge,
                            e.version_stamp_.defuse_version, e.version_stamp_.soa_generation);
        e.mutation_count = 0;
        e.last_seen_epoch_ = 0;
        return true;
    }

    // Issue #3481 test: clear dirty bits without a content store / restamp
    // so lookup must still refuse a clean hit (content latch + stamp lag).
    bool clear_ir_cache_dirty_bits_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& e = it->second;
        e.dirty = false;
        e.clear_all_block_dirty();
        e.clear_all_instruction_dirty();
        return true;
    }

    // Issue #4103: dual-shape body-only dirty. Prepends a clean __top__
    // when the stored bundle dropped it, marks only the body, and
    // optionally poisons operand[3] of every function after the body so
    // a restamp of the partial peel is distinguishable from
    // store_define_v2. Clears soa_mod so the SoA gate does not force
    // full on the shape insert. Returns irs.size() (0 if the name is
    // missing).
    // Issue #4311: prepend_synthetic_top=false leaves the production
    // store shape [body, nested] (no inserted __top__). Default true
    // keeps the #4103 oracle.
    std::size_t plant_body_only_dirty_for_test(const std::string& name, bool poison_after_body,
                                               bool prepend_synthetic_top = true) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end() || it->second.irs.empty())
            return 0;
        auto& entry = it->second;
        if (prepend_synthetic_top && entry.irs[0].name != "__top__") {
            aura::ir::IRFunction top;
            top.id = 0;
            top.name = "__top__";
            aura::ir::BasicBlock blk;
            blk.id = 0;
            top.blocks.push_back(std::move(blk));
            entry.irs.insert(entry.irs.begin(), std::move(top));
            entry.invalidate_irs_layout_(); // Issue #4615: synthetic __top__ prepend.
        }
        entry.soa_mod = {};
        entry.init_block_dirty_from_irs();
        entry.init_instruction_dirty_from_irs();
        entry.clear_all_block_dirty();
        entry.clear_all_instruction_dirty();
        (void)entry.mark_body_only_dirty();
        if (poison_after_body) {
            // __top__ prefix: body at 1, poison nested at >=2 (unchanged
            // #4103 oracle). Dropped __top__: body at 0, poison >=1.
            const std::size_t poison_from =
                (!entry.irs.empty() && entry.irs[0].name == "__top__") ? 2 : 1;
            for (std::size_t fi = poison_from; fi < entry.irs.size(); ++fi) {
                auto& fn = entry.irs[fi];
                if (fn.blocks.empty()) {
                    aura::ir::BasicBlock blk;
                    blk.id = 0;
                    fn.blocks.push_back(std::move(blk));
                }
                if (fn.blocks[0].instructions.empty()) {
                    aura::ir::IRInstruction ins;
                    ins.opcode = aura::ir::IROpcode::Nop;
                    ins.operands[3] = 0x4103u;
                    fn.blocks[0].instructions.push_back(ins);
                } else {
                    fn.blocks[0].instructions[0].operands[3] = 0x4103u;
                }
            }
            entry.invalidate_irs_layout_(); // Issue #4615: poison may add blocks/instructions.
        }
        return entry.irs.size();
    }

    // Issue #4103: drive relower_define_blocks from the workspace define
    // whose name matches, using the cached source string.
    bool relower_define_from_workspace_for_test(const std::string& name) {
        auto* flat = evaluator_.workspace_flat();
        auto* pool = evaluator_.workspace_pool();
        if (!flat || !pool)
            return false;
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        const std::string source = it->second.source;
        aura::ast::NodeId lambda = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            auto v = flat->get(id);
            if (v.tag != aura::ast::NodeTag::Define || v.sym_id == aura::ast::INVALID_SYM)
                continue;
            if (std::string(pool->resolve(v.sym_id)) != name || v.children.empty())
                continue;
            auto body = v.child(0);
            if (body < flat->size() && flat->get(body).tag == aura::ast::NodeTag::Lambda)
                lambda = body;
            break;
        }
        if (lambda == aura::ast::NULL_NODE)
            return false;
        return relower_define_blocks(name, source, *flat, *pool, lambda);
    }

    // Issue #4436: full-arm relower of the live workspace body.
    // commit_per_fn_partial=false so a body-only dirty bundle cannot
    // return from the per-fn arm. Live unparse, not the cached source.
    // relower_define_from_workspace_for_test keeps its default.
    bool relower_current_define_full_for_test(const std::string& name) {
        auto* flat = evaluator_.workspace_flat();
        auto* pool = evaluator_.workspace_pool();
        if (!flat || !pool)
            return false;
        if (ir_cache_v2_.find(name) == ir_cache_v2_.end())
            return false;
        aura::ast::NodeId lambda = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (flat->is_free_slot(id))
                continue;
            auto v = flat->get(id);
            if (v.tag != aura::ast::NodeTag::Define || v.sym_id == aura::ast::INVALID_SYM)
                continue;
            if (std::string(pool->resolve(v.sym_id)) != name || v.children.empty())
                continue;
            auto body = v.child(0);
            if (body < flat->size() && flat->get(body).tag == aura::ast::NodeTag::Lambda)
                lambda = body;
            break;
        }
        if (lambda == aura::ast::NULL_NODE)
            return false;
        const std::string canonical = workspace_define_canonical_source(*flat, *pool, name, lambda);
        return relower_define_blocks(name, canonical, *flat, *pool, lambda,
                                     /*commit_per_fn_partial=*/false);
    }

    // Issue #4436: #4228 registers a non-high-bit lambda closure as a
    // value cell, so a later lower emits TopCellLoad and never copies
    // ir_cache_. The embed hole is the function-cache copy. Tests drop
    // the cells to reach that copy.
    void clear_ir_value_cells_for_test() { ir_value_cell_bindings_.clear(); }

    // Issue #4681: whether `name` is registered as a call slot, i.e. its
    // top-level lambda references lower through a live TopCellLoad. Tests
    // use it to pick the slot face vs the embed face deterministically.
    bool has_call_slot_for_test(const std::string& name) const {
        return ir_call_slots_4681_.count(name) > 0;
    }

    // Issue #4528: simulate the orphan producer — erase ONLY the name's
    // ir_cache_v2_ row (what pre-#4528 eviction did), leaving the v1 /
    // bridge / strings rows holding the pre-mutate body. Returns true if
    // a row was erased.
    bool erase_ir_cache_v2_row_for_test(const std::string& name) {
        return ir_cache_v2_.erase(name) > 0;
    }

    // Issue #3484 test: persist-empty / body-only mark missed. Keep the
    // cache entry so #3381 still unions the caller into dirty_names, but
    // drop irs so mark_caller_body_dirty hits the empty-irs path and
    // does not set dirty. Production peel must fail-closed full, not
    // ++ok skip. Soft does not union, so this plant is a no-op there.
    bool plant_zero_mask_caller_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& e = it->second;
        e.irs.clear();
        e.invalidate_irs_layout_(); // Issue #4615
        e.block_dirty_per_func_.clear();
        e.instruction_dirty_per_func_.clear();
        e.dirty = false;
        e.clear_all_block_dirty();
        e.clear_all_instruction_dirty();
        return true;
    }

    // Issue #2183 test: force restamp to live counters (monotonic check).
    // Issue #4341: passes the epoch explicitly — sample-at-call == live
    // for this synchronous helper.
    bool restamp_cache_entry_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        restamp_cache_entry_live_(it->second, aura::core::current_mutation_epoch(), name);
        ack_peer_ir_stale_on_restamp_(it->second, name);
        // Issue #3136: success-path bitmap coherence (see store_ir_cache_v2).
        if (aura_production_defaults_active_probe() != 0) {
            hot_update_registry().note_relower_success_coverage(1ULL << (fnv1a_64(name) & 63));
            // Issue #3229: define-id side set so a 6-bit collision does
            // not cover a peer. Soft skipped by the probe above.
            hot_update_registry().note_relower_success_define(relower_success_define_id(name));
        }
        return true;
    }

    // Issue #2181 test helper: mark block 0 dirty and clear instruction
    // dirty bits on the cached SoA module so partial entry sees desync.
    // Returns true if a desync was successfully injected.
    // When dual-emit left soa_mod empty, synthesizes a minimal SoA skeleton
    // from AoS irs so the desync gate remains hard-testable.
    bool inject_soa_dirty_desync_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& entry = it->second;
        if (entry.soa_mod.functions.empty()) {
            if (entry.irs.empty())
                return false;
            // Synthesize skeleton: one SoA fn/block range per AoS block so
            // block↔instr desync is well-defined without dual-emit.
            for (const auto& aos_fn : entry.irs) {
                const auto fi = entry.soa_mod.add_function(aos_fn.name, aos_fn.local_count);
                for (const auto& blk : aos_fn.blocks) {
                    const auto bi = entry.soa_mod.add_block(fi);
                    const auto n = static_cast<std::uint32_t>(
                        blk.instructions.empty() ? 1 : blk.instructions.size());
                    for (std::uint32_t k = 0; k < n; ++k)
                        (void)entry.soa_mod.add_instruction(fi, aura::ir::IROpcode::ConstI64);
                    entry.soa_mod.seal_block(fi, bi);
                    auto& soa_fn = entry.soa_mod.functions[fi];
                    if (soa_fn.block_dirty_.size() <= bi)
                        soa_fn.block_dirty_.resize(bi + 1, 0);
                }
                auto& soa_fn = entry.soa_mod.functions[fi];
                if (soa_fn.blocks_.empty()) {
                    const auto bi = entry.soa_mod.add_block(fi);
                    (void)entry.soa_mod.add_instruction(fi, aura::ir::IROpcode::ConstI64);
                    entry.soa_mod.seal_block(fi, bi);
                    soa_fn.block_dirty_.assign(1, 0);
                }
            }
        }
        if (entry.soa_mod.functions.empty())
            return false;
        auto& fn = entry.soa_mod.functions[0];
        if (fn.blocks_.empty())
            return false;
        // Ensure block 0 has a non-empty instruction range so clearing
        // instruction_dirty_ produces a real desync.
        if (fn.blocks_[0].start_idx >= fn.blocks_[0].end_idx) {
            (void)entry.soa_mod.add_instruction(0, aura::ir::IROpcode::ConstI64);
            fn.blocks_[0].end_idx = static_cast<std::uint32_t>(fn.size());
        }
        // Issue #3355: test helper uses batch-of-1 (production TU must not
        // call IRFunctionSoA::mark_block_dirty).
        const std::uint32_t one[] = {0};
        fn.mark_blocks_dirty(one);
        // Also keep AoS bit set so force_soa mirrors the dirty story.
        if (entry.block_dirty_per_func_.empty())
            entry.block_dirty_per_func_.resize(1);
        if (entry.block_dirty_per_func_[0].empty())
            entry.block_dirty_per_func_[0].resize(fn.blocks_.size(), 0);
        if (!entry.block_dirty_per_func_[0].empty())
            entry.block_dirty_per_func_[0][0] = 1;
        for (auto& b : fn.instruction_dirty_)
            b = 0;
        return entry.soa_mod.count_block_instr_dirty_desync() > 0;
    }

    // Issue #2181 test: run partial gate on a define; returns gate result.
    // true = partial allowed; false = force full.
    [[nodiscard]] bool gate_partial_soa_dirty_sync_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return true;
        return gate_partial_soa_dirty_sync_(it->second);
    }

    // Issue #2206 test: inject a stale source_to_ir_map entry (invalid block)
    // so partial-path recovery is hard-testable. Also marks block 0 dirty so
    // dirty_count > 0 and should_partial_relower remains true after recover
    // when below threshold. Returns true when a desync was successfully injected.
    bool inject_source_to_ir_map_desync_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end() || it->second.irs.empty())
            return false;
        auto& entry = it->second;
        ensure_source_to_ir_map_(entry);
        if (entry.source_to_ir_map.empty()) {
            // Stamp a synthetic mapping if lower left the map empty.
            rebuild_source_to_ir_map_from_irs(entry.irs, entry.source_to_ir_map);
        }
        if (entry.source_to_ir_map.empty()) {
            // Force at least one reverse entry so corruption is observable.
            entry.source_to_ir_map[1] = SourceIrLoc{/*fi*/ 0, /*bi*/ 0, /*ii*/ 0};
        }
        // Corrupt first map entry to an out-of-range block index.
        auto mit = entry.source_to_ir_map.begin();
        mit->second = SourceIrLoc{/*fi*/ 0, /*bi*/ 99, /*ii*/ 0};
        // Keep a small dirty surface so partial stays eligible after recover.
        if (entry.block_dirty_per_func_.empty())
            entry.block_dirty_per_func_.resize(entry.irs.size());
        if (entry.block_dirty_per_func_[0].empty())
            entry.block_dirty_per_func_[0].assign(1, 0);
        entry.block_dirty_per_func_[0][0] = 1;
        entry.dirty = true;
        return !source_to_ir_map_is_consistent(entry.irs, entry.source_to_ir_map);
    }

    // Issue #2206 test: run the same recover path as try_partial_invalidate_relower.
    // Returns true when map is consistent after recovery (partial may continue).
    [[nodiscard]] bool recover_source_to_ir_desync_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return true;
        auto& entry = it->second;
        if (source_to_ir_map_is_consistent(entry.irs, entry.source_to_ir_map))
            return true;
        const auto bad = count_source_to_ir_map_inconsistencies(entry.irs, entry.source_to_ir_map);
        if (bad > 0)
            metrics_.source_to_ir_map_inconsistency_total.fetch_add(bad, std::memory_order_relaxed);
        std::vector<std::size_t> preferred;
        preferred.reserve(entry.block_dirty_per_func_.size());
        for (std::size_t fi = 0; fi < entry.block_dirty_per_func_.size(); ++fi) {
            if (entry.func_dirty_block_count(fi) > 0)
                preferred.push_back(fi);
        }
        auto rec = recover_source_to_ir_map_desync(entry.irs, entry.source_to_ir_map, preferred,
                                                   /*force_full_rebuild=*/entry.abort_map_invalid);
        if (rec.funcs_patched > 0) {
            metrics_.source_to_ir_desync_funcs_patched.fetch_add(
                static_cast<std::uint64_t>(rec.funcs_patched), std::memory_order_relaxed);
            metrics_.source_to_ir_map_patch_total.fetch_add(
                static_cast<std::uint64_t>(rec.funcs_patched), std::memory_order_relaxed);
        }
        if (rec.used_full_rebuild)
            metrics_.source_to_ir_map_rebuild_total.fetch_add(1, std::memory_order_relaxed);
        if (rec.recovered) {
            metrics_.source_to_ir_desync_recovered_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.source_to_ir_map_consistent_checks_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        }
        return rec.recovered;
    }

    // Issue #3068 test: drop instr precision on one reverse-index entry
    // (prefer CastOp) so missing-loc prepare must recover or force full.
    bool inject_source_to_ir_map_drop_instr_loc_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end() || it->second.irs.empty())
            return false;
        auto& entry = it->second;
        ensure_source_to_ir_map_(entry);
        if (entry.source_to_ir_map.empty())
            rebuild_source_to_ir_map_from_irs(entry.irs, entry.source_to_ir_map);
        SourceToIrMap::iterator drop = entry.source_to_ir_map.end();
        for (auto mit = entry.source_to_ir_map.begin(); mit != entry.source_to_ir_map.end();
             ++mit) {
            if (!mit->second.has_instr())
                continue;
            const auto fi = mit->second.function_index;
            const auto bi = mit->second.block_index;
            const auto ii = mit->second.instr_index;
            if (fi >= entry.irs.size() || bi >= entry.irs[fi].blocks.size() ||
                ii >= entry.irs[fi].blocks[bi].instructions.size())
                continue;
            if (entry.irs[fi].blocks[bi].instructions[ii].opcode == aura::ir::IROpcode::CastOp) {
                drop = mit;
                break;
            }
            if (drop == entry.source_to_ir_map.end())
                drop = mit;
        }
        if (drop == entry.source_to_ir_map.end())
            return false;
        drop->second.instr_index = UINT32_MAX;
        if (entry.block_dirty_per_func_.empty())
            entry.block_dirty_per_func_.resize(entry.irs.size());
        if (!entry.block_dirty_per_func_.empty() && entry.block_dirty_per_func_[0].empty())
            entry.block_dirty_per_func_[0].assign(1, 0);
        if (!entry.block_dirty_per_func_.empty() && !entry.block_dirty_per_func_[0].empty())
            entry.block_dirty_per_func_[0][0] = 1;
        entry.dirty = true;
        return source_to_ir_map_missing_instr_loc(entry.irs, entry.source_to_ir_map);
    }

    // Issue #3068 test: same prepare used by the production decision.
    [[nodiscard]] bool prepare_source_to_ir_map_for_partial_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return true;
        return prepare_source_to_ir_map_for_partial_(it->second);
    }

    // Get all cached defines' names (for the (eval-current) full-lower fallback).
    std::vector<std::string> list_cached_defines_v2() const {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        std::vector<std::string> out;
        out.reserve(ir_cache_v2_.size());
        for (auto& [name, _] : ir_cache_v2_)
            out.push_back(name);
        return out;
    }

    // FNV-1a 64-bit hash, for stable source canonicalization.
    // Public so caller-side debugging can compute the same hash.
    static std::size_t fnv1a_64(const std::string& s) {
        std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
        for (unsigned char c : s) {
            h ^= c;
            h *= ::aura::compiler::stats::kFnvPrime;
        }
        return static_cast<std::size_t>(h);
    }

    // Unload a module: reset its arena and remove cached defines.
    // Does NOT remove evaluator env bindings (they persist for the session).
    // Issue #3762: production incremental serve is ir_cache_v2_ +
    // lookup_define_v2. V1-era erase of ir_cache_ / jit_cache_ / string
    // dep_graph_ left V2 callers as clean hits of the unloaded encoding
    // and left node_dep_graph_ with extra edges (graphs_consistent is
    // one-way). Snapshot called_by before string erase, drop V2, remirror
    // node_dep from the post-unload string graph, mark remaining callers
    // dirty. Soft/Off tear V2 down too (AC3).
    void unload_module(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_);        // #4717 shared IR/closure cache
        aura::diag::unbind_source_file_flats(name); // Issue #4916
        arena_group_.reset_module(name);

        std::vector<std::string> to_remove;
        if (auto it = module_functions_.find(name); it != module_functions_.end()) {
            to_remove = it->second;
            module_functions_.erase(it);
        }

        using lock_order::Level;
        using lock_order::OrderedUniqueLock;
        OrderedUniqueLock<std::shared_mutex> mutate_lock =
            OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mutate_mtx_, Level::Mutate);

        std::unordered_set<std::string> callers_to_dirty;
        {
            OrderedUniqueLock<std::shared_mutex> dep_write(dep_graph_mtx_, Level::DepGraph);
            std::unordered_set<std::string> removing(to_remove.begin(), to_remove.end());
            for (const auto& fname : to_remove) {
                auto dit = dep_graph_.find(fname);
                if (dit == dep_graph_.end())
                    continue;
                const auto callers = dit->second.called_by;
                const auto callees = dit->second.calls;
                dep_graph_.erase(dit);
                for (const auto& caller : callers) {
                    if (!removing.count(caller))
                        callers_to_dirty.insert(caller);
                    auto cit = dep_graph_.find(caller);
                    if (cit != dep_graph_.end()) {
                        auto& v = cit->second.calls;
                        v.erase(std::remove(v.begin(), v.end(), fname), v.end());
                    }
                }
                for (const auto& callee : callees) {
                    auto git = dep_graph_.find(callee);
                    if (git == dep_graph_.end())
                        continue;
                    auto& cb = git->second.called_by;
                    cb.erase(std::remove(cb.begin(), cb.end(), fname), cb.end());
                }
            }
            aura::compiler::dirty::rebuild_node_dep_graph_from_string(node_dep_graph_, dep_graph_,
                                                                      dep_name_to_slot_);
            dep_graph_generation_.fetch_add(1, std::memory_order_release);
            metrics_.dep_graph_generation_total.fetch_add(1, std::memory_order_relaxed);
        }

        for (auto& fname : to_remove) {
            ir_cache_.erase(fname);
            ir_cache_bridge_.erase(fname);
            ir_cache_strings_.erase(fname);
            // Issue #4091: drop the FnKey → name side index row.
            shape_fnkey_names_.erase(shape::make_fn_key(session_id_, fname));
            ir_cache_v2_.erase(fname);
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                jit_cache_.erase(fname);
            }
            function_sources_.erase(fname);
        }
        for (const auto& caller : callers_to_dirty) {
            auto cit = ir_cache_v2_.find(caller);
            if (cit != ir_cache_v2_.end()) {
                (void)cit->second.mark_caller_body_dirty();
                finish_cascade_soa_dirty_sync_(cit->second);
            }
        }

        loaded_modules_.erase(name);
        module_states_.erase(name);

        // Remove disk cache (find by name prefix, any hash)
        auto sanitized = name;
        for (auto& c : sanitized) {
            if (c == '/' || c == '\\' || c == ':' || c == ' ')
                c = '_';
        }
        if (sanitized.empty())
            sanitized = "__default__";
        auto dir = module_cache_dir();
        if (!dir.empty() && module_cache_dir_trusted(dir)) {
            try {
                for (auto& entry : std::filesystem::directory_iterator(dir)) {
                    auto fn = entry.path().filename().string();
                    if (fn.starts_with(sanitized) && fn.ends_with(".abfc")) {
                        aura::compiler::cache::remove_cache(entry.path().string());
                    }
                }
            } catch (...) {
                // [SILENCE-PRIM-#615] directory_iterator / cache unlink
                // best-effort; always fall through to legacy path remove
                // (#1669 class B intentional-state).
            }
            // Also try without hash (legacy format)
            aura::compiler::cache::remove_cache(dir + sanitized + ".abfc");
        }
    }

    // Check if a module is loaded
    bool is_module_loaded(const std::string& name) const { return loaded_modules_.count(name) > 0; }

    // List loaded module names
    std::vector<std::string> loaded_modules() const {
        std::vector<std::string> result;
        for (auto& n : loaded_modules_)
            result.push_back(n);
        return result;
    }

    // ---- Diagnostics ------------------------------------------------

    ast::ArenaStats memory_stats() const {
        auto s = arena_.stats();
        s.merge(arena_group_.total_stats());
        return s;
    }

    std::vector<std::pair<std::string, ast::ArenaStats>> module_memory_stats() const {
        return arena_group_.module_stats();
    }

    // ---- Hot swap (M2.6) ----------------------------------------------

    EvalResult hot_swap(std::string_view new_code) {
        if (!last_ir_mod_) {
            // No cache yet — seed it with a regular eval_ir first
            return eval_ir(new_code);
        }
        // Issue #4936: one checked deref; the cache is seeded above.
        auto& live_ir_mod = *last_ir_mod_;

        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(new_code, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat.root = pr.root;

        auto new_mod = aura::compiler::lower_to_ir(flat, pool, arena_, &evaluator_.primitives(),
                                                   &type_registry_);

        // Hot-swap each function from new_mod into the cached module
        for (auto& new_func : new_mod.functions) {
            auto func_id = new_func.id;
            if (func_id < live_ir_mod.functions.size()) {
                new_func.id = func_id;
                live_ir_mod.functions[func_id] = std::move(new_func);
            } else {
                live_ir_mod.functions.push_back(std::move(new_func));
            }
        }
        live_ir_mod.entry_function_id = new_mod.entry_function_id;

        // Re-run passes on the hot-swapped module
        TypeSpecializationWrap ts(&type_registry_);
        TypePropagationPass tprop(&type_registry_);
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        // Issue #1418: DCE on hot-swap re-lower so evolve! /
        // cache_define paths don't leave dead CastOps in the
        // live IR module.
        // Issue #1457: TypePropagation before DCE.
        DeadCoercionEliminationPass dce(&type_registry_);
        const std::uint64_t pipeline_epoch = aura::core::current_mutation_epoch();
        ts.set_pipeline_epoch(pipeline_epoch);
        tprop.set_pipeline_epoch(pipeline_epoch);
        ar.set_pipeline_epoch(pipeline_epoch);
        cf.set_pipeline_epoch(pipeline_epoch);
        dce.set_pipeline_epoch(pipeline_epoch);
        // Issue #163: run_pipeline (Pass concept fold) replaces
        // the individual *.run() calls. Issue #1418: include DCE.
        // Issue #3329: production purity / SoA / DirtyPropagator gate.
        // Issue #4007: hot-swap still restamps the cached AoS module;
        // named factory stays the V2 PureWrap pack.
        aura::compiler::run_production_pipeline(live_ir_mod, ts, tprop, ck, ar, cf, dce);
        accumulate_type_propagation_metrics(tprop);
        accumulate_coercion_pass_metrics(ts, dce);

        if (ar.has_error()) {
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        // Issue #253: accumulate linear-move elision count.
        if (ts.linear_elide_count() > 0) {
            metrics_.linear_elide_count.fetch_add(ts.linear_elide_count(),
                                                  std::memory_order_relaxed);
        }

        aura::compiler::IRContext ctx(evaluator_.primitives(), &type_registry_, &metrics_,
                                      &evaluator_);
        aura::compiler::IRInterpreter ir_interp(live_ir_mod, ctx);
        ir_interp.set_strategy(strategy_);
        if (strict_mode_)
            ir_interp.set_strict_mode(true);
        auto result = ir_interp.execute();

        last_cells_ = ir_interp.list_cells();
        last_closures_ = ir_interp.list_closures();
        return result;
    }

    // ---- Runtime reflection (M3 Phase 2) ------------------------------

    // Closures persisted from last IR execution
    std::vector<aura::compiler::ClosureSnapshot> last_closures() const { return last_closures_; }
    std::vector<aura::compiler::CellSnapshot> last_cells() const { return last_cells_; }
    const aura::compiler::EvalStrategy& strategy() const { return strategy_; }
    void set_strategy(const aura::compiler::EvalStrategy& s) { strategy_ = s; }

    // ---- Module caching (for on_module_loaded callback) ---------------

    // Parse module content and cache all top-level defines in ir_cache_.
    // Called by Evaluator after each successful module load.
    void cache_module(const std::string& content, const std::string& path);

    // ---- Issue #272: IR-native define env binding --------------------

    // ── Issue #4682 (6b): background native tier for hot workspace defines ──
    // Default-on; AURA_TIER_JIT=0 disables it. A workspace Lambda
    // define bound via IR (IRDefineEnvBinding) counts the calls that reach
    // it through dispatch_ir_define_closure. At AURA_TIER_JIT_HOT
    // calls (default 2) an eligible body is snapshotted on the calling
    // thread and handed to a background worker, which compiles it with a
    // private AuraJIT instance (the worker is the only thread that ever
    // touches that instance) and publishes the ScalarFn through an atomic
    // slot. Later calls run the native body when the entry guards hold;
    // everything else keeps the IR interpreter.
    //
    // Eligibility is deliberately narrow so the native body is pure and
    // exact: fixnum/bool arithmetic, compares, branches, and calls whose
    // callee is the define's own top cell (direct self-recursion, #4601).
    // No allocation, cells, closures, strings, floats, primitives or I/O.
    // A pure body can be re-run on the interpreter, so any guard exit,
    // deopt sentinel, non-fixnum/bool result or a moved self cell simply
    // discards the native result and interprets the call.
    //
    // Invalidation: a rebind / set-code replaces the IRDefineEnvBinding, so
    // the new binding starts with a fresh slot; an in-flight compile only
    // fills the orphaned slot it holds. Native code is never unmapped while
    // the worker lives (unique symbol per compile), so a body that is still
    // on the stack stays valid.
    struct TierSlot4682 {
        std::atomic<aura::jit::ScalarFn> fn{nullptr};
        // 0 counting, 1 queued, 2 ready, 3 ineligible / failed
        std::atomic<int> state{0};
        std::atomic<std::uint64_t> calls{0};
        std::uint32_t local_count = 0;
        std::uint32_t arg_count = 0;
        std::uint32_t self_cell = UINT32_MAX; // evaluator cell the body reads for self
    };
    struct TierJob4682 {
        std::shared_ptr<TierSlot4682> slot;
        std::vector<std::vector<aura::jit::FlatInstruction>> instrs;
        std::vector<aura::jit::FlatBlock> blocks;
        std::string name;
        aura::jit::FlatFunction fn{};
        // Self-calls the body makes; each must lower to the direct native
        // self-call (#4601). A generic aura_closure_call on an IR closure
        // word is not routable natively, so a shortfall refuses the code.
        std::uint64_t self_calls = 0;
    };
    struct TierStats4682 {
        std::atomic<std::uint64_t> queued{0}, compiled{0}, failed{0}, ineligible{0},
            native_calls{0}, fallbacks{0};
    };
    class TierWorker4682 {
    public:
        TierWorker4682() = default;
        TierWorker4682(const TierWorker4682&) = delete;
        TierWorker4682& operator=(const TierWorker4682&) = delete;
        TierWorker4682(TierWorker4682&&) = delete;
        TierWorker4682& operator=(TierWorker4682&&) = delete;
        ~TierWorker4682() { stop_and_join(); }
        // Issue #4705: stop the worker AND join it. join() waits for an
        // in-flight compile to finish, so once this returns no background
        // thread is still inside LLVM.
        void stop_and_join() noexcept {
            {
                std::scoped_lock<std::mutex> lk(mu_);
                stop_ = true;
            }
            cv_.notify_all();
            if (th_.joinable())
                th_.join();
        }
        void enqueue(std::unique_ptr<TierJob4682> job, TierStats4682& stats) {
            std::scoped_lock<std::mutex> lk(mu_);
            if (stop_)
                return;
            stats_ = &stats;
            q_.push_back(std::move(job));
            if (!th_.joinable())
                th_ = std::thread([this] { run_(); });
            cv_.notify_one();
        }
        // Test / bench hook: wait until the queue is drained.
        void drain() {
            std::unique_lock<std::mutex> lk(mu_);
            idle_cv_.wait(lk, [this] { return q_.empty() && !busy_; });
        }

    private:
        void run_() {
            // A private JIT must not reset the live process runtime (its
            // init() clears every hash table / string pool otherwise).
            aura_jit_suppress_runtime_reset_this_thread(1);
            std::unique_ptr<aura::jit::AuraJIT> jit;
            for (;;) {
                std::unique_ptr<TierJob4682> job;
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                    if (stop_)
                        break;
                    job = std::move(q_.front());
                    q_.pop_front();
                    busy_ = true;
                }
                if (!jit)
                    jit = std::make_unique<aura::jit::AuraJIT>();
                for (std::size_t i = 0; i < job->blocks.size(); ++i)
                    job->blocks[i].instructions = job->instrs[i].data();
                job->fn.name = job->name.c_str();
                job->fn.blocks = job->blocks.data();
                job->fn.num_blocks = static_cast<std::uint32_t>(job->blocks.size());
                aura::jit::ScalarFn p = nullptr;
                bool raw_ok = false;
                std::uint64_t lowered_n = 0;
                // Content-addressed reuse: a body whose flat form was already
                // compiled by this worker (rebind back to an earlier body,
                // set-code reload) reuses that native code. The key is the
                // full codegen input, so a hit is exact; names differ per
                // job and the worker never frees code, so the pointer stays
                // valid.
                std::string key = code_key_(job->fn);
                auto hit = code_cache_.find(key);
                if (hit != code_cache_.end()) {
                    p = hit->second.fn;
                    raw_ok = true;
                    lowered_n = hit->second.self_lowered;
                    if (lowered_n < job->self_calls)
                        p = nullptr;
                    if (tier_jit_trace_4682_())
                        std::fprintf(stderr, "[tier4682] %s cache hit\n", job->name.c_str());
                } else {
                    // available() is false until the first compile() runs init().
                    const auto before =
                        jit->metrics().l1_self_call_total.load(std::memory_order_relaxed);
                    const auto uh0 = jit->metrics().unhandled_opcode_count.load();
                    const auto vf0 = jit->metrics().verify_fail_count.load();
                    p = jit->compile(job->fn);
                    if (!p && tier_jit_trace_4682_())
                        std::fprintf(stderr, "[tier4682] compile fail unhandled+%llu verify+%llu\n",
                                     static_cast<unsigned long long>(
                                         jit->metrics().unhandled_opcode_count.load() - uh0),
                                     static_cast<unsigned long long>(
                                         jit->metrics().verify_fail_count.load() - vf0));
                    const auto lowered =
                        jit->metrics().l1_self_call_total.load(std::memory_order_relaxed) - before;
                    raw_ok = p != nullptr;
                    lowered_n = lowered;
                    if (p)
                        code_cache_.emplace(std::move(key), CachedCode_{p, lowered});
                    if (p && lowered < job->self_calls)
                        p = nullptr;
                }
                if (tier_jit_trace_4682_())
                    std::fprintf(stderr, "[tier4682] %s %s (raw=%d self_lowered=%llu/%llu)\n",
                                 job->name.c_str(), p ? "compiled" : "refused", int(raw_ok),
                                 static_cast<unsigned long long>(lowered_n),
                                 static_cast<unsigned long long>(job->self_calls));
                if (p) {
                    job->slot->fn.store(p, std::memory_order_release);
                    job->slot->state.store(2, std::memory_order_release);
                    if (stats_)
                        stats_->compiled.fetch_add(1, std::memory_order_relaxed);
                } else {
                    job->slot->state.store(3, std::memory_order_release);
                    if (stats_)
                        stats_->failed.fetch_add(1, std::memory_order_relaxed);
                }
                {
                    std::scoped_lock<std::mutex> lk(mu_);
                    busy_ = false;
                }
                idle_cv_.notify_all();
            }
            // Code owned by `jit` is unmapped here, after the loop: the
            // worker only stops when the service is being destroyed.
            {
                std::scoped_lock<std::mutex> lk(mu_);
                busy_ = false;
            }
            idle_cv_.notify_all();
        }
        struct CachedCode_ {
            aura::jit::ScalarFn fn;
            std::uint64_t self_lowered;
        };
        // Every FlatFunction / FlatInstruction field that reaches codegen,
        // except the per-job symbol name and provenance.
        static std::string code_key_(const aura::jit::FlatFunction& fn) {
            std::vector<std::uint32_t> k;
            k.reserve(16);
            k.insert(k.end(),
                     {fn.entry_block, fn.local_count, fn.arg_count, fn.num_blocks, fn.num_callees,
                      fn.region, fn.linear_ownership_state, fn.source_marker, fn.env_count,
                      static_cast<std::uint32_t>(fn.func_id_map != nullptr),
                      static_cast<std::uint32_t>(fn.const_tags != nullptr),
                      static_cast<std::uint32_t>(fn.shape_map != nullptr),
                      static_cast<std::uint32_t>(fn.escape_map != nullptr)});
            for (std::uint32_t i = 0; fn.func_id_map && i < fn.num_callees; ++i)
                k.push_back(fn.func_id_map[i]);
            for (std::uint32_t i = 0; i < fn.local_count; ++i) {
                k.push_back((fn.const_tags ? fn.const_tags[i] : 0u) |
                            (fn.shape_map ? std::uint32_t{fn.shape_map[i]} << 8 : 0u) |
                            (fn.escape_map ? std::uint32_t{fn.escape_map[i]} << 16 : 0u));
            }
            for (std::uint32_t b = 0; b < fn.num_blocks; ++b) {
                const auto& blk = fn.blocks[b];
                k.push_back(blk.id);
                k.push_back(blk.num_instructions);
                for (std::uint32_t i = 0; i < blk.num_instructions; ++i) {
                    const auto& in = blk.instructions[i];
                    k.insert(k.end(), {in.opcode, in.ops[0], in.ops[1], in.ops[2], in.ops[3],
                                       in.shape_id, in.narrow_evidence, in.type_id,
                                       std::uint32_t{in.linear_ownership_state} |
                                           (std::uint32_t{in.dirty} << 8) |
                                           (std::uint32_t{in.source_marker} << 16)});
                }
            }
            return std::string(reinterpret_cast<const char*>(k.data()),
                               k.size() * sizeof(std::uint32_t));
        }
        std::unordered_map<std::string, CachedCode_> code_cache_; // worker thread only
        std::mutex mu_;
        std::condition_variable cv_;
        std::condition_variable idle_cv_;
        std::deque<std::unique_ptr<TierJob4682>> q_;
        bool stop_ = false;
        bool busy_ = false;
        TierStats4682* stats_ = nullptr;
        std::thread th_;
    };
    static bool tier_jit_enabled_4682_() noexcept {
        static const bool on = [] {
            // Default-on (#4679); AURA_TIER_JIT=0 opts out.
            const char* e = ::aura::config::raw(::aura::config::Key::AURA_TIER_JIT);
            return !(e && e[0] == '0');
        }();
        return on;
    }
    static std::uint64_t tier_jit_hot_4682_() noexcept {
        static const std::uint64_t n = [] {
            const char* e = ::aura::config::raw(::aura::config::Key::AURA_TIER_JIT_HOT);
            if (!e || !e[0])
                return std::uint64_t{2};
            const long long v = std::atoll(e);
            return v < 1 ? std::uint64_t{1} : static_cast<std::uint64_t>(v);
        }();
        return n;
    }
    static bool tier_jit_sync_4682_() noexcept {
        // AURA_TIER_JIT_SYNC=1: wait for the worker right after queueing
        // (deterministic tests; the compile still runs on the worker thread).
        static const bool on = [] {
            const char* e = ::aura::config::raw(::aura::config::Key::AURA_TIER_JIT_SYNC);
            return e && e[0] == '1';
        }();
        return on;
    }
    static bool tier_jit_trace_4682_() noexcept {
        static const bool on = [] {
            const char* e = ::aura::config::raw(::aura::config::Key::AURA_TIER_JIT_TRACE);
            return e && e[0] == '1';
        }();
        return on;
    }

    struct IRDefineEnvBinding {
        // Issue #4717 (P4): the interpreter below owns mutable execution
        // state (frames, cells, runtime_closures_, id cursor). It is not
        // reentrant and it is not per-thread, so a call into it must be
        // serialized while the registry entry itself stays replaceable.
        // Taken WITH the IR cache gate released (never both at once — see
        // ir_cache_gate_ ) so a running body never blocks a compile and a
        // compile never blocks a call for longer than one dispatch.
        std::recursive_mutex exec_mtx;
        aura::ir::IRModule module;
        aura::compiler::IRContext context;
        std::unique_ptr<aura::compiler::IRInterpreter> interpreter;
        types::EvalValue bound_closure{};
        aura::compiler::ClosureId closure_id = 0;
        // Issue #4626: the closure-id floor of this binding's runtime
        // mints - the interpreter's id cursor when its first call started.
        // Ids at or above the floor were minted during a call; the
        // bind-time execute() mint is below it. Unset = the binding never
        // ran a call, so it owns no runtime mint.
        std::optional<std::uint64_t> runtime_mint_floor;
        // Issue #4682 (6b): native tier slot (null until the first counted call).
        std::shared_ptr<TierSlot4682> tier;

        IRDefineEnvBinding(aura::ir::IRModule mod, aura::compiler::Primitives& prim,
                           const aura::core::TypeRegistry* reg,
                           aura::compiler::CompilerMetrics* metrics, Evaluator* eval)
            : module(std::move(mod))
            , context(prim, reg, metrics, eval) {}
    };

    // Issue #4681: memoised SymId -> lowest live Define NodeId, rebuilt from
    // the maintained tag index when the flat / its mutation log moves.
    // AURA_DEFINE_LOOKUP_INDEX=0 restores the per-name O(N) scan.
    static bool define_lookup_enabled_4681_() noexcept {
        static const bool on = [] {
            const char* e = ::aura::config::raw(::aura::config::Key::AURA_DEFINE_LOOKUP_INDEX);
            return !(e && e[0] == '0');
        }();
        return on;
    }
    aura::ast::NodeId first_define_for_sym_4681_(aura::ast::FlatAST& f, aura::ast::SymId sid) {
        // Issue #4682 step 2: persistent FlatAST define index (no per-
        // mutation memo rebuild).
        const auto ids = f.defines_for_sym(sid);
        return ids.empty() ? aura::ast::NULL_NODE : ids.front();
    }
    // Issue #4681: top-level lambda define name -> env cell (its call slot).
    aura::compiler::CallSlotCells4681 ir_call_slots_4681_;
    // caller -> callees its last lowering reached through a slot.
    std::unordered_map<std::string, std::unordered_set<std::string>> slot_edges_4681_;
    // callee -> lambda arity when its slot was registered.
    std::unordered_map<std::string, int> slot_arity_4681_;
    std::vector<std::string> slot_hits_scratch_4681_;
    std::uint64_t slot_edges_skipped_4681_ = 0;
    // Called right after a lowering of `caller` that fed value_cells_for_lowering.
    void note_slot_edges_4681_(const std::string& caller) {
        if (!call_slots_enabled_4681_())
            return;
        auto& set = slot_edges_4681_[caller];
        set.clear();
        for (auto& c : slot_hits_scratch_4681_) {
            if (c == caller)
                continue;
            set.insert(c);
            record_dependency(caller, c);
        }
        slot_hits_scratch_4681_.clear();
    }
    // Current lambda arity of a workspace define; -1 when unknown / not a lambda.
    int workspace_lambda_arity_4681_(const std::string& name) {
        auto* f = evaluator_.workspace_flat();
        auto* p = evaluator_.workspace_pool();
        if (!f || !p)
            return -1;
        const auto key = f->all_mutations().size();
        if (arity_memo_flat_4681_ == f && arity_memo_key_4681_ == key &&
            arity_memo_name_4681_ == name)
            return arity_memo_val_4681_;
        const int r = workspace_lambda_arity_scan_4681_(f, p, name);
        arity_memo_flat_4681_ = f;
        arity_memo_key_4681_ = key;
        arity_memo_name_4681_ = name;
        arity_memo_val_4681_ = r;
        return r;
    }
    const void* arity_memo_flat_4681_ = nullptr;
    std::size_t arity_memo_key_4681_ = 0;
    std::string arity_memo_name_4681_;
    int arity_memo_val_4681_ = -1;
    static int workspace_lambda_arity_scan_4681_(aura::ast::FlatAST* f, aura::ast::StringPool* p,
                                                 const std::string& name) {
        int found = -1;
        int n = 0;
        for (auto id : f->collect_by_tag(static_cast<std::uint32_t>(aura::ast::NodeTag::Define))) {
            if (id >= f->size() || f->is_free_slot(id))
                continue;
            auto v = f->get(id);
            if (v.tag != aura::ast::NodeTag::Define || v.children.empty() ||
                p->resolve(v.sym_id) != name)
                continue;
            ++n;
            auto lv = f->get(v.child(0));
            found = lv.tag == aura::ast::NodeTag::Lambda ? static_cast<int>(lv.params.size()) : -1;
        }
        return n == 1 ? found : -1; // multi-define: unknown
    }
    // Issue #4681: true when `caller` reaches `callee` only through its
    // call slot and callee's signature (arity) did not change, so the
    // caller's IR stays valid and the cascade can skip it.
    bool slot_edge_skippable_4681_(const std::string& callee, const std::string& caller,
                                   bool callee_changed) {
        if (!call_slots_enabled_4681_())
            return false;
        auto it = slot_edges_4681_.find(caller);
        if (it == slot_edges_4681_.end() || it->second.count(callee) == 0)
            return false;
        if (callee_changed) {
            auto ait = slot_arity_4681_.find(callee);
            if (ait == slot_arity_4681_.end() || ait->second < 0)
                return false;
            if (workspace_lambda_arity_4681_(callee) != ait->second)
                return false;
        }
        ++slot_edges_skipped_4681_;
        return true;
    }
    static bool call_slots_enabled_4681_() noexcept {
        static const bool on = [] {
            // Default on since the #4681 flip; AURA_CALL_SLOTS=0 opts out.
            const char* e = ::aura::config::raw(::aura::config::Key::AURA_CALL_SLOTS);
            return !(e && e[0] == '0');
        }();
        return on;
    }
    void install_persistent_define_closure_bridge() {
        evaluator_.set_closure_bridge(
            [this](aura::compiler::ClosureId cid,
                   std::span<const types::EvalValue> args) -> std::optional<types::EvalValue> {
                return dispatch_ir_define_closure(cid, args);
            });
    }

    std::size_t bind_define_value_in_env(const std::string& name, const types::EvalValue& value) {
        // Issue #4715 AC2: a define / rebind evaluated inside a body with an
        // open overlay never reaches the shared env - it lands in the fiber's
        // private cell and publishes at fiber:overlay-commit, so a concurrent
        // eval-current / mutate:rebind observer sees the OLD or the NEW cell,
        // never a half-written one. One thread_local load when no overlay is
        // open (every pre-existing program).
        if (auto private_cell =
                primitives_detail::fiber_cow_overlay_capture_define(evaluator_, name, value))
            return *private_cell;
        // Issue #4717: this publishes a NEW cell slot and its top-env binding
        // while fiber bodies read both (TSan: cells() slot write vs
        // sync_workspace_value_cells_from_env / eval_flat_body). Lock order
        // everywhere is bindings -> cell storage.
        auto bind_guard = evaluator_.top_env().lock_bindings_exclusive();
        std::scoped_lock<std::recursive_mutex> cells_lock(evaluator_.cells_storage_mutex());
        auto existing = evaluator_.top_env().lookup_binding(name);
        if (existing && types::is_cell(*existing)) {
            auto ci = types::as_cell_id(*existing);
            evaluator_.cells()[ci] = value;
            return ci;
        }
        auto ci = evaluator_.cells().size();
        evaluator_.cells().push_back(value);
        evaluator_.top_env().bind(name, types::make_cell(ci));
        return ci;
    }

    [[nodiscard]] const std::unordered_map<std::string, std::size_t,
                                           aura::core::TransparentStringHash, std::equal_to<>>*
    value_cells_for_lowering() {
        // Issue #4681: arm call slots for the lowering this feeds.
        aura::compiler::call_slot_cells_4681() =
            (call_slots_enabled_4681_() && !ir_call_slots_4681_.empty()) ? &ir_call_slots_4681_
                                                                         : nullptr;
        slot_hits_scratch_4681_.clear();
        aura::compiler::call_slot_hits_4681() =
            aura::compiler::call_slot_cells_4681() ? &slot_hits_scratch_4681_ : nullptr;
        return ir_value_cell_bindings_.empty() ? nullptr : &ir_value_cell_bindings_;
    }

    [[nodiscard]] bool define_body_needs_tree_walker_fallback(
        const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
        aura::ast::NodeId body_id, const std::string& self_name = {},
        const std::unordered_set<std::string>* extra_known = nullptr) const {
        if (body_id == aura::ast::NULL_NODE || body_id >= flat.size())
            return false;
        std::unordered_set<std::string> param_names;
        auto body_v = flat.get(body_id);
        if (body_v.tag == aura::ast::NodeTag::Lambda) {
            for (auto pid : body_v.params)
                param_names.insert(std::string(pool.resolve(pid)));
            body_id = body_v.children.empty() ? aura::ast::NULL_NODE : body_v.child(0);
        }
        struct BodyWalker {
            const aura::ast::FlatAST& f;
            const aura::ast::StringPool& p;
            const std::string& self_name;
            const std::unordered_set<std::string>& param_names;
            const Evaluator& eval;
            const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>,
                                     aura::core::TransparentStringHash, std::equal_to<>>& ir_cache;
            const std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                                     std::equal_to<>>& value_cells;
            // Whole-program Begin eval: sibling defines register sequentially —
            // body refs to their names are known at gate time (asan-verify 937d53d22).
            const std::unordered_set<std::string>* extra_known;
            bool needs_fallback = false;
            void walk(aura::ast::NodeId id) {
                if (needs_fallback || id == aura::ast::NULL_NODE || id >= f.size())
                    return;
                auto nv = f.get(id);
                // Issue #2213 follow-up (lyapunov-fact demo 2026-07-28):
                // `(define X (quote ...))` body is literal data, never
                // evaluated at runtime. Walking into Quote's children
                // and analyzing free variables (e.g. `fact` inside the
                // quoted define form) spuriously trips needs_fallback,
                // which under strict pipeline (Forbidden) returns
                // HardError and hangs Aura REPL on the first form. Skip
                // the entire Quote subtree — its contents are not code.
                if (nv.tag == aura::ast::NodeTag::Quote)
                    return;
                if (nv.tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                    auto callee_id = nv.child(0);
                    if (callee_id < f.size()) {
                        auto callee_v = f.get(callee_id);
                        if (callee_v.tag == aura::ast::NodeTag::Variable) {
                            auto callee_name = std::string(p.resolve(callee_v.sym_id));
                            if (callee_name == "fiber:spawn" || callee_name == "fiber:join")
                                needs_fallback = true;
                            // Non-lambda (while cond body) lowers to Branch/Jump (#4320).
                            // Lambda form still goes through the while primitive, and an
                            // IR-cached define that only Call'd it miscomputed (loop never
                            // ran; multi-session leak oracle returned growth=0). Keep the
                            // fallback bit so Allow uses the walker and Forbidden hard-errors.
                            else if (callee_name == "while")
                                needs_fallback = true;
                        }
                    }
                }
                if (nv.tag == aura::ast::NodeTag::Variable) {
                    auto var_name = std::string(p.resolve(nv.sym_id));
                    if (param_names.count(var_name) || var_name == self_name)
                        return;
                    if (extra_known && extra_known->count(var_name))
                        return;
                    if (eval.primitives().slot_for_name(var_name) < eval.primitives().slot_count())
                        return;
                    if (ir_cache.count(var_name) || value_cells.count(var_name))
                        return;
                    needs_fallback = true;
                }
                for (auto c : nv.children)
                    walk(c);
            }
        };
        BodyWalker bw{flat,
                      pool,
                      self_name,
                      param_names,
                      evaluator_,
                      ir_cache_,
                      ir_value_cell_bindings_,
                      extra_known};
        bw.walk(body_id);
        return bw.needs_fallback;
    }

    // Issue #4621: a lambda with a rest parameter cannot be lowered to IR.
    // The rest parameter is lowered as a single Arg slot, not as a pair
    // list, so the IR closure binds the rest argument AS-IS: the define's
    // arity silently changes (a two-argument call binds the rest to the
    // second argument, a one-argument call is an arity error). The
    // file-level gate already refuses this shape for a whole file
    // (needs_tree_walker_fallback: "Dotted rest lambda cannot be lowered
    // to IR"), so a file define keeps the tree-walker closure and binds
    // the rest as a pair list. The workspace IR paths (the #4357
    // specialise pass, the v2 IR cache populate, the dirty re-lower)
    // bypassed that gate, so a workspace define with a rest parameter was
    // rebound to a fixed-arity IR closure and its arity changed silently
    // (issue #4621). Such a define stays on the tree walker - the same
    // rest-parameter semantics a file define has.
    //
    // Lambda::int_value bit 0 is the dotted-rest flag (FlatAST::add_lambda
    // stores `dotted ? 1 : 0`). Both rest shapes carry it: the dotted
    // formal list `(lambda (a . r) ...)` and the all-args shorthand
    // `(lambda xs ...)`.
    [[nodiscard]] static bool define_body_has_rest_param_4621(const aura::ast::FlatAST& flat,
                                                              aura::ast::NodeId root) {
        if (root == aura::ast::NULL_NODE || root >= flat.size() || flat.is_free_slot(root))
            return false;
        const auto v = flat.get(root);
        aura::ast::NodeId body = root;
        if (v.tag == aura::ast::NodeTag::Define) {
            if (v.children.empty())
                return false;
            body = v.child(0);
        }
        if (body >= flat.size() || flat.is_free_slot(body))
            return false;
        const auto bv = flat.get(body);
        if (bv.tag != aura::ast::NodeTag::Lambda)
            return false;
        return (bv.int_value & 1) != 0;
    }

    // Issue #4618: lowering_impl.cpp's NodeTag::Set path stores only into a
    // lambda scope (BindingKind::Local / Cell) or a captured cell. A target
    // it cannot resolve is returned unchanged, so the store is silently
    // dropped - the #3927 defect. cache_define's LambdaBodyWalker refuses
    // such a body, but two later paths lowered it anyway and rebound the
    // define to the IR closure, so the store vanished after (eval-current):
    //   - specialize_one_lambda_via_ir (#4357 hot workspace lambdas);
    //   - relower_define_blocks (#1495 dirty / mutate:rebind re-lower).
    // Both now keep the define on the tree walker, whose Set writes the
    // cell lookup_cell_index resolves (visible to the caller and to later
    // references). Same rule as the LambdaBodyWalker: a Set target that is
    // not a let/letrec/begin-define cell in the body, not the body lambda's
    // own parameter, and not the define's name is a non-local store.
    // Parameters (of this or any enclosing lambda) stay local: that is the
    // #3927 LambdaBodyWalker rule, and the tree walker is the weaker face
    // for them ("unbound variable: set!: x"), so those bodies keep IR.
    [[nodiscard]] bool define_body_sets_nonlocal_4618(const aura::ast::FlatAST& flat,
                                                      const aura::ast::StringPool& pool,
                                                      aura::ast::NodeId root,
                                                      const std::string& self_name = {}) const;

    void snapshot_ir_for_disk(const std::string& name) {
        auto it = ir_cache_.find(name);
        if (it == ir_cache_.end() || it->second.empty())
            return;
        ir_disk_snapshots_[name] = it->second;
    }

    bool bind_value_define_via_ir(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  aura::ast::NodeId expanded_root, const std::string& name_str) {
        // Issue #4747: the gate is a RUN-scoped guard here, not a whole-
        // function one. It was held across interp.execute() below - the #4717
        // rule "NEVER held across an IRInterpreter body run" - so this path
        // deadlocked against a concurrent fiber body: the body held its
        // define's exec_mtx and wanted the gate, this thread held the gate and
        // wanted that same exec_mtx (gdb: main in bind_value_define_via_ir ->
        // dispatch -> recursive_mutex::lock with the body as owner, the body
        // parked in dispatch on ir_cache_gate_). Lower the module, then drop
        // the gate for the run and re-take it for the binding writes.
        std::unique_lock<std::recursive_mutex> ir_gate(ir_cache_gate_, std::defer_lock);
        relock_ir_gate_(ir_gate); // #4765
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, nullptr, &evaluator_.primitives(), cache_bridge_ptr,
            cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());

        // Issue #4815: the value can hold closures this run mints - e.g.
        // (define t (mk 7)) or a vector of (mk i) results. They live in the
        // interpreter's runtime_closures_, so a stack-local interpreter left
        // the bound value pointing at ids nobody owns once this function
        // returned; the next top-level form (serve-async evaluates an exec
        // one form at a time, #4702) then failed with "invalid closure"
        // and parallel-intend reported every such task as apply-failed.
        // Run on a heap binding that mints from the shared id source
        // (#4357 / #4764) and keep it alive while it owns closures, so
        // dispatch_ir_define_closure can route those ids back to it.
        auto binding = std::make_shared<IRDefineEnvBinding>(
            std::move(ir_mod), evaluator_.primitives(), &type_registry_, &metrics_, &evaluator_);
        binding->interpreter =
            std::make_unique<aura::compiler::IRInterpreter>(binding->module, binding->context);
        if (!types::is_ir_closure_id(next_ir_define_closure_id_.load(std::memory_order_acquire)))
            next_ir_define_closure_id_.store(types::make_ir_closure_id(0),
                                             std::memory_order_release);
        binding->interpreter->set_next_closure_id(
            next_ir_define_closure_id_.load(std::memory_order_acquire));
        binding->interpreter->set_shared_closure_cursor(&next_ir_define_closure_id_);
        // Issue #4747: never run a body under the cache gate (see the note at
        // the gate declaration). A nested dispatch inside this run takes the
        // gate itself, and a sibling body may be inside exec_mtx waiting for
        // it - holding both is the inversion.
        // Issue #4904: the value's own computation can apply a closure it
        // just minted through the evaluator - (define r (parallel-intend
        // (vector (lambda () 1)))) hands the thunk to a worker that calls
        // apply_closure -> dispatch_ir_define_closure. The binding was only
        // published as an owner AFTER execute() returned, so that dispatch
        // found no owner and every task came back "apply-failed" (stage
        // status partial / ok #f in orch:run-workflow). Publish it under a
        // run-scoped key for the duration of the run; the name's previous
        // owner stays registered so the old value's closures still resolve.
        const std::string pending_key_4904 = std::string("\x01pending-4904:") + name_str;
        ir_value_define_closure_owners_[pending_key_4904] = binding;
        struct PendingOwnerErase4904 {
            PendingOwnerErase4904(CompilerService* service,
                                  std::unique_lock<std::recursive_mutex>& gate_lock,
                                  const std::string& erase_key)
                : service_(service)
                , gate_(gate_lock)
                , erase_key_(erase_key) {}
            ~PendingOwnerErase4904() {
                if (!gate_.owns_lock())
                    service_->relock_ir_gate_(gate_); // #4765
                service_->ir_value_define_closure_owners_.erase(erase_key_);
            }
            PendingOwnerErase4904(const PendingOwnerErase4904&) = delete;
            PendingOwnerErase4904& operator=(const PendingOwnerErase4904&) = delete;
            PendingOwnerErase4904(PendingOwnerErase4904&&) = delete;
            PendingOwnerErase4904& operator=(PendingOwnerErase4904&&) = delete;

        private:
            CompilerService* service_;
            std::unique_lock<std::recursive_mutex>& gate_;
            const std::string& erase_key_;
        };
        const PendingOwnerErase4904 pending_erase_4904{this, ir_gate, pending_key_4904};
        if (ir_gate.owns_lock())
            ir_gate.unlock();
        auto result = binding->interpreter->execute();
        relock_ir_gate_(ir_gate); // #4765
        ir_value_define_closure_owners_.erase(pending_key_4904);
        publish_ir_closure_cursor_(binding->interpreter->next_closure_id());
        if (!result)
            return false;

        auto ci = bind_define_value_in_env(name_str, *result);
        ir_value_cell_bindings_[name_str] = ci;
        // Issue #4815: one live owner per name; a re-define replaces it, so
        // its old closures deny like any superseded binding (#4349).
        if (binding->interpreter->closure_count() > 0)
            ir_value_define_closure_owners_[name_str] = std::move(binding);
        else
            ir_value_define_closure_owners_.erase(name_str);
        metrics_.value_define_ir_env_bind_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Issue #4587: true when IR lowering of this define would emit ConstI64 0
    // for a free name. Quote is data. Let binds the name in the body, and
    // letrec also binds it in the init. A syntax callee is not a value load.
    [[nodiscard]] bool workspace_lambda_free_name_unresolved(const aura::ast::FlatAST& flat,
                                                             const aura::ast::StringPool& pool,
                                                             aura::ast::NodeId root,
                                                             const std::string& self_name) const;

    // Issue #4357: lower one cached define and bind the IR-interpreter closure.
    // Copies non-entry functions into ir_cache_ first so a later caller lower
    // inlines the new body (the let-loop stays inside one interpreter).
    bool specialize_one_lambda_via_ir(const std::string& name);

    // Issue #4357: focus empty = every workspace lambda (at most
    // kWorkspaceSpecializeCap; a larger workspace stays on the tree walker).
    // Issue #4479: the cap was 8, so a 9th procedure define dropped every
    // workspace define (even unchanged, unrelated ones) off the IR fast path
    // (~35x slower calls). 64 keeps ordinary workspaces specialized. A name = that define, then its
    // direct callers, callee first so the caller lower copies the new body.
    // Two passes when focusing the whole workspace so cross-calls see a
    // filled cache. Production keeps its own hot-update path.
    void specialize_workspace_lambdas_via_ir(const std::string& focus) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (aura::compiler::typed_audit::production_defaults_active())
            return;
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return;
        std::vector<std::string> names;
        if (focus.empty()) {
            constexpr std::size_t kWorkspaceSpecializeCap = 64;
            for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                if (ws_flat->is_free_slot(id))
                    continue;
                // Issue #4682 (B): column prefilter before the full get().
                if (ws_flat->tag(id) != aura::ast::NodeTag::Define ||
                    ws_flat->sym_id(id) == aura::ast::INVALID_SYM)
                    continue;
                auto v = ws_flat->get(id);
                if (v.tag != aura::ast::NodeTag::Define || v.children.empty() ||
                    v.sym_id == aura::ast::INVALID_SYM)
                    continue;
                if (ws_flat->get(v.child(0)).tag != aura::ast::NodeTag::Lambda)
                    continue;
                auto n = std::string(ws_pool->resolve(v.sym_id));
                if (n.empty() || n[0] == '_')
                    continue;
                names.push_back(std::move(n));
                // Issue #4682 (B): over the cap the function returns below
                // regardless (loop is side-effect free) — stop scanning.
                if (names.size() > kWorkspaceSpecializeCap)
                    return;
            }
            if (names.empty() || names.size() > kWorkspaceSpecializeCap)
                return;
        } else {
            // Issue #4547: dep_graph_ also records module functions
            // (cache_module -> record_dependency) and their
            // function_sources_ row is the WHOLE module text. Lowering that
            // under the caller's name bound some other define's closure
            // (often a sibling) into the shared top_ cell, so after a
            // rebind of `wb` a module's `caller6:b` ran `caller6:a`, or hit
            // "invalid closure" / a foreign arity. Only live workspace
            // Lambda defines are specialised here; module functions keep
            // their tree-walker closure, which looks the callee up by name.
            auto is_ws_lambda = [&](const std::string& n) -> bool {
                return is_workspace_lambda_define(n);
            };
            if (!is_ws_lambda(focus))
                return;
            names.push_back(focus);
            // Issue #4598: transitive called_by, callee first. One hop
            // leaves a caller of the direct caller (chain2 of user of
            // base) holding the inlined pre-rebind body. Cap the cone so
            // a popular define does not re-lower the whole graph.
            constexpr std::size_t kFocusCallerCap = 16;
            std::unordered_set<std::string> seen;
            seen.insert(focus);
            std::vector<std::string> queue;
            queue.push_back(focus);
            for (std::size_t qi = 0; qi < queue.size() && names.size() < kFocusCallerCap; ++qi) {
                std::vector<std::string> callers;
                {
                    std::shared_lock dep_read(dep_graph_mtx_);
                    if (auto it = dep_graph_.find(queue[qi]); it != dep_graph_.end())
                        callers = it->second.called_by;
                }
                for (const auto& c : callers) {
                    if (c.empty() || !seen.insert(c).second)
                        continue;
                    if (names.size() >= kFocusCallerCap)
                        break;
                    if (!is_ws_lambda(c))
                        continue;
                    names.push_back(c);
                    queue.push_back(c);
                }
            }
        }
        // Value-cell erase happens inside specialize_one_lambda_via_ir,
        // after the Issue #4372 while check. A define that stays on the
        // tree walker keeps its cell so a specialised caller TopCellLoads
        // that closure instead of inlining a loop that cannot see set!.
        auto retw = [&](const std::string& name) {
            for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                if (ws_flat->is_free_slot(id))
                    continue;
                auto v = ws_flat->get(id);
                if (v.tag != aura::ast::NodeTag::Define || v.sym_id == aura::ast::INVALID_SYM)
                    continue;
                if (ws_pool->resolve(v.sym_id) != name)
                    continue;
                (void)evaluator_.eval_flat(*ws_flat, *ws_pool, id, evaluator_.top_env());
                return;
            }
        };
        const int passes = focus.empty() ? 2 : 1;
        for (int pass = 0; pass < passes; ++pass) {
            for (std::size_t i = 0; i < names.size(); ++i) {
                const bool ok = specialize_one_lambda_via_ir(names[i]);
                if (!ok && pass + 1 == passes && !focus.empty() && i > 0)
                    retw(names[i]);
            }
        }
    }

    // Issue #272 Cycle 2: bind env from an already-cached define (no cache update).
    // Used by compile_module disk-cache hits where ir_cache_ is pre-populated.
    bool bind_define_env_only(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                              aura::ast::NodeId expanded_root, const std::string& name_str) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, nullptr, &evaluator_.primitives(), cache_bridge_ptr,
            cache_strings_ptr, &name_str, &type_registry_, value_cells_for_lowering());
        return bind_function_define_via_ir(ir_mod, name_str);
    }

    bool bind_function_define_via_ir(const aura::ir::IRModule& ir_mod, const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (ir_mod.functions.empty())
            return false;

        auto binding = std::make_shared<IRDefineEnvBinding>(
            ir_mod, evaluator_.primitives(), &type_registry_, &metrics_, &evaluator_);
        binding->interpreter =
            std::make_unique<aura::compiler::IRInterpreter>(binding->module, binding->context);
        // Issue #4357: disjoint closure ids. Two defines used to both mint
        // kClosureIdHighBit, and the owner map kept only the later name, so
        // a call of the first re-entered the second (hang or a 0 result).
        if (!types::is_ir_closure_id(next_ir_define_closure_id_.load(std::memory_order_acquire)))
            next_ir_define_closure_id_.store(types::make_ir_closure_id(0),
                                             std::memory_order_release);
        binding->interpreter->set_next_closure_id(
            next_ir_define_closure_id_.load(std::memory_order_acquire));
        // Issue #4764: mint every closure id from the shared source, so a
        // define dispatched mid-call cannot reuse an id the caller already
        // minted but has not published yet.
        binding->interpreter->set_shared_closure_cursor(&next_ir_define_closure_id_);

        auto result = binding->interpreter->execute();
        publish_ir_closure_cursor_(binding->interpreter->next_closure_id());
        if (!result || !types::is_closure(*result))
            return false;

        binding->bound_closure = *result;
        binding->closure_id = types::as_closure_id(*result);

        // Publish, then retire (never retire first): a concurrent body that
        // resolves this define's top_env cell holds whatever closure id the
        // CELL publishes at that instant, and it may then park on
        // ir_cache_gate_ for the whole re-bind before it dispatches. The
        // superseded id therefore has to stay resolvable until the new one is
        // published, which retire_ir_define_closure_ keeps true by recording
        // it in the define's retired-id history that
        // dispatch_ir_define_closure consults before the strict runtime-mint
        // deny (#4450 / #4349).
        const auto new_cid = binding->closure_id;
        auto old_it = ir_define_env_bindings_.find(name);
        const std::shared_ptr<IRDefineEnvBinding> old_binding =
            old_it != ir_define_env_bindings_.end() ? old_it->second : nullptr;

        ir_define_closure_owner_[new_cid] = name;
        bind_define_value_in_env(name, binding->bound_closure);
        ir_define_env_bindings_[name] = std::move(binding);
        if (old_binding != nullptr && old_binding->closure_id != new_cid) {
            ir_define_closure_owner_.erase(old_binding->closure_id);
            retire_ir_define_closure_(name, old_binding);
        }

        metrics_.define_ir_env_bind_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Issue #4682 (6b): snapshot an eligible define body for the worker.
    // Returns null when the body is outside the pure subset.
    std::unique_ptr<TierJob4682> tier_snapshot_4682_(const aura::ir::IRModule& mod,
                                                     types::EvalValue self_word, TierSlot4682& slot,
                                                     const std::string& name,
                                                     std::uint32_t mk_self_cell = UINT32_MAX);

    // Issue #4682 (6b): tree-walker tier. A workspace over the
    // specialisation cap keeps its defines as TW closures; a hot
    // self-recursive pure define is lowered from its source on the side
    // and compiled by the same worker. Keyed by closure word, revalidated
    // by body identity + lifetime version; only the service's own thread.
    struct TwTierEntry4682 {
        std::shared_ptr<TierSlot4682> slot;
        const void* flat = nullptr;
        std::uint32_t body_id = 0;
        std::uint64_t lifetime_version = 0;
        std::string name;
    };
    std::unordered_map<std::int64_t, TwTierEntry4682> tw_tier_4682_;
    // name -> (self cell index, closure word last stored there)
    std::unordered_map<std::string, std::pair<std::uint32_t, types::EvalValue>> tw_self_cell_4682_;
    std::thread::id tw_tier_thread_4682_ = std::this_thread::get_id();

    static std::optional<types::EvalValue>
    tw_tier_hook_thunk_4682_(void* ctx, types::EvalValue fn, std::string_view name,
                             const void* flat, std::uint32_t body_id, std::uint64_t lv,
                             std::span<const types::EvalValue> args) {
        return static_cast<CompilerService*>(ctx)->tw_tier_try_4682_(fn, name, flat, body_id, lv,
                                                                     args);
    }

    static void exit_quiesce_thunk_(void* ctx) noexcept {
        static_cast<CompilerService*>(ctx)->quiesce_background_workers();
    }

    // Issue #4705: stop+join the background workers before a hard process
    // exit. Idempotent: the tier worker refuses new jobs once stopped, so
    // a second call is a no-op.
    void quiesce_background_workers() noexcept {
        if (tier_worker_4682_)
            tier_worker_4682_->stop_and_join();
    }

    std::unique_ptr<TierJob4682>
    tw_tier_lower_4682_(const std::string& name, types::EvalValue self_word, TierSlot4682& slot) {
        auto src_it = function_sources_.find(name);
        if (src_it == function_sources_.end() || src_it->second.empty())
            return nullptr;
        const std::string source = src_it->second;
        std::pmr::monotonic_buffer_resource scratch;
        const auto alloc = std::pmr::polymorphic_allocator<std::byte>{&scratch};
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(source, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return nullptr;
        flat.root = pr.root;
        if (define_body_has_rest_param_4621(flat, pr.root) ||
            define_body_sets_nonlocal_4618(flat, pool, pr.root, name) ||
            workspace_lambda_free_name_unresolved(flat, pool, pr.root, name))
            return nullptr;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, nullptr, nullptr, &evaluator_.primitives(), nullptr, nullptr, &name,
            &type_registry_, value_cells_for_lowering());
        if (tier_jit_trace_4682_()) {
            for (const auto& f : ir_mod.functions) {
                if (f.id == ir_mod.entry_function_id)
                    continue;
                std::fprintf(stderr, "[tier4682] tw %s fn locals=%u args=%u free=%zu\n",
                             name.c_str(), f.local_count, f.arg_count, f.free_vars.size());
                for (const auto& blk : f.blocks)
                    for (const auto& in : blk.instructions)
                        std::fprintf(stderr, "[tier4682]   op=%u %u %u %u %u\n",
                                     static_cast<unsigned>(in.opcode), in.operands[0],
                                     in.operands[1], in.operands[2], in.operands[3]);
            }
        }
        // One self cell per define name, reused across rebinds: the cell
        // index is baked into the lowered body, so a stable index keeps a
        // rebind back to an earlier body byte-identical (worker code-cache
        // hit) and stops a cell leak per recompile. Only the current top
        // binding of `name` runs native (tw_tier_self_bound_4682_), so the
        // previous closure word in the cell is dead.
        auto& cells = evaluator_.cells();
        auto sc = tw_self_cell_4682_.find(name);
        // The cell must still hold the word this map stored (a workspace
        // reset may have reused the index for another value).
        const bool reuse = sc != tw_self_cell_4682_.end() && sc->second.first < cells.size() &&
                           cells[sc->second.first] == sc->second.second;
        const auto ci = reuse ? sc->second.first : static_cast<std::uint32_t>(cells.size());
        types::EvalValue prev{};
        if (reuse) {
            prev = cells[ci];
            cells[ci] = self_word;
        } else {
            cells.push_back(self_word);
        }
        auto job = tier_snapshot_4682_(ir_mod, self_word, slot, name, ci);
        if (!job) {
            if (reuse)
                cells[ci] = prev;
            else if (cells.size() == ci + 1u)
                cells.pop_back();
        } else {
            tw_self_cell_4682_[name] = {ci, self_word};
        }
        return job;
    }

    // The top-level binding of `name` must still be this closure word.
    bool tw_tier_self_bound_4682_(std::string_view name, types::EvalValue fnv) {
        auto v = evaluator_.top_env().lookup(name);
        if (!v)
            return false;
        if (types::is_cell(*v)) {
            const auto id = types::as_cell_id(*v);
            if (id >= evaluator_.cells().size())
                return false;
            return evaluator_.cells()[id] == fnv;
        }
        return *v == fnv;
    }

    std::optional<types::EvalValue> tw_tier_try_4682_(types::EvalValue fnv, std::string_view name,
                                                      const void* flat, std::uint32_t body_id,
                                                      std::uint64_t lv,
                                                      std::span<const types::EvalValue> args) {
        if (std::this_thread::get_id() != tw_tier_thread_4682_)
            return std::nullopt;
        auto& e = tw_tier_4682_[fnv.val];
        if (!e.slot || e.flat != flat || e.body_id != body_id || e.lifetime_version != lv ||
            e.name != name) {
            e.slot = std::make_shared<TierSlot4682>();
            e.flat = flat;
            e.body_id = body_id;
            e.lifetime_version = lv;
            e.name = std::string(name);
        }
        auto slot_ref = e.slot; // the map may rehash under a nested call
        auto& slot = *slot_ref;
        if (!slot.fn.load(std::memory_order_acquire)) {
            if (slot.state.load(std::memory_order_acquire) != 0)
                return std::nullopt;
            if (slot.calls.fetch_add(1, std::memory_order_relaxed) + 1 < tier_jit_hot_4682_())
                return std::nullopt;
            const std::string nm(name);
            auto job = tw_tier_lower_4682_(nm, fnv, slot);
            if (!job || job->self_calls == 0 || slot.self_cell == UINT32_MAX) {
                slot.state.store(3, std::memory_order_release);
                tier_stats_4682_.ineligible.fetch_add(1, std::memory_order_relaxed);
                if (tier_jit_trace_4682_())
                    std::fprintf(stderr, "[tier4682] tw %s ineligible\n", nm.c_str());
                return std::nullopt;
            }
            job->slot = slot_ref;
            slot.state.store(1, std::memory_order_release);
            tier_stats_4682_.queued.fetch_add(1, std::memory_order_relaxed);
            if (tier_jit_trace_4682_())
                std::fprintf(stderr, "[tier4682] tw %s queued as %s\n", nm.c_str(),
                             job->name.c_str());
            if (!tier_worker_4682_)
                tier_worker_4682_ = std::make_unique<TierWorker4682>();
            tier_worker_4682_->enqueue(std::move(job), tier_stats_4682_);
            if (tier_jit_sync_4682_())
                tier_worker_4682_->drain();
        }
        if (!slot.fn.load(std::memory_order_acquire) || !tw_tier_self_bound_4682_(name, fnv))
            return std::nullopt;
        return tier_run_native_4682_(slot, fnv, args);
    }

    std::optional<types::EvalValue> tier_try_native_4682_(IRDefineEnvBinding& b,
                                                          const std::string& name,
                                                          std::span<const types::EvalValue> args) {
        if (!b.tier)
            b.tier = std::make_shared<TierSlot4682>();
        auto& slot = *b.tier;
        auto fn = slot.fn.load(std::memory_order_acquire);
        if (!fn) {
            if (slot.state.load(std::memory_order_acquire) != 0)
                return std::nullopt;
            if (slot.calls.fetch_add(1, std::memory_order_relaxed) + 1 < tier_jit_hot_4682_())
                return std::nullopt;
            // Issue #4682: the IR body names its own recursion with a
            // MakeClosure of its function id, so the snapshot needs a private
            // cell holding this binding's closure word to rewrite it into a
            // top-cell load (same one-self-cell-per-name contract as the
            // walker path). A stable index keeps a rebind back to an earlier
            // body byte-identical; only the current top binding runs native
            // (tw_tier_self_bound_4682_).
            auto& cells = evaluator_.cells();
            auto cell_row = tw_self_cell_4682_.find(name);
            const bool cell_reuse = cell_row != tw_self_cell_4682_.end() &&
                                    cell_row->second.first < cells.size() &&
                                    cells[cell_row->second.first] == cell_row->second.second;
            const auto self_cell =
                cell_reuse ? cell_row->second.first : static_cast<std::uint32_t>(cells.size());
            types::EvalValue cell_prev{};
            if (cell_reuse) {
                cell_prev = cells[self_cell];
                cells[self_cell] = b.bound_closure;
            } else {
                cells.push_back(b.bound_closure);
            }
            auto job = tier_snapshot_4682_(b.module, b.bound_closure, slot, name, self_cell);
            if (!job) {
                if (cell_reuse) {
                    cells[self_cell] = cell_prev;
                } else if (cells.size() == self_cell + 1u) {
                    cells.pop_back();
                }
                slot.state.store(3, std::memory_order_release);
                tier_stats_4682_.ineligible.fetch_add(1, std::memory_order_relaxed);
                if (tier_jit_trace_4682_())
                    std::fprintf(stderr, "[tier4682] %s ineligible\n", name.c_str());
                return std::nullopt;
            }
            tw_self_cell_4682_[name] = {self_cell, b.bound_closure};
            job->slot = b.tier;
            slot.state.store(1, std::memory_order_release);
            tier_stats_4682_.queued.fetch_add(1, std::memory_order_relaxed);
            if (tier_jit_trace_4682_())
                std::fprintf(stderr, "[tier4682] %s queued as %s\n", name.c_str(),
                             job->name.c_str());
            if (!tier_worker_4682_)
                tier_worker_4682_ = std::make_unique<TierWorker4682>();
            tier_worker_4682_->enqueue(std::move(job), tier_stats_4682_);
            if (tier_jit_sync_4682_())
                tier_worker_4682_->drain();
            fn = slot.fn.load(std::memory_order_acquire);
            if (!fn)
                return std::nullopt;
        }
        return tier_run_native_4682_(slot, b.bound_closure, args);
    }

    // Issue #4682 (6b): run a published native body. Exact: any guard
    // exit, self-cell change or non-scalar result returns nullopt and the
    // caller re-runs the (pure) body on its own interpreter.
    std::optional<types::EvalValue> tier_run_native_4682_(TierSlot4682& slot,
                                                          types::EvalValue self_word,
                                                          std::span<const types::EvalValue> args) {
        auto fn = slot.fn.load(std::memory_order_acquire);
        if (!fn)
            return std::nullopt;
        // Entry guards: exact arity, fixnum args, no outer deopt host, and
        // the self cell still names this closure.
        if (args.size() != slot.arg_count || aura_jit_deopt_frame_host_armed() != 0)
            return std::nullopt;
        for (const auto& a : args)
            if (!types::is_int(a))
                return std::nullopt;
        auto self_ok = [&]() {
            return slot.self_cell == UINT32_MAX ||
                   (slot.self_cell < evaluator_.cells().size() &&
                    evaluator_.cells()[slot.self_cell] == self_word);
        };
        if (!self_ok())
            return std::nullopt;
        std::int64_t stack_locals[32];
        std::vector<std::int64_t> heap_locals;
        std::int64_t* locals = stack_locals;
        if (slot.local_count > 32) {
            heap_locals.assign(slot.local_count, 0);
            locals = heap_locals.data();
        } else {
            for (std::uint32_t i = 0; i < slot.local_count; ++i)
                stack_locals[i] = 0;
        }
        for (std::size_t i = 0; i < args.size(); ++i)
            locals[i] = args[i].val;
        (void)aura_jit_take_prologue_deopt_sentinel();
        aura_jit_set_linear_env_none();
        // Issue #4682 (6b): pin the self-cell load to this call's self word
        // (the process-wide top-cell getter may be wired to another
        // evaluator). Save/restore so nested native calls compose.
        std::int64_t self_idx =
            slot.self_cell == UINT32_MAX ? -1 : static_cast<std::int64_t>(slot.self_cell);
        std::int64_t self_w = self_word.val;
        aura_jit_tier_self_cell_swap_4682(&self_idx, &self_w);
        const std::int64_t prev_self = aura_jit_exchange_native_self_id(self_word.val);
        const std::int64_t raw = fn(locals, slot.arg_count);
        (void)aura_jit_exchange_native_self_id(prev_self);
        aura_jit_tier_self_cell_swap_4682(&self_idx, &self_w);
        const bool deopted = aura_jit_take_prologue_deopt_sentinel() != 0;
        const types::EvalValue rv(raw);
        if (deopted || !self_ok() || !(types::is_int(rv) || raw == 3 || raw == 7)) {
            tier_stats_4682_.fallbacks.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        tier_stats_4682_.native_calls.fetch_add(1, std::memory_order_relaxed);
        return rv;
    }

    std::optional<types::EvalValue>
    dispatch_ir_define_closure(aura::compiler::ClosureId cid,
                               std::span<const types::EvalValue> args);

    // Issue #4547: true when `name` is a live (define name (lambda ...)) of
    // the current workspace. dep_graph_ also holds module functions
    // (cache_module -> record_dependency); those are not re-specialised
    // from function_sources_ (that row is the whole module text) and the
    // invalidate cascade must not drop their IR binding either: nothing
    // would reinstall it, so the module's top cell kept a closure id with
    // no owner ("eval_flat: invalid closure" after mutate:atomic-batch).
    bool is_workspace_lambda_define(const std::string& name) {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool || name.empty())
            return false;
        // Issue #4598: tag/sym_id are column reads. get() only the define
        // whose name matches, then the lambda child.
        //
        // Issue #4717: this scan runs on the specialize/drain thread while
        // the mutating thread parses and recycles workspace slots, so it must
        // follow the #2488 SoA reader contract instead of reading columns
        // bare (TSan: reset_node_slot tag_ write / recycle_dead_nodes
        // node_gen_ write vs this scan). SoA shared guard covers the
        // add_node/reset columns; the slot-publication guard covers the
        // node_gen_ tombstones. Order: SoA first, publish second (the
        // writer order is flatast_mutex_ then publish). Column reads only —
        // get() would take the same SoA lock a second time.
        auto soa_guard = ws_flat->try_acquire_soa_reader_lock();
        std::shared_lock<std::shared_mutex> publish_guard(ws_flat->node_slot_publish_mutex());
        for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
            if (ws_flat->is_free_slot(id))
                continue;
            if (ws_flat->tag(id) != aura::ast::NodeTag::Define)
                continue;
            const auto sym = ws_flat->sym_id(id);
            if (sym == aura::ast::INVALID_SYM)
                continue;
            if (ws_pool->resolve(sym) != name)
                continue;
            const auto kids = ws_flat->children(id);
            if (kids.empty())
                return false;
            const auto body = kids[0];
            if (body >= ws_flat->size() || ws_flat->is_free_slot(body))
                return false;
            return ws_flat->tag(body) == aura::ast::NodeTag::Lambda;
        }
        return false;
    }

    // Retirement, not forgetting: the dropped closure id keeps resolving to
    // this define (see retire_ir_define_closure_), so a body that resolved
    // the cell just before the drop still dispatches.
    //
    // Issue #4753: never retire a publication the define top_env cell still
    // advertises. A retired id resolves only while the define has a CURRENT
    // publication to run (the #4715 rule), and neither of the drop sites
    // guarantees one is in place at that instant: mutate:rebind drops its
    // binding and only later re-evaluates the new body, and the
    // mutation-boundary BFS drain can retire the freshly published id with
    // nothing re-binding behind it. A fiber that sampled the cell inside
    // either gap resolved an id with no owner row and no binding to resolve
    // to, and died with "eval_flat: invalid closure"
    // (suite/fiber_rebind_while_running: 1-331 catches per standalone run;
    // the row contracts never-an-error). Deferring keeps the invariant
    // "every id the cell can advertise is dispatchable": the cell keeps
    // dispatching this publication (the old-or-new half of the contract)
    // until bind_function_define_via_ir publishes its replacement, which
    // retires it under the same gate (retire_ir_define_closure_) with a
    // current publication in place. A define that is gone from the live
    // workspace is still dropped here, so a removed define cannot resurrect
    // (#2026).
    // Issue #4349 / #4753: the workspace-replace face of
    // clear_ir_define_env_binding. A replaced workspace's publication must
    // never stay dispatchable (a cell that still advertises it would run the
    // rolled-back body, #4349), so this path never defers: retire + erase.
    void invalidate_ir_define_binding_for_replace(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto it = ir_define_env_bindings_.find(name);
        if (it == ir_define_env_bindings_.end())
            return;
        retire_ir_define_closure_(name, it->second);
        ir_define_closure_owner_.erase(it->second->closure_id);
        ir_define_env_bindings_.erase(it);
    }

    void clear_ir_define_env_binding(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto it = ir_define_env_bindings_.find(name);
        if (it == ir_define_env_bindings_.end())
            return;
        if (top_env_cell_holds_closure_(name, it->second->closure_id) &&
            is_workspace_lambda_define(name))
            return;
        retire_ir_define_closure_(name, it->second);
        ir_define_closure_owner_.erase(it->second->closure_id);
        ir_define_env_bindings_.erase(it);
    }


    // Issue #4753: the define top_env cell publication (env binding ->
    // cell), the same indirection the env read path uses. nullopt when the
    // name has no cell or the cell does not hold a closure.
    std::optional<aura::compiler::ClosureId> top_env_cell_closure_(const std::string& name) {
        auto bound = evaluator_.top_env().lookup_binding(name);
        if (!bound)
            return std::nullopt;
        types::EvalValue v = *bound;
        if (types::is_cell(v)) {
            const auto ci = types::as_cell_id(v);
            if (ci >= evaluator_.cells().size())
                return std::nullopt;
            v = evaluator_.cells()[static_cast<std::size_t>(ci)];
        }
        if (!types::is_closure(v))
            return std::nullopt;
        return types::as_closure_id(v);
    }

    // Issue #4753: does the define top_env cell still advertise this id?
    bool top_env_cell_holds_closure_(const std::string& name, aura::compiler::ClosureId cid) {
        const auto cell = top_env_cell_closure_(name);
        return cell.has_value() && *cell == cid;
    }

    // Issue #4753: retried retired-id resolutions per thread (one hop is
    // the cell publication, a second can be an IR dispatch onto it).
    static inline thread_local std::uint32_t retired_resolve_depth_ = 0;


    // Issue #4715 regression (agent-fibers-8): a define's superseded closure
    // ids keep resolving to that define.
    //
    // A body that reads the define's top_env cell resolves a closure id and
    // only THEN enters dispatch_ir_define_closure, which takes
    // ir_cache_gate_. The parent's (mutate :rebind ...) + (eval-current) holds
    // that gate across the whole lowering window, so the body resolves the
    // pre-publication id and is parked behind the gate while one or more
    // new publications take over - two per (eval-current) (the whole-
    // workspace specialize pass runs twice) plus the rebind's. Erasing the
    // superseded ids made every such call die with "eval_flat: invalid
    // closure" (agent-fibers-8 failed in ~3 of 4 runs).
    //
    // The id -> define name row is what the live index would have held, so a
    // superseded id dispatches exactly like a name lookup: the define's
    // CURRENT publication runs. Nothing is retained (no interpreter / module
    // lifetime extension) and mark_all_defines_dirty drops the record on a
    // workspace replace / ast:restore, where a value from the previous
    // workspace must not resurrect at all (#2026 / #4349).
    //
    // Issue #4753: the per-define record must outlive any fiber that can
    // still be holding one of the define's retired ids. Eight ids was sized
    // for "one or two publications in flight", but a fiber can be
    // descheduled across a whole mutation window, and a window publishes
    // more than once (rebind re-eval + the eval-current passes): measured on
    // suite/fiber_rebind_while_running, a parked caller missed the 8-entry
    // window in 4 of 5 runs (dispatch fell through to the strict
    // runtime-mint deny). The cap is now a memory backstop only - one 8-byte
    // id plus a name per publication, ~0.25 MB per define at the cap -
    // instead of the resolution window. Every id retired within the define
    // current 4096 publications still resolves to the define, and
    // mark_all_defines_dirty still drops the whole record on a replace.
    static constexpr std::size_t kRetiredDefineHistory = 4096;
    void retire_ir_define_closure_(const std::string& name,
                                   const std::shared_ptr<IRDefineEnvBinding>& binding) {
        if (!binding || name.empty())
            return;
        const auto retired = binding->closure_id;
        retired_define_closure_owner_[retired] = name;
        auto& order = retired_define_closure_order_[name];
        order.push_back(retired);
        while (order.size() > kRetiredDefineHistory) {
            retired_define_closure_owner_.erase(order.front());
            order.erase(order.begin());
        }
    }

    // Issue #4753: record a dropped tree-walker publication id against its
    // define (see retired_tw_closure_owner_). Called from the evaluator
    // under the closure shard locks, so this takes only the leaf mutex.
    void note_retired_tw_closure_(std::uint64_t cid, const std::string& name) {
        if (cid == 0 || name.empty())
            return;
        const auto id = static_cast<aura::compiler::ClosureId>(cid);
        std::scoped_lock<std::mutex> lk(retired_tw_closure_mtx_);
        retired_tw_closure_owner_[id] = name;
        auto& order = retired_tw_closure_order_[name];
        order.push_back(id);
        while (order.size() > kRetiredDefineHistory) {
            retired_tw_closure_owner_.erase(order.front());
            order.pop_front();
        }
    }

    std::optional<std::string> retired_tw_closure_name_(aura::compiler::ClosureId cid) {
        std::scoped_lock<std::mutex> lk(retired_tw_closure_mtx_);
        auto it = retired_tw_closure_owner_.find(cid);
        if (it == retired_tw_closure_owner_.end())
            return std::nullopt;
        return it->second;
    }

    // ---- Define caching (shared by eval, eval_ir, define_function) -----

    // Issue #1506 / #1555: prefer partial re-lower (or clean-hit reuse)
    // when ir_cache_v2_ already has this define. Production eval / eval_ir
    // define paths use this so AI mutate:set-body / re-eval does not always
    // full-lower via cache_define.
    //
    // Decision (matches #1555 AC2):
    //   lookup_define_v2 → 0 (hit, clean): reuse; no lower
    //   lookup_define_v2 → 1 (dirty or source changed):
    //     if same source_hash && dirty blocks → relower_define_blocks
    //       (dispatches relower_define_function when dirty_func_count==1)
    //     else full cache_define
    //   lookup_define_v2 → 2 (miss): full cache_define
    // Issue #1506 / #1555 / #1601 / #1623: prefer partial re-lower.
    // `from_eval_ir` distinguishes eval vs eval_ir for AC metrics.
    EvalResult cache_define_prefer_partial(std::string_view source, aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool,
                                           aura::ast::NodeId expanded_root,
                                           const std::string& name_str, bool bind_in_env = true,
                                           const std::string& module_name = "__repl__",
                                           bool from_eval_ir = false) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        const std::string src(source);
        const auto hash = fnv1a_64(src);
        const int st = lookup_define_v2(name_str, hash);

        // Issue #1555 AC1: clean hit — reuse cached IR (no full lower).
        if (st == 0) {
            metrics_.relower_skipped_entirely_count.fetch_add(1, std::memory_order_relaxed);
            if (bind_in_env) {
                auto bound = evaluator_.top_env().lookup_binding(name_str);
                if (!bound) {
                    // Cached but unbound (e.g. after env reset) — full bind.
                    return cache_define(source, flat, pool, expanded_root, name_str, bind_in_env,
                                        module_name);
                }
            }
            return EvalResult(types::make_void());
        }

        if (st == 1) {
            auto it = ir_cache_v2_.find(name_str);
            // #1623: set-body / rebind change source_hash but mark body
            // dirty bitmasks — still prefer relower_define_blocks so AI
            // mutate loops do not skip partial solely due to hash mismatch.
            // Require dirty_block_count>0 when hash differs so we never
            // "skip" (clean-bitmask early return) with stale IR.
            const bool has_ir = it != ir_cache_v2_.end() && !it->second.irs.empty();
            const std::size_t dirty_n = has_ir ? it->second.dirty_block_count() : 0;
            // Prefer partial whenever dirty bitmasks fire — including when
            // source_hash changed (mutate :set-body). dirty_n>0 is required so
            // we never take the clean-bitmask early-return with stale IR.
            bool try_partial = has_ir && dirty_n > 0;
            // Issue #4422: production must consult impact before a per-fn
            // peel, including unknown ub==0. Helper false falls through to
            // cache_define (invalidate_function + record_dependency).
            // Soft/Off keeps ub==0 threshold partial and does not load impact.
            if (try_partial && (aura::compiler::typed_audit::production_defaults_active() ||
                                aura::compiler::typed_audit::get_strategy() ==
                                    aura::compiler::typed_audit::AuditStrategy::Full)) {
                const std::size_t impact_ub = impact_upper_bound_for_entry_(name_str, it->second);
                if (!should_partial_relower_impact_checked_prod(dirty_n, impact_ub,
                                                                /*production=*/true)) {
                    metrics_.partial_forced_full_by_impact_total.fetch_add(
                        1, std::memory_order_relaxed);
                    try_partial = false;
                }
            }
            if (try_partial) {
                // Prefer Lambda body node for per-function re-lower.
                aura::ast::NodeId expanded = expanded_root;
                if (expanded_root < flat.size()) {
                    auto dv = flat.get(expanded_root);
                    if (dv.tag == aura::ast::NodeTag::Define && !dv.children.empty()) {
                        auto body = dv.child(0);
                        if (body < flat.size() && flat.get(body).tag == aura::ast::NodeTag::Lambda)
                            expanded = body;
                    }
                }
                // #1555/#1601/#1623: dirty → relower_define_blocks first
                // (never full lower as the default when bitmasks fire).
                metrics_.eval_path_relower_total.fetch_add(1, std::memory_order_relaxed);
                if (from_eval_ir)
                    metrics_.eval_ir_path_relower_total.fetch_add(1, std::memory_order_relaxed);
                const auto per_before =
                    metrics_.relower_per_function_called_count.load(std::memory_order_relaxed);
                const auto blocks_before =
                    metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed);
                if (relower_only_dirty_blocks(name_str, source, flat, pool, expanded)) {
                    // True partial win (per-function / per-block), not
                    // internal full-fallback success of relower_define_blocks.
                    const bool true_partial =
                        metrics_.relower_per_function_called_count.load(std::memory_order_relaxed) >
                            per_before ||
                        metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed) >
                            blocks_before;
                    if (true_partial) {
                        metrics_.incremental_eval_relower_hits.fetch_add(1,
                                                                         std::memory_order_relaxed);
                        metrics_.incremental_partial_relower_total.fetch_add(
                            1, std::memory_order_relaxed);
                        // Issue #2032: stamp threshold used at partial decision.
                        metrics_.partial_relower_threshold_used.store(
                            get_partial_relower_threshold(), std::memory_order_relaxed);
                    }
                    // relower_define_blocks / store_define_v2 already stamp
                    // source_hash; avoid touching `it` (may invalidate on store).
                    // Partial path updated IR; still ensure env binding
                    // when requested (first define may not have bound).
                    if (bind_in_env) {
                        auto bound = evaluator_.top_env().lookup_binding(name_str);
                        if (!bound) {
                            // No env binding yet — full cache_define for bind.
                            return cache_define(source, flat, pool, expanded_root, name_str,
                                                bind_in_env, module_name);
                        }
                    }
                    return EvalResult(types::make_void());
                }
            }
            // Source changed, no IR, or partial failed → full path.
            metrics_.incremental_full_fallback_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #3892: st==2 cold miss — dedicated counter (not skip, not
        // fail-closed-full). st==1 fallback stays on incremental_full_fallback.
        if (st == 2)
            metrics_.incremental_relower_cold_miss_total.fetch_add(1, std::memory_order_relaxed);
        // st == 2 (miss) or st == 1 fallback
        return cache_define(source, flat, pool, expanded_root, name_str, bind_in_env, module_name);
    }

    // Lower a define expression to IR, cache it, and bind env via IR when possible.
    // Falls back to eval_flat on skip_ir_cache or IR bind failure.
    EvalResult cache_define(std::string_view source, aura::ast::FlatAST& flat,
                            aura::ast::StringPool& pool, aura::ast::NodeId expanded_root,
                            const std::string& name_str, bool bind_in_env = true,
                            const std::string& module_name = "__repl__");

    // ---- Accessors ---------------------------------------------------

    ast::ASTArena& arena() { return arena_; }
    // (Issue #1385: Evaluator& evaluator() public accessor already
    // exists above at line 5733 — see [[nodiscard]] declaration.)
    Evaluator& evaluator() { return evaluator_; }
    // Issue #354: passthrough for the
    // mutation-boundary-held check. Returns true
    // when an outermost MutationBoundaryGuard is
    // currently alive. Useful for tests + agent
    // monitoring; the underlying flag is an atomic
    // bool on the Evaluator (cheap read).
    bool mutation_boundary_held() const { return evaluator_.mutation_boundary_held(); }

    // Issue #62 Iter 1: observability counters accessor.
    // Surfaced via --evo-explain (Iter 3) and AuraQuery (Iter 4).
    CompilerMetrics& metrics() { return metrics_; }
    const CompilerMetrics& metrics() const { return metrics_; }

    // Issue #62 Iter 3: a POD snapshot of the observability state
    // for --evo-explain. Atomics are loaded with relaxed order;
    // counts are advisory, not contractual.
    //
    // Issue #1856: prefer try_snapshot() from EDSL stats primitives
    // so a throw cannot escape as an unhandled AuraError and so
    // failures are countable (snapshot_failures_total). snapshot()
    // itself remains for internal / CLI paths that already handle
    // errors.
    [[nodiscard]] std::optional<CompilerSnapshot> try_snapshot() const noexcept {
        try {
            return snapshot();
        } catch (...) {
            // [SILENCE-PRIM-#615] #1856: intentional nullopt + metric
            // (stats callers return void, not a false-clean zero hash).
            metrics_.snapshot_failures_total.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
    }

    CompilerSnapshot snapshot() const;
    void set_workspace_tree(void* wt) { evaluator_.set_workspace_tree(wt); }

    // Return current number of cached define functions
    std::size_t cached_function_count() const { return ir_cache_.size(); }

    // Inspect support: expose last parsed AST (for --inspect typecheck etc.)
    const aura::ast::FlatAST& last_flat() const { return *current_ast_; }
    const aura::ast::StringPool& last_pool() const { return *current_pool_; }

    // Expose evaluator env for --inspect evaluator
    std::string inspect_env() const {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        // Issue #209 (Cycle 3): migrated to use
        // bindings_with_names() instead of bindings().
        // The legacy bindings() accessor bumps the
        // bindings_legacy_uses metric; the new path
        // routes through bindings_symid_ + pool_->resolve()
        // (with '@symid:N' fallback for envs without
        // pool_ set, e.g., top_ which has pool_ == nullptr
        // per the post-canonical-pool migration).
        std::string out;
        auto& env = evaluator_.top_env();
        std::size_t count = 0;
        const aura::compiler::Env* e = &env;
        while (e) {
            // Use the new accessor (no metric bump).
            // For envs without pool_ (e.g., top_), the
            // names fall back to '@symid:N' (a reasonable
            // display for an env-inspector primitive).
            auto named = const_cast<aura::compiler::Env&>(*e).bindings_with_names();
            for (auto& [name, val] : named) {
                out += "  " + name + " → " + aura::compiler::types::format_value(val) + "\n";
                ++count;
            }
            e = e->parent();
        }
        return "env: " + std::to_string(count) + " bindings\n" + out;
    }

    // Check if a cached function exists
    bool has_cached_function(const std::string& name) const {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        return ir_cache_.find(name) != ir_cache_.end();
    }

    // ---- Phase 5: serve integration (define/exec JSON protocol) ----

    // Define a function: both tree-walker eval (for env persistence) and IR cache.
    // Returns the tree-walker evaluation result (for backward compat).
    // Define a function: both tree-walker eval (for env persistence) and IR cache.
    // Returns the tree-walker evaluation result (for backward compat).
    //
    // Dependency tracking: when lowering with cache, records which cached functions
    // this new definition calls. On redefinition, invalidates all transitive dependents.
    EvalResult define_function(std::string_view code) {
        // Arena-allocate FlatAST/Pool so closures can reference them across calls.
        auto alloc = arena_.allocator();
        aura::ast::StringPool* pool_ptr = nullptr;
        aura::ast::FlatAST* flat_ptr = nullptr;
        // Issue #4070: EXEMPT — stack void** dies before the next Moving rewrite.
        pool_ptr = arena_.create_with_cover<aura::ast::StringPool>(
            nullptr, "stack-cover-dies-at-return", alloc);
        flat_ptr = arena_.create_with_cover<aura::ast::FlatAST>(
            nullptr, "stack-cover-dies-at-return", alloc);
        if (!pool_ptr || !flat_ptr) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "define_function: arena allocate failed"});
        }
        auto pr = aura::parser::parse_to_flat(code, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;

        // Check if root is a Define node
        if (flat_ptr->get(flat_ptr->root).tag == aura::ast::NodeTag::Define) {
            auto name = pool_ptr->resolve(flat_ptr->get(flat_ptr->root).sym_id);
            // Issue #1601: prefer partial re-lower on redefine (same path as
            // eval/eval_ir via cache_define_prefer_partial) so serve define
            // after mutate:set-body does not always full-lower.
            auto result = cache_define_prefer_partial(code, *flat_ptr, *pool_ptr, flat_ptr->root,
                                                      std::string(name));
            return result; // tree-walker result (not void — serve protocol needs return value)
        }

        // Not a define -- just tree-walker eval
        return evaluator_.eval_flat(*flat_ptr, *pool_ptr, flat_ptr->root, evaluator_.top_env());
    }

    EvalResult exec_with_cache(std::string_view code) {
        // Use tree-walker (full language support including strings, modules)
        return eval(code);
    }

    // ── Persistent AST for mutation workflows ───────────────────

    // Parse input into a persistent AST (stored in the arena).
    // Subsequent typed_mutate / query_mutation_log calls operate on this AST.
    // Call set_code() again to replace the program.
    void set_code(std::string_view input) {
        auto alloc = arena_.allocator();
        current_ast_ = arena_.create_with_cover<aura::ast::FlatAST>(
            reinterpret_cast<void**>(&current_ast_), nullptr, alloc);
        current_pool_ = arena_.create_with_cover<aura::ast::StringPool>(
            reinterpret_cast<void**>(&current_pool_), nullptr, alloc);
        if (!current_ast_ || !current_pool_) {
            current_ast_ = nullptr;
            current_pool_ = nullptr;
            return;
        }
        auto pr = aura::parser::parse_to_flat(input, *current_ast_, *current_pool_);
        if (pr.success && pr.root != aura::ast::NULL_NODE) {
            current_ast_->root = pr.root;
        } else {
            current_ast_ = nullptr;
            current_pool_ = nullptr;
        }
    }

    // Evaluate the persistent AST (tree-walker only).
    EvalResult eval_current() {
        if (!current_ast_ || !current_pool_ || current_ast_->root == aura::ast::NULL_NODE) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::InternalError, "no code loaded — call set_code() first"});
        }
        return evaluator_.eval_flat(*current_ast_, *current_pool_, current_ast_->root,
                                    evaluator_.top_env());
    }

    // Result of a mutation operation.
    struct MutationResult {
        std::uint64_t mutation_id;
        bool success;
        std::string error;
        // Issue #147: post-mutation invariant check result.
        // invariant_status mirrors the per-record status on
        // MutationRecord.invariant_status (set after check runs).
        // invariant_diagnostics is empty when status==Ok or
        // NotChecked. In WarningsOnly mode, non-empty diagnostics
        // do NOT block success. In Strict mode, any non-empty
        // diagnostics turn success=false with the first diagnostic
        // promoted to .error.
        aura::ast::InvariantStatus invariant_status = aura::ast::InvariantStatus::NotChecked;
        std::vector<aura::compiler::OwnershipNote> invariant_diagnostics;
        // Issue #1538: combined linear post-mutate (runtime half of
        // #1458 + #1478 pipeline). linear_post_mutate_status is one of
        // "NotRun" | "Ok" | "Unsafe". Frames_checked is the number of
        // EnvFrames swept by linear_post_mutate_enforce_all.
        bool linear_post_mutate_enforced = false;
        bool linear_post_mutate_safe = true;
        std::uint64_t linear_post_mutate_frames_checked = 0;
        std::string linear_post_mutate_status = "NotRun";
    };

    // Issue #147: configure post-mutation invariant check mode.
    // - Disabled: check is skipped (status stays NotChecked, no
    //   diagnostics emitted). Useful for tight loops where the
    //   check would dominate runtime.
    // - WarningsOnly (default): check runs, notes surfaced via
    //   MutationResult::invariant_diagnostics, success is not
    //   affected.
    // - Strict: any note causes typed_mutate to return
    //   success=false with the first note's message promoted to
    //   MutationResult::error.
    void set_invariant_check_mode(InvariantCheckMode m) {
        if (invariant_check_mode_ != m) {
            invariant_check_mode_ = m;
            ++mode_flip_count_; // Issue #1383: throttle disable-warn.
        }
    }
    InvariantCheckMode invariant_check_mode() const { return invariant_check_mode_; }

    // Issue #1383: read accumulated eval-mode warnings. Cleared
    // via clear_eval_warnings(). Includes the one-shot warning
    // emitted when invariant_check_mode is set to Disabled on a
    // workspace with prior typed mutations (so an operator can
    // see the silent-bypass risk). Surfaced via (eval-warnings)
    // primitive and `--serve warnings`.
    [[nodiscard]] const std::vector<std::string>& eval_warnings() const noexcept {
        return mode_warnings_;
    }
    void clear_eval_warnings() noexcept { mode_warnings_.clear(); }

    // Issue #411: post-mutation auto-incremental typecheck mode.
    // See the IncrementalTypecheckMode enum above for the three
    // modes and their semantics. Default Eager; switch to Lazy or
    // Disabled in batch-mutation pipelines to defer the
    // typecheck cost. The mode is read at the end of a successful
    // typed_mutate (after tx.commit, after the post-mutation
    // invariant check, after the macro re-expand) and routes
    // through TypeChecker::infer_flat_partial on the workspace
    // flat (post-COW) — the same path that the manual
    // (typecheck-incremental) Aura primitive uses.
    void set_incremental_typecheck_mode(IncrementalTypecheckMode m) {
        incremental_typecheck_mode_ = m;
    }
    IncrementalTypecheckMode incremental_typecheck_mode() const {
        return incremental_typecheck_mode_;
    }

    // Issue #411: auto-invoke helper. Runs
    // TypeChecker::infer_flat_partial on the supplied flat +
    // pool for the given mutation record, accumulates
    // per-call engine stats into CompilerMetrics, and bumps
    // the lifetime-total auto-invocation counters. Called
    // from both the typed_mutate C++ method (post-tx.commit
    // path) and the cs.eval method (post-eval path, when the
    // mutation log grew during the eval). The `source` tag
    // is for the debug log message only.
    //
    // Returns the number of nodes re-inferred. The caller can
    // ignore the return value; the metrics are what matter.
    std::size_t auto_invoke_incremental_typecheck_for(aura::ast::FlatAST& flat,
                                                      aura::ast::StringPool& pool,
                                                      const aura::ast::MutationRecord& rec,
                                                      std::uint64_t mutation_id_for_log,
                                                      const char* source);

    // Issue #411: RAII guard that fires the post-eval auto-
    // incremental typecheck on scope exit. Constructed at the
    // entry of each public eval method (eval / eval_ir / exec_jit
    // / typed_mutate) and captures the workspace mutation_log_
    // size at construction time. On destruction, if the log grew
    // during the eval (i.e. the user called a mutate:* primitive)
    // and the mode is Eager, the guard runs infer_flat_partial
    // on the most recent MutationRecord via
    // auto_invoke_incremental_typecheck_for(). The guard is a
    // no-op when the mode is Lazy / Disabled or when the log
    // didn't grow.
    //
    // The guard-based design (vs explicit calls at every return
    // point) covers the early returns from cs.eval (the
    // tree-walker fallback, the IR pipeline's pre_const_eval
    // path, etc.) without invasive edits to every return
    // statement. Same pattern as MutationBoundaryGuard in
    // evaluator.ixx. The guard is a private nested class (not
    // exported) because it's only used by CompilerService's
    // own methods.
    struct PostEvalAutoInvokeGuard {
        CompilerService* service;
        std::size_t entry_size;
        bool fired = false;
        PostEvalAutoInvokeGuard(CompilerService* s, std::size_t e)
            : service(s)
            , entry_size(e) {}
        ~PostEvalAutoInvokeGuard() {
            if (fired || service == nullptr)
                return;
            if (auto* ws_flat = service->evaluator_.workspace_flat()) {
                if (auto* ws_pool = service->evaluator_.workspace_pool()) {
                    const auto& log = ws_flat->all_mutations();
                    if (!log.empty() && entry_size < log.size()) {
                        const auto& rec = log.back();
                        if (rec.target_node != aura::ast::NULL_NODE ||
                            rec.parent_id != aura::ast::NULL_NODE) {
                            service->auto_invoke_incremental_typecheck_for(
                                *ws_flat, *ws_pool, rec, rec.mutation_id, "PostEvalGuard");
                        }
                    }
                }
            }
        }
        PostEvalAutoInvokeGuard(const PostEvalAutoInvokeGuard&) = delete;
        PostEvalAutoInvokeGuard& operator=(const PostEvalAutoInvokeGuard&) = delete;
        PostEvalAutoInvokeGuard(PostEvalAutoInvokeGuard&&) = delete;
        PostEvalAutoInvokeGuard& operator=(PostEvalAutoInvokeGuard&&) = delete;
    };

    // Issue #169: incremental-strictness setter/getter.
    // See the IncrementalStrictness enum above for semantics.
    // The default is Balanced (existing behavior). Future
    // Goals 1-4 of #169 will read this flag to decide between
    // safe over-invalidation (Conservative) and precise
    // minimal invalidation (Aggressive).
    void set_incremental_strictness(IncrementalStrictness s) { incremental_strictness_ = s; }
    IncrementalStrictness incremental_strictness() const { return incremental_strictness_; }

    // Mutation log entry (for JSON serialization).
    struct MutationLogEntry {
        std::uint64_t mutation_id;
        std::uint64_t timestamp_ms;
        std::uint32_t target_node;
        std::string operator_name;
        std::string old_type;
        std::string new_type;
        std::string summary;
        std::string status; // "Committed" or "RolledBack"
        // Issue #147: per-record invariant check status. Stringified
        // to one of "NotChecked" | "Ok" | "Warnings" | "Violations"
        // so JSON consumers don't need to know the enum's numeric
        // encoding. Default "NotChecked" for records that pre-date
        // the post-mutation check or were never run.
        std::string invariant_status;
        // Issue #1419: compound provenance (0 = system / none).
        std::uint64_t author_fingerprint = 0;
        std::uint64_t parent_mutation_id = 0;
        std::uint64_t composite_transaction_id = 0;
        // Issue #1538: linear post-mutate enforce status for --serve
        // mutation-log JSON ("NotRun" | "Ok" | "Unsafe").
        std::string linear_post_mutate_status = "NotRun";
    };

    // RAII transaction guard for mutation operations.
    // Records the current mutation state on construction.
    // If commit() is not called before destruction, automatically
    // rolls back all mutations since construction point.
    struct MutationTransaction {
        aura::ast::FlatAST* ast;
        std::uint64_t snapshot_id;
        bool committed = false;

        MutationTransaction(aura::ast::FlatAST& a)
            : ast(&a)
            , snapshot_id(a.next_mutation_id()) {}

        void commit() { committed = true; }

        ~MutationTransaction() {
            if (!committed && ast) {
                ast->rollback_since(snapshot_id);
            }
        }

        // Disallow copy
        MutationTransaction(const MutationTransaction&) = delete;
        MutationTransaction& operator=(const MutationTransaction&) = delete;
        MutationTransaction& operator=(MutationTransaction&&) = delete;
        // Allow move
        MutationTransaction(MutationTransaction&& other) noexcept
            : ast(other.ast)
            , snapshot_id(other.snapshot_id)
            , committed(other.committed) {
            other.ast = nullptr;
        }
    };

    // Issue #1408: RAII transaction guard for atomic multi-mutate.
    // Records the current mutation_id on construction. If commit() is
    // not called before destruction, automatically rolls back ALL
    // mutations since the snapshot via
    // workspace_flat_->rollback_since(snapshot_id).
    //
    // Issue #3918 follow-up: after typed-mutate-atomic abort restores
    // AST, re-eval top-level Defines into env cells. Lockless rebind
    // writes cells in place; rollback_since does not undo that.
    void resync_top_env_from_workspace_after_abort() {
        auto* flat = evaluator_.workspace_flat();
        auto* pool = evaluator_.workspace_pool();
        if (!flat || !pool)
            return;
        const auto defs = aura::compiler::find_top_level_defines(*flat, *pool, flat->root);
        auto& tenv = evaluator_.top_env();
        for (const auto& [name, def_id] : defs) {
            if (name.empty() || def_id >= flat->size())
                continue;
            auto dv = flat->get(def_id);
            if (dv.children.empty())
                continue;
            auto body = dv.child(0);
            if (body >= flat->size() || flat->is_free_slot(body))
                continue;
            auto refreshed = evaluator_.eval_flat(*flat, *pool, body, tenv);
            if (!refreshed)
                continue;
            auto existing = tenv.lookup_binding(name);
            if (!existing || !types::is_cell(*existing))
                continue;
            const auto ci = types::as_cell_id(*existing);
            if (ci < evaluator_.cells().size())
                evaluator_.cells()[ci] = *refreshed;
        }
    }

    // Used by typed_mutate_atomic to wrap N typed_mutate calls into
    // a single atomic operation: if any one fails, all prior are
    // rolled back. Distinct from MutationTransaction (which wraps
    // current_ast_); TypedTransactionGuard is for cross-mutate
    // atomicity on the workspace.
    struct TypedTransactionGuard {
        CompilerService* svc;
        aura::ast::FlatAST* ws_flat;
        std::uint64_t snapshot_id;
        std::vector<std::uint64_t> applied_mutation_ids;
        bool committed = false;
        // Issue #1419: composite provenance for the atomic batch.
        std::uint64_t composite_tx_id = 0;
        std::uint64_t prev_author = 0;
        std::uint64_t prev_parent = 0;
        std::uint64_t prev_composite = 0;
        bool provenance_active = false;

        explicit TypedTransactionGuard(CompilerService* s)
            : svc(s)
            , ws_flat(s->evaluator_.workspace_flat())
            , snapshot_id(ws_flat ? ws_flat->next_mutation_id() : 0) {
            // Issue #1419: open a composite transaction on the
            // workspace so every sub-mutation stamps the same
            // composite_transaction_id. Parent chain: first
            // mutation is root (parent=0); subsequent ones link
            // to the first applied mutation_id.
            if (ws_flat) {
                prev_author = ws_flat->mutation_author_fingerprint();
                prev_parent = ws_flat->mutation_parent_mutation_id();
                prev_composite = ws_flat->mutation_composite_transaction_id();
                // Prefer evaluator agent fingerprint when set.
                const auto agent_fp = s->evaluator_.current_agent_fingerprint();
                if (agent_fp != 0)
                    ws_flat->set_mutation_author_fingerprint(agent_fp);
                composite_tx_id = snapshot_id != 0 ? snapshot_id : 1;
                ws_flat->set_mutation_composite_transaction_id(composite_tx_id);
                ws_flat->set_mutation_parent_mutation_id(0);
                provenance_active = true;
            }
        }

        // Apply one mutation. Returns true on success (records id),
        // false on failure (caller should stop, destructor will roll back).
        bool apply_one(std::string_view sexpr) {
            auto result = svc->typed_mutate(sexpr);
            if (!result.success)
                return false;
            applied_mutation_ids.push_back(result.mutation_id);
            // After the first sub-mutation, chain subsequent records
            // to the batch root (parent_mutation_id = first mid).
            if (ws_flat && applied_mutation_ids.size() == 1)
                ws_flat->set_mutation_parent_mutation_id(result.mutation_id);
            return true;
        }

        MutationResult commit() {
            if (committed)
                return {0, false, "double-commit"};
            committed = true;
            restore_provenance();
            return MutationResult{0, true, "", aura::ast::InvariantStatus::Ok, {}};
        }

        void restore_provenance() noexcept {
            if (!provenance_active || !ws_flat)
                return;
            ws_flat->set_mutation_author_fingerprint(prev_author);
            ws_flat->set_mutation_parent_mutation_id(prev_parent);
            ws_flat->set_mutation_composite_transaction_id(prev_composite);
            provenance_active = false;
        }

        ~TypedTransactionGuard() {
            if (!committed && ws_flat && snapshot_id > 0) {
                // Issue #1408 RAII rollback — two-phase:
                //
                // Phase 1 (rollback_since): undo effects of mutations added
                // during the batch. Structural ops restore children_; Issue
                // #1441 try_rollback_rebind_op restores mutate:rebind Define
                // bodies when the rebind record has has_rollback_data.
                //
                // Phase 2 (erase_mutations_since): physically erase all
                // records with mutation_id >= snapshot_id from the audit
                // log. This guarantees the "0 applied" AC
                // (committed_mutation_count() returns to its pre-batch
                // value) regardless of whether Phase 1 actually undid
                // the underlying effects. The two-phase approach gives
                // us a strict contract on the audit-log side while
                // leaving the variable-state follow-up to a separate
                // issue.
                ws_flat->rollback_since(snapshot_id);
                ws_flat->erase_mutations_since(snapshot_id);
                // Issue #3918: lockless rebind env-refreshes cells in
                // place. AST rollback does not restore those cells, so
                // typed-mutate-atomic abort left x/y at the rebound
                // values. Re-eval restored top-level Defines into env.
                svc->resync_top_env_from_workspace_after_abort();
            }
            restore_provenance();
        }

        // Disallow copy
        TypedTransactionGuard(const TypedTransactionGuard&) = delete;
        TypedTransactionGuard& operator=(const TypedTransactionGuard&) = delete;
        TypedTransactionGuard& operator=(TypedTransactionGuard&&) = delete;
        // Allow move
        TypedTransactionGuard(TypedTransactionGuard&& other) noexcept
            : svc(other.svc)
            , ws_flat(other.ws_flat)
            , snapshot_id(other.snapshot_id)
            , applied_mutation_ids(std::move(other.applied_mutation_ids))
            , committed(other.committed)
            , composite_tx_id(other.composite_tx_id)
            , prev_author(other.prev_author)
            , prev_parent(other.prev_parent)
            , prev_composite(other.prev_composite)
            , provenance_active(other.provenance_active) {
            other.svc = nullptr;
            other.ws_flat = nullptr;
            other.provenance_active = false;
        }
    };

    // Issue #1408: Atomic multi-mutate. Runs N typed_mutate calls as one
    // transaction — all-or-nothing semantics via TypedTransactionGuard
    // (RAII rollback if any sub-mutation fails).
    //
    // AC:
    //  - all sub-mutations succeed → all visible (mutation_log entries
    //    per individual sub-mutation; typed_mutate internally bumps
    //    mutation_id once per call)
    //  - any sub-mutation fails → 0 applied (rollback_since on snapshot)
    //  - on success, single combined post_mutation_invariant_check runs
    //    once on the dirty union (Issue #1408 AC3 — follows from
    //    per-mutation typed_mutate calls already running their checks;
    //    a future issue can suppress per-mutation checks during the
    //    batch and run only the combined check at commit)
    // Issue #1524: typed_mutate_atomic is the multi-mutate entry; each
    // sub-call goes through typed_mutate which uses
    // atomic_bump_epochs_and_stamp_bridge on success. Do not add a
    // second bare bump_bridge_epoch here.
    [[nodiscard]] MutationResult typed_mutate_atomic(std::span<const std::string_view> mutations) {
        if (mutations.empty()) {
            return {0, false, "empty mutations"};
        }
        if (!current_ast_ || !current_pool_) {
            return {0, false, "no AST loaded"};
        }
        TypedTransactionGuard guard{this};

        // Issue #1415: batch optimization — suppress per-mutation
        // post_mutation_invariant_check during the batch. The
        // combined check on the dirty union runs once at commit
        // below. Same final status, 1/N the work for N mutations.
        // Restore the flag before running the combined check so
        // the check path itself is not gated.
        const bool prev_suppress = suppress_invariant_check_;
        suppress_invariant_check_ = true;

        for (const auto& m : mutations) {
            if (!guard.apply_one(m)) {
                // Destructor will roll back via TypedTransactionGuard's RAII.
                // Restore suppress flag before returning so subsequent
                // single typed_mutate runs its check normally.
                suppress_invariant_check_ = prev_suppress;
                return {0, false, "tx_abort"};
            }
        }

        // Restore the flag before running the combined check.
        suppress_invariant_check_ = prev_suppress;

        auto result = guard.commit();
        if (result.success) {
            // Combined post_mutation_invariant_check on the dirty
            // union. Same pipeline as the per-mutation check
            // (PostMutationInvariantVisitor walks the workspace
            // log and emits OwnershipNotes), but runs once for the
            // batch instead of N times — the result is identical
            // because the visitor's analysis depends on the final
            // workspace state, not the intermediate states.
            auto* ws_flat = evaluator_.workspace_flat();
            if (ws_flat) {
                // Issue #2286: pass current_cache_epoch so the
                // OwnershipEscapeSummary publish key (metrics, cache_epoch)
                // matches what CompilerService uses for the thread-local
                // lookup before lower_to_ir.
                aura::compiler::PostMutationInvariantVisitor invariant_visitor(
                    *current_pool_, type_registry_, &metrics_, evaluator_.current_cache_epoch());
                aura::ast::run_mutation_pipeline(*ws_flat, invariant_visitor);
                invariant_visitor.apply_status_updates(*ws_flat);
                result.invariant_status = invariant_visitor.worst_status();
                result.invariant_diagnostics =
                    std::vector<aura::compiler::OwnershipNote>(invariant_visitor.notes());
            }
            // Issue #1538: runtime linear post-mutate half (after type-checker).
            apply_linear_post_mutate_pipeline_(result, result.mutation_id);
            // Issue #1524: batch-level dual-epoch catch-all after all
            // sub-mutations committed. typed_mutate already ran the helper
            // per success; this final empty-name stamp ensures any define
            // only dirtied mid-batch still sees the new epoch on both domains.
            atomic_bump_epochs_and_stamp_bridge(/*name=*/{});
            metrics_.typed_mutate_atomic_invalidations_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
        }
        return result;
    }

    // Evaluate an S-expression by parsing it INTO persistent AST (current_ast_).
    // Issue #958: also wrap MutationBoundaryGuard (same contract as
    // typed_mutate) so defuse_version_ / workspace_mtx_ stay consistent
    // when --serve routes mutate/rollback through eval_on_current.
    EvalResult eval_on_current(std::string_view sexpr) {
        if (!current_ast_ || !current_pool_) {
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError, "no AST loaded"});
        }
        MutationTransaction tx(*current_ast_);
        auto pr = aura::parser::parse_to_flat(sexpr, *current_ast_, *current_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        bool boundary_success = true;
        // Issue #1547 / #1556: typed try_acquire (quota → AuraError, not throw).
        auto guard_r = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
            evaluator_, /*pending_count=*/1, &boundary_success);
        if (!guard_r) {
            // Surface ResourceQuotaExceeded message for Agents (retry/skip).
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::OutOfMemory,
                                                          std::string("ResourceQuotaExceeded: ") +
                                                              guard_r.error().message});
        }
        auto guard = std::move(*guard_r);
        auto result =
            evaluator_.eval_flat(*current_ast_, *current_pool_, pr.root, evaluator_.top_env());
        if (result) {
            tx.commit();
        } else {
            boundary_success = false;
        }
        return result;
    }

    // Apply a mutation expression by parsing it INTO the persistent AST.
    // Returns the mutation ID (0 on failure).
    [[nodiscard]] MutationResult typed_mutate(std::string_view sexpr);

    // Query mutation log for a specific node (or all nodes if NULL_NODE).
    //
    // Issue #147 follow-up: mutation primitives (mutate :rebind,
    // mutate:replace-type, ...) append MutationRecord entries to
    // workspace_flat_ — NOT current_ast_. The pre-#147 implementation
    // read from current_ast_, so callers saw an empty log even after
    // a successful typed_mutate. Now we read from workspace_flat_
    // first (the canonical source for typed_mutate records), with a
    // fallback to current_ast_ for backward compatibility with
    // tests that manually add records to the C++ set_code AST.
    std::vector<MutationLogEntry>
    query_mutation_log(aura::ast::NodeId node = aura::ast::NULL_NODE) const {
        std::vector<MutationLogEntry> result;
        const auto* source = evaluator_.workspace_flat();
        if (!source)
            source = current_ast_;
        if (!source)
            return result;
        // Issue #1389: acquire workspace shared lock for the
        // duration of the mutation_log_ copy. Without this,
        // a concurrent typed_mutate from another thread can
        // push_back mid-iteration (std::pmr::vector reallocation
        // invalidates the iterator → use-after-free / size
        // mismatch → UB).
        //
        // Mirror the const_cast pattern at service.ixx:6222-6250
        // (CompilerMetrics::snapshot): lock_workspace_shared is
        // non-const but the shared_mutex's internal state is
        // mutable, so const_cast on the Evaluator reference is
        // safe. Plain lock_shared (not try_lock_shared) is used
        // here because query_mutation_log is a leaf call from
        // --serve protocol / test code, not from the IR
        // interpreter. The serialize-workspace pattern at
        // evaluator_primitives_persist.cpp:206-227 is the same
        // (plain lock_shared).
        auto& eval_mut = const_cast<Evaluator&>(evaluator_);
        eval_mut.lock_workspace_shared();
        struct UnlockGuard : ::aura::core::NoCopy {
            Evaluator& e;
            ~UnlockGuard() { e.unlock_workspace_shared(); }
        } guard{{}, eval_mut};
        auto hist = (node == aura::ast::NULL_NODE) ? source->all_mutations()
                                                   : source->mutation_history(node);
        for (auto& rec : hist) {
            // Issue #147: stringify the per-record invariant_status enum
            // so the JSON response is human-readable.
            const char* ist = "NotChecked";
            switch (rec.invariant_status) {
                case aura::ast::InvariantStatus::Ok:
                    ist = "Ok";
                    break;
                case aura::ast::InvariantStatus::Warnings:
                    ist = "Warnings";
                    break;
                case aura::ast::InvariantStatus::Violations:
                    ist = "Violations";
                    break;
                case aura::ast::InvariantStatus::NotChecked:
                default:
                    ist = "NotChecked";
                    break;
            }
            // Issue #1538: linear post-mutate status from combined pipeline.
            std::string lin_status = "NotRun";
            if (auto lit = mutation_linear_status_.find(rec.mutation_id);
                lit != mutation_linear_status_.end()) {
                lin_status = lit->second;
            }
            result.push_back(MutationLogEntry{
                rec.mutation_id, rec.timestamp_ms, rec.target_node, rec.operator_name,
                rec.old_type_str, rec.new_type_str, rec.summary,
                rec.status == aura::ast::MutationStatus::Committed ? "Committed" : "RolledBack",
                ist,
                // Issue #1419: surface compound provenance
                rec.author_fingerprint, rec.parent_mutation_id, rec.composite_transaction_id,
                lin_status});
        }
        return result;
    }

    // Get the current persistent AST (for direct inspection).
    aura::ast::FlatAST* current_ast() const { return current_ast_; }
    aura::ast::StringPool* current_pool() const { return current_pool_; }

    // Issue #147: accessor for the evaluator's workspace FlatAST.
    // The workspace is where mutation primitives (mutate :rebind,
    // mutate:replace-type, ...) append MutationRecords. Note that
    // the workspace pointer can be replaced by lazy COW during
    // eval, so callers should re-query this after each mutation.
    // Returns nullptr if no workspace is loaded.
    aura::ast::FlatAST* workspace_flat() { return evaluator_.workspace_flat(); }
    aura::ast::StringPool* workspace_pool() { return evaluator_.workspace_pool(); }

    // Issue #411 fu1 follow-up #2: per-DefUseIndex caller
    // tracker accessor. The tracker is per-service
    // (lifetime = CompilerService lifetime). Aura
    // primitives access it via the Evaluator's
    // compiler_service_ void* pointer. The tracker is
    // NOT auto-populated by mutations yet — that's the
    // next follow-up. For now, the Aura primitives
    // (compile:per-defuse-index-add, etc.) let the user
    // populate it explicitly, which is the same pattern
    // as the existing dep_caller_fn_ registration hooks.
    per_defuse_index::PerDefUseIndexTracker& per_defuse_index_tracker() {
        return per_defuse_index_tracker_;
    }
    const per_defuse_index::PerDefUseIndexTracker& per_defuse_index_tracker() const {
        return per_defuse_index_tracker_;
    }

    // Get last compiled IR module (for --inspect dump).
    const std::optional<aura::ir::IRModule>& last_ir_module() const { return last_ir_mod_; }
    // Issue #375: snapshot of the encoding stats computed when
    // last_ir_mod_ was set. Read by (compile:ir-stats) Aura
    // primitive + by C++ tests that need AoS-vs-compact numbers
    // without going through the primitive (which would clobber
    // last_ir_mod_ on its own compilation).
    const aura::ir::IRStatsSnapshot& last_ir_stats() const noexcept { return last_ir_stats_; }

    // Issue #2100: after JIT deopt of MacroIntroduced code, re-apply
    // IRInstruction::source_marker + provenance (+ source_ast_node_id)
    // onto the workspace FlatAST so Agents still see is_macro_introduced
    // and provenance-blame after expand→JIT→deopt. Uses the durable
    // macro_ir_attr_cache_ (survives last_ir form overwrite) plus a
    // fresh walk of last_ir_mod_. Returns # of nodes restored.
    // Bumps jit_macro_introduced_preserved/lost totals.
    std::uint64_t restore_macro_introduced_from_ir_after_deopt() noexcept {
        auto* flat = evaluator_.workspace_flat();
        if (!flat)
            return 0;
        std::uint64_t restored = 0;
        std::uint64_t lost = 0;
        constexpr auto kExpansion =
            static_cast<std::uint8_t>(aura::ast::FlatAST::MacroDirtyReason::kMacroExpansion);
        auto apply_one = [&](std::uint32_t node_u32, std::uint32_t provenance) {
            const auto nid = static_cast<aura::ast::NodeId>(node_u32);
            if (nid == 0 || nid >= flat->size() || !flat->is_live_node(nid)) {
                ++lost;
                return;
            }
            flat->set_marker(nid, aura::ast::SyntaxMarker::MacroIntroduced);
            if (provenance != 0)
                flat->set_provenance(nid, provenance);
            flat->apply_macro_dirty_bits(nid, kExpansion);
            ++restored;
        };
        // Durable cache first (attrs from earlier forms / cached defines).
        for (const auto& e : macro_ir_attr_cache_)
            apply_one(e.source_ast_node_id, e.provenance);
        // Also walk current last_ir for any attrs not yet cached.
        if (last_ir_mod_) {
            for (const auto& fn : last_ir_mod_->functions) {
                for (const auto& blk : fn.blocks) {
                    for (const auto& instr : blk.instructions) {
                        if (instr.source_marker != 1 || instr.source_ast_node_id == 0)
                            continue;
                        bool already = false;
                        for (const auto& e : macro_ir_attr_cache_) {
                            if (e.source_ast_node_id == instr.source_ast_node_id) {
                                already = true;
                                break;
                            }
                        }
                        if (!already)
                            apply_one(instr.source_ast_node_id, instr.provenance);
                    }
                }
            }
        }
        if (restored) {
            aura_jit_macro_introduced_preserved_inc(restored);
            // Issue #2764: multi-eval denseness / rebind — restore success
            // means MacroIntroduced identity survived deopt (Agents sample
            // multi-eval-macro-marker-preserved-total under #2736 loads).
            aura_multi_eval_macro_marker_preserved_inc(restored);
        }
        if (lost)
            aura_jit_macro_introduced_lost_inc(lost);
        return restored;
    }

    // Issue #2100: # of durable MacroIntroduced IR attrs retained for deopt.
    std::uint64_t macro_ir_attr_cache_size() const noexcept {
        return static_cast<std::uint64_t>(macro_ir_attr_cache_.size());
    }

    // Issue #2100 test/Agent helper: seed durable IR attrs from live
    // MacroIntroduced workspace nodes when natural IR lowering has not
    // yet filled the cache (e.g. tree-walker-only forms). Returns # stamped.
    std::uint64_t seed_macro_ir_attrs_from_workspace() noexcept {
        auto* flat = evaluator_.workspace_flat();
        if (!flat)
            return 0;
        std::uint64_t n = 0;
        for (aura::ast::NodeId id = 0; id < flat->size(); ++id) {
            if (!flat->is_live_node(id) || !flat->is_macro_introduced(id))
                continue;
            const auto nid_u = static_cast<std::uint32_t>(id);
            const auto prov = flat->provenance(id);
            bool found = false;
            for (auto& e : macro_ir_attr_cache_) {
                if (e.source_ast_node_id == nid_u) {
                    e.provenance = prov;
                    found = true;
                    break;
                }
            }
            if (!found) {
                macro_ir_attr_cache_.push_back(MacroIrAttrEntry{nid_u, prov});
                ++n;
            }
        }
        // Mirror into last_ir_mod_ when present so walks see attrs too.
        if (last_ir_mod_ && n > 0) {
            if (last_ir_mod_->functions.empty()) {
                aura::ir::IRFunction stub;
                stub.name = "__macro_hygiene_2100";
                stub.blocks.emplace_back();
                last_ir_mod_->functions.push_back(std::move(stub));
            }
            auto& blk = last_ir_mod_->functions.back().blocks.back();
            for (const auto& e : macro_ir_attr_cache_) {
                aura::ir::IRInstruction instr;
                instr.opcode = aura::ir::IROpcode::Nop;
                instr.source_marker = 1;
                instr.source_ast_node_id = e.source_ast_node_id;
                instr.provenance = e.provenance;
                blk.instructions.push_back(instr);
            }
        }
        return n;
    }

    // Issue #3124: non-allocating arena compact hook (ShapeProfiler + IR dirty).
    static void on_arena_compact_hook_thunk(void* ctx) noexcept {
        if (ctx)
            static_cast<CompilerService*>(ctx)->on_arena_compact_notify();
    }
    void on_arena_compact_notify() noexcept {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        // Issue #1521 / #3455: ShapeProfiler versions dirty ∪ relocated
        // FnKeys only (IR cache any_block_dirty — existing define-dirty
        // mask). Empty cone → no-op on shape (touched==0). Do not walk
        // every tracked FnKey. Storm isolation (#2617) unchanged.
        std::vector<shape::FnKey> cone;
        cone.reserve(ir_cache_v2_.size());
        for (auto& [name, entry] : ir_cache_v2_) {
            if (!entry.any_block_dirty())
                continue;
            cone.push_back(shape::make_fn_key(session_id_, name));
        }
        const auto touched = shape_profiler_.on_arena_compact(cone);
        // Issue #2984: note_arena_compact_linear_root_consistency (count).
        // Issue #3227: rebind / reject + invalidate_gen after compact.
        // Issue #3448: last==0 green face still drops (not quiet).
        (void)typed_audit::rebind_linear_proof_after_root_migration();
        metrics_.shape_inval_on_compact_triggered_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.shape_stability_post_compact_preserved_total.store(
            shape::shape_stability_post_compact_preserved.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        evaluator_.clear_sym_intern_after_compact();
        metrics_.deopt_from_arena_compact_total.store(
            shape::deopt_from_arena_compact_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        (void)touched;

        if (aura::core::arena_policy::try_render_deopt_throttle(500)) {
            metrics_.arena_compact_deopt_triggered_total.fetch_add(1, std::memory_order_relaxed);
            aura::core::arena_policy::record_compact_deopt_triggered();
            aura::core::arena_policy::signal_jit_deopt_pressure();
        } else {
            metrics_.arena_compact_deopt_throttled_total.fetch_add(1, std::memory_order_relaxed);
            aura::core::arena_policy::record_compact_deopt_throttled();
            aura::core::arena_policy::signal_jit_deopt_pressure();
        }
        (void)shape_profiler_.on_boundary_or_fiber_sync(/*clear_compact_only_storm=*/true);
        aura::core::arena_policy::record_post_compact_resync();
        aura::core::arena_policy::publish_shape_deopt_metrics(shape_profiler_.deopt_rate_per_fn(),
                                                              shape_profiler_.deopt_storm_active(),
                                                              shape_profiler_.shape_stable_ratio());
        aura::core::arena_policy::record_shape_inval_on_compact();
        // Issue #3356: densify success + dirty-cone-limited IR rewrite.
        // Pins / EnvFrame known-root slots already remapped in live_compact
        // via last_object_remap_ (objects_moved>0). Do NOT mark_all_blocks_dirty
        // the whole cache (O(module) deopt). objects_moved==0 (empty remap)
        // or empty dirty cone → zero extra IR restamp (AC2). Non-empty cone:
        // finish_dirty_sync only already-dirty entries (AC1). Production pin
        // rewrite-miss stays fail-closed in verify_pins_under_moving_compact.
        const bool moved = arena_.object_remap_size() > 0;
        std::uint64_t cone_n = 0;
        if (moved) {
            for (auto& [_, entry] : ir_cache_v2_) {
                if (!entry.any_block_dirty())
                    continue; // outside cone: leave alone (no global restamp)
                ++cone_n;
                (void)entry.force_soa_instruction_dirty_sync();
            }
        }
        aura::core::post_compact_lifecycle::note_densify_cone_rewrite(cone_n);
        aura::core::post_compact_lifecycle::note_lifecycle_ir_sync();
    }

private:
    // Issue #2100: durable MacroIntroduced IR attrs for deopt restore.
    // last_ir_mod_ is overwritten per eval form; this cache accumulates.
    struct MacroIrAttrEntry {
        std::uint32_t source_ast_node_id = 0;
        std::uint32_t provenance = 0;
    };
    std::vector<MacroIrAttrEntry> macro_ir_attr_cache_;
    // Issue #4436: callers a full relower refused because the bundle copied
    // a callee whose v2 row is still dirty. One retry at the end of the peel.
    // Not a metric and not a query key.
    std::vector<std::string> embed_refused_4436_;
    // Callees precompute marked that were already clean and content-stored.
    // The scheduling mark does not rewrite irs. Not a metric or a query key.
    std::vector<std::string> embed_safe_callees_4436_;

    void note_macro_ir_attrs_from_module(const aura::ir::IRModule& mod) {
        for (const auto& fn : mod.functions) {
            for (const auto& blk : fn.blocks) {
                for (const auto& instr : blk.instructions) {
                    if (instr.source_marker != 1 || instr.source_ast_node_id == 0)
                        continue;
                    bool found = false;
                    for (auto& e : macro_ir_attr_cache_) {
                        if (e.source_ast_node_id == instr.source_ast_node_id) {
                            e.provenance = instr.provenance;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        macro_ir_attr_cache_.push_back(
                            MacroIrAttrEntry{instr.source_ast_node_id, instr.provenance});
                    }
                }
            }
        }
    }
    // Issue #1457 / #1874: surface TypePropagationPass work into metrics.
    void accumulate_type_propagation_metrics(const aura::compiler::TypePropagationPass& tp) {
        metrics_.type_propagation_runs_.fetch_add(1, std::memory_order_relaxed);
        const auto total =
            tp.propagated_count() + tp.cast_result_stamped() + tp.narrow_propagated();
        if (total > 0) {
            metrics_.type_propagation_total_.fetch_add(total, std::memory_order_relaxed);
        }
        // Issue #1530: extended opcode stamp hits.
        if (tp.extended_ops_propagated() > 0) {
            metrics_.type_propagation_extended_ops_total.fetch_add(tp.extended_ops_propagated(),
                                                                   std::memory_order_relaxed);
        }
        // Issue #1874: fixpoint rounds (sum across blocks).
        if (tp.fixpoint_rounds() > 0) {
            metrics_.type_propagation_fixpoint_rounds.fetch_add(tp.fixpoint_rounds(),
                                                                std::memory_order_relaxed);
        }
        // Issue #1884: feed TypedMutationAudit correlation window.
        typed_audit::note_type_propagation_pass(
            static_cast<std::uint64_t>(tp.fixpoint_rounds()),
            static_cast<std::uint64_t>(tp.narrow_propagated()),
            static_cast<std::uint64_t>(tp.extended_ops_propagated()));
    }

    // Issue #538: accumulate coercion zero-overhead metrics from a
    // TypeSpecializationWrap + DeadCoercionEliminationPass run.
    void accumulate_coercion_pass_metrics(const aura::compiler::TypeSpecializationWrap& ts,
                                          const aura::compiler::DeadCoercionEliminationPass& dce) {
        if (dce.eliminated_count() > 0) {
            metrics_.dead_coercion_eliminated_total.fetch_add(dce.eliminated_count(),
                                                              std::memory_order_relaxed);
            metrics_.dead_coercion_elision_elided_casts_total.fetch_add(dce.eliminated_count(),
                                                                        std::memory_order_relaxed);
            metrics_.dead_coercion_elision_runtime_check_savings_total.fetch_add(
                dce.eliminated_count(), std::memory_order_relaxed);
        }
        // Issue #1884: DCE narrow_evidence hits → audit correlation window.
        typed_audit::note_dce_narrow_hits(static_cast<std::uint64_t>(dce.narrow_evidence_hits()));
        if (dce.elapsed_us() > 0) {
            metrics_.dead_coercion_elapsed_us_total.fetch_add(dce.elapsed_us(),
                                                              std::memory_order_relaxed);
        }
        if (dce.kept_for_debug_count() > 0) {
            metrics_.dead_coercion_kept_for_debug_total.fetch_add(dce.kept_for_debug_count(),
                                                                  std::memory_order_relaxed);
        }
        if (ts.castop_emitted() > 0) {
            metrics_.coercion_castop_emitted_total.fetch_add(ts.castop_emitted(),
                                                             std::memory_order_relaxed);
        }
        if (dce.type_prop_hits() > 0) {
            metrics_.coercion_type_prop_hits_total.fetch_add(dce.type_prop_hits(),
                                                             std::memory_order_relaxed);
        }
        const std::uint64_t narrow_hits_run =
            dce.narrow_evidence_hits() + ts.narrow_evidence_skipped();
        if (narrow_hits_run > 0) {
            metrics_.coercion_narrow_evidence_hits_total.fetch_add(narrow_hits_run,
                                                                   std::memory_order_relaxed);
        }
        if (dce.narrow_evidence_hits() > 0) {
            metrics_.dead_coercion_elision_evidence_hits_total.fetch_add(dce.narrow_evidence_hits(),
                                                                         std::memory_order_relaxed);
        }
        if (narrow_hits_run > 0) {
            metrics_.dead_coercion_elision_narrowing_stable_paths_total.fetch_add(
                narrow_hits_run, std::memory_order_relaxed);
        }
        const std::uint64_t zerooverhead_win_run =
            dce.type_prop_hits() + dce.narrow_evidence_hits() + ts.narrow_evidence_skipped();
        if (zerooverhead_win_run > 0) {
            metrics_.coercion_zerooverhead_win_total.fetch_add(zerooverhead_win_run,
                                                               std::memory_order_relaxed);
        }
        // Issue #1530: latest CastOp elision win rate (basis points).
        // win = eliminated / (castop_emitted + eliminated); store as 0..10000.
        {
            const auto eliminated = static_cast<std::uint64_t>(dce.eliminated_count());
            const auto emitted = static_cast<std::uint64_t>(ts.castop_emitted());
            const auto denom = emitted + eliminated;
            if (denom > 0) {
                const auto bp = static_cast<std::uint64_t>(
                    ::aura::core::consts::basis_points(eliminated, denom));
                metrics_.cast_elision_win_rate_bp.store(bp, std::memory_order_relaxed);
                // Issue #1925: alias elision-rate-bp for narrow/mutation AC.
                metrics_.dead_coercion_elision_rate_bp.store(bp, std::memory_order_relaxed);
            }
            if (eliminated > 0) {
                metrics_.dce_cast_elision_total.fetch_add(eliminated, std::memory_order_relaxed);
                // Issue #1874: DCE eliminations in a pipeline that
                // always runs TypeProp first (see callers below).
                metrics_.cast_eliminated_after_propagation.fetch_add(eliminated,
                                                                     std::memory_order_relaxed);
            }
        }
        // Issue #1925: narrow-mutation + Dynamic passthrough counters.
        if (dce.narrow_evidence_hits() > 0) {
            metrics_.dead_coercion_narrow_mutation_elided_total.fetch_add(
                dce.narrow_evidence_hits(), std::memory_order_relaxed);
        }
        if (dce.dynamic_hits() > 0) {
            metrics_.dead_coercion_dynamic_passthrough_total.fetch_add(dce.dynamic_hits(),
                                                                       std::memory_order_relaxed);
        }
        metrics_.dead_coercion_narrow_mutation_wired.store(1, std::memory_order_relaxed);
    }

    // Issue #2044 / #1574 / #2907 / Issue #3822: full incremental dirty pass suite
    // shared by relower_define_blocks full-fallback and invalidate_function
    // cascade. When mask_ptr is non-null, DirtyAware passes skip clean blocks;
    // when null, every block is treated as dirty (safe full re-opt).
    // Issue #3822: under production_defaults + non-empty soa_mod, SoA
    // PureWrap+DCE is authority (hot pack); dirty AoS blocks are synced from
    // SoA before writeback SSOT (`entry.irs`). Soft keeps AoS suite only
    // (no dual PureWrap divergence). Zero SoAtoAoSBridgePass / to_aos_view.
    // Returns clean blocks skipped (0 if no mask / all dirty).
    std::size_t run_incremental_dirty_pass_suite_(aura::ir::IRModule& ir_mod,
                                                  const DefineDirtyMaskView* mask_ptr,
                                                  IRModuleV2* soa_mod = nullptr) {
        aura::compiler::phase_stats::Scope phase_passes(aura::compiler::phase_stats::Passes);
        ComputeKindWrap ck_pass;
        ConstantFoldingWrap cf_pass;
        TypePropagationPass tp_pass;
        ShapeWrap shape_pass;

        // Issue #3454 AC3 grandfather (length-capped 5): ComputeKindWrap,
        // ConstantFoldingWrap, TypePropagationPass, ShapeWrap,
        // EscapeAnalysisWrap. Soft/unit keep AoS DirtySoAEntryPass.
        // Issue #3488/#3795: under production_defaults + soa_mod, PureWrap
        // stages peel SoA dirty blocks and skip the AoS walk. Issue
        // #3701/#3795: Escape uses columnar run_dirty_escape_on_soa
        // (now ProductionPureWrapPass). InlinePass SoA stays the #3403
        // production dispatch target; do not add it to this AoS suite.
        // Issue #4254: when pending_soa_snapshot_ holds this lower's SoA,
        // use it as PureWrap+sync authority — entry.soa_mod is still the
        // pre-re-lower module until store_define_v2 attaches pending.
        IRModuleV2* soa_auth = soa_mod;
        if (pending_soa_snapshot_ && !pending_soa_snapshot_->module.functions.empty())
            soa_auth = &pending_soa_snapshot_->module;
        const bool soa_hot = soa_auth && !soa_auth->functions.empty();
        const bool prod_soa = soa_hot && aura::compiler::typed_audit::production_defaults_active();
        // Issue #3690 / Issue #3831: Production last-look at the suite envelope.
        // Storm-exit cooldown and dense-under-Global may all-1s rewrite;
        // sparse-under-Global keeps dirty-cone amortize (#3831). Soft/Off:
        // production_dirty_aware_storm_force_full is consult-only.
        std::vector<std::vector<std::uint8_t>> storm_full_bits;
        DefineDirtyMaskView storm_full_view;
        if (mask_ptr && mask_ptr->block_dirty_per_func) {
            const auto dirty_n = static_cast<std::size_t>(mask_ptr->dirty_block_count());
            if (production_dirty_aware_storm_force_full(dirty_n)) {
                // Issue #3869: the fire was counted at the consult — do
                // not double-bump here.
                storm_full_bits = *mask_ptr->block_dirty_per_func;
                for (auto& fb : storm_full_bits)
                    for (auto& b : fb)
                        b = 1;
                storm_full_view.block_dirty_per_func = &storm_full_bits;
                mask_ptr = &storm_full_view;
                if (soa_auth) {
                    for (auto& fn : soa_auth->functions) {
                        if (fn.block_dirty_.size() < fn.blocks_.size())
                            fn.block_dirty_.resize(fn.blocks_.size(), 0);
                        fn.mark_all_blocks_dirty();
                    }
                }
            }
        }
        if (!prod_soa) {
            EscapeAnalysisWrap escape_pass;
            (void)run_production_incremental_dirty_pipeline(ir_mod, ck_pass, mask_ptr);
            (void)run_production_incremental_dirty_pipeline(ir_mod, cf_pass, mask_ptr);
            (void)run_production_incremental_dirty_pipeline(ir_mod, tp_pass, mask_ptr);
            (void)run_production_incremental_dirty_pipeline(ir_mod, shape_pass, mask_ptr);
            (void)run_production_incremental_dirty_pipeline(ir_mod, escape_pass, mask_ptr);
        } else {
            (void)run_dirty_escape_on_soa(*soa_mod, last_escape_maps_);
        }

        // Issue #3689: Production/Full partial peel must DCE with the same
        // dirty mask as CK/CF/TP. Unmasked AoS DeadCoercion walked clean
        // blocks against mixed old/new IR (partial relower rewrote only
        // dirty blocks). Soft/Off keeps full-fn DCE (extra work, not a
        // hole). mask_ptr==null / size mismatch → whole-fn fallback.
        // SoA hot pack DCE stays cone-limited (#2907). Do not add
        // InlinePass to this suite.
        for (std::size_t fi = 0; fi < ir_mod.functions.size(); ++fi) {
            auto& func = ir_mod.functions[fi];
            if (func.id == ir_mod.entry_function_id)
                continue;
            if (aura::compiler::typed_audit::production_hard_face_active() && mask_ptr &&
                mask_ptr->block_dirty_per_func && fi < mask_ptr->block_dirty_per_func->size()) {
                const auto& db = (*mask_ptr->block_dirty_per_func)[fi];
                if (db.size() == func.blocks.size()) {
                    run_coercion_elim_on_function(func, db);
                    continue;
                }
            }
            // Issue #538 / #611: DCE after re-lower (full fn when no mask).
            run_coercion_elim_on_function(func);
        }

        // Issue #3822 / #2907 / #3488: under prod_soa, SoA is sole PureWrap+DCE
        // authority (hot pack). Soft + soa_hot used to run both AoS suite and
        // SoA pack → dual PureWrap with only AoS writeback — gate hot pack on
        // prod_soa and mirror dirty SoA bodies into AoS before restamp/writeback.
        // Soft: AoS suite only (no hot-pack divergence beyond Soft contract).
        // Soak: production_soa_dirty_hot_pack_invocations_total advances from
        // this suite only when writeback SSOT is synced to match.
        if (prod_soa) {
            (void)aura::compiler::run_production_soa_dirty_hot_pack(*soa_auth, &type_registry_);
            (void)aura::compiler::sync_soa_dirty_blocks_into_aos(*soa_auth, ir_mod);
        }

        std::size_t clean_skipped = 0;
        if (mask_ptr && mask_ptr->block_dirty_per_func) {
            const auto total = mask_ptr->total_block_count();
            const auto dirty = mask_ptr->dirty_block_count();
            if (total > dirty)
                clean_skipped = static_cast<std::size_t>(total - dirty);
        }
        return clean_skipped;
    }

    // Issue #538: TypeSpecialization + DCE on one function after
    // post-mutate re-lower (incremental path).
    //
    // Issue #611: when dirty_blocks matches func.blocks.size(),
    // DCE runs only on dirty blocks (TypeSpec still runs on the
    // full function so Branch/successor casts are inserted).
    void run_coercion_elim_on_function(aura::ir::IRFunction& func,
                                       std::span<const std::uint8_t> dirty_blocks = {}) {
        const auto saved_id = func.id;
        aura::ir::IRModule mod;
        mod.functions.push_back(func);
        aura::compiler::TypeSpecializationWrap ts(&type_registry_);
        aura::compiler::TypePropagationPass tprop(&type_registry_);
        aura::compiler::DeadCoercionEliminationPass dce(&type_registry_);
        const std::uint64_t pipeline_epoch = aura::core::current_mutation_epoch();
        ts.set_pipeline_epoch(pipeline_epoch);
        tprop.set_pipeline_epoch(pipeline_epoch);
        dce.set_pipeline_epoch(pipeline_epoch);
        ts.run(mod);
        // Issue #1457: propagate type_id / narrow_evidence before DCE.
        tprop.run(mod);
        accumulate_type_propagation_metrics(tprop);
        const bool dirty_dce =
            !dirty_blocks.empty() && dirty_blocks.size() == mod.functions[0].blocks.size();
        if (dirty_dce) {
            std::size_t clean_blocks = 0;
            for (auto b : dirty_blocks) {
                if (b == 0)
                    ++clean_blocks;
            }
            if (clean_blocks > 0) {
                // Issue #526: DirtyAwarePass short-circuit observability.
                evaluator_.bump_passes_skipped_type_dirty(clean_blocks);
            }
            dce.run_function(mod.functions[0], dirty_blocks);
        } else {
            dce.run_function(mod.functions[0]);
        }
        func = std::move(mod.functions[0]);
        func.id = saved_id;
        accumulate_coercion_pass_metrics(ts, dce);
        // Issue #3007: after CoercionMap rebuild + incremental DCE, Production
        // full-fn residual sweep so identity CastOps outside the dirty cone
        // do not remain in JIT-bound IR. Soft observes only.
        // Issue #3689: skip the residual sweep when DCE was cone-limited —
        // clean blocks must not be DCE'd against mixed old/new IR after a
        // successful partial peel. Full fallback (no matching mask) still
        // sweeps.
        if (!dirty_dce) {
            (void)aura::compiler::opt_registry::sweep_production_hot_residual_castops(
                func, &type_registry_, pipeline_epoch);
        }
    }

    // Fast eval for primitive literal args inside the workspace-aware
    // call dispatch. Parses just enough to recognize integers/floats/
    // booleans/strings/void and returns the corresponding EvalValue
    // directly. Avoids the standard pipeline's global-state mutation
    // (set_flat_pool, last_ir_mod_, etc.) which was causing the second
    // recursive eval() call to return the first's value.
    //
    // For non-primitive inputs (variables, function calls, etc.),
    // returns EvalValue(0) as a sentinel so the caller can fall back
    // to a full eval() pass.
    [[nodiscard]] aura::compiler::types::EvalValue eval_arg_fast(std::string_view s) const {
        using namespace aura::compiler::types;
        // Trim
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a]))
            ++a;
        while (b > a && std::isspace((unsigned char)s[b - 1]))
            --b;
        if (a >= b)
            return make_void();
        std::string_view t = s.substr(a, b - a);

        // Integer?
        if (t.size() > 0 && (std::isdigit((unsigned char)t[0]) ||
                             (t.size() > 1 && t[0] == '-' && std::isdigit((unsigned char)t[1])))) {
            // Parse as int64 (don't support hex/oct/bin here — full
            // eval handles them).
            try {
                const std::string ts(t);
                std::size_t consumed = 0;
                long long v = std::stoll(ts, &consumed);
                if (consumed == ts.size())
                    return make_int_or_promote(v);
            } catch (...) {
                // [SILENCE-PRIM-#615] int parse fail — fall through to
                // float/bool/void cascade (#1669 class A).
            }
        }
        // Float?
        if (t.find('.') != std::string_view::npos) {
            try {
                const std::string ts(t);
                std::size_t consumed = 0;
                double v = std::stod(ts, &consumed);
                if (consumed == ts.size())
                    return make_float(v);
            } catch (...) {
                // [SILENCE-PRIM-#615] float parse fail — fall through to
                // bool/void cascade (#1669 class A).
            }
        }
        // Booleans
        if (t == "#t")
            return make_bool(true);
        if (t == "#f")
            return make_bool(false);
        // Void
        if (t == "()" || t == "#!void")
            return make_void();
        // Sentinel for non-primitive input — caller falls back to full
        // eval() (recursive under eval_mutex_ / recursive_mutex).
        return EvalValue(0);
    }

    // Try to extract a define/let/letrec binding from the FlatAST root.
    // Returns {name, body_node_id} if root is a Define node.
    static std::optional<std::pair<std::string, aura::ast::NodeId>>
    try_extract_define(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                       aura::ast::NodeId root) {
        if (root == aura::ast::NULL_NODE)
            return std::nullopt;
        auto v = flat.get(root);
        if (v.tag == aura::ast::NodeTag::Define) {
            auto name = pool.resolve(v.sym_id);
            aura::ast::NodeId body = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            return std::make_pair(std::string(name), body);
        }
        return std::nullopt;
    }

    // Issue #2732: (f arg…) where f is a top-level define already bound in
    // top_env / ir_define_env_bindings_ — apply without re-lowering into a
    // fresh module (avoids MakeClosure remap hang on multi-define graphs).
    std::optional<EvalResult> try_dispatch_toplevel_define_call_(aura::ast::FlatAST& flat,
                                                                 aura::ast::StringPool& pool,
                                                                 aura::ast::NodeId root) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (root == aura::ast::NULL_NODE || root >= flat.size())
            return std::nullopt;
        auto v = flat.get(root);
        if (v.tag != aura::ast::NodeTag::Call || v.children.empty())
            return std::nullopt;
        auto callee = flat.get(v.child(0));
        if (callee.tag != aura::ast::NodeTag::Variable)
            return std::nullopt;
        const std::string name(pool.resolve(callee.sym_id));
        if (name.empty())
            return std::nullopt;
        // Prefer env binding (string) — IR define closures live here after
        // cache_define(bind_in_env=true) / eval-current.
        auto env_val = evaluator_.top_env().lookup(name);
        if (!env_val)
            return std::nullopt;
        types::EvalValue fn = *env_val;
        if (types::is_cell(fn)) {
            auto ci = types::as_cell_id(fn);
            if (ci >= evaluator_.cells().size())
                return std::nullopt;
            fn = evaluator_.cells()[ci];
        }
        if (!types::is_closure(fn))
            return std::nullopt;
        // Only short-circuit when this is a known define (ir_cache or
        // ir_define map) — leave pure TW lambdas / primitives to normal IR.
        const bool known_define = ir_cache_.count(name) > 0 || ir_cache_v2_.count(name) > 0 ||
                                  ir_define_env_bindings_.count(name) > 0;
        if (!known_define)
            return std::nullopt;
        std::vector<types::EvalValue> args;
        args.reserve(v.children.size() > 0 ? v.children.size() - 1 : 0);
        for (std::size_t i = 1; i < v.children.size(); ++i) {
            auto ar = evaluator_.eval_flat(flat, pool, v.child(i), evaluator_.top_env());
            if (!ar)
                return EvalResult(std::unexpected(ar.error()));
            args.push_back(*ar);
        }
        auto cid = types::as_closure_id(fn);
        if (auto r = evaluator_.apply_closure(cid, args))
            return EvalResult(*r);
        return std::nullopt;
    }

    // Try to evaluate a define value expression at compile time.
    // Returns the value if the expression is a pure constant (no side effects,
    // no runtime dependencies), or nullopt if it needs runtime evaluation.
    std::optional<types::EvalValue> try_const_eval(const aura::ast::FlatAST& flat,
                                                   const aura::ast::StringPool& pool,
                                                   aura::ast::NodeId node_id) {
        if (node_id >= flat.size())
            return std::nullopt;
        auto v = flat.get(node_id);
        switch (v.tag) {
            case aura::ast::NodeTag::LiteralInt:
                return types::make_int_or_promote(v.int_value);
            case aura::ast::NodeTag::LiteralFloat:
                return types::make_float(v.float_value);
            case aura::ast::NodeTag::LiteralString: {
                // Push string to heap at compile time
                auto str = std::string(pool.resolve(v.sym_id));
                auto sid = evaluator_.primitives().string_heap().mint(str);
                return types::make_string(sid);
            }
            case aura::ast::NodeTag::Call: {
                if (v.children.size() < 1)
                    return std::nullopt;
                auto callee = flat.get(v.child(0));
                if (callee.tag != aura::ast::NodeTag::Variable)
                    return std::nullopt;
                auto name = std::string(pool.resolve(callee.sym_id));
                if (name == "hash" && v.children.size() == 1) {
                    // (hash) — call the hash primitive at compile time
                    auto pfn = evaluator_.primitives().lookup("hash");
                    if (pfn)
                        return (*pfn)({});
                }
                return std::nullopt;
            }
            default:
                return std::nullopt;
        }
    }

    // Check if a node is a require/import/use call.
    static bool is_require_call(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                aura::ast::NodeId id) {
        if (id >= flat.size())
            return false;
        auto v = flat.get(id);
        if (v.tag != aura::ast::NodeTag::Call)
            return false;
        auto callee = v.child(0);
        if (callee >= flat.size())
            return false;
        auto cv = flat.get(callee);
        if (cv.tag != aura::ast::NodeTag::Variable)
            return false;
        auto name = pool.resolve(cv.sym_id);
        return name == "require" || name == "import"; // use returns a value (module object)
    }

    // Pre-execute top-level require/import/use calls, removing them from
    // the expression so the remaining body can go through IR without fallback.
    // Returns the new root node (with requires removed), or original root.
    // Side effect: fills ir_cache_ + evaluator env via compile_module.
    // Issue #4918: pre_exec_requires left a failed require in the tree.
    bool pre_exec_kept_failed_require_ = false;
    aura::ast::NodeId pre_exec_requires(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                        aura::ast::NodeId root) {
        if (root >= flat.size())
            return root;
        auto v = flat.get(root);
        // Issue #4916: loading a module walks the module's own flat, and the
        // walker leaves current_flat()/current_pool() on it. The IR program
        // that runs next reads node locations through that hint, so a deny
        // in the entry file was reported at "io.aura:1:1". Put the entry
        // flat back once the requires are done.
        // Restore the hints as they were on entry (only when a require ran).
        class CurrentSourceRestore {
        public:
            CurrentSourceRestore(Evaluator& evaluator, aura::ast::FlatAST* flat,
                                 aura::ast::StringPool* pool)
                : ev_(&evaluator)
                , flat_(flat)
                , pool_(pool) {}
            CurrentSourceRestore(const CurrentSourceRestore&) = delete;
            CurrentSourceRestore& operator=(const CurrentSourceRestore&) = delete;
            CurrentSourceRestore(CurrentSourceRestore&&) = delete;
            CurrentSourceRestore& operator=(CurrentSourceRestore&&) = delete;
            ~CurrentSourceRestore() {
                if (!armed_)
                    return;
                ev_->set_current_flat(flat_);
                ev_->set_current_pool(pool_);
            }
            void arm() { armed_ = true; }

        private:
            Evaluator* ev_;
            aura::ast::FlatAST* flat_;
            aura::ast::StringPool* pool_;
            bool armed_ = false;
        } restore_current_source{evaluator_, evaluator_.current_flat(), evaluator_.current_pool()};

        // Top-level standalone require: execute, no body left.
        // The caller should detect NULL_NODE and skip the rest
        // of the evaluation (or treat it as a no-op).
        // Issue #2570: do not swallow module-load failures — leave the
        // require Call in the AST so normal eval returns the error value
        // (and fail-closed load is observable to scripts / try-catch).
        if (is_require_call(flat, pool, root)) {
            restore_current_source.arm();
            auto rr = evaluator_.eval_flat(flat, pool, root, evaluator_.top_env());
            if (!rr || (types::is_error(*rr) && !types::is_string(*rr))) {
                // leave the Call so normal eval surfaces the failure
                pre_exec_kept_failed_require_ = true; // Issue #4918
                return root;
            }
            return aura::ast::NULL_NODE;
        }

        // (begin ...) — scan children, execute requires, rebuild begin
        // with only non-require children. If all children were requires,
        // the stripped begin is empty; we return NULL_NODE.
        // Issue #2570: on module-load error, keep failed require in the
        // begin so subsequent forms are not evaluated as if load succeeded.
        if (v.tag == aura::ast::NodeTag::Begin) {
            std::vector<aura::ast::NodeId> non_require_children;
            for (auto c : v.children) {
                if (is_require_call(flat, pool, c)) {
                    restore_current_source.arm();
                    auto rr = evaluator_.eval_flat(flat, pool, c, evaluator_.top_env());
                    if (!rr || (rr && types::is_error(*rr) && !types::is_string(*rr))) {
                        pre_exec_kept_failed_require_ = true; // Issue #4918
                        // Keep this require + remaining siblings for normal eval
                        // (begin abort-on-error will stop after the failed require).
                        non_require_children.push_back(c);
                        // Append rest of forms without pre-executing them.
                        bool after = false;
                        for (auto c2 : v.children) {
                            if (c2 == c) {
                                after = true;
                                continue;
                            }
                            if (after)
                                non_require_children.push_back(c2);
                        }
                        if (non_require_children.size() == v.children.size())
                            return root;
                        return flat.add_begin(non_require_children);
                    }
                } else {
                    non_require_children.push_back(c);
                }
            }
            if ((int)non_require_children.size() == (int)v.children.size())
                return root; // no require → unchanged
            if (non_require_children.empty())
                return aura::ast::NULL_NODE;
            // Rebuild the begin with only the non-require children
            return flat.add_begin(non_require_children);
        }

        return root;
    }

    // ── Compile-time AST validation ───────────────────────────
    // Validates macro-expanded AST for structural correctness.
    // Non-fatal: prints warnings; in strict mode becomes fatal.
    struct ValidationNote {
        aura::ast::NodeId node;
        std::string message;
    };

    void validate_ast(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                      aura::ast::NodeId root) const {
        std::vector<ValidationNote> notes;

        // Walk the AST checking structural rules
        auto walk = [&](this const auto& self, aura::ast::NodeId id) -> void {
            if (id >= flat.size())
                return;
            auto v = flat.get(id);

            switch (v.tag) {
                case aura::ast::NodeTag::IfExpr:
                    if (v.children.size() != 3)
                        notes.push_back(
                            {id,
                             "if requires 3 arguments (condition then-branch else-branch), got " +
                                 std::to_string(v.children.size())});
                    break;

                case aura::ast::NodeTag::Lambda:
                    if (v.children.empty())
                        notes.push_back({id, "lambda requires a body expression"});
                    if (v.params.empty() && !v.children.empty() &&
                        flat.get(v.child(0)).tag == aura::ast::NodeTag::Lambda) {
                        // (lambda () (lambda ...)) — ok
                    }
                    break;

                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec:
                    if (v.children.size() < 2)
                        notes.push_back(
                            {id, std::string(v.tag == aura::ast::NodeTag::Let ? "let" : "letrec") +
                                     " requires a value and body"});
                    break;

                case aura::ast::NodeTag::Define:
                    if (v.children.empty())
                        notes.push_back({id, "define requires a value expression"});
                    break;

                case aura::ast::NodeTag::Set:
                    if (v.children.empty())
                        notes.push_back({id, "set! requires a value expression"});
                    break;

                case aura::ast::NodeTag::Quote:
                    // Quote with no children is valid (quoting empty list)
                    break;

                default:
                    break;
            }

            // Recurse into children
            for (auto c : v.children)
                self(c);
        };

        if (root < flat.size())
            walk(root);

        // Print warnings (force flush so output is visible before potential crash)
        if (!notes.empty()) {
            for (auto& n : notes) {
                auto loc = flat.get(n.node);
                std::println("syntax: {}:{}: {}", loc.line, loc.col, n.message);
            }
        }
    }

    // ── Register ADT constructors in TypeRegistry (for match exhaustiveness) ──
    // ── Re-collect match clause metadata from expanded AST (stable node IDs) ──
    // The parser stores match_info on pre-expansion IDs. Macro expansion may shift
    // node IDs, so we re-derive match info from the expanded flat here.
    void collect_match_info(aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                            aura::ast::NodeId root) {
        auto is_ignore_name = [&](aura::ast::SymId sid) -> bool {
            if (sid == aura::ast::INVALID_SYM)
                return true;
            auto n = pool.resolve(sid);
            return n == "_" || (n.size() > 1 && n[0] == '_');
        };
        auto extract_ctor = [&](aura::ast::NodeId nid, auto& minfo) -> void {
            if (nid >= flat.size())
                return;
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                auto callee_v = flat.get(nv.child(0));
                if (callee_v.tag == aura::ast::NodeTag::Variable &&
                    !is_ignore_name(callee_v.sym_id) && nv.children.size() >= 1) {
                    minfo.used_constructors.push_back(callee_v.sym_id);
                }
            }
        };
        auto walk = [&](this const auto& self, aura::ast::NodeId id) -> void {
            if (id >= flat.size())
                return;
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::IfExpr && v.children.size() >= 3 &&
                v.child(0) < flat.size()) {
                auto test_v = flat.get(v.child(0));
                // Detect if: (if test body else-if-chain) — walk both branches
                // The then-branch (child 1) is a body, check its let for bindings
                auto then_id = v.child(1);
                if (then_id < flat.size()) {
                    auto then_v = flat.get(then_id);
                    // If then body is a let and we can resolve the arg to a ctor
                    if (then_v.tag == aura::ast::NodeTag::Let && !then_v.children.empty()) {
                        // Check if this let has match_info already
                        if (!flat.has_match_info(id)) {
                            aura::ast::MatchClauseInfo minfo;
                            extract_ctor(then_v.child(0), minfo);
                            flat.set_match_info(id, minfo);
                        }
                    }
                }
            }
            for (auto c : v.children)
                self(c);
        };
        if (root < flat.size())
            walk(root);
    }

    void register_adt_from_define_types(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId root) {
        auto walk = [&](this const auto& self, aura::ast::NodeId id) -> void {
            if (id >= flat.size())
                return;
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::DefineType) {
                auto type_name = std::string(pool.resolve(v.sym_id));
                std::vector<std::string> ctors;
                for (auto cid : v.children) {
                    if (cid >= flat.size())
                        continue;
                    auto cv = flat.get(cid);
                    if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty())
                        continue;
                    // First element of quoted list is constructor name
                    auto walk_quoted = cv.child(0);
                    if (walk_quoted >= flat.size())
                        continue;
                    auto wv = flat.get(walk_quoted);
                    if (wv.tag == aura::ast::NodeTag::Pair && !wv.children.empty()) {
                        auto car_id = wv.child(0);
                        if (car_id < flat.size()) {
                            auto car_v = flat.get(car_id);
                            if (car_v.tag == aura::ast::NodeTag::Variable) {
                                auto cname = std::string(pool.resolve(car_v.sym_id));
                                if (!cname.empty())
                                    ctors.push_back(cname);
                            }
                        }
                    }
                }
                if (!ctors.empty()) {
                    // Register the ADT type if not already registered, then add constructors
                    auto tid = type_registry_.lookup_type(type_name);
                    if (!tid.valid()) {
                        tid = type_registry_.register_type(aura::core::TypeTag::VARIANT, type_name);
                    }
                    if (tid.valid())
                        type_registry_.register_adt_constructors(tid, ctors);
                }
            }
            for (auto c : v.children)
                self(c);
        };
        if (root < flat.size())
            walk(root);
    }

    // IR function cache: name → bundle of IR functions for cached defines.
    // The LAST function in the bundle is the user-defined lambda itself.
    // When inlined, all functions are added to the current module in order,
    // preserving func id references across cached calls.
    std::unordered_map<std::string, std::vector<aura::ir::IRFunction>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        ir_cache_;

    // Issue #272 Cycle 4: pre-bind IR snapshots for disk cache serialization.
    // Captured in cache_define immediately after ir_cache_ is populated,
    // before IRInterpreter env binding can alias/mutate live IR state.
    std::unordered_map<std::string, std::vector<aura::ir::IRFunction>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        ir_disk_snapshots_;

    // Bridge data cached alongside ir_cache_ (same keys, parallel indices).
    std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        ir_cache_bridge_;
    // Issue #741: func indices from the latest impact_scope computation,
    // wired into lowering hooks to limit bridge shared_ptr copy scope.
    std::unordered_set<std::size_t> pending_impact_func_indices_;
    // String pool cached alongside ir_cache_ (same keys).
    std::unordered_map<std::string, std::vector<std::string>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        ir_cache_strings_;

    // Issue #272: persistent IR runtimes for function-define env bindings.
    // Issue #4717: shared_ptr (was unique_ptr) — dispatch_ir_define_closure
    // resolves a binding under ir_cache_gate_, then releases the gate to
    // run the body; the shared_ptr keeps the interpreter alive even when a
    // concurrent mutate:rebind replaces or clears the registry row.
    std::unordered_map<std::string, std::shared_ptr<IRDefineEnvBinding>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        ir_define_env_bindings_;
    // Issue #272 Cycle 3: top-level value defines bound via IR (name → cell index).
    std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash, std::equal_to<>>
        ir_value_cell_bindings_;
    // Issue #4717 (P4) — the shared IR / closure cache gate.
    //
    // #4707 removed the whole-body fiber mutex, so two bodies (and the CLI
    // main thread) now reach the compiler's shared IR state concurrently.
    // That state — ir_cache_, ir_cache_v2_/IRCacheEntry contents,
    // ir_cache_bridge_/strings, the define→interpreter registry,
    // ir_define_closure_owner_, the closure-id cursor and the #4682 tier
    // side tables — was written by whichever thread compiled or
    // invalidated a define; the body mutex made that single-threaded by
    // accident.
    //
    // Mechanism choice (option (b), NOT a per-body cache): every entry is
    // keyed by define NAME and is re-derived from the workspace source.
    // It is NOT eval-state dependent — the eval-state split already landed
    // in 3ccf14275 (fiber Context). A per-body cache would fork the view
    // of one shared workspace: a body that rebinds `ka` then evaluates
    // `kb` from its own cache would serve kb's pre-rebind body after a
    // sibling's rebind, and the CLI main thread would keep serving both
    // stale bodies afterwards. So the cache stays single and shared, and
    // this gate serializes it (sharded/striped would not help: entries
    // cross-reference each other through dep_graph_ / source_to_ir_map).
    //
    // Ordering rules (keep them if you touch this):
    //   * INNERMOST rank. Only taken under the mutation rank (Mutate) or
    //     by a short read that takes nothing else while holding it.
    //   * NEVER held across an eval_flat / IRInterpreter body run, nor
    //     across anything that can wait on another fiber (fiber:join, a
    //     condvar): a body running under it could deadlock against its
    //     own mutate:* or against the sibling it waits for.
    //   * NEVER taken while holding IRDefineEnvBinding::exec_mtx (gate is
    //     the outer of the two). Dispatch takes the gate, pins the binding,
    //     DROPS the gate, then takes exec_mtx for the run; the registry
    //     walks hold the gate and take exec_mtx per binding.
    //     Issue #4747: a RUN legitimately re-enters dispatch (the body calls
    //     a closure), so "nothing" was wrong — the run's nested dispatch
    //     does take the gate with exec_mtx held, and it must never WAIT for
    //     it while another thread waits for that exec_mtx. The invariant that
    //     makes that safe is the "NEVER held across a body run" rule above:
    //     anyone waiting on an exec_mtx has already dropped the gate, so the
    //     nested dispatch only ever finds the gate free or held by a
    //     same-thread recursion. bind_value_define_via_ir was the one site
    //     still holding the gate across a run (fixed in #4747).
    //     Issue #4765: the registry walks DO wait for exec_mtx under the
    //     gate, so a nested dispatch can find the gate held by a walker that
    //     waits for this body's exec_mtx. Gate waits park the waiter's
    //     exec_mtx holds (lock_ir_gate_body_safe_4765) and walkers take
    //     exec_mtx through ExecUnderGate4765, which borrows a parked one.
    //   * recursive: relower_* / store_define_v2 / bind_* nest arbitrarily
    //     deep inside one another.
    mutable std::recursive_mutex ir_cache_gate_;
    // Issue #4765: every gate acquisition parks this thread's exec_mtx holds
    // while it waits (see lock_ir_gate_body_safe_4765).
    void relock_ir_gate_(std::unique_lock<std::recursive_mutex>& g) const {
        lock_ir_gate_body_safe_4765(ir_cache_gate_);
        g = std::unique_lock<std::recursive_mutex>(ir_cache_gate_, std::adopt_lock);
    }
    struct IrCacheGate {
        std::recursive_mutex& m;
        explicit IrCacheGate(std::recursive_mutex& gm)
            : m(gm) {
            lock_ir_gate_body_safe_4765(gm);
        }
        IrCacheGate(const IrCacheGate&) = delete;
        IrCacheGate& operator=(const IrCacheGate&) = delete;
        IrCacheGate(IrCacheGate&&) = delete;
        IrCacheGate& operator=(IrCacheGate&&) = delete;
        ~IrCacheGate() { m.unlock(); }
    };

    std::unordered_map<aura::compiler::ClosureId, std::string> ir_define_closure_owner_;
    // Issue #4715 regression: superseded define-closure ids stay resolvable
    // to the define that published them (see retire_ir_define_closure_).
    // Bounded per define; cleared with ir_define_closure_owner_ on a
    // workspace replace. Guarded by ir_cache_gate_.
    std::unordered_map<aura::compiler::ClosureId, std::string> retired_define_closure_owner_;
    std::unordered_map<std::string, std::vector<aura::compiler::ClosureId>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        retired_define_closure_order_;
    // Issue #4753: dropped tree-walker publication ids of a define
    // (must_deopt_closures_named drops the superseded generations). Their
    // reporter runs under the closure shard locks, so this record has its
    // own leaf mutex: dispatch (which holds ir_cache_gate_) may take it, and
    // the reporter never takes the IR gate - the pair cannot invert.
    std::mutex retired_tw_closure_mtx_;
    std::unordered_map<aura::compiler::ClosureId, std::string> retired_tw_closure_owner_;
    std::unordered_map<std::string, std::deque<aura::compiler::ClosureId>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        retired_tw_closure_order_;
    // Issue #4682 (6b): tier stats + worker (worker joins in its dtor).
    TierStats4682 tier_stats_4682_;
    std::unique_ptr<TierWorker4682> tier_worker_4682_;
    // Issue #4357: next id for bind_function_define_via_ir. Starts at the
    // high-bit base; each persistent interpreter consumes a disjoint span.
    // Issue #4717: atomic + max-publisher. Dispatch runs a body with the
    // cache gate RELEASED, so the cursor cannot be a gated field any more;
    // ids only ever move forward, so a max-CAS publish is exact (two
    // bodies that each mint from the same floor is the per-fiber-minting
    // follow-up, step 3 of #4717 — not a lost update here).
    std::atomic<std::uint64_t> next_ir_define_closure_id_{types::make_ir_closure_id(0)};

    // Issue #4895: true when an argument's source text contains a
    // closure-building form head token (lambda / define / delay ...).
    [[nodiscard]] static bool arg_text_builds_closure_4895_(std::string_view s) noexcept {
        static constexpr std::string_view kHeads[] = {
            "lambda", "\xCE\xBB",    "case-lambda",  "named-lambda",  "define",
            "delay",  "delay-force", "make-promise", "make-parameter"};
        auto is_delim = [](char c) {
            return std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' ||
                   c == '[' || c == ']' || c == '\'' || c == '`' || c == ',';
        };
        std::size_t i = 0;
        while (i < s.size()) {
            if (is_delim(s[i])) {
                ++i;
                continue;
            }
            if (s[i] == '"') { // skip string literal
                ++i;
                while (i < s.size() && s[i] != '"') {
                    if (s[i] == '\\')
                        ++i;
                    ++i;
                }
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < s.size() && !is_delim(s[j]) && s[j] != '"')
                ++j;
            const auto tok = s.substr(i, j - i);
            for (auto h : kHeads)
                if (tok == h)
                    return true;
            i = j;
        }
        return false;
    }

    // Issue #4895: a top-level expression's IR interpreter mints its
    // closures from the same service-wide cursor as the persistent define
    // bindings. It started at kClosureIdHighBit, so under the form-by-form
    // force-soa face `(f (lambda () 1))` minted the SAME id as the first
    // define's own closure: f's interpreter resolved (g) to itself
    // ("arity mismatch: f expects 1 argument, got 0") or to a foreign slot
    // ("eval_flat: invalid closure"). Seeding turns any remaining escape of
    // an expression-interpreter closure into a located invalid-closure
    // error instead of a silent call to the wrong procedure.
    void seed_eval_interp_closure_cursor_4895_(aura::compiler::IRInterpreter& interp) noexcept {
        interp.set_next_closure_id(next_ir_define_closure_id_.load(std::memory_order_acquire));
        interp.set_shared_closure_cursor(&next_ir_define_closure_id_);
    }

    // Issue #4717: drain an interpreter's advanced id cursor back into the
    // shared monotonic source (release; max-only so a concurrent publish
    // from a sibling body is never lowered).
    void publish_ir_closure_cursor_(std::uint64_t v) noexcept {
        auto cur = next_ir_define_closure_id_.load(std::memory_order_relaxed);
        while (v > cur && !next_ir_define_closure_id_.compare_exchange_weak(
                              cur, v, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }

    // Issue #4717 (P4): run one resolved closure on its PINNED binding with
    // the IR cache gate released. `b` is a shared_ptr copy: a concurrent
    // rebind can replace or clear the registry row without freeing the
    // interpreter under the call. exec_mtx serializes the interpreter's
    // mutable execution state (frames / cells / runtime_closures_ / cursor)
    // across sibling bodies — the interpreter is not reentrant and not
    // per-thread. mode 0 = owner path (cid is the binding's own closure or
    // one of its runtime closures); mode 1 = runtime-mint search (strict
    // #4450 / #4626 rule: only a call-time mint is routable).
    enum class PinnedRun4717 { NotMinted, Failed, Ok };
    PinnedRun4717 run_pinned_ir_closure_(const std::shared_ptr<IRDefineEnvBinding>& b,
                                         ClosureId cid, std::span<const types::EvalValue> args,
                                         int mode, types::EvalValue* out) {
        if (!b || !b->interpreter)
            return PinnedRun4717::Failed;
        auto* interp = b->interpreter.get();
        ExecRunLock4765 exec(b->exec_mtx); // #4765 parks while blocked
        // Issue #4717 follow-up: a body run can block on another fiber (fiber:join of a
        // sibling this same body spawned - orch:parallel / orch:scope). The
        // wait hooks drop exec_mtx across that wait; declared AFTER `exec`, so
        // this scope is destroyed before the lock_guard re-locks.
        ExecMutexWaitPublishScope exec_wait_publish(b->exec_mtx);
        const auto ucid = static_cast<std::uint64_t>(cid);
        if (mode == 1) {
            // Issue #4450 / #4626: a bind-time execute() mint must NOT
            // resurrect after a workspace restore / rebind; only an id at
            // or above the call-time mint floor that the interpreter still
            // holds is routable.
            if (!interp->owns_runtime_mint(ucid)) {
                const auto& floor = b->runtime_mint_floor;
                if (!floor.has_value() || ucid < *floor || !interp->owns_runtime_closure(ucid))
                    return PinnedRun4717::NotMinted;
            }
        } else if (b->closure_id != cid && !interp->owns_runtime_closure(ucid)) {
            return PinnedRun4717::NotMinted;
        }
        interp->set_next_closure_id(next_ir_define_closure_id_.load(std::memory_order_acquire));
        // Issue #4626: remember the cursor the first call started at, so a
        // callback this call mints stays routable.
        if (!b->runtime_mint_floor.has_value())
            b->runtime_mint_floor = interp->next_closure_id();
        auto r = interp->call_closure(ucid, args);
        publish_ir_closure_cursor_(interp->next_closure_id());
        if (!r)
            return PinnedRun4717::Failed;
        *out = *r;
        return PinnedRun4717::Ok;
    }


    // Issue #4815: run a closure owned by a retained value-define
    // interpreter (see bind_value_define_via_ir). Same locking as
    // run_pinned_ir_closure_ (exec_mtx, IR cache gate released by caller).
    PinnedRun4717 run_value_define_closure_4815_(const std::shared_ptr<IRDefineEnvBinding>& b,
                                                 ClosureId cid,
                                                 std::span<const types::EvalValue> args,
                                                 types::EvalValue* out) {
        if (!b || !b->interpreter)
            return PinnedRun4717::Failed;
        auto* interp = b->interpreter.get();
        ExecRunLock4765 exec(b->exec_mtx);
        ExecMutexWaitPublishScope exec_wait_publish(b->exec_mtx);
        const auto ucid = static_cast<std::uint64_t>(cid);
        if (!interp->owns_runtime_closure(ucid))
            return PinnedRun4717::NotMinted;
        interp->set_next_closure_id(next_ir_define_closure_id_.load(std::memory_order_acquire));
        auto r = interp->call_closure(ucid, args);
        publish_ir_closure_cursor_(interp->next_closure_id());
        if (!r)
            return PinnedRun4717::Failed;
        *out = *r;
        return PinnedRun4717::Ok;
    }
    // Issue #4815: value define name -> the interpreter that minted the
    // closures its value holds. Guarded by ir_cache_gate_.
    std::unordered_map<std::string, std::shared_ptr<IRDefineEnvBinding>,
                       aura::core::TransparentStringHash, std::equal_to<>>
        ir_value_define_closure_owners_;

    // Source code for each cached function, used for re-lowering on dependency changes.
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash, std::equal_to<>>
        function_sources_;


    // Dependency tracking for incremental compilation.
    // DepEntry.calls = functions this one calls; DepEntry.called_by = functions that call this one.
    // When a function is redefined, all transitively dependent functions are invalidated.
    struct DepEntry {
        std::vector<std::string> calls;
        std::vector<std::string> called_by;
    };
    std::unordered_map<std::string, DepEntry, aura::core::TransparentStringHash, std::equal_to<>>
        dep_graph_;
    // Issue #2110: NodeId DepGraph mirror of function-level edges
    // (encode_fn_node slots). Protected by the same dep_graph_mtx_.
    // Dirty propagates callee → caller (same as called_by).
    aura::compiler::dirty::DepGraph node_dep_graph_;
    std::unordered_map<std::string, std::uint32_t, aura::core::TransparentStringHash,
                       std::equal_to<>>
        dep_name_to_slot_;
    std::vector<std::string> dep_slot_to_name_;
    // Issue #2032: generation bumped under exclusive dep_graph_mtx_ when
    // cascade erase/rebuild runs; record_dependency rejects edges stamped
    // against a stale generation (concurrent fiber/GC/mutate window).
    std::atomic<std::uint64_t> dep_graph_generation_{0};
    // Issue #3067: stale record_dependency rejects queued for hybrid
    // cascade drain (relower / end of invalidate). Armed only when a
    // reject actually happened (clean path: zero extra).
    std::vector<std::pair<std::string, std::string>> deferred_hybrid_edges_{};
    std::atomic<std::uint32_t> deferred_hybrid_armed_{0};
    // Issue #3283: generation counter on the deferred-hybrid queue. Bumped
    // on every emplace_back (both record_dependency and
    // record_block_dependency stale-reject paths) under
    // cascade_decision_mtx_ (#3135 parity). relower_dirty_defines_from_
    // workspace snapshots it after the entry drain + lock and re-checks it
    // before committing to partial: any bump during the decision window =
    // a concurrent stale-reject re-armed edges the drain/snapshot never
    // saw → fail-closed force full (or late-edge attribution) instead of a
    // partial peel with a stale impact bound.
    std::atomic<std::uint64_t> deferred_hybrid_gen_{0};
    // Issue #3135: cascade-decision lock (ordered AFTER mutate_mtx_).
    // Serializes record_dependency reject/re-arm against the drain →
    // impact_ub consult → partial/full decision window in
    // relower_dirty_defines_from_workspace. Single-fiber + clean
    // (deferred_hybrid_armed_==0) skips the lock (zero cost per AC2).
    // Production + armed>0 takes the lock + re-checks immediately
    // before peel; if re-armed since snapshot, force full or
    // re-drain + re-consult (bounded once).
    std::mutex cascade_decision_mtx_;

    // Issue #2110: dense slot for encode_fn_node (under dep_graph_mtx_).
    [[nodiscard]] std::uint32_t ensure_dep_fn_slot_(const std::string& name) {
        auto it = dep_name_to_slot_.find(name);
        if (it != dep_name_to_slot_.end())
            return it->second;
        const auto slot = static_cast<std::uint32_t>(dep_slot_to_name_.size());
        dep_slot_to_name_.push_back(name);
        dep_name_to_slot_.emplace(name, slot);
        return slot;
    }

    // Issue #2110: after string BFS cascade, run NodeId cascade_mark_dirty
    // from the mutated function root and refine block dirty via body-only
    // marks for any caller still under-invalidated. Returns nodes marked.
    std::size_t hybrid_node_cascade_(const std::string& root_name,
                                     const std::vector<std::string>& string_dependents);

    // Issue #3550: mark callees dirty before partial decision so
    // estimate_relower_blocks / impact_checked cannot miss a lockless
    // batch. Reuses cascade_mark_dirty + mark_body_only_dirty (no
    // second cone). Soft/Off: observe only.
    // Issue #3584: returns already-dirty callee *blocks* (block unit),
    // not the number of marked defines.
    // Issue #3656: returns kUnknownCalleeConeBlocks when map nonempty,
    // string calls empty, but node_dep still has fn edges.
    // Issue #3760: production empty source_to_ir_map is also unknown
    // (not 0 / skip cone). Soft still returns 0 at the helper gate.
    std::size_t precompute_callee_cascade_for_partial(const std::string& name);

    // Issue #3345: production hybrid depth-1 IR dirty of direct called_by
    // after facade early-return. Soft owns full BFS. Empty IR cache
    // (never-lowered / pure-AOT with no interpreter entries) is a no-op.
    void mark_direct_hybrid_dependents_body_dirty_(const std::string& name);

    // Issue #3474: production FIFO called_by cone IR dirty after facade
    // success. Marks every reachable caller (not only direct). Shared
    // lock only — Soft owns exclusive invalidate teardown. Empty IR
    // cache is a no-op.
    void mark_called_by_cone_body_dirty_(const std::string& name);

    // Issue #3823: Production mark-time union of node-dep dependents of
    // encode_fn_node(slot(name)) into the same body-dirty set as #3474.
    // Reuses #3761 decode walk. Shared lock only — no remirror/erase.
    // Soft inject remains observe-only (helper only on facade success).
    void mark_node_dep_dependents_body_dirty_(const std::string& name);

    void record_dependency(const std::string& caller, const std::string& callee);

    // Issue #2187: record a block-level DepGraph edge under dep_graph_mtx_.
    // Dirty propagates encode_fn_node(callee) → encode_block_dep_node(caller,fi,bi).
    // Also ensures the string + fn-level mirror edge exists (idempotent).
    void record_block_dependency(const std::string& caller, const std::string& callee,
                                 std::uint16_t caller_func_idx, std::uint16_t call_block_idx) {
        // Ensure string/fn edge first (takes its own exclusive lock).
        record_dependency(caller, callee);
        metrics_.dep_graph_record_total.fetch_add(1, std::memory_order_relaxed);
        const auto epoch_before = aura::core::current_mutation_epoch();
        const auto gen_before = dep_graph_generation_.load(std::memory_order_acquire);
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        sync_lock_order_metrics_();
        const auto epoch_after = aura::core::current_mutation_epoch();
        const auto gen_after = dep_graph_generation_.load(std::memory_order_acquire);
        if (epoch_before != epoch_after || gen_before != gen_after) {
            metrics_.dep_graph_edge_reject_stale_total.fetch_add(1, std::memory_order_relaxed);
            // Issue #3283: record_block_dependency's stale-reject must
            // serialize under cascade_decision_mtx_ like record_dependency
            // (#3135 parity) — otherwise it can append a deferred edge
            // DURING a concurrent relower decision window (which holds the
            // lock) and the re-arm escapes the #3257 last-look / #3168
            // attribution, under-marking the caller cone on a partial peel.
            std::scoped_lock<std::mutex> cascade_guard(cascade_decision_mtx_);
            deferred_hybrid_edges_.emplace_back(caller, callee);
            // Issue #3657: reject MUST arm (same contract as
            // record_dependency) so pending UB consult sees the edge.
            deferred_hybrid_armed_.store(1, std::memory_order_release);
            deferred_hybrid_gen_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto caller_slot = ensure_dep_fn_slot_(caller);
        const auto callee_slot = ensure_dep_fn_slot_(callee);
        mirror_block_dep_edge_unlocked_(callee_slot, caller_slot, caller_func_idx, call_block_idx);
    }

    // Issue #3067: NodeId fn + body-block 0 mirror. Caller holds exclusive
    // dep_graph_mtx_. Idempotent (DepGraph::add_edge dedups).
    void mirror_fn_dep_edge_unlocked_(const std::string& caller, const std::string& callee) {
        const auto caller_slot = ensure_dep_fn_slot_(caller);
        const auto callee_slot = ensure_dep_fn_slot_(callee);
        const auto from = aura::compiler::dirty::encode_fn_node(callee_slot);
        const auto to = aura::compiler::dirty::encode_fn_node(caller_slot);
        const auto edges_before = node_dep_graph_.edge_count();
        node_dep_graph_.add_edge(from, to);
        if (node_dep_graph_.edge_count() > edges_before)
            metrics_.dep_graph_node_mirror_edges_total.fetch_add(1, std::memory_order_relaxed);
        std::uint16_t body_fi = 0;
        auto cit = ir_cache_v2_.find(caller);
        // Issue #4331: same slot rule as hybrid_node_cascade_ apply_body_only
        // — legacy __top__ marker at slot 0 puts the body at irs[1]; #4311
        // dropped-marker bundles ([body, nested...]) keep the body at
        // irs[0]. The size guess recorded block edges on the nested slot,
        // so the drain dirtied the wrong function and served a stale body.
        if (cit != ir_cache_v2_.end() && cit->second.irs.size() >= 2 &&
            cit->second.irs[0].name == "__top__")
            body_fi = 1;
        mirror_block_dep_edge_unlocked_(callee_slot, caller_slot, body_fi, /*block_idx=*/0);
    }

    // Issue #3067: drain stale-reject queue. Re-record edges (restores
    // NodeId even on string dedup) then hybrid_node_cascade_ so callers
    // are dirty. Quiet when no reject armed (zero extra).
    void drain_deferred_hybrid_cascade_() {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (deferred_hybrid_armed_.load(std::memory_order_acquire) == 0)
            return;
        std::vector<std::pair<std::string, std::string>> pending;
        {
            lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                                   lock_order::Level::DepGraph);
            sync_lock_order_metrics_();
            if (deferred_hybrid_edges_.empty()) {
                deferred_hybrid_armed_.store(0, std::memory_order_release);
                return;
            }
            pending.swap(deferred_hybrid_edges_);
            deferred_hybrid_armed_.store(0, std::memory_order_release);
        }
        std::unordered_map<std::string, std::vector<std::string>, aura::core::TransparentStringHash,
                           std::equal_to<>>
            by_callee;
        for (auto& [caller, callee] : pending) {
            record_dependency(caller, callee);
            by_callee[callee].push_back(caller);
        }
        for (auto& [callee, callers] : by_callee) {
            std::sort(callers.begin(), callers.end());
            callers.erase(std::unique(callers.begin(), callers.end()), callers.end());
            (void)hybrid_node_cascade_(callee, callers);
            metrics_.hybrid_deferred_cascade_total.fetch_add(1, std::memory_order_relaxed);
        }
        if (aura::compiler::typed_audit::production_defaults_active()) {
            lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                                   lock_order::Level::DepGraph);
            sync_lock_order_metrics_();
            if (!aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                          dep_name_to_slot_)) {
                aura::compiler::dirty::rebuild_node_dep_graph_from_string(
                    node_dep_graph_, dep_graph_, dep_name_to_slot_);
                metrics_.dual_dep_graph_parity_fail_total.fetch_add(1, std::memory_order_relaxed);
                // Issue #3165: under Strict, fail-closed force dirty of
                // ALL callers in the graph (same all-callers walk as the
                // record_dependency parity-fail branch) so the deferred
                // hybrid drain's freshly-restored edges force all affected
                // callers full on the next relower (no stale partial IR).
                // Soft / off (non-Strict): rebuild only, zero extra force.
                // Issue #3187: production fail-closed default — the Strict
                // gate now fires under production_defaults_active() (or
                // AuditStrategy::Full) too, not just the explicit Strict
                // toggle. Closes the residual dual-graph fork window
                // (lockless batch / cross-fiber record_dependency → stale
                // deferred_hybrid_edges_ → hybrid_node_cascade_ on
                // inconsistent graph → silent under-cascade).
                if (aura::compiler::dirty::dual_dep_graph_strict_or_production()) {
                    std::unordered_set<std::string, aura::core::TransparentStringHash,
                                       std::equal_to<>>
                        affected;
                    for (const auto& [callee_name, callee_entry] : dep_graph_) {
                        (void)callee_name;
                        for (const auto& caller_name : callee_entry.called_by) {
                            affected.insert(caller_name);
                        }
                    }
                    for (const auto& caller_name : affected) {
                        auto cit2 = ir_cache_v2_.find(caller_name);
                        if (cit2 != ir_cache_v2_.end()) {
                            cit2->second.dirty = true;
                            cit2->second.mark_all_blocks_dirty();
                            finish_cascade_soa_dirty_sync_(cit2->second);
                        }
                    }
                }
            }
        }
    }

    // Issue #2187: unlocked helper — caller holds exclusive dep_graph_mtx_.
    void mirror_block_dep_edge_unlocked_(std::uint32_t callee_slot, std::uint32_t caller_slot,
                                         std::uint16_t func_idx, std::uint16_t block_idx) {
        const auto from = aura::compiler::dirty::encode_fn_node(callee_slot);
        const auto to =
            aura::compiler::dirty::encode_block_dep_node(caller_slot, func_idx, block_idx);
        const auto edges_before = node_dep_graph_.edge_count();
        node_dep_graph_.add_edge(from, to);
        if (node_dep_graph_.edge_count() > edges_before)
            metrics_.dep_graph_block_mirror_edges_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Scan FlatAST from the given node for Variable nodes that reference cached functions.
    // Records these as dependencies of `def_name`.
    void track_define_dependencies(const std::string& def_name, aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (ir_cache_.empty())
            return;

        struct DepWalker {
            const std::string& def_name;
            aura::ast::FlatAST& flat;
            aura::ast::StringPool& pool;
            CompilerService* self;

            void walk(aura::ast::NodeId id) {
                if (id == aura::ast::NULL_NODE || id >= flat.size())
                    return;
                auto nv = flat.get(id);
                if (nv.tag == aura::ast::NodeTag::Variable) {
                    auto name = pool.resolve(nv.sym_id);
                    auto name_str = std::string(name);
                    // Don't record self-reference. Idempotence + lock
                    // live inside record_dependency (#687 + #1376) —
                    // never touch dep_graph_ directly from this walker.
                    if (name_str != def_name && self->ir_cache_.count(name_str)) {
                        self->record_dependency(def_name, name_str);
                    }
                }
                for (auto c : nv.children)
                    walk(c);
            }
        };

        DepWalker{def_name, flat, pool, this}.walk(flat.root);
    }

    // Invalidate a function and all its transitive dependents (called_by chain).
    // Instead of removing from cache, re-lowers each dependent with the current cache
    // so they stay resolvable in the IR pipeline with updated dependencies.
    // Wave 6: body in service_dirty.cpp (with mark_define_dirty / mark_all).
    // Issue #1523 lock order for invalidate_function:
    //   mutate_mtx_ (unique, FIRST)
    //     → dep_graph_mtx_ (unique, for BFS/erase window)
    //     → jit_cache_mtx_ (not in #1388 quartet; after mutate)
    //   Does NOT take workspace_mtx_ / env_frames_mtx_ directly.
    //   invalidate_bridge_for / batch_deopt run under mutate only.
    void invalidate_function(const std::string& name);


    // Issue #4630: hash-cons the functions of a lowered module.
    //
    // The workspace compile inlines a callee's cached IR bundle at EVERY
    // call site (the lowering copies the whole cached bundle per hit). A
    // define whose body calls the next define twice therefore carries two
    // copies of that define's bundle, and a chain of such defines carries
    // 2^depth copies - while the call graph has only `depth` edges. Every
    // later step (the per-function coercion elimination, the source->IR map
    // rebuild, the bundle copies into the IR cache) is then O(2^depth), so
    // the workspace set-code / eval-current / mutate:rebind cost doubles per
    // chain level (#4630).
    //
    // The copies of one callee function differ ONLY in the function ids they
    // reference (the lowering remaps the MakeClosure target per copy), so
    // the copies are interchangeable: keep one copy per name group and point
    // every reference at the kept copy. The kept copy runs the same code, so
    // behaviour is unchanged - the bundle simply stops carrying duplicates.
    // That is the memoisation the call graph deserves: a callee visited
    // twice is carried once.
    //
    // Returns the number of duplicate functions removed.
    std::size_t hash_cons_module_functions_4630(aura::ir::IRModule& mod) {
        auto& fns = mod.functions;
        if (fns.size() < 2)
            return 0;
        // Shape of a function: its instructions with every function-id
        // operand masked out. Two copies of one function share the shape;
        // two genuinely different functions that happen to share a name do
        // not (both are then kept).
        auto shape_of = [](const aura::ir::IRFunction& f) -> std::uint64_t {
            std::uint64_t h = 1469598103934665603ULL;
            auto mix = [&h](std::uint64_t v) noexcept {
                h ^= v;
                h *= ::aura::compiler::hash::kFnvPrime;
            };
            mix(f.blocks.size());
            for (const auto& blk : f.blocks) {
                mix(blk.instructions.size());
                for (const auto& inst : blk.instructions) {
                    mix(static_cast<std::uint64_t>(inst.opcode));
                    for (std::size_t k = 0; k < inst.operands.size(); ++k) {
                        // MakeClosure operand[1] is the callee function id
                        // (IRModule::find_callers_of reads it the same way).
                        if (inst.opcode == aura::ir::IROpcode::MakeClosure && k == 1)
                            mix(0); // masked - the copies differ only here
                        else
                            mix(inst.operands[k]);
                    }
                }
            }
            return h;
        };
        // Group by function name: every member of a group is a copy of the
        // same cached function, so one member can serve all of them.
        struct Group {
            std::size_t keeper = 0;
            std::uint64_t shape = 0;
        };
        std::unordered_map<std::string, Group, aura::core::TransparentStringHash, std::equal_to<>>
            groups;
        groups.reserve(fns.size());
        std::vector<char> keep(fns.size(), 1);
        std::size_t duplicates = 0;
        for (std::size_t i = 0; i < fns.size(); ++i) {
            const auto shape = shape_of(fns[i]);
            auto it = groups.find(fns[i].name);
            if (it == groups.end()) {
                groups.emplace(fns[i].name, Group{i, shape});
                continue;
            }
            if (it->second.shape != shape)
                continue; // same name, different body - keep both
            keep[i] = 0;
            ++duplicates;
        }
        if (duplicates == 0)
            return 0;
        // dropped function id -> kept function id
        std::unordered_map<std::uint32_t, std::uint32_t> to_kept;
        to_kept.reserve(duplicates * 2);
        for (std::size_t i = 0; i < fns.size(); ++i) {
            if (keep[i] != 0)
                continue;
            const auto& g = groups[fns[i].name];
            to_kept[fns[i].id] = fns[g.keeper].id;
        }
        // Point every reference at the kept copy.
        auto retarget = [&](std::vector<aura::ir::IRFunction>& fns_in) {
            for (auto& f : fns_in)
                for (auto& blk : f.blocks)
                    for (auto& inst : blk.instructions) {
                        if (inst.opcode != aura::ir::IROpcode::MakeClosure)
                            continue;
                        if (inst.operands.size() < 2)
                            continue;
                        auto it = to_kept.find(inst.operands[1]);
                        if (it != to_kept.end())
                            inst.operands[1] = it->second;
                    }
        };
        retarget(fns);
        // Drop the duplicates and renumber the survivors.
        auto old_bridge = std::move(mod.closure_bridge);
        std::vector<aura::ir::IRFunction> kept;
        kept.reserve(fns.size() - duplicates);
        std::vector<std::uint32_t> old_ids;
        old_ids.reserve(fns.size() - duplicates);
        std::unordered_map<std::uint32_t, std::uint32_t> to_new;
        to_new.reserve(fns.size());
        for (std::size_t i = 0; i < fns.size(); ++i) {
            if (keep[i] == 0)
                continue;
            to_new[fns[i].id] = static_cast<std::uint32_t>(kept.size());
            old_ids.push_back(fns[i].id);
            kept.push_back(std::move(fns[i]));
        }
        for (auto& f : kept) {
            for (auto& blk : f.blocks)
                for (auto& inst : blk.instructions) {
                    if (inst.opcode != aura::ir::IROpcode::MakeClosure)
                        continue;
                    if (inst.operands.size() < 2)
                        continue;
                    auto it = to_new.find(inst.operands[1]);
                    if (it != to_new.end())
                        inst.operands[1] = it->second;
                }
            // generic_id indexes IRModule.functions[] - remap it too.
            if (f.generic_id != 0xFFFFFFFFu) {
                auto it = to_new.find(f.generic_id);
                f.generic_id = (it == to_new.end()) ? 0xFFFFFFFFu : it->second;
            }
            f.id = to_new[f.id];
        }
        mod.functions = std::move(kept);
        mod.closure_bridge.assign(mod.functions.size(), aura::ir::ClosureBridgeData{});
        for (std::size_t j = 0; j < mod.functions.size(); ++j) {
            const auto old = old_ids[j];
            if (old < old_bridge.size())
                mod.closure_bridge[j] = std::move(old_bridge[old]);
        }
        if (auto it = to_new.find(mod.entry_function_id); it != to_new.end())
            mod.entry_function_id = it->second;
        return duplicates;
    }


    // Issue #684: lower + absorb SoA dual-emit snapshot.
    aura::ir::IRModule lower_to_ir_with_cache_tracked(
        ast::FlatAST& flat, ast::StringPool& pool, ast::ASTArena& /*arena*/,
        const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>,
                                 aura::core::TransparentStringHash, std::equal_to<>>* cache,
        std::vector<std::string>* cache_hits, const Primitives* primitives,
        const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>,
                                 aura::core::TransparentStringHash, std::equal_to<>>* cache_bridge,
        const std::unordered_map<std::string, std::vector<std::string>,
                                 aura::core::TransparentStringHash, std::equal_to<>>* cache_strings,
        const std::string* self_name, const aura::core::TypeRegistry* /*type_reg*/ = nullptr,
        const std::unordered_map<std::string, std::size_t, aura::core::TransparentStringHash,
                                 std::equal_to<>>* value_cells = nullptr,
        std::uint32_t narrowing_evidence = 0) {
        metrics_.hotpath_lowering_calls.fetch_add(1, std::memory_order_relaxed);
        install_lowering_compiler_core_hooks();
        auto mod = aura::compiler::lower_to_ir_with_cache(
            flat, pool, arena_, cache, cache_hits, primitives, cache_bridge, cache_strings,
            self_name, &type_registry_, value_cells, narrowing_evidence);
        absorb_lower_soa_snapshot();
        return mod;
    }

    // Issue #684: absorb dual-emit snapshot after lowering.
    // Issue #1377: no-op when dual-emit is off (snapshot cleared /
    // zero-emitted) — skips metric updates and pending_soa_snapshot_.
    void absorb_lower_soa_snapshot() {
        if (!aura::compiler::ir_soa_migration::soa_dual_emit_enabled())
            return;
        if (const auto* snap = aura::compiler::lower_last_soa_snapshot()) {
            if (snap->instructions_emitted == 0 && snap->functions_emitted == 0)
                return;
            metrics_.ir_soa_instructions_emitted.fetch_add(snap->instructions_emitted,
                                                           std::memory_order_relaxed);
            metrics_.ir_soa_functions_emitted.fetch_add(snap->functions_emitted,
                                                        std::memory_order_relaxed);
            metrics_.irsoa_wired_hits.fetch_add(1, std::memory_order_relaxed);
            metrics_.hotpath_soa_dual_emit_hits.fetch_add(1, std::memory_order_relaxed);
            // Issue #603: view-equivalent SoA column access in the
            // dual-emit lowering path. Each instruction emitted to
            // the SoA columns is one column read / one "view" at
            // the IRFunctionSoA level. Tracking this lets the AI
            // self-modify loop measure whether the SoA path is
            // being exercised under mutation churn.
            metrics_.ir_soa_view_cache_hits_total.fetch_add(snap->instructions_emitted,
                                                            std::memory_order_relaxed);
            if (snap->type_metadata_stamped > 0) {
                jit_typed_mutation::type_propagation_stamped_total.fetch_add(
                    snap->type_metadata_stamped, std::memory_order_relaxed);
            }
            pending_soa_snapshot_ = *snap;
        }
    }

    // Issue #683 / #1515 / #2043: LinearOwnershipRevalidate after
    // invalidate/re-lower. Delegates to the atomic linear+GC window
    // (finalize_linear_gc_invalidation_window_) so soft/hard share one path.
    void run_linear_ownership_revalidate_after_invalidate(const std::string& name) {
        finalize_linear_gc_invalidation_window_(name);
    }

    // Issue #2043: atomic linear-ownership + GC root window under mutate_mtx_.
    // Must run after dual-epoch bump + live-closure expire, and after any
    // re-lower that may have recreated EnvFrames. Sequence (lock order doc
    // in lock_order_audit.h):
    //   1. scan_live_closures_for_linear_captures (mark invalid)
    //   2. linear_post_mutate_enforce_all
    //   3. sync_linear_roots_and_bridge_epoch (GC roots + probe)
    //   4. bump linear_ownership_epoch + dual-write C for JIT fence
    //   5. run_linear_gc_root_audit
    //   6. release fence so concurrent apply sees a complete window
    void finalize_linear_gc_invalidation_window_(const std::string& name) {
        (void)name;
        metrics_.linear_gc_window_finalize_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.linear_relower_revalidate_hits.fetch_add(1, std::memory_order_relaxed);
        metrics_.linear_post_mutate_revalidations_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.linear_jit_post_invalidate_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
        if (lock_order::is_held(lock_order::Level::Mutate))
            metrics_.linear_gc_window_under_mutate_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #4681: name-independent window; one per restamp batch.
        if (dirty_batch_depth_ > 0) {
            if (dirty_batch_finalize_done_) {
                ++dirty_batch_coalesced_4678_;
                return;
            }
            dirty_batch_finalize_done_ = true;
        }
        if (boundary_prep_depth_ > 0) {
            if (boundary_finalize_done_) {
                ++dirty_batch_coalesced_4678_;
                return;
            }
            boundary_finalize_done_ = true;
        }

        // Issue #4683: no binding ever carried a tracked linear state and
        // no linear root is live -> the scans below cannot find anything;
        // each walks every closure / EnvFrame (O(W) per rebind). The
        // epoch bump, root sync and release fence still run.
        const auto lin_snap_4683 = aura::core::lifetime::linear_root_snapshot();
        const bool skip_linear_scans_4683 = !linear_rt::state_ever_written() &&
                                            lin_snap_4683.live_count == 0 &&
                                            lin_snap_4683.pin_total == 0;
        if (skip_linear_scans_4683)
            ++linear_window_scans_skipped_4683_;
        // Full scan (not only_if_moved): after invalidate every linear
        // capture must revalidate before apply is allowed again.
        if (!skip_linear_scans_4683) {
            (void)evaluator_.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                                    /*only_if_moved=*/false);
            (void)evaluator_.linear_post_mutate_enforce_all();
        }
        evaluator_.sync_linear_roots_and_bridge_epoch();

        const auto ep = evaluator_.bump_linear_ownership_epoch();
        aura_set_linear_ownership_epoch(ep);
        metrics_.linear_ownership_epoch_bumps_total.fetch_add(1, std::memory_order_relaxed);

        if (!skip_linear_scans_4683) {
            (void)evaluator_.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditInvalidate);
            // Issue #2197: post-GC-window linear×type provenance revalidate (R6).
            // Soft = metric-only incomplete; Strict = hard-fail + force-audit arm.
            // mark_all_linear=false avoids re-invalidating already-scanned captures
            // (scan above already mark_invalid=true for all linear).
            (void)evaluator_.revalidate_linear_type_provenance_after_migration(
                Evaluator::kLinearGcRootAuditInvalidate, /*mark_all_linear=*/false);
        }
        // Issue #2131: PostAudit phase — close GcCoordScope if active.
        gc_coord::note_post_audit_if_active();
        // Publish complete linear+GC window to concurrent apply / fiber steal.
        aura::util::thread_fence(std::memory_order_release);
    }

    // Issue #1385: CountingMR — wraps new_delete_resource and
    // tracks cumulative bytes allocated via do_allocate. Used as
    // arena_'s upstream so fallback allocations beyond the
    // arena's monotonic_buffer_resource initial buffer are
    // observable via ast_arena_upstream_bytes metric. The counter
    // is monotonic (deallocate is no-op for monotonic upstream).
    // Declared BEFORE arena_ so arena_ can reference it in member
    // init list.
    struct CountingMR : std::pmr::memory_resource {
        std::atomic<std::uint64_t> bytes_allocated{0};
        std::atomic<std::uint64_t> alloc_count{0};

    private:
        void* do_allocate(std::size_t bytes, std::size_t align) override {
            bytes_allocated.fetch_add(bytes, std::memory_order_relaxed);
            alloc_count.fetch_add(1, std::memory_order_relaxed);
            return std::pmr::new_delete_resource()->allocate(bytes, align);
        }
        void do_deallocate(void* p, std::size_t bytes, std::size_t align) override {
            std::pmr::new_delete_resource()->deallocate(p, bytes, align);
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    };
    CountingMR arena_upstream_mr_{};
    ast::ASTArena arena_{8uz * 1024 * 1024, &arena_upstream_mr_};
    ast::ASTArena temp_arena_;
    ast::ArenaGroup arena_group_;
    Evaluator evaluator_;
    aura::compiler::EvalStrategy strategy_;
    aura::core::TypeRegistry type_registry_; // persistent type registry (L6)
    std::vector<aura::compiler::ClosureSnapshot> last_closures_;
    std::vector<aura::compiler::CellSnapshot> last_cells_;
    std::optional<aura::ir::IRModule> last_ir_mod_;
    aura::ir::IRStatsSnapshot
        last_ir_stats_; // Issue #375: snapshot taken on last_ir_mod_ assignment
    std::vector<std::vector<std::uint8_t>> last_escape_maps_;

    // Set of loaded module names (for ArenaGroup tracking).
    std::unordered_set<std::string> loaded_modules_;

    // Reverse map: module_name → set of cached function names from that module.
    std::unordered_map<std::string, std::vector<std::string>, aura::core::TransparentStringHash,
                       std::equal_to<>>
        module_functions_;

    // Per-module state for incremental compilation (dirty tracking).
    std::unordered_map<std::string, ModuleState, aura::core::TransparentStringHash, std::equal_to<>>
        module_states_;

    // Persistent AST for mutation workflows (set_code / typed_mutate).
    aura::ast::FlatAST* current_ast_ = nullptr;
    // One-shot eval AST. Destroyed on the next pure eval. Retained when
    // the tree can still be referenced (define / lambda / closure).
    std::unique_ptr<aura::ast::FlatAST> recyclable_eval_flat_;
    std::unique_ptr<aura::ast::StringPool> recyclable_eval_pool_;
    bool recyclable_eval_escaped_ = false;
    std::vector<std::unique_ptr<aura::ast::FlatAST>> retained_eval_flats_;
    std::vector<std::unique_ptr<aura::ast::StringPool>> retained_eval_pools_;

    void release_dead_oneshot_eval_ast() {
        if (!recyclable_eval_flat_)
            return;
        if (!recyclable_eval_escaped_ && recyclable_eval_flat_.get() == current_ast_) {
            recyclable_eval_flat_.reset();
            recyclable_eval_pool_.reset();
        } else {
            retained_eval_flats_.push_back(std::move(recyclable_eval_flat_));
            retained_eval_pools_.push_back(std::move(recyclable_eval_pool_));
        }
        recyclable_eval_escaped_ = false;
    }

    void note_oneshot_eval_escaped(const aura::ast::FlatAST& flat, std::size_t closures_before) {
        if (evaluator_.closures_size() != closures_before) {
            recyclable_eval_escaped_ = true;
            return;
        }
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            switch (flat.get(id).tag) {
                case aura::ast::NodeTag::LiteralInt:
                case aura::ast::NodeTag::LiteralFloat:
                case aura::ast::NodeTag::LiteralString:
                case aura::ast::NodeTag::Variable:
                case aura::ast::NodeTag::Call:
                    break;
                default:
                    recyclable_eval_escaped_ = true;
                    return;
            }
        }
    }
    aura::ast::StringPool* current_pool_ = nullptr;

    // Issue #147: post-mutation invariant check mode. Default
    // WarningsOnly so existing call sites see diagnostics but
    // are not blocked. Can be promoted to Strict via
    // set_invariant_check_mode() when soundness enforcement is
    // desired (e.g. in CI runs or --strict CLI flag).
    InvariantCheckMode invariant_check_mode_ = InvariantCheckMode::WarningsOnly;
    // Issue #1415: per-call flag to suppress post_mutation_invariant_check
    // inside typed_mutate. Set to true during typed_mutate_atomic so
    // per-mutation checks don't fire 3× for a 3-mutation batch; the
    // combined check on the dirty union runs once at commit instead.
    // Distinct from invariant_check_mode_ (which is a user-facing
    // strict/permissive/disabled knob) — this is an internal batch
    // optimization. Reset by typed_mutate_atomic before returning so
    // a follow-up typed_mutate (single) runs its check normally.
    bool suppress_invariant_check_ = false;
    // Issue #1538: mutation_id → linear_post_mutate_status for mutation_log
    // JSON ("Ok" | "Unsafe"). Filled by apply_linear_post_mutate_pipeline_.
    // mutable: query_mutation_log is const but needs read access.
    mutable std::unordered_map<std::uint64_t, std::string> mutation_linear_status_;

    // Issue #1383: mode-flip counter + last-disabled-warn flip.
    // Bumped by set_invariant_check_mode on every change. The
    // Disabled branch of typed_mutate consults these to throttle
    // the "invariant checks disabled on workspace with N typed
    // mutations" warning to ONCE per flip (not per-mutation
    // spam). See mode_warnings_ below.
    std::uint64_t mode_flip_count_ = 0;
    std::uint64_t last_disabled_warn_flip_ = 0;
    std::vector<std::string> mode_warnings_;

    // Issue #169: incremental-strictness mode. Default Balanced
    // (= existing behavior). Future Goals 1-4 will read this
    // flag to decide between safe over-invalidation and
    // precise minimal invalidation. The field is private to
    // preserve the invariant; access via set/get above.
    IncrementalStrictness incremental_strictness_ = IncrementalStrictness::Balanced;

    // Issue #411: post-mutation auto-incremental typecheck mode.
    // Default Eager so that (query:type <name>) returns up-to-date
    // results immediately after any (mutate:*) call. Set to Lazy
    // (manual (typecheck-incremental) needed) or Disabled (full
    // pre-#411 behavior) via set_incremental_typecheck_mode().
    IncrementalTypecheckMode incremental_typecheck_mode_ = IncrementalTypecheckMode::Eager;

    // Issue #411: per-eval mutation_log_ size snapshot. Set at
    // the entry of every public eval method (eval / eval_ir /
    // exec_jit), read at the end to detect whether the eval
    // produced a new mutation record. When the size grew, the
    // post-eval auto-invoke runs infer_flat_partial on the most
    // recent record. When the size stayed the same, the eval
    // was a pure read and we skip the auto-invoke. A member
    // field (not a local) because the eval pipeline has
    // multiple return points and the early returns also need
    // the auto-invoke check (the mutate:* primitives can be
    // triggered from the workspace-aware eval shortcut paths).
    std::size_t cs_eval_mutation_log_size_at_entry_ = 0;

    // Nested CompilerService save/restore (process-wide last-writer wires).
    void* prev_compiler_service_ = nullptr;
    void* prev_query_evaluator_ = nullptr;
    void* prev_aot_metrics_ = nullptr;
    const Primitives* prev_jit_prim_ctx_ = nullptr;
    Evaluator* prev_jit_prim_owner_ = nullptr; // Issue #3593: owner for the wired ctx

public:
    // Issue #300 follow-up #1: drop per-fiber / main-thread
    // mutation checkpoints before arena teardown so PCV
    // children_snapshot copies do not race ~workspace_flat_.
    CompilerService(const CompilerService&) = delete;
    CompilerService& operator=(const CompilerService&) = delete;
    CompilerService(CompilerService&&) = delete;
    CompilerService& operator=(CompilerService&&) = delete;
    ~CompilerService() {
        evaluator_.set_tw_tier_hook_4682(nullptr, nullptr);
        evaluator_.set_exit_quiesce_hook(nullptr, nullptr);
        if (tier_worker_4682_)
            tier_worker_4682_->drain();
        // Issue #4377: drop the boundary-exit hook before teardown.
        pop_ir_cache_boundary_restamp(this);
        // Issue #3394: drain outstanding thread-backend fiber workers
        // BEFORE any service teardown touches the workspace/AST —
        // release_children_for_teardown() below frees the flat children
        // vectors a still-running fiber body may be reading (UAF →
        // PersistentChildVector OOB abort). ~Evaluator drains again for
        // bare-Evaluator users; this site covers the earlier service-body
        // window.
        // Issue #4831: drain/cancel this Evaluator's orch agent bodies
        // BEFORE release_children_for_teardown() frees the FlatAST they
        // are still reading.
        evaluator_.cleanup_orch_agents();
        evaluator_.drain_thread_fibers();
        // Issue #2576-lifecycle: unregister the storm listener before the
        // SpecJITController (member spec_jit_) is destroyed. The listener
        // captures `this`; without removal a later deopt storm (any test /
        // agent triggering deopts after this service goes out of scope)
        // would call on_deopt_storm on the freed controller (UAF).
        if (storm_listener_id_ != 0)
            hot_update_registry().unregister_storm_listener(storm_listener_id_);
        // Nested CompilerService: keep outer lowering / mutation-stack
        // wiring. Only the outermost service wipes process-wide TLS.
        if (prev_compiler_service_ == nullptr) {
            clear_lowering_compiler_core_hooks();
            Evaluator::clear_main_thread_mutation_stack();
        }
        if (auto* wf = evaluator_.workspace_flat())
            wf->release_children_for_teardown();
        if (current_ast_ && current_ast_ != evaluator_.workspace_flat())
            current_ast_->release_children_for_teardown();
        // Issue #1996 (B-003): clear g_batch_deopt_jit before jit_ is
        // destroyed. The file-scope pointer in aura_jit_bridge.cpp
        // was wired at service.ixx:668 to &jit_; a late batch_deopt
        // that fires after this destructor runs (residual worker
        // fiber, test teardown, repeated (query:jit-reset) lifecycle)
        // would dereference a freed AuraJIT. Pass &jit_ so the clear
        // only matches our own wire — a sibling CompilerService that
        // happens to own the live pointer (multi-service scenario)
        // is preserved.
        aura_clear_jit_batch_deopt_target(&jit_);
        // Issue #2438 (extends #765): clear arena auto-compact notify hooks and
        // drain in-flight notify_* before nulling g_current_compiler_service.
        // Without the drain, concurrent notify_auto_compact_trigger can
        // load g_current, then we free this service, then the notify body
        // touches metrics() → UAF (TOCTOU on the process-wide service ptr).
        // Order (do not reverse):
        //   1. clear_arena_compact_notify_hooks() — nullptr + wait in_flight
        //   2. g_current_compiler_service = nullptr (if we own it)
        //   3. member destruction continues
        // Only clear process-wide hooks when we still own g_current (another
        // live CompilerService may have re-wired them).
        if (aura::messaging::g_current_compiler_service == this) {
            if (prev_compiler_service_ == nullptr) {
                aura::gc_hooks::clear_arena_compact_notify_hooks();
                aura::messaging::g_current_compiler_service = nullptr;
                // Issue #2100: drop deopt restore hook with the owning service.
                aura_jit_set_macro_deopt_restore_fn(nullptr);
            } else {
                aura::messaging::g_current_compiler_service = prev_compiler_service_;
            }
        }
        // Issue #2106: drop cascade_skip metrics sink if it points at our
        // metrics_ (avoid UAF after service teardown, same pattern as
        // aura_clear_jit_batch_deopt_target).
        if (aura::compiler::dirty::g_cascade_skip_subtree_metrics ==
            &metrics_.cascade_skip_subtree_total) {
            aura::compiler::dirty::set_cascade_skip_subtree_metrics(nullptr);
        }
        // Issue #1368: unwire the process-wide AOT metrics pointer if it
        // still points at our metrics_ (wired at ctor via
        // evaluator_.set_compiler_metrics(&metrics_) and again in
        // register_jit_primitives). A stack-resident CompilerService that
        // goes out of scope before a later Evaluator teardown must not
        // leave g_aot_metrics dangling — aura_cleanup_aot_state →
        // aura_aot_invalidate_all_stale_slots_for_eval bumps counters via
        // aot_metrics() (ASan: stack-use-after-scope, test_ir.cpp 'cs'
        // scope). Only clear our own wire — a sibling CompilerService
        // that owns the live pointer is preserved (same pattern as
        // aura_clear_jit_batch_deopt_target).
        if (aura_get_aot_metrics() == static_cast<void*>(&metrics_)) {
            aura_set_aot_metrics(static_cast<CompilerMetrics*>(prev_aot_metrics_));
        }
        if (g_jit_prim_ctx.load(std::memory_order_acquire) == &evaluator_.primitives()) {
            g_jit_prim_ctx.store(prev_jit_prim_ctx_, std::memory_order_release);
            // Issue #3593: clear the owner with the ctx it belongs to.
            g_jit_prim_owner.store(prev_jit_prim_owner_, std::memory_order_release);
        }
        Evaluator::set_query_evaluator(static_cast<Evaluator*>(prev_query_evaluator_));
        pop_linear_hooks_for_eval(&evaluator_);
        pop_lock_hooks_for_eval(&evaluator_);
        // Nested CompilerService overwrites process-wide lock / top-cell
        // hooks with &evaluator_. Clear only our own wire so ~inner does
        // not leave lock_write on a destroyed workspace_mtx_ (heap
        // "corrupted double-linked list" / SIGBUS in aura_alloc_closure).
        aura_clear_evaluator_runtime_hooks(static_cast<void*>(&evaluator_));
    }

    // Issue #411 fu1 follow-up #2: per-DefUseIndex caller
    // tracker. Per-service state (lifetime = CompilerService
    // lifetime). See per_defuse_index_tracker() accessor
    // above. Holds the per-DefUseIndex map of callers;
    // queried by the (compile:per-defuse-index-callers)
    // primitive and the upcoming (infer_flat_partial)
    // indexed path (#411 fu1 follow-up #3).
    per_defuse_index::PerDefUseIndexTracker per_defuse_index_tracker_;

    // Issue #412 follow-up #1: per-binding gen bumps
    // accumulator. snapshot() is const so we can't
    // fetch_add on the CompilerMetrics atomic. Instead,
    // we accumulate here (mutable, lifetime total) and
    // read the FlatAST's per-binding gen bumps counter
    // each snapshot to compute the delta. This gives a
    // persistent lifetime total across workspace swaps.
    mutable std::uint64_t per_binding_gen_bumps_acc_ = 0;
    mutable std::uint64_t last_per_binding_gen_bumps_ = 0;
    // Issue #413: invalidation trace records
    // accumulator. Same pattern as
    // per_binding_gen_bumps_acc_: read the FlatAST's
    // per-snapshot counter, accumulate the delta into
    // this lifetime total. snapshot() is const so we
    // can't fetch_add on a CompilerMetrics atomic.
    mutable std::uint64_t invalidation_trace_records_acc_ = 0;
    mutable std::uint64_t last_invalidation_trace_records_ = 0;

    // Issue #225 cycle 3: public test hook for the bridge
    // invalidation helper. Production code triggers this
    // through mark_define_dirty / invalidate_function /
    // hot_swap_function_impl / reset(). Tests can call
    // this directly to verify the helper's behavior.
    // Issue #426: total dirty block count across the
    // entire ir_cache_v2_. Used by the
    // (query:compiler-cache-stats) primitive. Public so
    // the query layer (which holds a void* to this
    // service) can call it without a friend
    // declaration.
    [[nodiscard]] std::size_t total_dirty_block_count() const noexcept {
        std::size_t n = 0;
        for (const auto& [name, entry] : ir_cache_v2_) {
            n += entry.dirty_block_count();
        }
        return n;
    }

    // Issue #293: count of functions (across all ir_cache_v2_
    // entries) that have at least one dirty block.
    [[nodiscard]] std::size_t total_dirty_func_count() const noexcept {
        std::size_t n = 0;
        for (const auto& [name, entry] : ir_cache_v2_) {
            n += entry.dirty_func_count();
        }
        return n;
    }

    // Issue #298: total number of cached defines (used as
    // denominator for the recompile-ratio basis-points calc).
    [[nodiscard]] std::size_t ir_cache_v2_size() const noexcept { return ir_cache_v2_.size(); }

    // Issue #293: count of functions that are "incremental
    // re-lower" candidates (1..7 dirty blocks per function).
    // Excludes functions with 8+ dirty blocks (full re-lower
    // territory) and 0 dirty blocks (already clean).
    [[nodiscard]] std::size_t total_incremental_candidates() const noexcept {
        std::size_t n = 0;
        for (const auto& [name, entry] : ir_cache_v2_) {
            n += entry.incremental_candidates_count();
        }
        return n;
    }

    // Issue #293: read-only view of an ir_cache_v2_ entry.
    // Returns nullptr if the function is not in the cache.
    // Used by (compile:relower-strategy) to look up
    // dirty_block_count for a specific function.
    //
    // Issue #1855: unlocked pointer return is racy vs concurrent
    // mark/clear/invalidate — prefer ir_cache_v2_dirty_block_count()
    // which snapshots under jit_cache_mtx_ shared_lock. Kept for
    // single-threaded / already-locked call sites.
    [[nodiscard]] const IRCacheEntry* ir_cache_v2_find(const std::string& name) const noexcept {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return nullptr;
        return &it->second;
    }

    // Issue #1855: coherent dirty_block_count for
    // (compile:relower-strategy). shared_lock pairs with
    // mark_block_dirty_v2 / clear_block_dirty_v2 / invalidate
    // unique_lock on jit_cache_mtx_. nullopt = not in cache.
    [[nodiscard]] std::optional<std::size_t>
    ir_cache_v2_dirty_block_count(const std::string& name) const noexcept {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        std::shared_lock cache_read(jit_cache_mtx_);
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return std::nullopt;
        return it->second.dirty_block_count();
    }

    // Issue #4357: true when bind_function_define_via_ir owns this name.
    // compile:relower-strategy reports incremental for a clean entry only
    // when this is set. Ordinary clean cache rows stay 'none' (#293).
    [[nodiscard]] bool ir_define_env_bound(const std::string& name) const noexcept {
        return ir_define_env_bindings_.find(name) != ir_define_env_bindings_.end();
    }

    // Issue #429: SoA dirty stats aggregate. The hook
    // closure for (query:soa-dirty-stats) reads these
    // 8 fields in one pass. Layout (issue #429 scope-limited
    // ship — fills the gap that (query:ir-soa-incremental-stats)
    // leaves: that primitive reports mutation-event counts
    // (sums of lifetime atomic counters), but doesn't expose
    // the live per-block / per-instruction dirty state of
    // the cache). The 8 fields:
    //   - cached_fns            : # entries in ir_cache_v2_
    //   - dirty_fns             : # entries with entry.dirty == true
    //   - total_blocks          : sum of block_dirty_per_func_[i].size() across all entries+funcs
    //   - dirty_blocks          : sum of #dirty blocks (== entry.dirty_block_count() per entry)
    //   - total_instructions    : sum of IRFunction.instructions.size() across all entries+funcs
    //   - dirty_instructions    : sum of #dirty instructions across all entries+funcs
    //                             (issue #380 instruction_dirty_ column)
    //   - dirty_block_pct       : 100 * dirty_blocks / max(1, total_blocks) (integer percent)
    //   - dirty_instruction_pct : 100 * dirty_instructions / max(1, total_instructions)
    //
    // The migration_progress field (10th, per the issue body)
    // is deferred — the issue's intended formula
    // "cached_fns / (cached_fns + uncached_defines)" requires
    // enumerating the workspace defines, which would couple
    // the primitive to a separate traversal. A future follow-up
    // (after #167 SoA migration Phase 2 ships) will add
    // it once the IRModuleV2 entry-point gives a single
    // source of truth for "what's in the cache vs not".
    //
    // Returns the same SoaDirtyStats struct as
    // Evaluator::SoaDirtyStats (single source of truth — we
    // don't redefine it here to avoid the
    // "different-SoaDirtyStats-from-Service" type mismatch
    // the hook closure would otherwise see).
    [[nodiscard]] Evaluator::SoaDirtyStats get_soa_dirty_stats() const noexcept {
        Evaluator::SoaDirtyStats s;
        for (const auto& [name, entry] : ir_cache_v2_) {
            ++s.cached_fns;
            if (entry.dirty)
                ++s.dirty_fns;
            // Per-block aggregate: each IRFunction has its own
            // dirty column; we sum across all funcs in the entry.
            for (std::size_t fi = 0; fi < entry.irs.size(); ++fi) {
                const auto& func = entry.irs[fi];
                // IRFunction uses `instructions_` (private field) as
                // its instruction storage. We need a public accessor
                // for the size — see IRFunction::instruction_count()
                // (added in the same issue as the SoA dirty stats).
                s.total_instructions += entry.func_instruction_count(fi);
                if (fi < entry.block_dirty_per_func_.size()) {
                    const auto& fb = entry.block_dirty_per_func_[fi];
                    s.total_blocks += fb.size();
                    for (auto b : fb)
                        if (b)
                            ++s.dirty_blocks;
                }
                // Issue #684: per-instruction dirty from
                // instruction_dirty_per_func_ (SoA cascade).
                if (fi < entry.instruction_dirty_per_func_.size()) {
                    for (auto d : entry.instruction_dirty_per_func_[fi])
                        if (d)
                            ++s.dirty_instructions;
                }
            }
        }
        s.dirty_block_pct = s.total_blocks > 0 ? (s.dirty_blocks * 100) / s.total_blocks : 0;
        s.dirty_instruction_pct =
            s.total_instructions > 0 ? (s.dirty_instructions * 100) / s.total_instructions : 0;
        return s;
    }
    // Issue #1523: public bridge invalidate takes mutate FIRST then stamps.
    void public_invalidate_bridges_for(const std::string& name) {
        using aura::compiler::lock_order::Level;
        using aura::compiler::lock_order::OrderedUniqueLock;
        OrderedUniqueLock<std::shared_mutex> mutate_guard(mutate_mtx_, Level::Mutate);
        sync_lock_order_metrics_();
        invalidate_bridge_for(name);
    }
    // Issue #1522 / #1524: public test hook for atomic epoch + bridge + fn_trackers deopt.
    void public_atomic_bump_epochs_and_stamp_bridge(const std::string& name) {
        atomic_bump_epochs_and_stamp_bridge(name);
    }
    // Issue #1524: public typed_mutate surface for dual-epoch stress tests.
    [[nodiscard]] MutationResult public_typed_mutate(std::string_view sexpr) {
        return typed_mutate(sexpr);
    }
    [[nodiscard]] MutationResult
    public_typed_mutate_atomic(std::span<const std::string_view> mutations) {
        return typed_mutate_atomic(mutations);
    }
    [[nodiscard]] std::uint64_t public_typed_mutate_atomic_invalidations_total() const noexcept {
        return metrics_.typed_mutate_atomic_invalidations_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_typed_mutate_epoch_bumps() const noexcept {
        return metrics_.typed_mutate_epoch_bumps.load(std::memory_order_relaxed);
    }
    // Issue #1523: lock-order metrics surface for Agents / tests.
    [[nodiscard]] std::uint64_t public_lock_inversion_detected_total() const noexcept {
        return metrics_.lock_inversion_detected_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_mutate_mtx_contended_total() const noexcept {
        return metrics_.mutate_mtx_contended_total.load(std::memory_order_relaxed);
    }
    void public_sync_lock_order_metrics() noexcept { sync_lock_order_metrics_(); }
    [[nodiscard]] std::uint64_t public_jit_batch_deopt_for_total() const noexcept {
        return jit_.metrics().batch_deopt_for_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_jit_batch_deopt_entries_marked() const noexcept {
        return jit_.metrics().batch_deopt_entries_marked.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool public_jit_is_deopt_pending(const char* name) const noexcept {
        return jit_.is_deopt_pending(name);
    }
    [[nodiscard]] std::uint64_t public_jit_deopt_pending_count() const noexcept {
        return jit_.deopt_pending_count();
    }
    // Issue #1536: public test hook for bulk walk_active_closures.
    std::size_t public_walk_active_closures() { return jit_.walk_active_closures(bridge_epoch()); }
    [[nodiscard]] std::uint64_t public_walk_active_closures_total() const noexcept {
        return metrics_.jit_walk_active_closures_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_walk_active_closures_stale_total() const noexcept {
        return metrics_.jit_walk_active_closures_stale_total.load(std::memory_order_relaxed);
    }
    // Issue #2042: test seam — run comprehensive live-closure expire at
    // current bridge epoch (same helper as soft/hard invalidate).
    std::size_t public_expire_stale_live_closures() {
        return expire_stale_live_closures_(bridge_epoch());
    }
    [[nodiscard]] std::uint64_t public_expire_stale_live_closures_total() const noexcept {
        return metrics_.expire_stale_live_closures_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_expire_stale_ir_closures_total() const noexcept {
        return metrics_.expire_stale_ir_closures_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_expire_stale_tree_walker_closures_total() const noexcept {
        return metrics_.expire_stale_tree_walker_closures_total.load(std::memory_order_relaxed);
    }
    // Issue #2043: test seam for linear+GC finalize window.
    void public_finalize_linear_gc_window(const std::string& name = {}) {
        finalize_linear_gc_invalidation_window_(name);
    }
    [[nodiscard]] std::uint64_t public_linear_gc_window_finalize_total() const noexcept {
        return metrics_.linear_gc_window_finalize_total.load(std::memory_order_relaxed);
    }
    // Issue #2131: GcCoordScope observability + test seams.
    [[nodiscard]] std::uint64_t public_gc_coord_scopes_opened() const noexcept {
        return gc_coord::scopes_opened_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_gc_coord_post_audit_total() const noexcept {
        return gc_coord::post_audit_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_gc_coord_phase_violations() const noexcept {
        return gc_coord::phase_violations_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_gc_coord_reverse_order() const noexcept {
        return gc_coord::reverse_order_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_gc_coord_scopes_for_path(std::uint8_t path) const noexcept {
        if (path >= static_cast<std::uint8_t>(gc_coord::Path::Count))
            return 0;
        return gc_coord::scopes_by_path[path].load(std::memory_order_relaxed);
    }
    void public_gc_coord_set_strict(bool on) noexcept {
        gc_coord::strict_mode.store(on, std::memory_order_relaxed);
    }
    void public_gc_coord_force_reverse_violation() noexcept {
        // Keep strict off so the test does not abort.
        const bool prev = gc_coord::strict_mode.exchange(false, std::memory_order_relaxed);
        gc_coord::Scope::force_reverse_violation_for_test();
        gc_coord::strict_mode.store(prev, std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_linear_ownership_epoch() const noexcept {
        return evaluator_.linear_ownership_epoch();
    }
    [[nodiscard]] std::uint64_t public_linear_ownership_epoch_bumps_total() const noexcept {
        return metrics_.linear_ownership_epoch_bumps_total.load(std::memory_order_relaxed);
    }

    // Issue #401: public test hook for invalidate_function. Lets tests
    // drive the BFS traversal directly without going through the Aura
    // (mutate :rebind) EDSL surface, which would also rebuild the function
    // body (mixing the traversal test with the rebind path).
    void public_invalidate_function(const std::string& name) { invalidate_function(name); }
    // Issue #4570: optimized LLVM IR of a JIT-compiled function (empty if
    // never compiled). Backs `aura --jit-session` ";;ir NAME" and the L1
    // no-runtime-call IR snapshot test.
    [[nodiscard]] std::string public_jit_function_ir(const std::string& name) {
        return jit_.compile_function_to_llvm_ir_by_name(name.c_str());
    }
    // Issue #1514: test/Agent seam for dirty cascade without full invalidate.
    void public_mark_define_dirty(const std::string& name) { mark_define_dirty(name); }
    // Issue #2137: test/Agent entry to drain deferred non-render cascades.
    void public_flush_frame_budget_deferred() { flush_frame_budget_deferred_(); }
    // Issue #1495: test/agent entry for partial re-lower of dirty defines.
    std::size_t public_relower_dirty_defines_from_workspace() {
        return relower_dirty_defines_from_workspace();
    }

    // Issue #3069 / #3117: abort-force fence test hooks.
    void public_begin_abort_ir_cache_force_fence() { begin_abort_ir_cache_force_fence(); }
    void public_force_ir_cache_dirty_after_abort() { force_ir_cache_dirty_after_abort(); }
    // Issue #3985: test seam for Phase-5 densify map invalidation.
    void public_force_ir_cache_map_invalid_after_densify() {
        force_ir_cache_map_invalid_after_densify();
    }
    void public_set_abort_force_hold(bool on) {
        abort_force_hold_.store(on ? 1 : 0, std::memory_order_release);
    }
    // Issue #3159 AC4 alias (set_abort_force_hold_for_test).
    void set_abort_force_hold_for_test(bool on) { public_set_abort_force_hold(on); }
    [[nodiscard]] std::uint64_t public_abort_force_generation() const noexcept {
        return abort_force_generation_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool public_abort_force_in_progress() const noexcept {
        return abort_force_in_progress_.load(std::memory_order_acquire) != 0;
    }
    [[nodiscard]] bool public_source_to_ir_map_consistent(const std::string& name) const {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return true;
        return source_to_ir_map_is_consistent(it->second.irs, it->second.source_to_ir_map);
    }

    // Issue #1378: test accessors for cascade ordering / epoch accounting.
    [[nodiscard]] std::uint64_t public_mutation_epoch() const noexcept {
        return aura::core::current_mutation_epoch();
    }
    [[nodiscard]] std::uint64_t public_invalidate_function_calls() const noexcept {
        return metrics_.invalidate_function_calls.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool public_jit_cache_contains(const std::string& name) const {
        std::shared_lock cache_read(jit_cache_mtx_);
        return jit_cache_.find(name) != jit_cache_.end();
    }
    // Issue #3749: seed a dummy jit_cache_ entry so light-link tests
    // can observe production facade eviction without a real compile.
    void public_jit_cache_insert_dummy_for_test(const std::string& name) {
        std::unique_lock cache_write(jit_cache_mtx_);
        auto [it, _ins] = jit_cache_.try_emplace(name);
        it->second.fn_ptr.store(nullptr, std::memory_order_release);
        it->second.last_seen_epoch_ = aura::core::current_mutation_epoch();
    }
    // Issue #4510: test entry for the ArenaCompact dirty-scope arm
    // (on_arena_compact_notify is otherwise reachable only via the arena
    // compact hook chain). Fires kShapeDirtyScopeArenaCompact deopt hooks
    // for the dirty ∪ relocated cone (#3455) — the same trampoline the
    // production compact drives.
    void public_on_arena_compact_notify() noexcept { on_arena_compact_notify(); }
    // Issue #4578: fire the ArenaCompact arm for one define. The cone
    // notify only reaches defines whose IR row is block-dirty.
    void public_arena_compact_deopt_for_test(const std::string& name) noexcept {
        on_shape_deopt_hook(shape::make_fn_key(session_id_, name), /*version=*/0,
                            shape::kShapeDirtyScopeArenaCompact);
    }
    // Issue #4577: empty v2 row so mark_all_defines_dirty sees the name.
    void public_seed_define_cache_shell_for_test(const std::string& name) {
        ir_cache_v2_.try_emplace(name);
    }
    // Issue #3758: light-link / no-LLVM get_function_ptr is null. Seed a
    // named jit_cache_ hit + counting ScalarFn override so try_jit_execute
    // reaches the execute catalog. Returns how many times ScalarFn ran.
    std::uint32_t public_try_jit_execute_counting_scalar_for_test() {
        try_jit_scalar_invokes_for_test_.store(0, std::memory_order_relaxed);
        try_jit_scalar_override_for_test_ = &counting_scalar_fn_for_test_;
        constexpr const char* kName = "ac3758_cached";
        aura::ir::IRModule mod;
        aura::ir::IRFunction fn;
        fn.id = 0;
        fn.name = kName;
        fn.local_count = 1;
        fn.arg_count = 0;
        aura::ir::BasicBlock block;
        block.id = 0;
        aura::ir::IRInstruction ret;
        ret.opcode = aura::ir::IROpcode::Return;
        ret.operands[0] = 0;
        block.instructions.push_back(ret);
        fn.blocks.push_back(std::move(block));
        mod.functions.push_back(std::move(fn));
        mod.entry_function_id = 0;
        // Issue #4569: seed the assumption fingerprint too — jit_entry_stale_
        // treats fingerprint_==0 as a miss, so a default-constructed row
        // never hits in assumption mode and the counting ScalarFn stayed
        // at 0 (3758 AC3 vacuous-green AC1).
        const JitAssumeKey akey =
            jit_assume_key_(mod.functions[0], mod.string_pool, nullptr, nullptr, 0);
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            auto [it, _ins] = jit_cache_.try_emplace(kName);
            it->second.fn_ptr.store(&counting_scalar_fn_for_test_, std::memory_order_release);
            it->second.local_count = 1;
            it->second.arg_count = 0;
            it->second.last_seen_epoch_ = aura::core::current_mutation_epoch();
            it->second.fingerprint_ = akey.fp;
        }
        (void)try_jit_execute(mod, nullptr);
        try_jit_scalar_override_for_test_ = nullptr;
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            jit_cache_.erase(kName);
        }
        return try_jit_scalar_invokes_for_test_.load(std::memory_order_relaxed);
    }

    // Issue #4073: seed a jit_cache_ hit, advance the mutation epoch
    // without evict_jit_cache_after_production_facade_ on this name, then
    // try_jit_execute. Returns ScalarFn invoke count. Non-zero sentinels
    // mean the entry was already gone or its epoch matched (precondition).
    std::uint32_t public_try_jit_stale_epoch_scalar_invokes_for_test() {
        try_jit_scalar_invokes_for_test_.store(0, std::memory_order_relaxed);
        try_jit_scalar_override_for_test_ = &counting_scalar_fn_for_test_;
        constexpr const char* kName = "ac4073_stale";
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            auto [it, _ins] = jit_cache_.try_emplace(kName);
            it->second.fn_ptr.store(&counting_scalar_fn_for_test_, std::memory_order_release);
            it->second.local_count = 1;
            it->second.arg_count = 0;
            it->second.last_seen_epoch_ = aura::core::current_mutation_epoch();
        }
        aura::core::bump_mutation_epoch();
        {
            std::shared_lock cache_read(jit_cache_mtx_);
            auto it = jit_cache_.find(kName);
            if (it == jit_cache_.end() ||
                it->second.last_seen_epoch_ == aura::core::current_mutation_epoch()) {
                try_jit_scalar_override_for_test_ = nullptr;
                return it == jit_cache_.end() ? 0xffffffffu : 0xfffffffeu;
            }
        }
        aura::ir::IRModule mod;
        aura::ir::IRFunction fn;
        fn.id = 0;
        fn.name = kName;
        fn.local_count = 1;
        fn.arg_count = 0;
        aura::ir::BasicBlock block;
        block.id = 0;
        aura::ir::IRInstruction ret;
        ret.opcode = aura::ir::IROpcode::Return;
        ret.operands[0] = 0;
        block.instructions.push_back(ret);
        fn.blocks.push_back(std::move(block));
        mod.functions.push_back(std::move(fn));
        mod.entry_function_id = 0;
        (void)try_jit_execute(mod, nullptr);
        try_jit_scalar_override_for_test_ = nullptr;
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            jit_cache_.erase(kName);
        }
        return try_jit_scalar_invokes_for_test_.load(std::memory_order_relaxed);
    }

    // Issue #4388: low 8 bits are ScalarFn invokes, bit 8 is set when the
    // cache hit was refused before publish / invoke. mode 0 leaves the hit
    // alone. mode 1 bumps the mutation epoch (single-eval face). mode 2
    // erases the cache entry and drops native without an epoch bump
    // (owner-scoped face). Production defaults must already be active.
    std::uint32_t public_try_jit_facade_raced_hit_for_test(int mode) {
        try_jit_scalar_invokes_for_test_.store(0, std::memory_order_relaxed);
        try_jit_entry_hit_refused_for_test_ = 0;
        try_jit_scalar_override_for_test_ = &counting_scalar_fn_for_test_;
        try_jit_facade_skip_mode_for_test_ = mode;
        constexpr const char* kName = "ac4388_hit";
        aura::ir::IRModule mod;
        aura::ir::IRFunction fn;
        fn.id = 0;
        fn.name = kName;
        fn.local_count = 1;
        fn.arg_count = 0;
        aura::ir::BasicBlock block;
        block.id = 0;
        aura::ir::IRInstruction ret;
        ret.opcode = aura::ir::IROpcode::Return;
        ret.operands[0] = 0;
        block.instructions.push_back(ret);
        fn.blocks.push_back(std::move(block));
        mod.functions.push_back(std::move(fn));
        mod.entry_function_id = 0;
        // Issue #4388 (after #4569): stamp the forged entry the way a real
        // insert does (assumption fingerprint + deps). An unstamped entry
        // is stale under #4569, so the hit arm this probe drives never ran.
        {
            const auto& pf = mod.functions[0];
            const JitAssumeKey akey = jit_assume_key_(pf, mod.string_pool, nullptr,
                                                      pf.escape_map.data(), pf.escape_map.size());
            std::unique_lock cache_write(jit_cache_mtx_);
            auto [it, _ins] = jit_cache_.try_emplace(kName);
            it->second.fn_ptr.store(&counting_scalar_fn_for_test_, std::memory_order_release);
            it->second.local_count = 1;
            it->second.arg_count = 0;
            it->second.last_seen_epoch_ = aura::core::current_mutation_epoch();
            it->second.fingerprint_ = akey.fp;
            it->second.deps_ = jit_assume_snapshot_(
                akey, aura::compiler::jit_assume::define_clock().load(std::memory_order_acquire));
        }
        (void)try_jit_execute(mod, nullptr);
        try_jit_scalar_override_for_test_ = nullptr;
        try_jit_facade_skip_mode_for_test_ = 0;
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            jit_cache_.erase(kName);
        }
        const auto invokes = try_jit_scalar_invokes_for_test_.load(std::memory_order_relaxed);
        return (invokes & 0xffu) | (try_jit_entry_hit_refused_for_test_ != 0 ? 0x100u : 0u);
    }

    // Issue #4388: an armed cache-hit register must not replace a live
    // ScalarFn after the mutation epoch moves or a define drop runs, and
    // a matching sample must still publish. Returns 1 when both hold.
    int public_cache_hit_republish_refuses_for_test() {
        constexpr std::int64_t kId = 438800;
        aura_register_fn_named("ac4388_republish", kId, &ac4388_sentinel_a_, 0, 0, 0);
        const auto installed = aura_jit_fn_ptr_for_test(kId);
        const auto epoch = aura::core::current_mutation_epoch();
        const auto gen = aura_jit_define_drop_gen();
        aura_jit_cache_hit_republish_arm(epoch, gen);
        aura::core::bump_mutation_epoch();
        aura_register_fn_named("ac4388_republish", kId, &ac4388_sentinel_b_, 0, 0, 0);
        const int epoch_refused = aura_jit_cache_hit_republish_refused();
        const auto after_epoch = aura_jit_fn_ptr_for_test(kId);
        aura_jit_cache_hit_republish_disarm();

        const auto epoch_now = aura::core::current_mutation_epoch();
        const auto gen_now = aura_jit_define_drop_gen();
        aura_jit_cache_hit_republish_arm(epoch_now, gen_now);
        aura_register_fn_named("ac4388_republish", kId, &ac4388_sentinel_b_, 0, 0, 0);
        const int fresh_refused = aura_jit_cache_hit_republish_refused();
        const auto published = aura_jit_fn_ptr_for_test(kId);
        aura_jit_cache_hit_republish_disarm();

        aura_jit_cache_hit_republish_arm(aura::core::current_mutation_epoch(),
                                         aura_jit_define_drop_gen());
        aura_drop_jit_fn_native_for_define("ac4388_unrelated");
        aura_register_fn_named("ac4388_republish", kId, &ac4388_sentinel_a_, 0, 0, 0);
        const int drop_refused = aura_jit_cache_hit_republish_refused();
        const auto after_drop = aura_jit_fn_ptr_for_test(kId);
        aura_jit_cache_hit_republish_disarm();
        aura_drop_jit_fn_native_for_define("ac4388_republish");

        const auto sentinel_b = reinterpret_cast<std::int64_t>(&ac4388_sentinel_b_);
        if (installed == 0 || after_epoch != installed || epoch_refused == 0)
            return 0;
        if (fresh_refused != 0 || published != sentinel_b)
            return 0;
        if (drop_refused == 0 || after_drop != sentinel_b)
            return 0;
        return 1;
    }

    // Issue #1377: opt-in SoA dual-emit (default off). When false,
    // lower_to_ir skips IRFunctionSoA columns + bridge counters.
    void set_soa_dual_emit(bool enable) noexcept {
        enable_soa_dual_emit_.store(enable, std::memory_order_relaxed);
        aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(enable);
    }
    [[nodiscard]] bool soa_dual_emit_enabled() const noexcept {
        return enable_soa_dual_emit_.load(std::memory_order_relaxed);
    }

    // Issue #401: public test accessors for the dep_graph_.
    //
    // The dep_graph_ is a private member (no public introspection in the
    // production API), but the determinism contract for invalidate_function
    // needs to be verifiable from tests: that BFS traversal visits every
    // transitively-called_by node exactly once, and that the recorded
    // calls/called_by edges are symmetric across invalidate cycles.
    //
    // These hooks return only counts and presence — not the underlying
    // vector contents — so test code can verify integrity without leaking
    // the unordered_map layout.

    // Number of entries currently in dep_graph_ (post-cleanup state).
    [[nodiscard]] std::size_t public_dep_graph_size() const noexcept {
        std::shared_lock dep_read(dep_graph_mtx_);
        return dep_graph_.size();
    }

    // Whether a name is in dep_graph_.
    [[nodiscard]] bool public_dep_graph_contains(const std::string& name) const noexcept {
        std::shared_lock dep_read(dep_graph_mtx_);
        return dep_graph_.find(name) != dep_graph_.end();
    }

    // Outgoing-edge count (this name calls N functions).
    [[nodiscard]] std::size_t public_dep_graph_calls_for(const std::string& name) const noexcept {
        std::shared_lock dep_read(dep_graph_mtx_);
        auto it = dep_graph_.find(name);
        if (it == dep_graph_.end())
            return 0;
        return it->second.calls.size();
    }

    // Incoming-edge count (N functions call this name).
    [[nodiscard]] std::size_t
    public_dep_graph_called_by_for(const std::string& name) const noexcept {
        std::shared_lock dep_read(dep_graph_mtx_);
        auto it = dep_graph_.find(name);
        if (it == dep_graph_.end())
            return 0;
        return it->second.called_by.size();
    }

    // Whether a directed edge caller → callee is recorded.
    [[nodiscard]] bool public_dep_graph_has_edge(const std::string& caller,
                                                 const std::string& callee) const noexcept {
        std::shared_lock dep_read(dep_graph_mtx_);
        auto cit = dep_graph_.find(caller);
        if (cit == dep_graph_.end())
            return false;
        for (auto& c : cit->second.calls) {
            if (c == callee)
                return true;
        }
        return false;
    }

    // ast:restore dep-graph upkeep (test hooks): every caller->callee edge,
    // sorted, plus each ir_cache_v2_ shell's source hash ("name#hash") so a
    // test can compare the incremental restore result with a full rebuild.
    [[nodiscard]] std::vector<std::string> public_dep_graph_fingerprint() {
        std::vector<std::string> out;
        {
            std::shared_lock dep_read(dep_graph_mtx_);
            for (const auto& [caller, entry] : dep_graph_)
                for (const auto& callee : entry.calls)
                    out.push_back(caller + "->" + callee);
        }
        {
            IrCacheGate ir_gate(ir_cache_gate_);
            for (const auto& [name, entry] : ir_cache_v2_)
                out.push_back(name + "#" + std::to_string(entry.source_hash));
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    [[nodiscard]] std::uint64_t public_restore_dep_graph_names_walked() const noexcept {
        return restore_dep_graph_names_walked_;
    }

    // Issue #1376: test hook — route through locked record_dependency.
    void public_record_dependency(const std::string& caller, const std::string& callee) {
        record_dependency(caller, callee);
    }

    // Issue #3486: test-only one-sided writes. Do not dual-write, rebuild,
    // or force-dirty — production peel must survive the fork. String graph
    // remains source of truth (rebuild_node_dep_graph_from_string).
    void inject_string_only_edge_for_test(const std::string& caller, const std::string& callee) {
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        auto& caller_entry = dep_graph_[caller];
        if (std::find(caller_entry.calls.begin(), caller_entry.calls.end(), callee) ==
            caller_entry.calls.end())
            caller_entry.calls.push_back(callee);
        auto& callee_entry = dep_graph_[callee];
        if (std::find(callee_entry.called_by.begin(), callee_entry.called_by.end(), caller) ==
            callee_entry.called_by.end())
            callee_entry.called_by.push_back(caller);
        (void)ensure_dep_fn_slot_(caller);
        (void)ensure_dep_fn_slot_(callee);
    }
    // Issue #3657: string called_by without occupying dep_name_to_slot_
    // (production graphs_consistent must return false; Soft continue).
    void inject_string_called_by_unslotted_for_test(const std::string& caller,
                                                    const std::string& callee) {
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        auto& caller_entry = dep_graph_[caller];
        if (std::find(caller_entry.calls.begin(), caller_entry.calls.end(), callee) ==
            caller_entry.calls.end())
            caller_entry.calls.push_back(callee);
        auto& callee_entry = dep_graph_[callee];
        if (std::find(callee_entry.called_by.begin(), callee_entry.called_by.end(), caller) ==
            callee_entry.called_by.end())
            callee_entry.called_by.push_back(caller);
    }
    void inject_node_only_edge_for_test(const std::string& caller, const std::string& callee) {
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        mirror_fn_dep_edge_unlocked_(caller, callee);
    }
    // Issue #3760: empty map + live irs + abort latch off — production
    // precompute used to return 0 (skip cone). Tests inject this residual.
    bool public_clear_source_to_ir_map_keep_irs_for_test(const std::string& name) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end() || it->second.irs.empty())
            return false;
        it->second.source_to_ir_map.clear();
        it->second.abort_map_invalid = false;
        return true;
    }
    [[nodiscard]] std::size_t public_precompute_callee_cascade_for_test(const std::string& name) {
        return precompute_callee_cascade_for_partial(name);
    }
    // Issue #3761: run the cone hole helper without a full peel so tests
    // can see node-only callers enter the peel set before remirror.
    bool public_fail_closed_cone_parity_for_test(std::vector<std::string>& cone_names,
                                                 bool& want_partial) {
        return fail_closed_soft_dual_graph_parity_before_partial_cone_(cone_names, want_partial);
    }
    // Issue #3762: register fname as belonging to `module` so unload_module
    // tears that define without unloading the caller's REPL module.
    void public_note_module_function_for_test(const std::string& module, const std::string& fname) {
        module_functions_[module].push_back(fname);
    }
    // Issue #3656: drop string calls/called_by but keep the NodeId fn
    // mirror — production precompute must not treat that as "no callee".
    void inject_drop_string_calls_keep_node_for_test(const std::string& caller,
                                                     const std::string& callee) {
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        auto cit = dep_graph_.find(caller);
        if (cit != dep_graph_.end()) {
            auto& v = cit->second.calls;
            v.erase(std::remove(v.begin(), v.end(), callee), v.end());
        }
        auto git = dep_graph_.find(callee);
        if (git != dep_graph_.end()) {
            auto& v = git->second.called_by;
            v.erase(std::remove(v.begin(), v.end(), caller), v.end());
        }
    }

    // Issue #3067: test hooks — inject stale reject, drain, fork NodeId, parity.
    void public_note_stale_dep_reject(const std::string& caller, const std::string& callee) {
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        deferred_hybrid_edges_.emplace_back(caller, callee);
        deferred_hybrid_armed_.store(1, std::memory_order_release);
        metrics_.dep_graph_edge_reject_stale_total.fetch_add(1, std::memory_order_relaxed);
    }
    void public_drain_deferred_hybrid_cascade() { drain_deferred_hybrid_cascade_(); }
    // Issue #3097: test hook for deferred_hybrid_pending_upper_bound_.
    std::size_t
    public_deferred_hybrid_pending_upper_bound_for_test(const std::string& name) const noexcept {
        return deferred_hybrid_pending_upper_bound_(name);
    }
    // Issue #3657: lockless reject must leave armed==1 so pending UB
    // consults the queued edge (armed==0 → helper returns 0).
    [[nodiscard]] std::uint32_t public_deferred_hybrid_armed_for_test() const noexcept {
        return deferred_hybrid_armed_.load(std::memory_order_acquire);
    }
    void public_drop_node_dep_mirror_edge(const std::string& caller, const std::string& callee) {
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        auto cit = dep_name_to_slot_.find(callee);
        auto ait = dep_name_to_slot_.find(caller);
        if (cit == dep_name_to_slot_.end() || ait == dep_name_to_slot_.end())
            return;
        const auto from = aura::compiler::dirty::encode_fn_node(cit->second);
        const auto to = aura::compiler::dirty::encode_fn_node(ait->second);
        auto it = node_dep_graph_.adj.find(from);
        if (it == node_dep_graph_.adj.end())
            return;
        auto& tos = it->second;
        tos.erase(std::remove(tos.begin(), tos.end(), to), tos.end());
    }
    void public_bump_dep_graph_generation() {
        dep_graph_generation_.fetch_add(1, std::memory_order_release);
        metrics_.dep_graph_generation_total.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool public_graphs_consistent() const noexcept {
        lock_order::OrderedSharedLock<std::shared_mutex> r(dep_graph_mtx_,
                                                           lock_order::Level::DepGraph);
        return aura::compiler::dirty::graphs_consistent(dep_graph_, node_dep_graph_,
                                                        dep_name_to_slot_);
    }
    [[nodiscard]] std::uint64_t public_hybrid_deferred_cascade_total() const noexcept {
        return metrics_.hybrid_deferred_cascade_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_dep_graph_edge_reject_stale_total() const noexcept {
        return metrics_.dep_graph_edge_reject_stale_total.load(std::memory_order_relaxed);
    }

    // Issue #2187: test hook — block-level DepGraph edge.
    void public_record_block_dependency(const std::string& caller, const std::string& callee,
                                        std::uint16_t caller_func_idx,
                                        std::uint16_t call_block_idx) {
        record_block_dependency(caller, callee, caller_func_idx, call_block_idx);
    }

    // Issue #1376: record path counters (for concurrent dedup assertions).
    [[nodiscard]] std::uint64_t public_dep_graph_record_total() const noexcept {
        return metrics_.dep_graph_record_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_dep_graph_record_dedup_total() const noexcept {
        return metrics_.dep_graph_record_dedup_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_dep_graph_record_inserted() const noexcept {
        return metrics_.dep_graph_record_inserted.load(std::memory_order_relaxed);
    }
    // Issue #2110: NodeId mirror observability (test + Agent).
    [[nodiscard]] std::uint64_t public_dep_graph_node_mirror_edges() const noexcept {
        return metrics_.dep_graph_node_mirror_edges_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_dep_graph_hybrid_cascade_hits() const noexcept {
        return metrics_.dep_graph_hybrid_cascade_hits.load(std::memory_order_relaxed);
    }
    // Issue #2187: block-level mirror / cascade observability.
    [[nodiscard]] std::uint64_t public_dep_graph_block_mirror_edges() const noexcept {
        return metrics_.dep_graph_block_mirror_edges_total.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t public_dep_graph_node_cascade_block_hits() const noexcept {
        return metrics_.dep_graph_node_cascade_block_hits.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t public_node_dep_graph_edge_count() const noexcept {
        lock_order::OrderedSharedLock<std::shared_mutex> r(dep_graph_mtx_,
                                                           lock_order::Level::DepGraph);
        return node_dep_graph_.edge_count();
    }
    [[nodiscard]] bool public_node_dep_has_mirror_edge(const std::string& caller,
                                                       const std::string& callee) const noexcept {
        lock_order::OrderedSharedLock<std::shared_mutex> r(dep_graph_mtx_,
                                                           lock_order::Level::DepGraph);
        auto cit = dep_name_to_slot_.find(callee);
        auto ait = dep_name_to_slot_.find(caller);
        if (cit == dep_name_to_slot_.end() || ait == dep_name_to_slot_.end())
            return false;
        const auto from = aura::compiler::dirty::encode_fn_node(cit->second);
        const auto to = aura::compiler::dirty::encode_fn_node(ait->second);
        const auto* deps = node_dep_graph_.dependents(from);
        if (!deps)
            return false;
        return std::find(deps->begin(), deps->end(), to) != deps->end();
    }
    // Issue #2187: true when NodeId graph has callee→caller-block edge.
    [[nodiscard]] bool public_node_dep_has_block_edge(const std::string& caller,
                                                      const std::string& callee,
                                                      std::uint16_t caller_func_idx,
                                                      std::uint16_t call_block_idx) const noexcept {
        lock_order::OrderedSharedLock<std::shared_mutex> r(dep_graph_mtx_,
                                                           lock_order::Level::DepGraph);
        auto cit = dep_name_to_slot_.find(callee);
        auto ait = dep_name_to_slot_.find(caller);
        if (cit == dep_name_to_slot_.end() || ait == dep_name_to_slot_.end())
            return false;
        const auto from = aura::compiler::dirty::encode_fn_node(cit->second);
        const auto to = aura::compiler::dirty::encode_block_dep_node(ait->second, caller_func_idx,
                                                                     call_block_idx);
        const auto* deps = node_dep_graph_.dependents(from);
        if (!deps)
            return false;
        return std::find(deps->begin(), deps->end(), to) != deps->end();
    }
    // Issue #2187: test — is block bi dirty on caller's body func?
    [[nodiscard]] bool public_is_block_dirty(const std::string& name, std::size_t func_idx,
                                             std::size_t block_idx) const noexcept {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        const auto& e = it->second;
        if (func_idx >= e.block_dirty_per_func_.size())
            return false;
        const auto& fb = e.block_dirty_per_func_[func_idx];
        if (block_idx >= fb.size())
            return false;
        return fb[block_idx] != 0;
    }

    // Issue #272: test/observability accessor.
    [[nodiscard]] std::uint64_t define_ir_env_bind_count() const noexcept {
        return metrics_.define_ir_env_bind_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t value_define_ir_env_bind_count() const noexcept {
        return metrics_.value_define_ir_env_bind_count.load(std::memory_order_relaxed);
    }

    // Issue #4228: Soft oneshot prelude calls this after require so
    // Soft TW Lambda cells (std/math preds) win TopCellLoad over IR
    // function-cache MakeClosure high-bit stubs.
    void sync_soft_export_cells_for_ir() { sync_soft_tw_export_cells_into_ir_value_bindings_(); }

    // Issue #272 Cycle 2: test hook for needs_tree_walker_fallback on defines.
    [[nodiscard]] bool public_needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                                         const aura::ast::StringPool& pool,
                                                         aura::ast::NodeId root) const {
        return needs_tree_walker_fallback(flat, pool, root);
    }

    // Issue #2213: test/Agent hook — consult needs + production policy.
    // disposition is the gate result; metrics always bump on needs=true.
    [[nodiscard]] TreeWalkerFallbackDisposition
    public_consult_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                        const aura::ast::StringPool& pool, aura::ast::NodeId root) {
        return consult_tree_walker_fallback_gate_(flat, pool, root);
    }

    // Issue #282: public accessors for the Occurrence Typing
    // provenance log. The Aura primitive (query :provenance-of)
    // uses these under the hood; tests can also use them
    // directly to verify the provenance-tracking contract.
    [[nodiscard]] std::size_t narrowing_count() const noexcept {
        if (!evaluator_.workspace_flat())
            return 0;
        return evaluator_.workspace_flat()->narrowing_count();
    }
    [[nodiscard]] const std::pmr::vector<aura::ast::NarrowingRecord>& all_narrowings() const {
        // The vector is always non-null when workspace_flat() is,
        // but C++'s reference return requires a static.
        static const std::pmr::vector<aura::ast::NarrowingRecord> empty_log;
        if (!evaluator_.workspace_flat())
            return empty_log;
        return evaluator_.workspace_flat()->all_narrowings();
    }

    // Track names defined via value define (tree-walker path) so subsequent
    // expressions referencing them fall back to tree-walker instead of IR.
    std::unordered_set<std::string> user_bindings_;

    // Strict mode: type errors → rejected instead of warnings only
    bool strict_mode_ = false;
    // Issue #283 follow-up #5: bidirectional Occurrence Typing
    // narrowing in check_flat. Default true (matches pre-#283
    // behavior post-#283). Set false to disable the narrowing
    // application in check_flat's If branch (falls back to the
    // original uniform check).
    bool bidirectional_mode_ = true;
    // Issue #2992: default Balanced (env AURA_GRADUAL_PERMISSIVENESS).
    GradualPermissiveness gradual_permissiveness_ = gradual_permissiveness_from_env();

    // Persistent JIT for --jit mode
    aura::jit::AuraJIT jit_;
    bool jit_initialized_ = false;

    // JIT function cache — maps function name → compiled function pointer + metadata
    struct JitCachedFn {
        // Issue #59 Iter 4: atomic function pointer. A fiber that
        // observed fn_ptr before a mutate is allowed to keep using
        // it; a fiber that observes it after the mutate sees nullptr
        // and falls through to recompile. The erase path stores
        // nullptr first, then optionally frees the original.
        std::atomic<aura::jit::ScalarFn> fn_ptr{nullptr};
        // Issue #166: epoch snapshot at compile time. Same as
        // IRCacheEntry::last_seen_epoch_ — used by the global
        // epoch check (mutation_epoch_) to detect stale JIT code
        // that wasn't explicitly invalidated by name (e.g., a
        // mutation in a different function that transitively
        // affects this one's compiled native code).
        std::uint64_t last_seen_epoch_ = 0;
        std::uint32_t local_count = 0;
        std::uint32_t arg_count = 0;
        std::uint32_t env_count = 0;
        bool has_shape_map = false; // true if compiled with shape_map
        // Issue #4567: generic body kept. A function-level dominant
        // shape is not a guarded per-arg map, so do not hot-recompile.
        bool spec_declined = false;
        // Issue #605: ShapeProfiler version at compile time.
        // Compared on cache hit; mismatch → re-compile + jit_shape_miss.
        std::uint64_t compiled_shape_version_ = 0;
        // Issue #4569: assumptions the native code was compiled under —
        // IR content fingerprint (0 = none recorded, never a hit in
        // assumption mode) and the DefineVersion words it depends on.
        std::uint64_t fingerprint_ = 0;
        std::vector<aura::compiler::jit_assume::Dep> deps_;
    };
    std::unordered_map<std::string, JitCachedFn, aura::core::TransparentStringHash, std::equal_to<>>
        jit_cache_;

    // Issue #4600 §8 (phase 4): session lowering memo. A re-run of the
    // identical program text whose own define names have not moved reuses
    // the post-pipeline IRModule (+ escape maps) and skips parse,
    // typecheck, lowering and the production pipeline. The per-function
    // jit_cache_ probe below still runs, so the #4569 assumption predicate
    // (IR fingerprint + DefineVersion deps) is unchanged: this memo only
    // removes re-lowering of an unchanged define. One entry — the last
    // program — which is the #4569/#4600 §8 session-bench shape. A dep
    // that moved (rebind / set-code cascade), a different text, or a
    // different strict mode is a memo miss and takes the full path.
    // AURA_JIT_REUSE_LOWERING=0 restores the old re-lower-every-run path.
    struct JitProgramMemo {
        std::uint64_t input_hash = 0;
        std::size_t input_size = 0;
        bool strict = false;
        aura::ir::IRModule mod;
        std::vector<std::vector<std::uint8_t>> escape_maps;
        std::vector<aura::compiler::jit_assume::Dep> deps;
    };
    std::unique_ptr<JitProgramMemo> jit_program_memo_;

    static bool jit_reuse_lowering_enabled() {
        static const bool on = [] {
            const char* v = ::aura::config::raw(::aura::config::Key::AURA_JIT_REUSE_LOWERING);
            return !(v && v[0] == '0');
        }();
        return on;
    }
    static std::uint64_t jit_program_hash_(std::string_view s) {
        aura::compiler::jit_assume::Fnv h;
        h.u64(s.size());
        h.bytes(s.data(), s.size());
        return h.done();
    }
    // Own define names + captured free variables of every function — the
    // same name set jit_assume_key_ records as DefineVersion deps.
    static std::vector<std::string> jit_program_dep_names_(const aura::ir::IRModule& mod) {
        std::vector<std::string> names;
        std::unordered_set<std::string> seen;
        for (const auto& f : mod.functions) {
            std::string_view own = f.name;
            if (auto hash = own.find('#'); hash != std::string_view::npos)
                own = own.substr(0, hash);
            if (!own.empty() && own != "__lambda__" && own != "__top__" &&
                own != "__self_placeholder__" && seen.insert(std::string(own)).second)
                names.emplace_back(own);
            for (const auto& fv : f.free_vars)
                if (!fv.empty() && seen.insert(fv).second)
                    names.push_back(fv);
        }
        return names;
    }
    static std::vector<aura::compiler::jit_assume::Dep>
    jit_program_deps_(const std::vector<std::string>& names) {
        std::vector<aura::compiler::jit_assume::Dep> deps;
        deps.reserve(names.size());
        for (const auto& n : names) {
            auto* w = aura::compiler::jit_assume::DefineVersions::instance().word(n);
            deps.push_back(
                aura::compiler::jit_assume::Dep{w, w->v.load(std::memory_order_acquire), n});
        }
        return deps;
    }

    // Issue #4569: per-function assumption key, computed from the IR the
    // cache is probed with. fp covers everything FlatFnBuilder hands the
    // JIT (opcode, operands, type/shape/narrow/linear/marker/provenance
    // stamps, ConstString text, escape map, dirty bits, counts); names are
    // the DefineVersion dependencies (own define name + captured free vars).
    struct JitAssumeKey {
        std::uint64_t fp = 0;
        std::vector<std::string> names;
    };
    [[nodiscard]] static JitAssumeKey jit_assume_key_(const aura::ir::IRFunction& f,
                                                      const std::vector<std::string>& pool,
                                                      const std::vector<std::uint8_t>* dirty,
                                                      const std::uint8_t* esc, std::size_t esc_n) {
        aura::compiler::jit_assume::Fnv h;
        h.u64(f.arg_count);
        h.u64(f.local_count);
        h.u64(f.variadic ? 1 : 0);
        h.u64(static_cast<std::uint64_t>(f.region));
        h.u64(f.marker);
        h.u64(f.entry_block);
        h.u64(f.free_vars.size());
        for (const auto& fv : f.free_vars)
            h.str(fv);
        h.u64(f.blocks.size());
        for (const auto& blk : f.blocks) {
            h.u64(blk.instructions.size());
            for (const auto& in : blk.instructions) {
                h.u64(static_cast<std::uint64_t>(in.opcode));
                for (auto op : in.operands)
                    h.u64(op);
                // type_id is a registry index that changes on every
                // typecheck (fresh type vars); the JIT only tests
                // type_id != 0 (aura_jit.cpp L2 pair stamp). Hash what the
                // JIT reads — update this if lowering starts using the id.
                h.u64(in.type_id != 0 ? 1 : 0);
                h.u64(in.shape_id);
                h.u64(in.narrow_evidence);
                h.u64(in.linear_ownership_state);
                h.u64(in.adt_variant_id);
                h.u64(in.source_marker);
                h.u64(in.provenance);
                if (in.opcode == aura::ir::IROpcode::ConstString) {
                    const auto idx = in.operands[1];
                    h.str(idx < pool.size() ? std::string_view(pool[idx]) : std::string_view{});
                }
            }
        }
        if (dirty) {
            h.u64(dirty->size());
            h.bytes(dirty->data(), dirty->size());
        }
        h.u64(esc_n);
        if (esc && esc_n)
            h.bytes(esc, esc_n);
        JitAssumeKey k;
        k.fp = h.done();
        // Own define name: "f" or "f#3" -> "f"; synthetic lambdas carry
        // no define name of their own.
        std::string_view own = f.name;
        if (auto hash = own.find('#'); hash != std::string_view::npos)
            own = own.substr(0, hash);
        if (!own.empty() && own != "__lambda__" && own != "__top__" &&
            own != "__self_placeholder__")
            k.names.emplace_back(own);
        for (const auto& fv : f.free_vars)
            if (!fv.empty())
                k.names.push_back(fv);
        return k;
    }
    // Snapshot the dependency versions. clock0 is the define clock read
    // before the IR was lowered: if any DefineVersion moved since, this
    // IR may predate it, so publish fail-closed (version 0 never matches).
    [[nodiscard]] static std::vector<aura::compiler::jit_assume::Dep>
    jit_assume_snapshot_(const JitAssumeKey& k, std::uint64_t clock0) {
        namespace ja = aura::compiler::jit_assume;
        std::vector<ja::Dep> deps;
        deps.reserve(k.names.size());
        for (const auto& n : k.names) {
            auto* w = ja::DefineVersions::instance().word(n);
            deps.push_back(ja::Dep{w, w->v.load(std::memory_order_acquire), n});
        }
        if (ja::define_clock().load(std::memory_order_acquire) != clock0) {
            ja::stats().torn_publishes.fetch_add(1, std::memory_order_relaxed);
            for (auto& d : deps)
                d.version = 0;
            if (deps.empty())
                deps.push_back(
                    ja::Dep{ja::DefineVersions::instance().word("__torn__"), 0, "__torn__"});
        }
        return deps;
    }
    // Staleness predicate shared by the exec_jit and try_jit_execute probes
    // (replaces the bare global-epoch compare, #166 / #4073). record=true on
    // the unique-lock re-probe only, so one stale entry logs one event.
    [[nodiscard]] static bool jit_entry_stale_(const JitCachedFn& e, const JitAssumeKey& k,
                                               std::string_view fn, bool record) {
        namespace ja = aura::compiler::jit_assume;
        if (ja::global_epoch_mode()) {
            const bool stale = e.last_seen_epoch_ != aura::core::current_mutation_epoch();
            if (stale && record) {
                ja::stats().epoch_misses.fetch_add(1, std::memory_order_relaxed);
                ja::EventLog::instance().record(ja::Event{
                    std::string(fn), std::string(), ja::Kind::GlobalEpoch, e.last_seen_epoch_,
                    aura::core::current_mutation_epoch(), aura::core::current_mutation_epoch()});
            }
            return stale;
        }
        if (e.fingerprint_ == 0 || e.fingerprint_ != k.fp) {
            if (record) {
                ja::stats().fingerprint_misses.fetch_add(1, std::memory_order_relaxed);
                ja::EventLog::instance().record(
                    ja::Event{std::string(fn), std::string(), ja::Kind::Fingerprint, e.fingerprint_,
                              k.fp, aura::core::current_mutation_epoch()});
            }
            return true;
        }
        if (const auto* d = ja::first_broken(e.deps_)) {
            if (record) {
                ja::stats().define_invalidations.fetch_add(1, std::memory_order_relaxed);
                ja::EventLog::instance().record(
                    ja::Event{std::string(fn), d->name, ja::Kind::Define, d->version,
                              d->word ? d->word->v.load(std::memory_order_acquire) : 0,
                              aura::core::current_mutation_epoch()});
            }
            return true;
        }
        return false;
    }
    // Issue #3758: light-link get_function_ptr is null. Test override lets
    // try_jit_execute reach ScalarFn so persist-reject can prove no invoke.
    aura::jit::ScalarFn try_jit_scalar_override_for_test_ = nullptr;
    inline static std::atomic<std::uint32_t> try_jit_scalar_invokes_for_test_{0};
    static int64_t counting_scalar_fn_for_test_(int64_t*, uint32_t) {
        try_jit_scalar_invokes_for_test_.fetch_add(1, std::memory_order_relaxed);
        return 42;
    }
    // Issue #4388: distinct pointers so a refused republish is observable.
    static int64_t ac4388_sentinel_a_(int64_t*, uint32_t) { return 1; }
    static int64_t ac4388_sentinel_b_(int64_t*, uint32_t) { return 2; }
    int try_jit_facade_skip_mode_for_test_ = 0;
    int try_jit_entry_hit_refused_for_test_ = 0;
    // Issue #59 Iter 2: shared_mutex for jit_cache_. Read-heavy access
    // pattern (most lookups just probe the cache), so multiple readers
    // can hold the shared lock concurrently. Writers take the unique
    // lock for `erase` and `[]` insert. Held briefly per call (a
    // map lookup/insert is sub-microsecond), so contention is
    // negligible in the common case.
    mutable std::shared_mutex jit_cache_mtx_;

    // Issue #166 Phase 1: global mutation epoch. Incremented
    // atomically on every mutation (in invalidate_function /
    // typed_mutate). Checked by every cache access (IR + JIT)
    // to detect stale entries that weren't explicitly invalidated
    // by name. Single process-wide counter, monotonic.
    //
    // Pattern (Issue #2039 / #1964 cycle 2d):
    //   - mutation: aura::core::bump_mutation_epoch() (process-global)
    //   - ir_cache lookup: if entry.last_seen_epoch_ != current,
    //                       treat as stale (re-lower)
    //   - jit_cache lookup: if entry.last_seen_epoch_ != current,
    //                        treat as stale (re-compile)
    //   - apply_closure: load current via bridge_epoch() acquire
    // Legacy field mutation_epoch_ deleted — sole storage is
    // WorkspaceEpoch::Mutation (src/core/workspace_epoch.hh).

    // Issue #1414: solved_delta_cache_ — engine-level cache for
    // ConstraintSystem::solve_delta results. Keyed by
    // (dirty_set_hash, vars_hash, cache_epoch). On cache hit,
    // skips the worklist scan and returns the cached SolveResult.
    // Cache lives on CompilerService (persistent across infer
    // calls — each infer call creates a fresh local TypeChecker,
    // so per-CS caching wouldn't persist). Invalidated by
    // on_typed_mutation_epoch_bump() called from
    // invalidate_function after bump_bridge_epoch().
    //
    // Target: ≥80% hit rate across 100 incremental mutations
    // on the same subtree (the AI evolve! pattern). The cache
    // helps when solve_delta is called multiple times within
    // a mutation's processing with identical dirty set + vars
    // state (e.g., re-verify of clean constraints, nested
    // partial inference, multi-stage delta work).
    struct CachedSolve {
        SolveResult result;
        std::uint64_t vars_hash;
        std::uint64_t cache_epoch;
    };
    std::unordered_map<std::uint64_t, CachedSolve> solved_delta_cache_;
    std::uint64_t solve_delta_cache_epoch_ = 0;
    std::atomic<std::uint64_t> solve_delta_cache_hits_{0};
    std::atomic<std::uint64_t> solve_delta_cache_misses_{0};
    std::atomic<std::uint64_t> solve_delta_cache_invalidations_{0};

    // Issue #223 / #296: bridge_epoch() returns the current
    // epoch for ClosureBridgeData lifetime tracking. The bridge
    // callback (IRExecutor::MakeClosure) and apply_closure
    // compare the bridge's captured epoch against this; a
    // mismatch means the bridge's flat*/pool* are stale (arena
    // was reset or a major mutation happened). The bridge
    // falls back to re-parse from body_source or invalidates
    // the closure.
    //
    // Issue #223 / #2039: process-global Mutation epoch (WorkspaceEpoch).
    // Cycle-1 lockstep: Bridge kind + C runtime mirror Mutation so
    // apply_closure / aura_closure_call share one published value.
    //
    // INVARIANT (Epoch Invariant): Every closure created through the
    // bridge must capture the current epoch at construction time
    // (via bridge_epoch()). Every subsequent apply_closure call must
    // check staleness via Evaluator::is_bridge_stale().
    [[nodiscard]] std::uint64_t bridge_epoch() const noexcept {
        return aura::core::current_mutation_epoch();
    }
    // Issue #223 / #2039: bump Mutation + Bridge kinds in lockstep and
    // dual-write C runtime g_current_bridge_epoch.
    //
    // INVARIANT: bump_bridge_epoch() and any cache invalidation must
    // happen as a paired operation — bumping without invalidating
    // leaves stale entries visible.
    void bump_bridge_epoch() noexcept {
        aura::core::bump_mutation_and_bridge_epochs();
        // Issue #1476: observability counter for every epoch advance.
        metrics_.bridge_epoch_bumps_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1485 / #2039: C runtime dual-write (WorkspaceEpoch is
        // canonical; C atom mirrors for lib/runtime.c).
        aura_set_current_bridge_epoch(aura::core::current_mutation_epoch());
    }

    // Issue #1414: invalidate the solved_delta_cache_ and
    // bump solve_delta_cache_epoch_. Called from
    // invalidate_function after bump_bridge_epoch() (the
    // pairing is intentional — epoch advance and cache wipe
    // are atomic w.r.t. the next solve_delta_cached call).
    // Also bumped from this method's own observability counter
    // so tests can verify invalidation is firing.
    void on_typed_mutation_epoch_bump() noexcept {
        solved_delta_cache_.clear();
        ++solve_delta_cache_epoch_;
        solve_delta_cache_invalidations_.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #1414: observability accessors for the
    // solved_delta_cache_. Exposed via metrics dump / test
    // harness. Read directly from the atomic counters so
    // concurrent solve_delta_cached callers see consistent
    // values without external locking.
    [[nodiscard]] std::uint64_t solve_delta_cache_hits() const noexcept {
        return solve_delta_cache_hits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t solve_delta_cache_misses() const noexcept {
        return solve_delta_cache_misses_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t solve_delta_cache_invalidations() const noexcept {
        return solve_delta_cache_invalidations_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t solve_delta_cache_size() const noexcept {
        return solved_delta_cache_.size();
    }
    [[nodiscard]] std::uint64_t solve_delta_cache_epoch() const noexcept {
        return solve_delta_cache_epoch_;
    }

    // Issue #1414: cached wrapper around
    // ConstraintSystem::solve_delta. Looks up the result in
    // solved_delta_cache_ keyed by
    // (dirty_set_hash, vars_hash, cache_epoch). On hit,
    // skips the worklist scan and returns the cached
    // SolveResult. On miss, calls cs.solve_delta_impl and
    // caches the result.
    //
    // Same trade-off as the existing cache_epoch_ gate on
    // TypeChecker: cache invalidates on
    // on_typed_mutation_epoch_bump() (which is called from
    // invalidate_function after bump_bridge_epoch()). The
    // pairing of bump + cache wipe + epoch advance is atomic
    // w.r.t. any subsequent solve_delta_cached call.
    //
    // The unresolved_out parameter is cleared on cache hit
    // (we don't cache the unresolved list — only the result).
    // If the caller needs the unresolved list, they should
    // use cs.solve_delta() directly on a cache miss.
    SolveResult
    solve_delta_cached(aura::compiler::TypeChecker& tc,
                       std::vector<aura::compiler::Constraint>* unresolved_out = nullptr) {
        auto& cs = tc.constraint_system();
        if (!cs.is_dirty()) {
            if (unresolved_out)
                unresolved_out->clear();
            return SolveResult::SOLVED;
        }
        const auto dirty_set_hash = cs.compute_dirty_set_hash();
        const auto vars_hash = cs.compute_vars_hash();
        const auto epoch = solve_delta_cache_epoch_;

        auto it = solved_delta_cache_.find(dirty_set_hash);
        if (it != solved_delta_cache_.end() && it->second.vars_hash == vars_hash &&
            it->second.cache_epoch == epoch) {
            solve_delta_cache_hits_.fetch_add(1, std::memory_order_relaxed);
            // Issue #1528: mirror into CompilerMetrics for unified
            // O(delta) dashboards (alongside cs_cache hits).
            metrics_.solve_delta_cache_hit_total.fetch_add(1, std::memory_order_relaxed);
            if (unresolved_out)
                unresolved_out->clear();
            return it->second.result;
        }

        solve_delta_cache_misses_.fetch_add(1, std::memory_order_relaxed);
        auto result = cs.solve_delta(unresolved_out);
        solved_delta_cache_[dirty_set_hash] = CachedSolve{result, vars_hash, epoch};
        return result;
    }

    // Issue #225 cycle 3: invalidate the bridge data for a
    // function. Bumps the bridge_epoch_ field on all bridge
    // entries in the entry's ir_cache_bridge_, so any closure
    // holding a reference will detect the staleness and fall
    // back to re-parse from body_source (or be invalidated).
    //
    // This is the "active" version of the safety check (when
    // something is invalidated, take action) vs the "passive"
    // version of Cycles 1+2 (just check the epoch + use
    // shared_ptr). All 3 compose: the invalidation is the
    // trigger, the epoch check is the detector, the
    // shared_ptr is the safety net.
    // Issue #682: pin compiler-managed IRClosure / EnvId roots for GC.
    // Called from Evaluator::flush_gc_roots at safepoint (via hook).
    void flush_compiler_gc_roots(void* root_set_out) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        auto& out = *static_cast<aura::serve::GCRootSet*>(root_set_out);
        const auto current_epoch = bridge_epoch();
        std::unordered_set<std::int64_t> seen_cl;
        std::unordered_set<std::int64_t> seen_env;
        auto add_closure = [&](std::int64_t id) {
            if (id <= 0 || !seen_cl.insert(id).second)
                return;
            out.compiler_closure_roots.push_back(id);
        };
        auto add_env = [&](std::int64_t id) {
            if (id <= 0 || !seen_env.insert(id).second)
                return;
            out.compiler_env_roots.push_back(id);
        };

        for (const auto& [name, bridges] : ir_cache_bridge_) {
            bool has_live_bridge = false;
            for (const auto& bridge : bridges) {
                if (bridge.bridge_epoch != 0 && bridge.bridge_epoch != current_epoch)
                    continue;
                if (!bridge.flat && !bridge.pool && bridge.body_source.empty())
                    continue;
                has_live_bridge = true;
                break;
            }
            if (!has_live_bridge)
                continue;
            auto bit = ir_define_env_bindings_.find(name);
            if (bit == ir_define_env_bindings_.end() || !bit->second)
                continue;
            add_closure(static_cast<std::int64_t>(bit->second->closure_id));
            if (bit->second->interpreter) {
                ExecUnderGate4765 exec(bit->second->exec_mtx); // #4765
                bit->second->interpreter->collect_active_gc_roots(out.compiler_closure_roots,
                                                                  current_epoch);
            }
        }

        for (auto& [name, binding] : ir_define_env_bindings_) {
            (void)name;
            if (!binding || !binding->interpreter)
                continue;
            add_closure(static_cast<std::int64_t>(binding->closure_id));
            ExecUnderGate4765 exec(binding->exec_mtx); // #4765
            binding->interpreter->collect_active_gc_roots(out.compiler_closure_roots,
                                                          current_epoch);
        }
        // Issue #4815: retained value-define interpreters own live closures.
        for (auto& [name, binding] : ir_value_define_closure_owners_) {
            (void)name;
            if (!binding || !binding->interpreter)
                continue;
            ExecUnderGate4765 exec(binding->exec_mtx); // #4765
            binding->interpreter->collect_active_gc_roots(out.compiler_closure_roots,
                                                          current_epoch);
        }

        std::vector<std::int64_t> tw_cl;
        std::vector<std::int64_t> tw_env;
        evaluator_.collect_compiler_managed_gc_roots(tw_cl, tw_env, current_epoch);
        for (auto id : tw_cl)
            add_closure(id);
        for (auto id : tw_env)
            add_env(id);

        std::unordered_set<std::int64_t> dedup;
        dedup.insert(out.compiler_closure_roots.begin(), out.compiler_closure_roots.end());
        out.compiler_closure_roots.assign(dedup.begin(), dedup.end());

        metrics_.ir_closure_roots_registered.store(out.compiler_closure_roots.size(),
                                                   std::memory_order_relaxed);
    }

    // Issue #682: invalidate / hot-swap GC coordination — detect
    // missing bridge roots and defer when IR frames are live.
    void on_compiler_invalidate_gc_coordination(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        const bool has_bridge = ir_cache_bridge_.count(name) > 0;
        const bool has_binding = ir_define_env_bindings_.count(name) > 0;
        if (!has_bridge && !has_binding)
            metrics_.hotswap_root_miss.fetch_add(1, std::memory_order_relaxed);

        for (auto& [bind_name, binding] : ir_define_env_bindings_) {
            if (bind_name != name || !binding || !binding->interpreter)
                continue;
            bool active = false;
            {
                ExecUnderGate4765 exec(binding->exec_mtx); // #4765
                active = binding->interpreter->has_active_frames();
            }
            if (active) {
                metrics_.compiler_gc_safepoint_defer_count.fetch_add(1, std::memory_order_relaxed);
                (void)evaluator_.request_gc_safepoint();
            }
        }
    }

    // Issue #1523 lock order for invalidate_bridge_for:
    //   NO mutex. Safe under mutate_mtx_ held by invalidate_function /
    //   mark_define_dirty. Public entry points must acquire mutate first
    //   (see public_invalidate_bridges_for / atomic_bump_epochs_and_stamp_bridge).
    void invalidate_bridge_for(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        std::vector<aura::ir::ClosureBridgeData>* bridges = nullptr;
        if (auto bit = ir_cache_bridge_.find(name); bit != ir_cache_bridge_.end())
            bridges = &bit->second;
        else if (auto vit = ir_cache_v2_.find(name);
                 vit != ir_cache_v2_.end() && !vit->second.bridges.empty())
            bridges = &vit->second.bridges;
        // Issue #1522: always notify JIT fn_trackers_ even if no bridge table
        // entry (direct-call path still holds native code). Soft batch_deopt
        // marks deopt_pending; hard invalidate still used by invalidate_function.
        const auto current_epoch = bridge_epoch();
        notify_jit_fn_trackers_batch_deopt_(name, current_epoch);
        if (!bridges || bridges->empty())
            return;
        for (auto& bridge : *bridges) {
            bridge.bridge_epoch = current_epoch;
            // Issue #681: expire captured views so live IRClosure /
            // bridge callbacks cannot retain stale flat/pool post-
            // invalidate. body_source remains for safe re-parse fallback.
            bridge.flat.reset();
            bridge.pool.reset();
            bridge.body_id = aura::ast::NULL_NODE;
        }
        metrics_.bridge_invalidations_count.fetch_add(1, std::memory_order_relaxed);
        metrics_.compiler_inval_bridge_epoch_total.fetch_add(
            static_cast<std::uint64_t>(bridges->size()), std::memory_order_relaxed);
    }

    // Issue #1522 / #1476 / #1524 / #1496: SINGLE AUTHORITATIVE dual-epoch
    // + bridge stamp + JIT soft-deopt protocol.
    //
    // Called by BOTH soft path (mark_define_dirty) and hard path
    // (invalidate_function) so writers cannot advance one domain without
    // the other. Forbidden patterns:
    //   - bare bump_bridge_epoch() without stamp / AOT table / batch_deopt
    //   - bare invalidate_bridge_for() without a preceding dual-epoch bump
    //     (except cascade dependents after this helper already bumped)
    //   - hand-rolled dual bump inside invalidate_function (pre-#1496)
    //
    // Dual domains advanced with release ordering:
    //   1. bridge / mutation_epoch_  (is_bridge_stale)
    //   2. defuse_version_           (is_env_frame_stale / #1475)
    //   3. AOT func table epoch      (aura_closure_call dual check)
    // Then: solve_delta wipe, stamp bridges, soft-deopt JIT fn_trackers_,
    // walk_active_closures for live-closure stale prevention.
    //
    // name empty → stamp every known bridge / ir_cache_v2 entry (typed_mutate
    // catch-all when the affected define set is unknown).
    //
    // Issue #1627 / #2131: shared pre-cascade for soft mark_define_dirty and
    // hard invalidate_function — live closures + linear enforce + GC
    // root pin/audit before publishing new epochs (GcCoord PrePin phase).
    // Caller must hold an active gc_coord::Scope (opened at entry).
    // Issue #4678: a restamp batch (drain_cascade_bfs_invalidate marking
    // every dependent of a hard-invalidated root) ran the full pre-cascade
    // audit and the global epoch bump + closure walk once per dependent.
    // Inside a batch the global halves run once (first name); per-name
    // work (gc coordination, feedback reset, bridge invalidate) still runs
    // for every name.
    std::atomic<int> dirty_batch_depth_ = 0;
    std::atomic<bool> dirty_batch_pre_done_ = false;
    // Boundary-exit prep coalescing (see set_boundary_prep_batch_fn).
    // Issue #4717: the boundary-prep bracket is armed/disarmed from ANY
    // body's mutate primitive (the set_boundary_prep_batch_fn hook), so the
    // depth and the once-per-bracket flags are shared atomics, not ints/bools.
    std::atomic<int> boundary_prep_depth_{0};
    std::atomic<bool> boundary_prep_done_{false};
    std::atomic<bool> boundary_finalize_done_{false};
    std::atomic<bool> boundary_rate_sampled_{false};
    // Dedupe the name-independent cascade tail (linear/GC window finalize,
    // workspace dirty-rate sample) across one boundary exit.
    [[nodiscard]] bool boundary_rate_sample_skip_() noexcept {
        if (boundary_prep_depth_.load(std::memory_order_acquire) <= 0)
            return false;
        if (boundary_rate_sampled_.load(std::memory_order_acquire))
            return true;
        boundary_rate_sampled_.store(true, std::memory_order_release);
        return false;
    }
    std::atomic<bool> dirty_batch_epoch_done_ = false;
    std::atomic<bool> dirty_batch_finalize_done_ = false;
    std::atomic<bool> dirty_batch_linear_scan_done_ = false;    // #4680
    std::atomic<bool> dirty_batch_hotswap_audit_done_ = false;  // #4680
    std::atomic<bool> dirty_batch_rate_sampled_ = false;        // #4681
    std::atomic<std::uint64_t> dirty_batch_walk_epoch_ = ~0ull; // #4680
    std::uint64_t dirty_batch_coalesced_4678_ = 0;
    void set_dirty_batch(bool begin) noexcept {
        if (begin) {
            if (dirty_batch_depth_.fetch_add(1, std::memory_order_acq_rel) == 0) {
                dirty_batch_pre_done_ = false;
                dirty_batch_epoch_done_ = false;
                dirty_batch_finalize_done_ = false;
                dirty_batch_linear_scan_done_ = false;
                dirty_batch_hotswap_audit_done_ = false;
                dirty_batch_rate_sampled_ = false;
                dirty_batch_walk_epoch_ = ~0ull;
            }
        } else if (dirty_batch_depth_.load(std::memory_order_acquire) > 0 &&
                   dirty_batch_depth_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            dirty_batch_pre_done_ = false;
            dirty_batch_epoch_done_ = false;
            dirty_batch_finalize_done_ = false;
            dirty_batch_linear_scan_done_ = false;
        }
    }
    [[nodiscard]] std::uint64_t dirty_batch_coalesced_4678() const noexcept {
        return dirty_batch_coalesced_4678_;
    }
    void prepare_unified_invalidation_pre_cascade_(const std::string& name) {
        aura::compiler::phase_stats::Scope phase_pre_audit(aura::compiler::phase_stats::PreAudit);
        if (dirty_batch_depth_ > 0) {
            if (dirty_batch_pre_done_) {
                on_compiler_invalidate_gc_coordination(name);
                ++dirty_batch_coalesced_4678_;
                return;
            }
            dirty_batch_pre_done_ = true;
        }
        if (boundary_prep_depth_ > 0) {
            if (boundary_prep_done_) {
                on_compiler_invalidate_gc_coordination(name);
                ++dirty_batch_coalesced_4678_;
                return;
            }
            boundary_prep_done_ = true;
        }
        // Issue #2131: PrePin work under active Scope (opened by soft/hard entry).
        (void)evaluator_.scan_live_closures_for_linear_captures(/*mark_invalid=*/true);
        (void)evaluator_.linear_post_mutate_enforce_all();
        on_compiler_invalidate_gc_coordination(name);
        (void)evaluator_.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditInvalidate);
        metrics_.invalidate_pre_cascade_prepare_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #1523 lock order: mutate_mtx_ unique FIRST (if not held),
    // then epoch + bridge stamp (no dep_graph / workspace).
    void atomic_bump_epochs_and_stamp_bridge(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        // Issue #4568: one-line Soft rebind returns before invalidate_shape.
        // Reset this name only. An empty catch-all must not wipe unrelated
        // defines (their feedback survives).
        if (!name.empty())
            aura::compiler::feedback::reset_name(name);
        // Issue #4678: second+ name of a restamp batch - the epoch bump,
        // closure walk and expire already ran for this batch.
        if (!name.empty() && dirty_batch_depth_ > 0) {
            if (dirty_batch_epoch_done_) {
                metrics_.unified_invalidation_protocol_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                invalidate_bridge_for(name);
                ++dirty_batch_coalesced_4678_;
                return;
            }
            dirty_batch_epoch_done_ = true;
        }
        using aura::compiler::lock_order::Level;
        using aura::compiler::lock_order::OrderedUniqueLock;
        // #4322: skip Mutate when Workspace is already held.
        OrderedUniqueLock<std::shared_mutex> mutate_guard;
        if (!lock_order::any_higher_held(Level::Mutate)) {
            mutate_guard =
                OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mutate_mtx_, Level::Mutate);
        }
        sync_lock_order_metrics_();
        metrics_.unified_invalidation_protocol_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1496: release fence before publishing new epochs so
        // concurrent apply_closure readers that acquire-load either
        // domain cannot observe half-updated bridge tables.
        aura::util::thread_fence(std::memory_order_release);
        // bridge_epoch FIRST (release ordering on mutation_epoch_).
        bump_bridge_epoch();
        // Issue #1476: defuse_version_ in lockstep (acq_rel) for #1475 readers.
        // Issue #4109: bare bump re-definitions nothing — ride captures.
        // Issue #4730: bump + ride under one shard barrier.
        (void)evaluator_.bump_defuse_and_ride_frames();
        metrics_.dep_graph_defuse_version_bumps.fetch_add(1, std::memory_order_relaxed);
        // Issue #2841 / #2744 / #2951: stamp current Evaluator as
        // reemit/register owner before the AOT table epoch bump so
        // multi-eval production cascade can prefer owner-scoped
        // stale-slot invalidate (no peer force-stale of g_aot_table_epoch).
        // Soft mark_define_dirty relies on this TLS alone; hard
        // invalidate_function under #2951 production multi-eval also
        // prefers owner-scoped (notes hard_owner_scoped, not force-bump);
        // Soft / env opt-out still force-bumps. Single-eval: map size ≤1
        // short-circuits throttle (zero extra cost beyond owner TLS).
        struct CascadeEvalOwnerGuard {
            void* prev_reemit;
            void* prev_reg;
            explicit CascadeEvalOwnerGuard(void* e) noexcept
                : prev_reemit(aura_aot_get_reemit_owner_eval())
                , prev_reg(aura_aot_get_register_owner_eval()) {
                aura_aot_set_reemit_owner_eval(e);
                aura_aot_set_register_owner_eval(e);
            }
            ~CascadeEvalOwnerGuard() noexcept {
                aura_aot_set_reemit_owner_eval(prev_reemit);
                aura_aot_set_register_owner_eval(prev_reg);
            }
            CascadeEvalOwnerGuard(const CascadeEvalOwnerGuard&) = delete;
            CascadeEvalOwnerGuard& operator=(const CascadeEvalOwnerGuard&) = delete;
            CascadeEvalOwnerGuard(CascadeEvalOwnerGuard&&) = delete;
            CascadeEvalOwnerGuard& operator=(CascadeEvalOwnerGuard&&) = delete;
        } cascade_owner_guard(static_cast<void*>(&evaluator_));
        // Keep AOT table epoch in lockstep for dual-check aura_closure_call
        // (or owner-scoped residual under multi-eval production — #2841).
        aura_aot_bump_func_table_epoch();
        // Issue #1414 / #1496: wipe solve_delta cache with the same
        // write-side protocol so soft dirty and hard invalidate agree.
        on_typed_mutation_epoch_bump();
        // Stamp bridges (if any) + soft-deopt JIT trackers at the NEW epoch.
        // invalidate_bridge_for already calls notify_jit_fn_trackers_batch_deopt_.
        if (name.empty()) {
            // typed_mutate catch-all: stamp every cached define / bridge.
            std::vector<std::string> names;
            names.reserve(ir_cache_bridge_.size() + ir_cache_v2_.size());
            for (const auto& [n, _] : ir_cache_bridge_)
                names.push_back(n);
            for (const auto& [n, _] : ir_cache_v2_) {
                if (ir_cache_bridge_.find(n) == ir_cache_bridge_.end())
                    names.push_back(n);
            }
            if (names.empty()) {
                notify_jit_fn_trackers_batch_deopt_("__typed_mutate__", bridge_epoch());
            } else {
                for (const auto& n : names)
                    invalidate_bridge_for(n);
            }
        } else {
            invalidate_bridge_for(name);
        }
        // Issue #1536: after name-specific batch_deopt, bulk-walk all
        // captured fns so un-named active closures also deopt-on-next-apply.
        notify_walk_active_closures_(bridge_epoch());
        // Issue #2042: comprehensive expire of live IR / tree-walker /
        // PrimCall closures so soft mark_define_dirty and hard invalidate
        // share one path (no missing fiber/JIT/primcall holders).
        (void)expire_stale_live_closures_(bridge_epoch());
        // Issue #2304 / #2366: post-bump epoch invariant walk.
        // Mode 0 (off): single relaxed load, zero walk cost.
        // Mode 1 (soft): per-entry + AOT + closure walk, metric only.
        // Mode 2 (hard): same walk + abort on any violation.
        run_epoch_invariant_if_enabled();
    }

    // Issue #3975: owner-scoped production facade skipped bump_bridge_epoch
    // so expire_stale_live_closures_(bridge_epoch()) is a no-op
    // (cl.bridge_epoch == cur_epoch). Name-precise MustDeopt + poison
    // bridge_epoch=0 on owner TW/IR closures whose define / .name matches
    // `name` (empty name → all live views). PrimCall cache still cleared.
    // JIT sid already ran via invalidate_bridge_for. Not a second table.
    void must_deopt_owner_live_closures_for_define_(const std::string& name) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        metrics_.expire_stale_live_closures_total.fetch_add(1, std::memory_order_relaxed);
        evaluator_.walk_active_closures([&]([[maybe_unused]] ClosureId id, Closure& cl) {
            if (!name.empty() && cl.name != name)
                return;
            const bool has_view =
                cl.flat != nullptr || cl.pool != nullptr || cl.body_id != aura::ast::NULL_NODE;
            if (!has_view && cl.must_deopt_before_next_call && cl.bridge_epoch == 0)
                return;
            cl.must_deopt_before_next_call = true;
            cl.bridge_epoch = 0;
        });
        auto poison_ir = [](IRClosure& cl) {
            cl.bridge_epoch = 0;
            cl.flat.reset();
            cl.pool.reset();
            cl.body_id = aura::ast::NULL_NODE;
        };
        if (name.empty()) {
            for (auto& [bname, binding] : ir_define_env_bindings_) {
                (void)bname;
                if (!binding || !binding->interpreter)
                    continue;
                ExecUnderGate4765 exec(binding->exec_mtx); // #4765
                binding->interpreter->walk_runtime_closures(
                    [&]([[maybe_unused]] std::uint64_t cid, IRClosure& cl) { poison_ir(cl); });
            }
        } else {
            if (auto it = ir_define_env_bindings_.find(name);
                it != ir_define_env_bindings_.end() && it->second && it->second->interpreter) {
                ExecUnderGate4765 exec(it->second->exec_mtx); // #4765
                it->second->interpreter->walk_runtime_closures(
                    [&]([[maybe_unused]] std::uint64_t cid, IRClosure& cl) { poison_ir(cl); });
            }
            for (auto& [bname, binding] : ir_define_env_bindings_) {
                if (bname == name || !binding || !binding->interpreter)
                    continue;
                ExecUnderGate4765 exec(binding->exec_mtx); // #4765
                binding->interpreter->walk_runtime_closures(
                    [&]([[maybe_unused]] std::uint64_t cid, IRClosure& cl) {
                        if (cl.name == name)
                            poison_ir(cl);
                    });
            }
        }
        aura_invalidate_all_closure_caches();
        metrics_.expire_primcall_cache_clear_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Issue #3219: production facade owns C-ABI epochs
    // (g_current_bridge_epoch / g_aot_defuse_version / g_aot_table_epoch).
    // Dual-write Evaluator defuse_version_ + core WorkspaceEpoch +
    // stamp/expire so is_bridge_stale / is_env_frame_stale observe the
    // same generation as aura_is_jit_closure_fresh. Does NOT re-bump
    // aura_aot_bump_func_table_epoch (preserves #2951 / #2841
    // owner-scoped; facade already noted hard_owner_scoped). Soft
    // never reaches this (hard_invalidate_via_facade returns false).
    // Caller holds mutate_mtx_ when nothing above Mutate is held
    // (acquire_if_needed is then a no-op). Under MutationBoundaryGuard,
    // Workspace is already held and this stamp skips the Mutate lock
    // (#4322) so the production canary does not abort.
    void stamp_eval_core_joint_after_production_facade_(const std::string& name) {
        using aura::compiler::lock_order::Level;
        using aura::compiler::lock_order::OrderedUniqueLock;
        // #4322: Workspace held; skip Mutate.
        OrderedUniqueLock<std::shared_mutex> mutate_guard;
        if (!lock_order::any_higher_held(Level::Mutate)) {
            mutate_guard =
                OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mutate_mtx_, Level::Mutate);
        }
        sync_lock_order_metrics_();
        metrics_.unified_invalidation_protocol_total.fetch_add(1, std::memory_order_relaxed);
        aura::util::thread_fence(std::memory_order_release);
        // Issue #3605: on the owner-scoped facade path the process C
        // clocks did not advance — the core bridge bump + C mirror SET
        // here would re-point g_current_bridge_epoch process-wide and
        // fail every peer closure's dual-fresh. Skip both on that path;
        // owner-local cleanup below still runs (peers ride #3300/#3351
        // name bits + #3377 owner slot clear).
        const bool os_table_bump = aura_aot_last_table_bump_owner_scoped() != 0;
        if (!os_table_bump) {
            bump_bridge_epoch();
            // Issue #4109: this dual-epoch bump re-definitions nothing —
            // ride still-valid captures up so the pure-counter advance does
            // not strand them behind the empty-Env contract.
            // Issue #4730: bump + ride under one shard barrier.
            (void)evaluator_.bump_defuse_and_ride_frames();
            metrics_.dep_graph_defuse_version_bumps.fetch_add(1, std::memory_order_relaxed);
        }
        on_typed_mutation_epoch_bump();
        if (name.empty()) {
            std::vector<std::string> names;
            names.reserve(ir_cache_bridge_.size() + ir_cache_v2_.size());
            for (const auto& [n, _] : ir_cache_bridge_)
                names.push_back(n);
            for (const auto& [n, _] : ir_cache_v2_) {
                if (ir_cache_bridge_.find(n) == ir_cache_bridge_.end())
                    names.push_back(n);
            }
            if (names.empty()) {
                notify_jit_fn_trackers_batch_deopt_("__typed_mutate__", bridge_epoch());
            } else {
                for (const auto& n : names)
                    invalidate_bridge_for(n);
            }
        } else {
            invalidate_bridge_for(name);
        }
        notify_walk_active_closures_(bridge_epoch());
        // Issue #3975: owner-scoped freeze leaves expire_stale_live_closures_
        // vacuous (cur_epoch == cl.bridge_epoch for every live TW/IR
        // closure). Name-precise MustDeopt + poison + TLS apply-epoch
        // bump replaces the generation-behind walk. Do not bump the
        // process C-bridge / table epoch (#3605 peer dual-fresh).
        if (os_table_bump) {
            must_deopt_owner_live_closures_for_define_(name);
            evaluator_.bump_closures_apply_epoch_public();
        } else {
            (void)expire_stale_live_closures_(bridge_epoch());
        }
        run_epoch_invariant_if_enabled();
    }

    // Issue #4510: production shape-deopt owner-scoped demotion for the
    // resolved define. The hook must not publish a process C-bridge or
    // Mutation bump under production defaults (#3605 freeze posture): the
    // historical bare bump_bridge_epoch() advanced the process clock while
    // g_jit_fns kept the pre-hook ScalarFn, so post-hook closures passed
    // dual-fresh against stale native and pre-hook peers false-deopted.
    // For the resolved define only:
    //   1. aura_drop_jit_fn_native_for_define — clears the installed
    //      ScalarFn (#4100 order: before any AuraJIT teardown, which does
    //      not clear g_jit_fns), MustDeopts matching live C-bridge
    //      closures, and bumps the define drop generation so a #4388
    //      cache-hit republish refuses. A closure allocated after the
    //      hook cannot enter the pre-hook ScalarFn.
    //   2. The existing name / slot soft-stale bits (#3300 / #3351 /
    //      #3750) when the multi-eval owner-scope is armed — the marks
    //      themselves no-op unless multi-eval live > 1 (Soft/Off zero-cost).
    //   3. Name-precise MustDeopt for owner live closures of that name +
    //      TLS apply-epoch bump (#3975 shape: the frozen process clocks
    //      make the epoch-expiry walk vacuous).
    //   4. Stamp would_allow_native=false on the existing reload proof —
    //      the hook demoted native (#3513 posture, without the facade
    //      reemit / table bump).
    // Peer closures of other defines stay dual-fresh green: no process
    // C-bridge / defuse / table epoch advances anywhere on this path.
    void demote_jit_native_owner_scoped_for_shape_deopt_(const std::string& name) {
        // (1) drop the installed ScalarFn first (#4100 order).
        aura_drop_jit_fn_native_for_define(name.c_str());
        // (2) existing name / slot soft-stale bits when the multi-eval
        // owner-scope is armed (facade #3300 / #3351 / #3750 parity).
        if (aura_aot_bump_will_be_owner_scoped() != 0 && aura_aot_state_map_size() > 1) {
            aura_aot_mark_peer_jit_name_soft_stale(name.c_str());
            aura_aot_mark_peer_ir_name_soft_stale(name.c_str());
            void* owner = aura_aot_get_reemit_owner_eval();
            if (!owner)
                owner = aura_aot_get_register_owner_eval();
            aura_aot_soft_stale_peer_slots_for_name(name.c_str(), owner);
        }
        // (3) owner live closures of that name ride the #3975 name-precise
        // MustDeopt (epochs will not retire them — clocks are frozen).
        must_deopt_owner_live_closures_for_define_(name);
        evaluator_.bump_closures_apply_epoch_public();
        // (4) demoted native → stamp the existing reload proof
        // would_allow_native=false (same shape as the #3513 facade stamp;
        // ABI early-exits must not leave a green proof from a prior store).
        AotReloadConsistencyProof p = load_aot_reload_consistency_proof_snapshot();
        p.would_allow_native = false;
        p.schema = kAotReloadConsistencyProofIssue;
        stamp_aot_reload_consistency_proof(p);
    }

    // Issue #3749: production facade early-return skipped jit_cache_
    // erase. try_jit_execute then cache-hit pre-mutate ScalarFn.
    // Same jit_cache_mtx_ window as Soft #1378 (erase + AuraJIT
    // invalidate + prefix). Soft never reaches this.
    // Issue #4100: AuraJIT::invalidate removes the ORC tracker and
    // erases deopt_pending; it does not clear g_jit_fns. Drop the
    // installed ScalarFn first, matching the partial-relower window.
    // The drop takes the closure table then the workspace write lock,
    // so jit_cache_mtx_ is not held across it.
    // Issue #4521: cache_define stores lowered bundles as name + "#" +
    // position (mul#0). The bare-name erase left those rows in
    // jit_cache_; under owner-scoped multi-eval the frozen facade clocks
    // keep them epoch-hits, so the next try_jit_execute / exec_jit
    // cache-hits the pre-mutate bundle and republishes its ScalarFn
    // through aura_register_fn_named. Walk the same name / name# rows
    // force_ir_cache_dirty_after_abort erases. Caller holds the
    // jit_cache_mtx_ unique window.
    void erase_jit_cache_bundle_rows_for_define_(const std::string& name) {
        const std::string facade_prefix = name + "#";
        for (auto it = jit_cache_.begin(); it != jit_cache_.end();) {
            if (it->first == name || it->first.rfind(facade_prefix, 0) == 0) {
                it = jit_cache_.erase(it);
                metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
            } else {
                ++it;
            }
        }
    }

    // Issue #4578: shape deopt resolves the bare define (ir_cache_v2_ is
    // searched first) while the live row is name#pos. A resolved bundle
    // key (trailing "#<digits>") strips back to the define so this walk
    // covers both. The native drop takes the closure table then the
    // workspace write lock, so this window is released before demote.
    void erase_shape_deopt_bundle_rows_(const std::string& resolved) {
        std::string bare = resolved;
        const auto hash = resolved.rfind('#');
        if (hash != std::string::npos && hash > 0 && hash + 1 < resolved.size()) {
            bool digits = true;
            for (std::size_t i = hash + 1; i < resolved.size(); ++i) {
                if (resolved[i] < '0' || resolved[i] > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits)
                bare.resize(hash);
        }
        std::unique_lock cache_write(jit_cache_mtx_);
        erase_jit_cache_bundle_rows_for_define_(bare);
    }

    void evict_jit_cache_after_production_facade_(const std::string& name) {
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            if (jit_cache_.erase(name) > 0)
                metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
            // Issue #4521: also erase name#pos bundle rows (abort walk).
            erase_jit_cache_bundle_rows_for_define_(name);
        }
        aura_drop_jit_fn_native_for_define(name.c_str());
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            jit_.invalidate(name.c_str());
            jit_.invalidate_prefix(name.c_str());
            metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #4146: the facade owns the whole dirty cone, not just the
        // root — fan the eviction out to every dependent that receives
        // IR body-dirty below (#3474/#3823 union), or an owner-scoped
        // caller keeps executing pre-mutate native targeting the root.
        drop_cone_jit_after_production_facade_(name);
    }

    // Issue #4146: production facade cone JIT drop. The #3188/#3345/#3474/
    // #3823 early-return marks every dependent IR body-dirty, but #3749
    // dropped jit_cache_ + AuraJIT native for the ROOT define only. Under
    // owner-scoped multi-eval the C clocks are intentionally frozen
    // (#2841/#2951/#3605) and dual-fresh only covers live closure views, so
    // a caller D of mutated F could keep executing pre-mutate JIT/AOT
    // native that still targets F while root F was fail-closed. Mirror the
    // Soft #491/#1378 per-dependent teardown for the same cone that
    // receives IR body-dirty: called_by BFS (#3474) ∪ node-dep dependents
    // (#3823). Same lock shape as the root drop (#4100): jit_cache_.erase
    // under jit_cache_mtx_, native drop outside, then AuraJIT invalidate +
    // prefix. Owner-scoped multi-eval MustDeopts live closures named d via
    // the #3975 name-precise walk (epochs will not save them) and bumps the
    // TLS apply epoch; cone names feed the #3373 production dirty ring so
    // BoundaryExit reemit covers callers. Soft / Off never reach this
    // (facade-success path only). No process-wide table epoch bump (#2951
    // owner scope); no second closure table.
    void drop_cone_jit_after_production_facade_(const std::string& name) {
        std::vector<std::string> dependents;
        {
            using aura::compiler::lock_order::Level;
            using aura::compiler::lock_order::OrderedSharedLock;
            OrderedSharedLock<std::shared_mutex> dep_read(dep_graph_mtx_, Level::DepGraph);
            std::deque<std::string> bfs;
            std::unordered_set<std::string> visited;
            bfs.push_back(name);
            visited.insert(name);
            while (!bfs.empty()) {
                auto current = bfs.front();
                bfs.pop_front();
                auto dit = dep_graph_.find(current);
                if (dit == dep_graph_.end())
                    continue;
                for (const auto& caller : dit->second.called_by) {
                    if (!visited.insert(caller).second)
                        continue;
                    dependents.push_back(caller);
                    bfs.push_back(caller);
                }
            }
            // Issue #3823 union: node-dep dependents (Soft-erased / node-only
            // callers the string called_by walk cannot see). Same decode walk
            // as mark_node_dep_dependents_body_dirty_.
            using aura::compiler::dirty::decode_block_dep_node;
            using aura::compiler::dirty::decode_fn_slot;
            using aura::compiler::dirty::encode_fn_node;
            using aura::compiler::dirty::is_block_dep_node;
            using aura::compiler::dirty::is_fn_node;
            const auto slot_it = dep_name_to_slot_.find(name);
            if (slot_it != dep_name_to_slot_.end()) {
                const auto* deps = node_dep_graph_.dependents(encode_fn_node(slot_it->second));
                if (deps) {
                    for (const auto n : *deps) {
                        std::uint32_t slot = UINT32_MAX;
                        if (is_fn_node(n))
                            slot = decode_fn_slot(n);
                        else if (is_block_dep_node(n))
                            slot = decode_block_dep_node(n).caller_slot;
                        else
                            continue;
                        if (slot >= dep_slot_to_name_.size())
                            continue;
                        const auto& nm = dep_slot_to_name_[slot];
                        if (nm.empty() || !visited.insert(nm).second)
                            continue;
                        dependents.push_back(nm);
                    }
                }
            }
        }
        for (const auto& dependent : dependents) {
            if (dependent.empty() || dependent == name)
                continue;
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                // Issue #4538: the cone drop must erase the full bundle
                // (name + name#* rows) like the root define — the bare-key
                // erase left D#0 behind, and the next try_jit_execute
                // cache-hit the stale lowered body (republishing the
                // pre-mutate ScalarFn through aura_register_fn_named).
                erase_jit_cache_bundle_rows_for_define_(dependent);
            }
            aura_drop_jit_fn_native_for_define(dependent.c_str());
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                jit_.invalidate(dependent.c_str());
                jit_.invalidate_prefix(dependent.c_str());
                metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
            }
            // Issue #3373: cone name feeds the production candidate ring so
            // BoundaryExit / reemit covers callers. Ring is bounded and the
            // pipeline is idempotent on duplicates.
            if (aura_production_defaults_active_probe() != 0)
                aura_production_dirty_ring_push(dependent.c_str(), 1ULL << 1, 0);
        }
        // Issue #3975 shape: under owner-scoped multi-eval the process C
        // clocks did not advance, so epoch checks cannot retire a live
        // closure named d — name-precise MustDeopt each dependent (no second
        // table), then bump the TLS apply epoch so owner apply_closure
        // observes the new flags. Global-bump path already retired
        // generation-behind closures via expire_stale_live_closures_.
        if (aura_aot_last_table_bump_owner_scoped() != 0) {
            for (const auto& dependent : dependents) {
                if (dependent.empty() || dependent == name)
                    continue;
                must_deopt_owner_live_closures_for_define_(dependent);
            }
            if (!dependents.empty())
                evaluator_.bump_closures_apply_epoch_public();
        }
    }

    // Issue #2304 / #2366 / #2501 / #2541: post-bump epoch invariant walk.
    //
    // Mode source: process-level aura_epoch_invariant_mode() (env +
    // C setter + production soft default #2501/#2541) OR local hard flag.
    //   0 = off — single relaxed load (zero-cost when disabled)
    //   1 = soft — walk + force MustDeopt + physically clear stale AOT
    //       slots (never abort). #2541: soft is not metric-only for slots.
    //   2 = hard — same enforcement + abort if violations remain
    //       after enforcement (stale IR stamps cannot be auto-healed)
    //
    // Invariant (#2366/#2501/#2541): after an epoch bump, every live AOT/JIT
    // executable slot is either current-generation or null; every
    // live closure is either remounted (bridge_epoch==current / 0) or
    // MustDeopt; every clean IR cache entry has a current bridge stamp.
    //
    // #2541 closes the production post-bump native-stale window:
    //   - production defaults → soft when AURA_EPOCH_INVARIANT unset
    //   - soft nulls generation-behind AOT fn_ptr (same order as #2271)
    //   - soft+hard set MustDeopt on tree-walker + JIT live closures
    //   - additive counters: slot-stale / closure-must-deopt
    void run_epoch_invariant_if_enabled() noexcept {
        // Zero-cost when off — prefer process mode, fall back to local hard flag.
        int mode = aura_epoch_invariant_mode();
        if (mode == 0 && epoch_invariant_hard_enabled_.load(std::memory_order_relaxed) != 0)
            mode = 2; // local hard enable (tests)
        if (mode == 0)
            return;

        const auto cur_epoch = bridge_epoch();
        std::uint64_t violations = 0;
        std::uint64_t slot_stale = 0;
        std::uint64_t closure_must_deopt = 0;

        // ── 1. IR cache v2: clean entries must not be generation-behind ──
        // ir_cache_bridge_ is ClosureBridgeData bundles (no version stamp);
        // stamp lives on IRCacheEntry in ir_cache_v2_.
        for (const auto& [n, entry] : ir_cache_v2_) {
            (void)n;
            if (entry.dirty)
                continue; // pending re-lower — not an executable stale slot
            const auto stamp_bridge = entry.version_stamp_.bridge_epoch;
            if (stamp_bridge == 0)
                continue; // never stamped
            if (stamp_bridge != cur_epoch)
                ++violations;
        }

        // ── 2. AOT func table: live slots (fn_ptr≠0) must be current gen ──
        // Count then physically null fn_ptr under soft+hard (#2541 soft
        // enforce). Order = #2271: fn_ptr release then generation.
        // Soft never aborts; hard may still abort on residual IR stamps.
        slot_stale = static_cast<std::uint64_t>(aura_aot_count_live_generation_behind_slots());
        violations += slot_stale;
        if (mode >= 1 && slot_stale > 0) {
            // Process-wide clear of generation-behind slots (eval_ptr=null).
            (void)aura_aot_invalidate_all_stale_slots_for_eval(nullptr);
        }

        // ── 3. Live tree-walker closures: set MustDeopt on gen-behind ──
        // (#2501: enforce, not metric-only count). Already-MustDeopt is OK.
        evaluator_.walk_active_closures([&]([[maybe_unused]] ClosureId id, Closure& cl) {
            const bool has_view =
                cl.flat != nullptr || cl.pool != nullptr || cl.body_id != aura::ast::NULL_NODE;
            if (!has_view)
                return; // expired / not live — not a violation
            if (cl.must_deopt_before_next_call)
                return; // MustDeopt path already enforced
            // Unstamped (0) or remounted at current epoch OK.
            if (cl.bridge_epoch == 0 || cl.bridge_epoch == cur_epoch)
                return;
            // Live, stamped, generation-behind → force MustDeopt + count.
            cl.must_deopt_before_next_call = true;
            ++violations;
            ++closure_must_deopt;
        });

        // ── 4. JIT runtime live-closure table (g_closure_*) ──
        // Same MustDeopt contract for AOT/JIT native dispatch path.
        const auto jit_md =
            static_cast<std::uint64_t>(aura_epoch_invariant_must_deopt_stale_live_closures());
        violations += jit_md;
        closure_must_deopt += jit_md;

        epoch_invariant_violation_total_.fetch_add(violations, std::memory_order_relaxed);
        epoch_invariant_walks_total_.fetch_add(1, std::memory_order_relaxed);
        if (slot_stale > 0)
            epoch_invariant_slot_stale_total_.fetch_add(slot_stale, std::memory_order_relaxed);
        if (closure_must_deopt > 0)
            epoch_invariant_closure_must_deopt_total_.fetch_add(closure_must_deopt,
                                                                std::memory_order_relaxed);
        // Mirror into C-readable process counters (test + dashboards).
        aura_epoch_invariant_note_walk(violations);
        aura_epoch_invariant_note_slot_stale(slot_stale);
        aura_epoch_invariant_note_closure_must_deopt(closure_must_deopt);

        // Soft never aborts (#2541 AC / #2501 AC4). Hard aborts only if
        // violations remain after enforcement (stale IR stamps).
        if (violations > 0 && mode >= 2) {
            std::fprintf(stderr,
                         "[#2366][#2501][#2541] epoch invariant HARD fail: %llu generation-behind "
                         "AOT/IR/closure survivors after bump "
                         "(slot_stale=%llu closure_must_deopt=%llu cur_bridge=%llu) — aborting\n",
                         static_cast<unsigned long long>(violations),
                         static_cast<unsigned long long>(slot_stale),
                         static_cast<unsigned long long>(closure_must_deopt),
                         static_cast<unsigned long long>(cur_epoch));
            std::abort();
        }
    }

    // Issue #2042: expire every live IRClosure / tree-walker Closure that
    // predates cur_epoch, plus bulk-clear PrimCall g_closure_cache.
    // Atomic w.r.t. apply only when caller holds mutate lock (soft/hard
    // invalidate paths). Returns total IR + tree-walker expirations.
    //
    // Sources covered:
    //   1. ir_define_env_bindings_ → IRInterpreter::runtime_closures_
    //   2. Evaluator::closures_ (tree-walker / fiber-held)
    //   3. PrimCall / JIT inline closure cache (aura_invalidate_all_closure_caches)
    //   4. AuraJIT walk_active_closures already ran via notify_walk_active_closures_
    std::size_t expire_stale_live_closures_(std::uint64_t cur_epoch) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        metrics_.expire_stale_live_closures_total.fetch_add(1, std::memory_order_relaxed);
        std::size_t ir_expired = 0;
        std::size_t tw_expired = 0;

        // ── 1. IR path (all define env bindings; not just the mutated name) ──
        // Issue #2569: previously we cleared flat/pool/body on every
        // epoch-stale IRClosure. That made soft mark_define_dirty (rebind of
        // score) kill unrelated top-level helpers (bump) whose IR function
        // body was still valid in their module — call_closure then failed
        // and returned void. Soft path: restamp bridge/env epochs when the
        // IR func_id is still in-module so helpers keep working. Hard clear
        // only when there is no IR function left (OOB func_id).
        // Issue #4815: also the retained value-define interpreters.
        for (auto* bindings_map : {&ir_define_env_bindings_, &ir_value_define_closure_owners_})
            for (auto& [bname, binding] : *bindings_map) {
                (void)bname;
                if (!binding || !binding->interpreter)
                    continue;
                // Issue #4717: serialize the interpreter's mutable execution state
                // against a body running on a sibling fiber (exec_mtx; taken under
                // the cache gate, and the gate is never taken while holding it).
                ExecUnderGate4765 exec(binding->exec_mtx); // #4765
                binding->interpreter->walk_runtime_closures([&]([[maybe_unused]] std::uint64_t cid,
                                                                IRClosure& cl) {
                    // Skip unstamped (legacy) and already-current stamps.
                    if (cl.bridge_epoch == 0 || cl.bridge_epoch == cur_epoch)
                        return;
                    // Soft recover: keep IR dispatch alive when func is still present.
                    if (cl.func_id < binding->module.functions.size()) {
                        cl.bridge_epoch = cur_epoch;
                        cl.env_version = evaluator_.defuse_version();
                        metrics_.live_closure_epoch_restamp_total.fetch_add(
                            1, std::memory_order_relaxed);
                        return;
                    }
                    cl.flat.reset();
                    cl.pool.reset();
                    cl.body_id = aura::ast::NULL_NODE;
                    cl.env_version = 0;
                    ++ir_expired;
                    metrics_.jit_hotswap_forced_deopt_total.fetch_add(1, std::memory_order_relaxed);
                    metrics_.ir_closure_invalidate_expired_total.fetch_add(
                        1, std::memory_order_relaxed);
                    metrics_.jit_hotswap_epoch_mismatch_prevented_total.fetch_add(
                        1, std::memory_order_relaxed);
                    metrics_.dangling_env_prevented.fetch_add(1, std::memory_order_relaxed);
                });
            }
        if (ir_expired > 0)
            metrics_.expire_stale_ir_closures_total.fetch_add(ir_expired,
                                                              std::memory_order_relaxed);

        // ── 2. Tree-walker / fiber-held Closures ──
        // Issue #2569: soft path restamps unimpacted closures that still have
        // a live body NodeId in their captured flat (top-level helpers). Only
        // null the view when the body is unusable (null flat or OOB body_id).
        // Hard invalidate of a specific define still clears via
        // clear_ir_define_env_binding / per-name bridge invalidation.
        evaluator_.walk_active_closures([&]([[maybe_unused]] ClosureId id, Closure& cl) {
            if (cur_epoch == 0)
                return;
            if (cl.bridge_epoch != 0 && cl.bridge_epoch == cur_epoch)
                return; // stamped at new epoch (should not happen mid-invalidate)
            const bool body_usable = cl.flat && cl.pool && cl.body_id != aura::ast::NULL_NODE &&
                                     cl.body_id < cl.flat->size();
            if (body_usable) {
                cl.bridge_epoch = cur_epoch;
                metrics_.live_closure_epoch_restamp_total.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // Only act when there is still a live view to expire (idempotent
            // on second call after atomic_bump + hard invalidate).
            const bool has_view =
                cl.flat != nullptr || cl.pool != nullptr || cl.body_id != aura::ast::NULL_NODE;
            if (!has_view)
                return;
            cl.flat = nullptr;
            cl.pool = nullptr;
            cl.body_id = aura::ast::NULL_NODE;
            // Keep bridge_epoch as pre-bump (or 0) — do not restamp.
            ++tw_expired;
            metrics_.dangling_env_prevented.fetch_add(1, std::memory_order_relaxed);
        });
        if (tw_expired > 0)
            metrics_.expire_stale_tree_walker_closures_total.fetch_add(tw_expired,
                                                                       std::memory_order_relaxed);

        // ── 3. PrimCall / JIT hot apply inline cache ──
        aura_invalidate_all_closure_caches();
        metrics_.expire_primcall_cache_clear_total.fetch_add(1, std::memory_order_relaxed);

        return ir_expired + tw_expired;
    }

    // Issue #1523: mirror process-wide lock_order atomics into CompilerMetrics.
    void sync_lock_order_metrics_() noexcept {
        metrics_.lock_inversion_detected_total.store(
            lock_order::g_lock_inversion_detected_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        metrics_.mutate_mtx_contended_total.store(
            lock_order::g_mutate_mtx_contended_total.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    // Issue #1526: remap IRClosure::env_id through compact_env_frames remap table.
    // Called under compact_env_frames_lock_ before dual-epoch bump.
    void remap_ir_closure_env_ids_on_compact_(const std::int64_t* remap, std::size_t n) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        if (!remap || n == 0)
            return;
        constexpr auto kNull = std::numeric_limits<std::uint32_t>::max();
        // Issue #4815: also the retained value-define interpreters.
        for (auto* bindings_map : {&ir_define_env_bindings_, &ir_value_define_closure_owners_})
            for (auto& [name, binding] : *bindings_map) {
                (void)name;
                if (!binding || !binding->interpreter)
                    continue;
                // Issue #4717: serialize the interpreter's mutable execution state
                // against a body running on a sibling fiber (exec_mtx; taken under
                // the cache gate, and the gate is never taken while holding it).
                ExecUnderGate4765 exec(binding->exec_mtx); // #4765
                binding->interpreter->walk_runtime_closures(
                    [&]([[maybe_unused]] std::uint64_t cid, IRClosure& cl) {
                        if (cl.env_id == kNull || static_cast<std::size_t>(cl.env_id) >= n)
                            return;
                        const auto np = remap[cl.env_id];
                        if (np >= 0)
                            cl.env_id = static_cast<std::uint32_t>(np);
                        else
                            cl.env_id = kNull;
                    });
            }
    }

    // Issue #1526: restamp IRClosure::bridge_epoch after dual-epoch bump.
    // Returns number of restamps for metrics aggregation.
    std::size_t restamp_ir_closure_bridge_epochs_(std::uint64_t new_epoch) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        std::size_t n = 0;
        // Issue #4815: also the retained value-define interpreters.
        for (auto* bindings_map : {&ir_define_env_bindings_, &ir_value_define_closure_owners_})
            for (auto& [name, binding] : *bindings_map) {
                (void)name;
                if (!binding || !binding->interpreter)
                    continue;
                // Issue #4717: serialize the interpreter's mutable execution state
                // against a body running on a sibling fiber (exec_mtx; taken under
                // the cache gate, and the gate is never taken while holding it).
                ExecUnderGate4765 exec(binding->exec_mtx); // #4765
                binding->interpreter->walk_runtime_closures(
                    [&]([[maybe_unused]] std::uint64_t cid, IRClosure& cl) {
                        if (cl.bridge_epoch != 0) {
                            cl.bridge_epoch = new_epoch;
                            ++n;
                        }
                    });
            }
        return n;
    }

    // Issue #1538: combined post-mutation linear pipeline (runtime half).
    // Type-checker half is PostMutationInvariantVisitor /
    // post_mutation_invariant_check. This runs linear_post_mutate_enforce_all
    // and stamps MutationResult + per-mutation_id linear status map for
    // mutation_log JSON exposure.
    void apply_linear_post_mutate_pipeline_(MutationResult& res, std::uint64_t mutation_id) {
        // Issue #1568: unified scan + enforce_all + epoch fence + GC root audit.
        // mark_all_linear=false → only Moved captures force-dropped (typed_mutate).
        const auto bound = evaluator_.enforce_linear_boundary_consistency(
            Evaluator::kLinearGcRootAuditTypedMutate, /*mark_all_linear=*/false);
        res.linear_post_mutate_enforced = true;
        res.linear_post_mutate_frames_checked = bound.frames_checked;
        res.linear_post_mutate_safe = bound.all_safe;
        res.linear_post_mutate_status = res.linear_post_mutate_safe ? "Ok" : "Unsafe";
        if (mutation_id > 0) {
            mutation_linear_status_[mutation_id] = res.linear_post_mutate_status;
        }
        // Workspace MutationRecord.mutation_id may differ from the
        // MutationResult.mutation_id (current_ast_ vs workspace_flat_
        // counters). Stamp the most recent workspace log entries so
        // query_mutation_log JSON surfaces linear status reliably.
        if (auto* ws = evaluator_.workspace_flat()) {
            const auto& log = ws->all_mutations();
            // Stamp up to the last 8 records (covers multi-mutate atomic).
            const std::size_t n = log.size();
            const std::size_t start = n > 8 ? n - 8 : 0;
            for (std::size_t i = start; i < n; ++i) {
                mutation_linear_status_[log[i].mutation_id] = res.linear_post_mutate_status;
            }
        }
        // When runtime half reports unsafe under Strict mode, surface as
        // failure if invariant path did not already fail.
        if (!res.linear_post_mutate_safe && invariant_check_mode_ == InvariantCheckMode::Strict &&
            res.success) {
            res.success = false;
            res.error = "linear post-mutate enforce reported unsafe env frames";
        }
        metrics_.linear_post_mutate_pipeline_total.fetch_add(1, std::memory_order_relaxed);
        if (!res.linear_post_mutate_safe) {
            metrics_.linear_post_mutate_pipeline_unsafe_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        }
        // GC root audit already run inside enforce_linear_boundary_consistency.
    }

    // Issue #1522: notify AuraJIT fn_trackers_ (name + name#*).
    void notify_jit_fn_trackers_batch_deopt_(const std::string& name, std::uint64_t current_epoch) {
        const auto marked = jit_.batch_deopt_for(name.c_str(), current_epoch);
        metrics_.jit_fn_trackers_batch_deopt_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.jit_fn_trackers_entries_marked_total.fetch_add(marked, std::memory_order_relaxed);
        // Mirror into #1508 closure safe-fallback family so Agents see one surface.
        if (marked > 0) {
            metrics_.jit_closure_stale_deopt_total.fetch_add(marked, std::memory_order_relaxed);
            metrics_.jit_closure_safe_fallbacks.fetch_add(marked, std::memory_order_relaxed);
            metrics_.jit_closure_safe_fallbacks_total.fetch_add(marked, std::memory_order_relaxed);
        }
        // Issue #1543 / #2131: JIT hot-swap / batch-deopt under GcCoord Scope.
        // Issue #4680: name-independent audit; once per dirty batch.
        if (dirty_batch_depth_ > 0) {
            if (dirty_batch_hotswap_audit_done_) {
                ++dirty_batch_coalesced_4678_;
                return;
            }
            dirty_batch_hotswap_audit_done_ = true;
        }
        {
            gc_coord::Scope hot_coord(gc_coord::Path::HotSwap);
            hot_coord.enter_cascade();
            (void)evaluator_.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditJitHotSwap);
            hot_coord.after_cascade();
        }
    }

    // Issue #1536: bulk walk_active_closures after epoch bump / invalidate.
    // Marks every captured fn that is_fn_epoch_stale for deopt-on-next-apply
    // and pairs jit_epoch_stale_check_total with live_closure_stale_prevented.
    void notify_walk_active_closures_(std::uint64_t current_epoch) {
        IrCacheGate ir_gate(ir_cache_gate_); // #4717 shared IR/closure cache
        const auto stale = jit_.walk_active_closures(current_epoch);
        metrics_.jit_walk_active_closures_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.jit_walk_active_closures_stale_total.fetch_add(stale, std::memory_order_relaxed);
        if (stale > 0) {
            // AuraJIT already bumped its own jit_epoch_stale_check_total;
            // mirror onto CompilerMetrics for dual-reader dashboards.
            metrics_.jit_epoch_stale_check_total.fetch_add(stale, std::memory_order_relaxed);
            metrics_.compiler_live_closure_stale_prevented_total.fetch_add(
                stale, std::memory_order_relaxed);
            metrics_.jit_closure_stale_deopt_total.fetch_add(stale, std::memory_order_relaxed);
            metrics_.jit_closure_safe_fallbacks_total.fetch_add(stale, std::memory_order_relaxed);
            metrics_.jit_fn_trackers_entries_marked_total.fetch_add(stale,
                                                                    std::memory_order_relaxed);
        }
    }

    // Issue #59 Iter 3: Mutation Lock. A mutate:* operation (which
    // mutates a function body and calls invalidate_function) holds
    // this for its duration. The JIT compile path also holds it
    // (shared is fine — the invalidate can wait for in-flight
    // compiles to drain). The pattern is: mutates hold SHARED (multiple
    // mutates can run concurrently), compile holds SHARED too (compile
    // doesn't need to be exclusive with mutate), but invalidate
    // reaches a unique section to drain readers and then erase.
    // Simpler: we use a plain mutex that both hold exclusively. The
    // critical section is sub-ms in practice.
    // Lock-order contract (Issue #1388): this is the FIRST lock
    // in the canonical order. Acquire this BEFORE workspace_mtx_,
    // env_frames_mtx_, or dep_graph_mtx_. Reverse is NOT allowed.
    std::shared_mutex mutate_mtx_;

    // Issue #1376: protects dep_graph_ (calls / called_by edges).
    // Writers (record_dependency, invalidate_function erase, unload_module)
    // take unique_lock; readers (public_dep_graph_*, get_dependents,
    // cascade BFS) take shared_lock.
    // Lock-order contract (Issue #1388): this is the LAST lock in
    // the canonical order. Acquire ONLY after mutate_mtx_ +
    // workspace_mtx_ + env_frames_mtx_. Reverse is NOT allowed.
    mutable std::shared_mutex dep_graph_mtx_;

    // Issue #1377: SoA dual-emit opt-in (default off). Mirrors the
    // process-wide ir_soa_migration::g_enable_soa_dual_emit flag so
    // per-service set_soa_dual_emit stays discoverable for agents/tests.
    std::atomic<bool> enable_soa_dual_emit_{false};

    // Issue #4388: a cache hit that has not yet published or called must
    // still see the sampled mutation epoch, the define-drop generation,
    // and the same jit_cache_ pointer. Workspace-held mark_define_dirty
    // skips mutate_mtx_, so the shared lock taken at entry does not
    // fence the facade. Soft/Off does not call this.
    [[nodiscard]] bool jit_cache_hit_still_current_(const std::string& name, aura::jit::ScalarFn fn,
                                                    std::uint64_t hit_epoch,
                                                    std::uint64_t hit_drop_gen) const {
        if (fn == nullptr)
            return false;
        if (aura::core::current_mutation_epoch() != hit_epoch)
            return false;
        if (aura_jit_define_drop_gen() != hit_drop_gen)
            return false;
        std::shared_lock cache_read(jit_cache_mtx_);
        auto it = jit_cache_.find(name);
        if (it == jit_cache_.end())
            return false;
        if (it->second.last_seen_epoch_ != hit_epoch)
            return false;
        return it->second.fn_ptr.load(std::memory_order_acquire) == fn;
    }

    // Try to execute an IRModule via LLVM JIT
    // Returns EvalResult on success, nullopt on failure (falls back to IR interpreter)
    // escape_maps: optional pre-computed escape maps from EscapeAnalysisWrap pass.
    //              If null, escape analysis is run inline.
    std::optional<types::EvalValue>
    try_jit_execute(aura::ir::IRModule& ir_mod,
                    const std::vector<std::vector<std::uint8_t>>* escape_maps = nullptr);

    // ── Shape profiler integration (#53) ────────────────────────

    // Record shape of eval result for profiling.
    // Called after each successful IR interpreter execution.
    // Uses the last module's entry function as the FnKey.
    void record_eval_result_shape(const std::string& session,
                                  const std::optional<aura::ir::IRModule>& mod,
                                  const aura::compiler::IRInterpreter* interp,
                                  const types::EvalValue& result) {
        auto& profiler = shape_profiler_;

        if (!mod) {
            // No IR module — record against a generic key
            auto fn_key = shape::make_fn_key(session, "__eval__");
            auto shape_id = shape::inline_shape_of(result.val);
            if (profiler.record_shape(fn_key, shape_id) && profiler.is_stable(fn_key))
                sync_shape_ids_for_fn_key(fn_key, "__eval__", shape_id);
            return;
        }

        // Record per-function: use IR function name as key
        for (auto& fn : mod->functions) {
            if (fn.id == mod->entry_function_id) {
                auto fn_key = shape::make_fn_key(session, fn.name);
                auto shape_id = shape::inline_shape_of(result.val);
                if (profiler.record_shape(fn_key, shape_id) && profiler.is_stable(fn_key))
                    sync_shape_ids_for_fn_key(fn_key, fn.name, shape_id);

                // Also record argument shapes if we have them
                // (Phase 2: expand to arg-level shape tracking)
                break;
            }
        }
    }

    // Get shape profiler metrics for a function name.
    // Returns empty metrics if the function hasn't been profiled.
    shape::ShapeFnMetrics shape_metrics(const std::string& name) const {
        auto fn_key = shape::make_fn_key(session_id_, name);
        return shape_profiler_.metrics(fn_key);
    }

    // Check if a function's shape has stabilized (for speculative JIT).
    bool is_shape_stable(const std::string& name) const {
        auto fn_key = shape::make_fn_key(session_id_, name);
        return shape_profiler_.is_stable(fn_key);
    }

    // Get the dominant shape ID for a function.
    shape::ShapeID dominant_shape(const std::string& name) const {
        auto fn_key = shape::make_fn_key(session_id_, name);
        return shape_profiler_.dominant_shape(fn_key);
    }

    // Issue #605: true when a shape-specialized JIT entry is stale
    // because ShapeProfiler bumped version after mutate:*.
    [[nodiscard]] bool jit_cache_shape_version_stale(const JitCachedFn& entry,
                                                     shape::FnKey fn_key) const {
        if (!entry.has_shape_map)
            return false;
        const auto snap = shape_profiler_.current_snapshot(fn_key);
        return entry.compiled_shape_version_ != snap.version;
    }

    // Issue #686: mark IR cache dirty for a ShapeProfiler FnKey.
    // Issue #4091: resolve the define via the FnKey → name side index
    // (O(1); the pre-#4090 loop hashed session_id_ + name for every cached
    // define on every result-shape flip) and dirty only the entry
    // function's result block(s) through the mark_blocks_dirty batch
    // entry — mark_all_blocks_dirty forced the next lower/opt to rebuild
    // the whole function instead of the dirty cone. A real structural
    // mutate keeps its existing cone dirty path.
    void mark_shape_dirty_for_fn_key(shape::FnKey fn_key) {
        const auto idx = shape_fnkey_names_.find(fn_key);
        if (idx != shape_fnkey_names_.end() && mark_shape_dirty_result_block_(idx->second))
            return;
        // Cold fallback: index row missing (entry stored before the row
        // existed / erased). One scan, then the row is cached.
        for (const auto& [name, entry] : ir_cache_v2_) {
            (void)entry;
            if (shape::make_fn_key(session_id_, name) != fn_key)
                continue;
            shape_fnkey_names_.insert_or_assign(fn_key, name);
            (void)mark_shape_dirty_result_block_(name);
            return;
        }
    }

    // Issue #4091: dirty the entry function's result block(s) — the blocks
    // holding Return ops of the define's function — via the
    // mark_blocks_dirty batch entry (#2522 / #2615 discipline; the
    // single-block mark_block_dirty is deleted under AURA_PRODUCTION_PACK).
    // Returns false when no v2 cache entry exists for `name`.
    bool mark_shape_dirty_result_block_(const std::string& name) {
        const auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& entry = it->second;
        if (entry.irs.empty()) {
            // Source-only shell — no lowered block to mark. Latch the entry
            // dirty anyway: the ArenaCompact cone (#3455 / #4510) is derived
            // from any_block_dirty(), and a shell row must still reach the
            // scope-3 arm so a re-published native is dropped.
            entry.dirty = true;
            metrics_.irsoa_dirty_cascade_savings.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // The define's entry function: named after the define when present,
        // else the first function.
        std::size_t func_idx = 0;
        for (std::size_t fi = 0; fi < entry.irs.size(); ++fi) {
            if (entry.irs[fi].name == name) {
                func_idx = fi;
                break;
            }
        }
        // Result block(s): blocks holding Return ops. block_dirty_per_func_
        // and the SoA mirror are indexed by block POSITION, not
        // BasicBlock::id — push positions.
        std::vector<std::uint32_t> result_blocks;
        if (func_idx < entry.irs.size()) {
            const auto& ir_fn = entry.irs[func_idx];
            for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                for (const auto& instr : ir_fn.blocks[bi].instructions) {
                    if (instr.opcode == aura::ir::IROpcode::Return) {
                        result_blocks.push_back(static_cast<std::uint32_t>(bi));
                        break;
                    }
                }
            }
        }
        if (result_blocks.empty() && func_idx < entry.soa_mod.functions.size()) {
            // Defensive: no Return mapped — dirty the function's last block
            // (still a strict subset, never mark_all_blocks_dirty).
            const auto& soa_fn = entry.soa_mod.functions[func_idx];
            if (!soa_fn.blocks_.empty())
                result_blocks.push_back(static_cast<std::uint32_t>(soa_fn.blocks_.size() - 1));
        }
        if (!result_blocks.empty())
            entry.mark_blocks_dirty(func_idx, std::span<const std::uint32_t>(result_blocks));
        else
            entry.dirty = true; // no Return mapped — latch for the compact cone
        metrics_.irsoa_dirty_cascade_savings.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Issue #686: propagate stable dominant shape into IRSoA shape_ids_.
    // Issue #4108: the stable path resolves the define through the #4091
    // FnKey → name side index exactly like mark_shape_dirty_for_fn_key —
    // the pre-fix body scanned ir_cache_v2_ on every stable result and
    // painted the result shape onto every unset shape_ids_ column of
    // EVERY function in the entry, so a nested function's unset columns
    // inherited the outer result shape and the next lower/JIT specialized
    // those ops to a shape they never return. Stamp only the entry
    // function's result-block columns (mark_shape_dirty_result_block_'s
    // function + Return-block selection); other functions stay untouched.
    void sync_shape_ids_for_fn_key(shape::FnKey fn_key, const std::string& want_name,
                                   shape::ShapeID shape_id) {
        if (shape_id == shape::SHAPE_UNKNOWN)
            return;
        const auto sid = static_cast<std::uint32_t>(shape_id);
        const auto idx = shape_fnkey_names_.find(fn_key);
        if (idx == shape_fnkey_names_.end()) {
            // Cold fallback, same shape as mark_shape_dirty_for_fn_key:
            // one scan, then the row is cached. Equality rechecks the
            // stored name, not only the hash (#4108): two different names
            // that share a make_fn_key hash must not share a side-index row.
            for (const auto& [name, entry] : ir_cache_v2_) {
                (void)entry;
                if (name != want_name || shape::make_fn_key(session_id_, name) != fn_key)
                    continue;
                shape_fnkey_names_.insert_or_assign(fn_key, name);
                break;
            }
        }
        // Resolve by exact name; a side-index row owned by a colliding
        // name is never consulted for this define.
        const auto it = ir_cache_v2_.find(want_name);
        if (it == ir_cache_v2_.end())
            return;
        auto& entry = it->second;
        // The define's entry function: named after the define when present,
        // else the first function (mark_shape_dirty_result_block_'s selection).
        std::size_t func_idx = 0;
        for (std::size_t fi = 0; fi < entry.irs.size(); ++fi) {
            if (entry.irs[fi].name == want_name) {
                func_idx = fi;
                break;
            }
        }
        if (func_idx >= entry.soa_mod.functions.size())
            return;
        auto& soa_fn = entry.soa_mod.functions[func_idx];
        // Result blocks: the blocks holding Return ops — the same
        // positions the dirty path marks; fall back to the function's
        // last block when no Return is mapped.
        std::vector<std::uint32_t> result_blocks;
        if (func_idx < entry.irs.size()) {
            const auto& ir_fn = entry.irs[func_idx];
            for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                for (const auto& instr : ir_fn.blocks[bi].instructions) {
                    if (instr.opcode == aura::ir::IROpcode::Return) {
                        result_blocks.push_back(static_cast<std::uint32_t>(bi));
                        break;
                    }
                }
            }
        }
        if (result_blocks.empty() && !soa_fn.blocks_.empty())
            result_blocks.push_back(static_cast<std::uint32_t>(soa_fn.blocks_.size() - 1));
        for (const auto bi : result_blocks) {
            if (bi >= soa_fn.blocks_.size())
                continue;
            const auto& blk = soa_fn.blocks_[bi];
            for (auto ci = blk.start_idx; ci < blk.end_idx && ci < soa_fn.shape_ids_.size(); ++ci) {
                if (soa_fn.shape_ids_[ci] == 0)
                    soa_fn.shape_ids_[ci] = sid;
            }
        }
        metrics_.shape_ids_sync_hits.fetch_add(1, std::memory_order_relaxed);
    }

    // Invalidate shape profile for a function (called after mutate:*).
    void invalidate_shape(const std::string& name) {
        // Issue #4568: drop this define's feedback (and name#* rows).
        // No synthetic SHAPE_INT seed. Other names stay.
        aura::compiler::feedback::reset_name(name);
        auto fn_key = shape::make_fn_key(session_id_, name);
        // Issue #4567: do not seed SHAPE_INT when the profile is empty.
        const bool had_profile = shape_profiler_.has_profile(fn_key);
        const bool needs_deopt = shape_profiler_.invalidate(fn_key);
        // Issue #4510: a define whose shape was never tracked (tier-JIT
        // faces never call record_shape) must still run the invalidate arm —
        // otherwise a pre-invalidate native, the peer soft-stale bits and
        // the reload proof all survive the shape flip.
        if (!had_profile)
            on_shape_deopt_hook(fn_key, /*version=*/0, shape::kShapeDirtyScopeInvalidate);
        bool in_jit_cache = false;
        {
            std::shared_lock cache_read(jit_cache_mtx_);
            in_jit_cache = jit_cache_.contains(name);
        }
        if (needs_deopt || in_jit_cache) {
            jit_.invalidate(name.c_str());
            shape_jit_pass::record_incremental_recompile_hit();
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                if (jit_cache_.erase(name) > 0)
                    metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Issue #570: ShapeProfiler stability observability accessors.
    [[nodiscard]] std::uint64_t get_shape_stability_hit_count() const noexcept {
        return shape::shape_stability_hit_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_shape_version_bump_count() const noexcept {
        return shape::shape_version_bump_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_shape_fiber_refresh_count() const noexcept {
        return shape::shape_fiber_refresh_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t get_mutation_shape_churn_count() const noexcept {
        return shape::mutation_shape_churn_count.load(std::memory_order_relaxed);
    }
    void bump_shape_fiber_refresh() noexcept { shape::record_shape_fiber_refresh(); }

    // Issue #2823: policy only — true when a fiber should yield between
    // pass stages. The cooperative yield itself is pipeline_fiber_yield_action
    // (invoked by run_one when this returns true).
    static bool pipeline_yield_trampoline() noexcept {
        return aura::serve::g_current_fiber != nullptr;
    }

    // Issue #2823: perform cooperative PassPipeline yield (action half).
    static void pipeline_fiber_yield_action() noexcept {
        if (!aura::serve::g_current_fiber)
            return;
        aura::serve::Fiber::yield(aura::serve::YieldReason::PassPipeline);
    }

    // Issue #657 / #984: lowering + JIT compiler-core incremental hooks.
    // install sets thread_local hooks; clear must be called on session
    // teardown / reset so cross-session routing cannot see stale hooks.
    void install_lowering_compiler_core_hooks() noexcept {
        LoweringObservabilityHooks hooks;
        hooks.bridge_epoch_capture = bridge_epoch();
        hooks.on_bridge_epoch_sync = &CompilerService::lowering_bridge_sync_trampoline;
        hooks.on_linear_metadata_flow = &CompilerService::lowering_linear_metadata_trampoline;
        hooks.on_env_version_resync = &CompilerService::lowering_env_version_resync_trampoline;
        hooks.on_quote_lambda_bridge_copy =
            &CompilerService::lowering_quote_lambda_bridge_trampoline;
        hooks.impact_func_indices =
            pending_impact_func_indices_.empty() ? nullptr : &pending_impact_func_indices_;
        set_lowering_observability_hooks(hooks);
    }
    void clear_lowering_compiler_core_hooks() noexcept { clear_lowering_observability_hooks(); }

    static void lowering_bridge_sync_trampoline() noexcept {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return;
        static_cast<CompilerService*>(raw)->evaluator_.bump_compiler_core_bridge_epoch_sync();
    }

    static void lowering_linear_metadata_trampoline() noexcept {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return;
        static_cast<CompilerService*>(raw)->evaluator_.bump_compiler_core_linear_metadata_flow();
    }

    static void lowering_env_version_resync_trampoline() noexcept {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return;
        static_cast<CompilerService*>(raw)
            ->evaluator_.bump_incremental_closure_env_version_resync();
    }

    static void lowering_quote_lambda_bridge_trampoline() noexcept {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return;
        auto* self = static_cast<CompilerService*>(raw);
        self->metrics_.incremental_closure_quote_lambda_stale_prevented_total.fetch_add(
            1, std::memory_order_relaxed);
        self->evaluator_.bump_incremental_closure_quote_lambda_stale_prevented();
    }

    static void jit_unhandled_invalidate_trampoline(const char* fn_name) noexcept {
        (void)fn_name;
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return;
        static_cast<CompilerService*>(raw)
            ->evaluator_.bump_compiler_core_jit_unhandled_invalidate();
    }

    // Issue #3271: ShapeProfiler dirty hook trampoline (C fn ptr; no std::function).
    static void shape_dirty_hook_trampoline(shape::FnKey fn_key,
                                            std::uint32_t dirty_scope) noexcept {
        (void)dirty_scope;
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return;
        static_cast<CompilerService*>(raw)->mark_shape_dirty_for_fn_key(fn_key);
    }

    // Issue #492: ShapeProfiler deopt hook trampoline (C fn ptr).
    static void shape_deopt_hook_trampoline(shape::FnKey fn_key, std::uint64_t version,
                                            std::uint32_t dirty_scope) noexcept {
        auto* raw = aura::messaging::g_current_compiler_service;
        if (!raw)
            return;
        static_cast<CompilerService*>(raw)->on_shape_deopt_hook(fn_key, version, dirty_scope);
    }

    // Issue #492/#1521: JIT/cache eviction + observability on shape deopt.
    // dirty_scope:
    //   1 = stability loss (mutation churn) → storm tally + full JIT evict
    //   2 = explicit invalidate → full JIT evict
    //   3 = ArenaCompact → selective mark-dirty + epoch bump; no storm tally
    void on_shape_deopt_hook(shape::FnKey fn_key, std::uint64_t version,
                             std::uint32_t dirty_scope) noexcept;

    // Register evaluator primitives with JIT runtime
    void register_jit_primitives() {
        // Set the global primitives pointer for the JIT dispatcher
        if (prev_jit_prim_ctx_ == nullptr)
            prev_jit_prim_ctx_ = g_jit_prim_ctx.load(std::memory_order_acquire);
        if (prev_jit_prim_owner_ == nullptr)
            prev_jit_prim_owner_ = g_jit_prim_owner.load(std::memory_order_acquire);
        g_jit_prim_ctx.store(&evaluator_.primitives(), std::memory_order_release);
        // Issue #3593: wire the owner Evaluator — aura_jit_prim_dispatch
        // routes through it (invoke_prim_with_telemetry choke point).
        g_jit_prim_owner.store(&evaluator_, std::memory_order_release);

        // Issue #452: wire AOT bridge metrics pointer so
        // aura_reload_aot_module can bump the
        // aot_stale_reject_count_, aot_region_mismatch_,
        // aot_hot_update_success_ counters exposed via
        // (query:aot-stats). Lifetime is tied to the
        // service (well-defined teardown order).
        aura_set_aot_metrics(&metrics_);
        // Issue #1522: re-bind batch_deopt target (lazy JIT prims path).
        aura_set_jit_batch_deopt_target(&jit_);
        // Issue #1540 / #1545: re-bind linear hooks (same stack as ctor).
        if (g_linear_hook_eval_stack.empty() || g_linear_hook_eval_stack.back() != &evaluator_)
            g_linear_hook_eval_stack.push_back(&evaluator_);
        install_linear_hooks_for_eval(&evaluator_);

        // Issue #157 Phase 1: bind the lock hooks. Pattern matches
        // the g_prim_dispatcher pattern above — the runtime bridges
        // (aura_alloc_pair, aura_pair_car, aura_prim_call, etc.) call
        // into the evaluator's lock + version methods via this hook
        // table. Without these hooks, the bridges are no-ops
        // (single-threaded default) and the bypass is unsafe.
        if (g_lock_hook_eval_stack.empty() || g_lock_hook_eval_stack.back() != &evaluator_)
            g_lock_hook_eval_stack.push_back(&evaluator_);
        install_lock_and_topcell_hooks_for_eval(&evaluator_);

        // Issue #3593: wire the C ABI dispatcher in ALL builds — the
        // telemetry choke point (invoke_prim_with_telemetry) is LLVM-free,
        // and an unarmed dispatcher silently bypasses it in non-LLVM
        // (light / embedded) builds. Hash callbacks stay LLVM-gated below.
        // aura_jit_prim_dispatch is defined at file scope (after imports)
        // and aura_set_prim_dispatcher is declared at file scope.
        aura_set_prim_dispatcher(aura_jit_prim_dispatch);

#ifdef AURA_HAVE_LLVM

        // Register the hash operation dispatchers (hash-ref, hash-set!, hash-remove!)
        // These are separate from the prim dispatcher because hash ops have dedicated
        // IROpcodes (OpHashRef/OpHashSet/OpHashRemove) for inline dispatch.

        aura_set_hash_str_eq_callback(aura_hash_string_cmp_fn);
        aura_set_hash_str_convert_callback(aura_hash_string_convert_fn);

#endif
    }

    // ── Messaging (P14) ───────────────────────────────────────
    int wake_eventfd_ = -1;
    std::vector<std::pair<std::string, std::string>> mailbox_; // (sender, msg)
    std::string last_sender_;
    std::string session_id_;
    std::unique_ptr<std::function<bool(const std::string&, const std::string&)>> msg_send_fn_;
    std::unique_ptr<std::function<std::optional<std::string>(int)>> msg_recv_fn_;
    std::unique_ptr<std::function<std::string()>> msg_id_fn_;

    // ── Shape profiler (Phase 1, #53) ─────────────────────────
    shape::ShapeProfiler shape_profiler_;
    // Issue #62 Iter 1: observability counters. Thread-safe
    // (atomics). Surfaced via `metrics()` accessor for --evo-explain
    // and AuraQuery (:deopt-count, :arena, etc.).
    //
    // Issue #402: `mutable` so const methods (e.g.
    // needs_tree_walker_fallback) can bump atomics for
    // observability. The metrics_ struct is observability-only
    // and doesn't represent logical compiler state — callers
    // that read it via get_*_count() accept that the value
    // may change under them.
    mutable CompilerMetrics metrics_;
    // Issue #2304: post-bump epoch invariant walk infrastructure.
    //   epoch_invariant_violation_total_: counter bumped by
    //     run_epoch_invariant_if_enabled() on each detected
    //     stale entry after atomic_bump_epochs_and_stamp_bridge.
    //   epoch_invariant_hard_enabled_: flag — when true, the walk
    //     actually runs and reports violations via the counter;
    //     when false (production default), the helper returns
    //     immediately after a single relaxed load (AC3 zero-cost).
    //   epoch_invariant_walks_total_: counter bumped on each
    //     completed walk (regardless of violation count), so
    //     operators can confirm the invariant-on path is reachable
    //     in production (vs always-off via env).
    //   All atomics are file-local — exposed to C-linkage readers
    //   in aura_jit_bridge.cpp + to tests via public getter methods.
    std::atomic<std::uint64_t> epoch_invariant_violation_total_{0};
    std::atomic<std::uint8_t> epoch_invariant_hard_enabled_{0};
    std::atomic<std::uint64_t> epoch_invariant_walks_total_{0};
    // Issue #2501: additive breakdown counters (subset of violations).
    std::atomic<std::uint64_t> epoch_invariant_slot_stale_total_{0};
    std::atomic<std::uint64_t> epoch_invariant_closure_must_deopt_total_{0};

public:
    // Issue #2304 / #2366: public setters for epoch invariant mode.
    // hard_enabled(true) → hard (mode 2); soft via set_epoch_invariant_mode.
    // Also mirrors process-level C API so env + tests agree.
    void set_epoch_invariant_hard_enabled(bool on) noexcept {
        epoch_invariant_hard_enabled_.store(on ? 1 : 0, std::memory_order_relaxed);
        aura_set_epoch_invariant_mode(on ? 2 : 0);
    }
    void set_epoch_invariant_mode(int mode) noexcept {
        if (mode < 0)
            mode = 0;
        if (mode > 2)
            mode = 2;
        epoch_invariant_hard_enabled_.store(mode >= 2 ? 1 : 0, std::memory_order_relaxed);
        aura_set_epoch_invariant_mode(mode);
    }
    [[nodiscard]] std::uint64_t epoch_invariant_violation_total() const noexcept {
        return epoch_invariant_violation_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t epoch_invariant_walks_total() const noexcept {
        return epoch_invariant_walks_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t epoch_invariant_slot_stale_total() const noexcept {
        return epoch_invariant_slot_stale_total_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t epoch_invariant_closure_must_deopt_total() const noexcept {
        return epoch_invariant_closure_must_deopt_total_.load(std::memory_order_relaxed);
    }
    // Issue #1431 follow-up: serialize the entire eval pipeline
    // across threads. Race #1 (TypeRegistry thread-safety) and
    // Race #2 (FlatAST add_node) were fixed with granular locks,
    // but each fix exposed the next race in the
    // parser/typecheck/eval pipeline. Race #3 (StringPool rehash
    // null memory_resource via ASTArena::create) is fixed by
    // serializing eval at the CompilerService entry. The push_back
    // lock inside the Evaluator (alloc_storage_lock_ tested by
    // test_stress_alloc_storage_lock) is unaffected — it protects
    // cells_/pairs_/string_heap_ writes during eval, while
    // eval_mutex_ protects the parse + typecheck setup that runs
    // before those writes. Heavy-handed but ship-fast: see
    // issue #1431 for the architectural follow-up.
    // recursive: Path B re-enters eval() for non-literal args.
    mutable std::recursive_mutex eval_mutex_;
    // Issue #684: pending SoA snapshot consumed by store_define_v2.
    std::optional<LowerSoAEmitSnapshot> pending_soa_snapshot_;
    // ── Speculative JIT controller (Phase 2, #53) ──────────────
    shape::SpecJITController spec_jit_{jit_};
    // Issue #2576-lifecycle: storm listener registration id so ~CompilerService
    // can unregister (the listener captures `this`; leaving it registered after
    // teardown makes any later deopt storm UAF into the freed controller).
    std::uint64_t storm_listener_id_{0};

    // ── Static registry ──────────────────────────────────────
    // Using Scott Meyer's singletons to avoid ODR issues with module static members
    static std::unordered_map<std::string, CompilerService*, aura::core::TransparentStringHash,
                              std::equal_to<>>&
    registry() {
        static std::unordered_map<std::string, CompilerService*, aura::core::TransparentStringHash,
                                  std::equal_to<>>
            reg;
        return reg;
    }
    static std::mutex& registry_mtx() {
        static std::mutex mtx;
        return mtx;
    }

    // Issue #1385: snapshot env_frames_ + arena metrics into the
    // CompilerMetrics struct. Called by the (compiler:metrics)
    // primitive before serializing. Does NOT touch jit / fiber /
    // shape counters — those have their own update paths.
    void refresh_env_arena_metrics(CompilerMetrics& m) const {
        evaluator_.refresh_env_arena_metrics(m);
        m.ast_arena_bytes_in_use.store(arena_.used(), std::memory_order_relaxed);
        m.ast_arena_upstream_bytes.store(
            arena_upstream_mr_.bytes_allocated.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
    }

    // Issue #1385: const accessor for the arena's upstream
    // CountingMR (used by tests).
    [[nodiscard]] const CountingMR& arena_upstream_for_test() const noexcept {
        return arena_upstream_mr_;
    }
};


extern "C" std::int64_t aura_hash_string_convert_fn(std::int64_t jit_key) {
    // Convert a JIT string pool key to evaluator string heap key.
    // Issue #181 Cycle 2: v2 string encoding.
    // JIT key: STRING_BIAS_VAL_2 - (jit_pool_idx << 2)
    // Evaluator key: STRING_BIAS_VAL_2 - (eval_heap_idx << 2)
    using aura::compiler::types::is_string_raw_v2;
    using aura::compiler::types::make_string_raw_v2;
    using aura::compiler::types::string_idx_raw_v2;
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    if (!prims)
        return 0;
    if (!is_string_raw_v2(jit_key))
        return 0;
    auto& sh = const_cast<aura::compiler::Primitives*>(prims)->string_heap();
    auto idx = static_cast<std::size_t>(string_idx_raw_v2(jit_key));
    // Get JIT string content via aura_jit_pool_string
    const char* content = aura_jit_pool_string(idx);
    if (!content)
        return 0;
    // Push into evaluator string heap
    auto new_idx = sh.mint(content ? content : "");
    return make_string_raw_v2(static_cast<std::uint64_t>(new_idx));
}

extern "C" std::int64_t aura_hash_string_cmp_fn(std::int64_t stored_key, std::int64_t search_key) {
    using aura::compiler::types::is_string_raw_v2;
    using aura::compiler::types::string_idx_raw_v2;
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    if (!prims)
        return 0;
    if (!is_string_raw_v2(stored_key) || !is_string_raw_v2(search_key))
        return 0;
    auto& sh = const_cast<aura::compiler::Primitives*>(prims)->string_heap();
    auto ai = static_cast<std::size_t>(string_idx_raw_v2(stored_key));
    auto bi = static_cast<std::size_t>(string_idx_raw_v2(search_key));
    if (ai >= sh.size() || bi >= sh.size())
        return 0;
    return (sh[ai] == sh[bi]) ? 1 : 0;
}

} // namespace aura::compiler
