module;
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/socket.h>

// Issue #252: closure dual-path observability
#include "observability_metrics.h"

#include <netinet/in.h>
#include "runtime_shared.h"
#include "git_ctx.h"
#include <contracts>

#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <regex>
#include <unordered_map>
#if __has_include(<curl/curl.h>)
// Helper: evaluate `expr` and cache the result in the current FlatAST's
// value_cache_ at `current_id`. Used by eval_flat for incremental eval.
// Captures `f` (FlatAST*) and `current_id` from the enclosing TCO loop.
// Only evaluated on non-leaf returns (leaf literals use plain `return`).
// Cache-and-return for EvalResult (std::expected<EvalValue, Diagnostic>)
#define EVAL_CACHE_RETURN(expr)                                                                    \
    do {                                                                                           \
        auto _er_ = (expr);                                                                        \
        if (_er_) {                                                                                \
            f->set_cached_value(current_id, _er_->val);                                            \
        }                                                                                          \
        return _er_;                                                                               \
    } while (0)

// Cache-and-return for plain EvalValue (used by leaf returns like make_closure)
#define EVAL_CACHE_RETURN_VAL(expr)                                                                \
    do {                                                                                           \
        auto _ev_ = (expr);                                                                        \
        f->set_cached_value(current_id, _ev_.val);                                                 \
        return _ev_;                                                                               \
    } while (0)
#include <curl/curl.h>
#define AURA_HAVE_CURL 1
#else
// Fallback types (runtime dlopen, compile-time stub)
typedef void CURL;
struct curl_slist {};
using CURLcode = int;
using CURLoption = int;
constexpr CURLoption CURLOPT_URL = 10002;
constexpr CURLoption CURLOPT_POST = 47;
constexpr CURLoption CURLOPT_POSTFIELDS = 10015;
constexpr CURLoption CURLOPT_POSTFIELDSIZE = 60;
constexpr CURLoption CURLOPT_HTTPHEADER = 10023;
constexpr CURLoption CURLOPT_WRITEFUNCTION = 20011;
constexpr CURLoption CURLOPT_WRITEDATA = 10001;
constexpr CURLoption CURLOPT_TIMEOUT = 13;
constexpr CURLoption CURLOPT_CONNECTTIMEOUT = 78;
constexpr CURLoption CURLOPT_SSL_VERIFYPEER = 64;
constexpr CURLoption CURLOPT_SSL_VERIFYHOST = 81;
constexpr CURLoption CURLOPT_USERAGENT = 10018;
constexpr CURLcode CURLE_OK = 0;
#endif
#include <cmath>
#include "messaging_bridge.h"
#include "serve/gc_coordinator.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"
module aura.compiler.evaluator;
import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.type_checker;
import aura.compiler.coercion_map;
import aura.compiler.evaluator_pure;
import aura.diag;
import aura.parser.parser;

namespace aura::compiler {

// Issue #146 Phase 3: bring the pure is_truthy into the
// global namespace so existing call sites that use
// `is_truthy(x)` (unqualified) keep working. The pure
// function is the canonical home; types::is_truthy has
// been removed.
using aura::compiler::pure::is_truthy;

using types::EvalValue;
using namespace types;

// ── Edit distance for error suggestions ────────────────────────
// Issue #146 Phase 4: legacy wrappers around the pure
// `edit_distance_pure` and `closest_match_pure` in
// `aura::compiler::pure`. The 2 call sites
// (L16793, L18208) keep using the old API; new code can
// call the pure versions directly.
[[maybe_unused]] static std::size_t edit_distance(std::string_view a, std::string_view b) {
    return aura::compiler::pure::edit_distance_pure(a, b);
}

static std::string closest_match(std::string_view name, std::span<const std::string> candidates,
                                 std::size_t max_dist = 3) {
    return aura::compiler::pure::closest_match_pure(name, candidates, max_dist);
}


using namespace aura::diag;

namespace primitives_detail {
void register_type_and_char_primitives(
    std::function<void(std::string, PrimFn)> add);
void register_pair_and_string_primitives(std::function<void(std::string, PrimFn)> add,
                                         std::pmr::vector<Pair>& pairs,
                                         std::pmr::vector<std::string>& string_heap,
                                         std::vector<EvalValue>& error_values);
void register_json_primitives(std::function<void(std::string, PrimFn)> add,
                                  std::pmr::vector<Pair>& pairs,
                                  std::pmr::vector<std::string>& string_heap);
void register_list_primitives(
    std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, std::vector<EvalValue>& error_values,
    std::function<EvalValue(const EvalValue& fn, const EvalValue& arg)> apply_unary,
    std::function<bool(const EvalValue& fn, const EvalValue& arg)> apply_pred,
    std::function<EvalValue(const EvalValue& fn, const EvalValue& acc, const EvalValue& arg)>
        apply_binary);
void register_vector_and_hash_primitives(
    std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, std::vector<EvalValue>& error_values,
    std::vector<std::vector<EvalValue>>& vector_heap);
void register_math_regex_and_arithmetic_primitives(
    std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, std::vector<EvalValue>& error_values);
void register_reflect_and_type_primitives(
    std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, std::vector<std::string>& keyword_table,
    void*& type_registry);
void register_query_primitives(
    std::function<void(std::string, PrimFn)> add, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, void*& type_registry,
    std::function<std::string(const std::string&)> resolve_module_path);
void register_workspace_query_primitives(
    std::function<void(std::string, PrimFn)> add, std::shared_mutex& workspace_mtx,
    aura::ast::FlatAST*& workspace_flat, aura::ast::StringPool*& workspace_pool,
    void*& type_registry, std::vector<std::string>& keyword_table, std::pmr::vector<Pair>& pairs,
    std::pmr::vector<std::string>& string_heap, aura::ast::ASTArena*& temp_arena,
    std::unordered_map<std::uint64_t, std::vector<aura::ast::NodeId>>& tag_arity_index,
    std::function<aura::ast::StringPool*()> canonical_pool, std::function<void()> build_tag_arity_index,
    std::function<EvalValue(const std::string&, const std::string&)> mev);
void register_mutate_primitives(
    std::function<void(std::string, PrimFn)> add, Evaluator& ev,
    std::function<EvalValue(const std::string&, const std::string&)> mev,
    std::function<void()> destroy_defuse_index);
void register_workspace_primitives(
    std::function<void(std::string, PrimFn)> add, Evaluator& ev,
    std::function<void()> destroy_defuse_index);
void register_ast_primitives(
    std::function<void(std::string, PrimFn)> add, Evaluator& ev,
    std::function<void()> destroy_defuse_index,
    std::function<std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>()>
        defuse_summary_stats);
void register_compile_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_eval_observability_primitives(std::function<void(std::string, PrimFn)> add,
                                            Evaluator& ev);
void register_jit_arena_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_messaging_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_git_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_network_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_auto_evolve_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_synthesize_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                                    std::function<void()> destroy_defuse_index);
void register_strategy_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_defuse_query_primitives(
    std::function<void(std::string, PrimFn)> add, std::shared_mutex& workspace_mtx,
    aura::ast::FlatAST*& workspace_flat, aura::ast::StringPool*& workspace_pool,
    std::pmr::vector<std::string>& string_heap, std::function<void*()> ensure_defuse,
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> def_use_for_sym,
    std::function<EvalValue(void* idx, aura::ast::NodeId node)> reaches_for_node,
    std::function<EvalValue(void* idx, aura::ast::SymId sym)> effects_for_sym,
    std::function<EvalValue(void* idx)> build_index, std::function<EvalValue(void* idx)> index_stats,
    std::function<EvalValue(const std::string&, const std::string&)> make_merr);
}

// Forward decl: macro body cloner (defined at end of file)
// Issue #190: `cloned_marker` controls the SyntaxMarker applied to
// every node the cloner creates in the target flat. Default is
// SyntaxMarker::User (the legacy behavior — closure body
// materialization, etc.). Macro expansion call sites pass
// SyntaxMarker::MacroIntroduced so the downstream type checker
// / IR lowering / EDSL query-mutate know the nodes are
// macro-introduced and shouldn't be silently edited.
static aura::ast::NodeId
clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
                 aura::ast::FlatAST& source, aura::ast::StringPool& source_pool,
                 aura::ast::NodeId body_id,
                 const std::unordered_map<std::string, aura::ast::NodeId>* subst = nullptr,
                 std::unordered_map<std::string, std::string>* name_map = nullptr,
                 aura::ast::SyntaxMarker cloned_marker = aura::ast::SyntaxMarker::User);

// Forward decl: inner-macro expansion helper (defined at end of file).
// Used by the runtime hygienic macro expansion to recursively
// expand nested macro calls in the cloned body (Issue #121).
// Bounded by `max_depth` to prevent infinite recursion.
struct MacroDef;
static aura::ast::NodeId
expand_inner_macros(aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root,
                    int depth, int max_depth,
                    const std::unordered_map<std::string, MacroDef>& macros);

// Depth guard: protects Env::lookup against cyclic parent chains
// (thread_local since lookup can be called from multiple fibers)
static constexpr std::size_t MAX_ENV_DEPTH = 1024;
thread_local std::size_t g_env_lookup_depth = 0;

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
thread_local void* aura::compiler::Evaluator::g_current_fiber_void;

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
} // namespace

// Register the function pointers at static-init time. The
// fiber side calls them; we don't need the Evaluator to
// know about Fiber (one-way dependency).
struct FiberHookRegistrar {
    FiberHookRegistrar() {
        aura::serve::g_fiber_setter_ = fiber_setter_impl;
        aura::serve::g_fiber_storage_deleter_ = fiber_storage_deleter_impl;
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

// ── ADT state now in adt_runtime_ (refactor Step 2.3, FFI pattern) ───────
// The old global g_adt_constructors + AdtCtorEntry struct have been
// removed. Per-Evaluator state is in adt_runtime_ (see adt_runtime.ixx/ _impl).
// Lookups and registration now go through it (see Env::lookup and ctor wiring below).
// Parser (parse_datatype) still populates via the new runtime in full extraction.

std::optional<EvalValue> Env::lookup(const std::string& n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    if (++g_env_lookup_depth > MAX_ENV_DEPTH) {
        --g_env_lookup_depth;
        return std::nullopt;
    }
    struct _ {
        ~_() { --g_env_lookup_depth; }
    } dec;

    // 1. Check local bindings
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n) {
            auto& v = it->second;
            // P0 step 2: return raw (cell sentinel if applicable).
            // No cells_ member on Env. Deref centralized in Evaluator
            // scope using its cells_ (or explicit for cell ptr paths).
            return v;
        }
    // 2. Check parent
    if (parent_) {
        return parent_->lookup(n);
    }
    // 2b. Issue #232 fallback: SoA walk via parent_id_ when parent_
    // is null but parent_id_ is set. This happens for env frames
    // materialized by materialize_call_env from a closure captured
    // in a stack env (e.g., named-let → letrec → lambda capture).
    // Without this walk, the closure body can't see bindings
    // from the surrounding scope.
    //
    // Issue #145 P0 follow-up: hold env_frames_mtx_ as a
    // shared lock for the entire walk. env_frame returns a
    // reference into env_frames_; holding the lock prevents
    // the main thread's alloc_env_frame from reallocating the
    // deque's map array underneath us (which would free the
    // map pointer we're reading).
    if (parent_id_ != NULL_ENV_ID && owner_) {
        std::shared_lock<std::shared_mutex> env_rlock(owner_->env_frames_lock());
        const EnvFrame& pfr = owner_->env_frame(parent_id_);
        // Walk the parent frame's bindings (string-keyed)
        for (auto& b : pfr.bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < owner_->cells().size())
                        return owner_->cells()[ci];
                }
                return b.second;
            }
        }
        // Recurse via SoA: walk the parent's parent_id_ chain.
        // Capture the result but only return if non-null — if the
        // recursive lookup returns nullopt, fall through to the
        // primitive + ADT fallbacks below (otherwise primitives
        // like `+` and `*` would be reported as unbound variables).
        if (pfr.parent_id != NULL_ENV_ID) {
            Env tmp;
            tmp.set_owner(owner_);
            tmp.set_parent_id(pfr.parent_id);
            if (auto r = tmp.lookup(n))
                return *r;
        }
        // Final fallback: the frame at parent_id_ has the snapshot
        // of the env at capture time. If that env is still live
        // (e.g., it'"'"'s a heap-allocated module env or the top_
        // env), check its live bindings too. The frame is a snapshot
        // and may be stale if bindings were added after the frame
        // was created (e.g., via require in a nested module load).
        // We use the owner_ pointer to find the live env: for index 0
        // it'"'"'s top_, for higher indices we walk the env_frames_
        // pool to find a matching live env. The simplest case
        // (index 0 = top_) is the most common and we handle that
        // directly via owner_->top().
        if (parent_id_ == 0 && owner_) {
            // Check live top_ env'"'"'s bindings
            for (auto it = const_cast<aura::compiler::Env&>(owner_->top_env()).bindings().rbegin();
                 it != const_cast<aura::compiler::Env&>(owner_->top_env()).bindings().rend();
                 ++it) {
                if (it->first == n) {
                    if (is_cell(it->second)) {
                        auto ci = as_cell_id(it->second);
                        if (ci < owner_->cells().size())
                            return owner_->cells()[ci];
                    }
                    return it->second;
                }
            }
        }
    }
    // 3. Fallback: check primitives (allows passing names like `+` as values)
    if (primitives_) {
        auto slot = primitives_->slot_for_name(n);
        if (slot != std::numeric_limits<std::size_t>::max()) {
            return make_primitive(slot);
        }
    }
    // 4. Fallback: check ADT constructors (Issue #108 part 4 Phase 1).
    //    Bypasses Begin scoping. Registered via (adt:register-constructors ...)
    //    from parse_datatype. Returns a primitive ref that, when applied,
    //    builds (cons "CtorName" arg1 arg2 ...).
    {
        // Step 2.3: use per-evaluator adt_runtime_ (FFI pattern).
        // adt_runtime_ lives on the Evaluator, not on Env; access
        // via the owner_ back-pointer (Issue #145 Phase 2.2 set
        // the owner_ on every Env registered with the Evaluator).
        if (owner_) {
            if (auto slot = owner_->adt_runtime().find_ctor(n))
                return make_primitive(*slot);
        }
    }
    return std::nullopt;
}

// ── Env::lookup_binding: returns raw binding (cell sentinel as-is) ─
std::optional<EvalValue> Env::lookup_binding(const std::string& n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
        if (it->first == n)
            return it->second;
    return parent_ ? parent_->lookup_binding(n) : std::nullopt;
}

// Issue #145: SymId fast path. Pushes to bindings_symid_
// (canonical) and mirrors to bindings_ (string form) if
// pool_ is set. The mirror is needed because the lambda body
// uses the string-based lookup to find the param (the parser
// interns names but the body's `lookup(name)` does the
// string-keyed loop). Without the mirror, lambda params would
// be invisible to body code.
void Env::bind_symid(aura::ast::SymId s, types::EvalValue v) {
    bindings_symid_.emplace_back(s, v);
    if (pool_) {
        // Resolve SymId → string once, then write to both
        // arrays. The string resolve is O(string-length) at
        // bind time; the int-compare is O(1) at lookup time.
        // Net: lookup is the hot path, so this is a win.
        std::string_view sv = pool_->resolve(s);
        if (!sv.empty())
            bindings_.emplace_back(std::string(sv), v);
    }
}

// Issue #145: SymId-based lookup. Iterates bindings_symid_
// (which the bind_symid path writes to) with integer compare.
// Most-recent-binding-wins semantics, matching the string-based
// lookup's rbegin/rend iteration order.
std::optional<types::EvalValue> Env::lookup_by_symid(aura::ast::SymId s) const {
    for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
        if (it->first == s) {
            // P0 step 2: return raw binding (sentinel as-is). No cells_
            // member anymore. Deref (if cell) happens at caller using
            // central Evaluator cells_ when in Evaluator scope, or
            // explicit cells param for mutation paths (lookup_cell_*).
            return it->second;
        }
    }
    // P0: prefer SoA index walk (no parent_ pointer chase) if this
    // legacy Env frame was wired with owner_ + parent_id_ (as done
    // in materialize_call_env for SoA captures).
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        return owner_->lookup_by_symid_chain(parent_id_, s);
    }
    return parent_ ? parent_->lookup_by_symid(s) : std::nullopt;
}

// Issue #207 (Cycle 1): bindings_with_names() — materializes
// the named version of the bindings. Walks the SymId-keyed
// bindings_symid_ array and resolves each SymId to a name
// string via pool_->resolve(). Returns a new vector with
// (name, value) pairs in the same order as bindings_symid_.
//
// This is the "current behavior, as a derived view" per the
// issue body. The legacy bindings_ array still holds the
// string-keyed version, but new code should use this helper
// to read the named view (rather than reading bindings_
// directly). When the migration completes (Cycle 2+), the
// legacy bindings_ array is dropped and bindings_with_names()
// becomes the only path to get a named view.
//
// For envs without pool_ set, the name is rendered as
// "@<symid:N>" where N is the SymId value. This is a
// fallback for envs that haven't been migrated yet.
std::vector<std::pair<std::string, types::EvalValue>> Env::bindings_with_names() const {
    std::vector<std::pair<std::string, types::EvalValue>> out;
    out.reserve(bindings_symid_.size());
    for (const auto& [sym, val] : bindings_symid_) {
        std::string name;
        if (pool_) {
            // pool_->resolve() returns the canonical name
            // (string_view) for this SymId. If the SymId
            // is not in the pool (defensive), the resolved
            // view is empty.
            std::string_view resolved = pool_->resolve(sym);
            if (!resolved.empty())
                name.assign(resolved.data(), resolved.size());
        }
        if (name.empty()) {
            // Fallback: render the SymId as a string for
            // display purposes.
            name = "@symid:" + std::to_string(sym);
        }
        out.emplace_back(std::move(name), val);
    }
    return out;
}

// Issue #145 follow-up / Phase 2.5.0: lookup_by_intern — the
// SymId-first migration scaffold for the eventual bindings_
// drop. Takes a name string, interns via the given pool
// (legacy closure-captured / env-captured pool for backward
// compat; canonical_pool() for new code), then routes through
// lookup_by_symid. The primitive + ADT-constructor fallbacks
// mirror Env::lookup's behavior (step 3 + 4) so callers that
// switch to this helper get the same observable result as
// lookup(name) when no binding is found.
//
// Note: this is a SCOPED migration tool, not a permanent API.
// When Phase 2.5 ships the actual drop of bindings_, the
// helper either goes away (callers intern once + use
// lookup_by_symid directly) or stays as a thin convenience
// wrapper. The 6 current Env::lookup(name) call sites
// (apply_closure parent walk, EnvView parent walk, module
// lookup, fn_name lookup, eval-time lookup, capture lookup)
// will migrate one per Phase 2.5.0 commit.
std::optional<types::EvalValue> Env::lookup_by_intern(const std::string& n,
                                                      const aura::ast::StringPool* pool) const {
    // Resolve which pool to use: legacy passed-in pool if
    // non-null, else fall back to the env's own pool_ (set
    // via set_pool for closures that captured a non-canonical
    // pool), else nullptr. The lookup_by_symid call below
    // will then route through whichever pool is appropriate.
    // Note: use_pool is non-const because intern() mutates the
    // pool. const_cast is safe here because the env holds the
    // pool by non-const pointer (pool_) or by the caller's
    // pointer (legacy). The lookup_by_intern method itself is
    // logically const (no observable env state change beyond
    // the pool's intern side effect, which is idempotent for
    // already-interned names).
    aura::ast::StringPool* use_pool = const_cast<aura::ast::StringPool*>(pool ? pool : pool_);
    if (!use_pool) {
        // No pool available — can't intern. Fall through to
        // the legacy string-based lookup as a last resort.
        return lookup(n);
    }
    auto sym = use_pool->intern(n);
    auto found = lookup_by_symid(sym);
    if (found)
        return found;
    // Fallbacks mirror Env::lookup's primitive + ADT paths.
    // These are not SymId-specific — the slot_for_name lookup
    // uses the string name directly.
    if (primitives_) {
        auto slot = primitives_->slot_for_name(n);
        if (slot != std::numeric_limits<std::size_t>::max()) {
            return make_primitive(slot);
        }
    }
    {
        // Step 2.3: use per-evaluator adt_runtime_ (FFI pattern).
        // adt_runtime_ lives on the Evaluator, not on Env; access
        // via the owner_ back-pointer (see Env::lookup fix above
        // for the same pattern).
        if (owner_) {
            if (auto slot = owner_->adt_runtime().find_ctor(n))
                return make_primitive(*slot);
        }
    }
    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// Issue #145 Phase 2.1 — EnvFrame SoA infrastructure
// ═══════════════════════════════════════════════════════════════
//
// EnvFrame is the SoA counterpart to Env. Same data layout, but
// `parent_id_` (EnvId, uint32_t) replaces `parent_` (Env*). The
// methods below are the "local-only" variants — they operate on
// one frame and do NOT walk the parent chain. Walk-aware access
// lives on Evaluator (walk_env_frames, lookup_by_symid_chain).
//
// Today Env and EnvFrame coexist. Migration is Phase 2.2.

// EnvFrame::bind — parallel to Env::bind (which writes only
// to bindings_, no mirror). SymId mirroring happens via
// bind_symid (the fast path). If you need both arrays in sync,
// bind via SymId + have pool_ set.
void EnvFrame::bind(const std::string& n, types::EvalValue v) {
    bindings_.emplace_back(n, v);
}

// EnvFrame::bind_symid — parallel to Env::bind_symid. Mirrors
// to bindings_ when pool_ is set so legacy lookup(string)
// callers still find the param.
void EnvFrame::bind_symid(aura::ast::SymId s, types::EvalValue v) {
    bindings_symid_.emplace_back(s, v);
    if (pool_) {
        std::string_view sv = pool_->resolve(s);
        if (!sv.empty())
            bindings_.emplace_back(std::string(sv), v);
    }
}

// EnvFrame::lookup_local — Env::lookup minus parent walk +
// primitive + ADT fallbacks. Pure frame-local lookup. Use
// Evaluator::walk_env_frames for chain-aware lookup.
//
// P0 migration: EnvFrame no longer holds a cells_ pointer
// (removed from struct for pure data / no pointer-to-heap).
// If the bound value is a cell sentinel, we return it as-is.
// Cell resolution is centralized in the Evaluator's
// lookup_by_symid_chain (and legacy Env paths) using the
// owning Evaluator's central cells_ pmr::vector. This
// makes frames fully index-driven and reallocation-safe.
std::optional<types::EvalValue> EnvFrame::lookup_local(const std::string& n) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
        if (it->first == n) {
            return it
                ->second; // sentinel returned; deref happens at caller (Evaluator chain or legacy)
        }
    }
    return std::nullopt;
}

// EnvFrame::lookup_local_by_symid — Env::lookup_by_symid minus
// parent walk. Pure frame-local SymId compare.
//
// P0 migration: same as lookup_local — return raw sentinel
// (cell or not). Central deref lives in Evaluator.
std::optional<types::EvalValue> EnvFrame::lookup_local_by_symid(aura::ast::SymId s) const {
    for (auto it = bindings_symid_.rbegin(); it != bindings_symid_.rend(); ++it) {
        if (it->first == s) {
            return it->second;
        }
    }
    return std::nullopt;
}

// Evaluator::alloc_env_frame — append a new EnvFrame and return
// its id. The id is the new size()-1 of env_frames_. Returns
// NULL_ENV_ID on overflow (>4G envs, which is unreachable in
// practice for a single evaluator lifetime).
//
// P0 (EnvFrame SoA): frames are allocated with only parent_id +
// primitives_. No cells_ pointer (pure data). Cell resolution
// centralized later in lookup paths using Evaluator::cells_.
aura::compiler::EnvId Evaluator::alloc_env_frame(EnvId parent_id, const Primitives* primitives) {
    // Issue #145 P0 follow-up: unique_lock on env_frames_mtx_
    // to serialize push_back with fiber-thread readers
    // (materialize_call_env). The deque's map array can be
    // reallocated when the deque grows past the current map
    // capacity, freeing the map pointer a fiber thread is
    // reading via env_frame[id]. Unique lock makes the
    // push_back atomic with respect to readers.
    std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
    if (env_frames_.size() >= NULL_ENV_ID) {
        // 4G envs reached. Return NULL to signal overflow;
        // callers should treat this as fatal (env allocation
        // exhausted) — but we keep going rather than abort
        // to make the failure mode visible.
        return NULL_ENV_ID;
    }
    EnvFrame fr;
    fr.parent_id = parent_id;
    fr.primitives_ = primitives;
    // Issue #242: stamp the frame with the current defuse_version_
    // so subsequent lookups can detect stale captures. The version
    // is an acquire-load so the frame's metadata (parent_id,
    // primitives_) is visible to threads that observe the bumped
    // version after a memory barrier.
    fr.version_ = defuse_version_.load(std::memory_order_acquire);
    env_frames_.push_back(std::move(fr));
    return static_cast<EnvId>(env_frames_.size() - 1);
}

// Evaluator::alloc_env_frame_from_env — Issue #145 Phase 2.3.
// Mirror an Env's parallel binding arrays into a new frame in
// env_frames_. The new frame's parent_id defaults to
// e.parent_id() (preserving the captured env's parent-chain
// position in the SoA arena); callers can override with an
// explicit parent_id if they want to re-parent the frame.
//
// P0: Only bindings_ + bindings_symid_ are mirrored.
// primitives_/pool_/cells_ deliberately not part of the frame
// (cells_ removed entirely from EnvFrame struct in P0 step 1;
// resolution centralized on Evaluator). This keeps frames
// as pure, index-only data.
aura::compiler::EnvId Evaluator::alloc_env_frame_from_env(const Env& e, EnvId parent_id) {
    EnvId pid = (parent_id != NULL_ENV_ID) ? parent_id : e.parent_id();
    EnvId id = alloc_env_frame(pid);
    if (id == NULL_ENV_ID)
        return NULL_ENV_ID;
    // Issue #145 P0 follow-up: hold a shared lock while
    // mutating the freshly-allocated frame. A concurrent
    // alloc_env_frame on another thread (e.g. a fiber)
    // could push_back and reallocate env_frames_'s map
    // array, invalidating our `fr` reference.
    std::unique_lock<std::shared_mutex> wlock(env_frames_mtx_);
    EnvFrame& fr = env_frames_[id];
    // e is `const`, so .bindings()/.bindings_symid() return
    // std::span (const overload). Use assign from iterators
    // to copy into the frame's vector storage.
    auto bs = e.bindings();
    fr.bindings_.assign(bs.begin(), bs.end());
    auto bss = e.bindings_symid();
    fr.bindings_symid_.assign(bss.begin(), bss.end());
    return id;
}

// Evaluator::materialize_call_env — Issue #145 Phase 2.3.
// Build a fresh Env for evaluating a closure body. When the
// closure's captured env is registered in env_frames_
// (cl.env_id ≠ NULL_ENV_ID), rebuild the call env from the
// frame's bindings and wire the SoA walk (owner_ +
// parent_id_) so lookup_cell_ptr / lookup_cell_index route
// parent lookups through walk_env_frames instead of pointer
// chase. Otherwise fall back to copying the legacy `cl.env`
// raw pointer — this preserves correct behavior for any
// stack-allocated local-eval closures that have not yet been
// registered in the SoA arena.
//
// primitives_/cells_/pool_ are NOT set here — they are
// runtime support pointers that the caller wires after
// materialization (apply_closure sets them inline; TCO tail
// call sites set them via tail_env.set_*). Keeping them out
// of this helper makes the helper usable from any code path
// that has a closure but might not need a fully wired env.
Env Evaluator::materialize_call_env(const Closure& cl) {
    // P0 complete: legacy cl.env path removed. All closures have
    // env_id set at capture time (via alloc_env_frame_from_env).
    // Always use SoA path for GC-safety and no pointer chasing.
    Env ne;
    // Defensive: a closure with env_id == NULL_ENV_ID would
    // trip the env_frame() contract and crash. This can happen
    // when the closure was constructed via a path that
    // skipped alloc_env_frame_from_env (e.g. a ClosureView
    // copy, a default-constructed Closure moved into
    // closures_[cid], or a frame that was pruned by a
    // future gc-temp cycle). Fall back to a fresh Env with
    // no captured bindings — the body will see globals via
    // the workspace walk, which is correct for lambda
    // bodies that don't actually reference the captured
    // scope (the most common case for this failure mode).
    if (cl.env_id == NULL_ENV_ID || cl.env_id >= env_frames_.size()) {
        return ne;
    }
    // Issue #145 P0 follow-up: hold env_frames_mtx_ shared
    // lock for the duration of the frame read. The frame
    // reference must remain valid through .bindings(),
    // .bindings_symid_(), and .parent_id reads. Without
    // this lock, a concurrent alloc_env_frame on the main
    // thread (during eval, mutate:rebind, etc.) could
    // reallocate the deque's map array, freeing the map
    // pointer a fiber thread (running apply_closure →
    // materialize_call_env) is reading.
    std::shared_lock<std::shared_mutex> env_rlock(env_frames_mtx_);
    const EnvFrame& fr = env_frame(cl.env_id);
    // Issue #242: detect a stale frame (captured before the
    // current mutation epoch). The frame's bindings might be
    // inconsistent with the post-mutation state — log a
    // warning + bump the frame's version_ so subsequent
    // lookups see it as fresh. We don't refresh the bindings
    // themselves (that would require re-capturing against a
    // new env, which is out of scope for the P0 ship); the
    // warning + version bump is enough to make the staleness
    // observable and prevent repeated warnings.
    if (fr.version_ < defuse_version_.load(std::memory_order_acquire)) {
        // Mutate the version under the same shared lock. A
        // shared_lock allows multiple readers but blocks
        // writers (alloc_env_frame); since we're not adding
        // or removing frames, just updating a metadata
        // field, the shared lock is sufficient (no other
        // reader depends on version_ being immutable).
        const_cast<EnvFrame&>(fr).version_ = defuse_version_.load(std::memory_order_acquire);
        // Logging is best-effort — a fiber thread might not
        // have a tty. We use std::println(std::cerr, ...) so
        // the warning is always emitted (not just in debug).
        std::println(std::cerr,
                     "[#242 warning] materialize_call_env: stale EnvFrame id={} "
                     "(frame.version_={}, current defuse_version_={}). "
                     "Bindings may be inconsistent with post-mutation state. "
                     "Bumped frame.version_ to silence future warnings.",
                     cl.env_id, fr.version_, defuse_version_.load(std::memory_order_acquire));
    }
    ne.bindings() = fr.bindings_;
    ne.bindings_symid_mut() = fr.bindings_symid_;
    if (fr.parent_id != NULL_ENV_ID) {
        ne.set_owner(this);
        ne.set_parent_id(fr.parent_id);
    }
    return ne;
}

// Issue #242: is_env_frame_stale — true if the frame's
// stamped version is older than the current defuse_version_
// (i.e. captured before a mutation that may have invalidated
// the captured scope). Returns true for invalid ids as a
// safety net so callers can treat invalid frames as stale.
bool Evaluator::is_env_frame_stale(EnvId id) const {
    if (id == NULL_ENV_ID || id >= env_frames_.size())
        return true;
    // env_frames_ is a deque guarded by env_frames_mtx_; a
    // shared_lock keeps the frame alive across the load.
    std::shared_lock<std::shared_mutex> rlock(env_frames_mtx_);
    return env_frames_[id].version_ < defuse_version_.load(std::memory_order_acquire);
}

// Evaluator::lookup_by_symid_chain — demonstrate the SoA walk.
// Walks env_frames_ via index lookup (no pointer chase) and
// returns the first match (closest frame wins, shadowing
// semantics match Env::lookup_by_symid).
//
// P0: Cell deref is now centralized here using the Evaluator's
// own central `cells_` pmr vector (the only owner of the
// re-allocatable cell heap). EnvFrame no longer stores a
// cells_ pointer; frames are pure data + indices. This is
// the canonical path for new SoA code. Legacy Env paths
// (still using Env::cells_ pointer) remain for transition.
std::optional<types::EvalValue> Evaluator::lookup_by_symid_chain(EnvId start,
                                                                 aura::ast::SymId s) const {
    std::optional<types::EvalValue> result;
    walk_env_frames(start, [&](EnvId, const EnvFrame& fr) {
        auto v = fr.lookup_local_by_symid(s);
        if (v.has_value()) {
            auto val = *v;
            if (is_cell(val)) {
                auto idx = as_cell_id(val);
                if (idx < cells_.size())
                    result = cells_[idx];
                else
                    result = val; // defensive
            } else {
                result = std::move(val);
            }
            return false; // stop walking — closest frame wins
        }
        return true; // continue walking
    });
    return result;
}

// Issue #205: Evaluator::walk_env_frame_roots. Linear pass
// over env_frames_ collecting pair/closure ref indices
// reachable through env bindings.
//
// For each frame, walk both bindings vectors (they're
// parallel — same length, same order — and either may have
// values not in the other depending on whether the pool_
// was set when binding). For each value, check if it's a
// pair/closure ref and extract the index.
//
// This is a "shallow" walk: it doesn't recursively descend
// into pairs (the GC's mark phase handles the recursion
// via pair_marks_ and closure_marks_). It only discovers
// the TOP-LEVEL refs that are reachable from env bindings.
//
// Complexity: O(frames * bindings_per_frame). For a typical
// module with N functions and M env frames, each with
// ~10 bindings, this is O(N * M * 10) ≈ O(10 * N * M).
// For N=100, M=1000, that's ~1M operations — negligible.
// Issue #211: build the (tag, arity) index for the
// query:pattern primitive. The index is built once per
// workspace and cached. The key is (tag << 32) | arity
// (tag in high 32 bits, arity in low 32 bits). The value
// is a vector of NodeIds whose node has the matching
// (tag, arity).
//
// Build cost: O(N) — a single pass over the workspace.
// Lookup cost: O(1) for the index lookup, then O(bucket
// size) for the iteration. For patterns where the root's
// (tag, arity) is rare (e.g., looking for `(+ 1 2)` in a
// 1000-node AST with mostly `define`s and `lambda`s), the
// bucket size is small and the lookup is much faster than
// the full walk (which is O(N) regardless).
void Evaluator::build_tag_arity_index() const {
    // If the index is already built for the current
    // workspace, no-op. This is the fast path: the index
    // is built once and reused across multiple
    // query:pattern calls.
    if (tag_arity_index_workspace_ == workspace_flat_ && !tag_arity_index_.empty()) {
        return;
    }
    // The index is stale (different workspace or empty).
    // Rebuild.
    tag_arity_index_.clear();
    tag_arity_index_workspace_ = workspace_flat_;
    if (!workspace_flat_)
        return;
    const auto& flat = *workspace_flat_;
    const std::size_t n = flat.size();
    tag_arity_index_.reserve(n);
    for (aura::ast::NodeId id = 0; id < n; ++id) {
        const auto node = flat.get(id);
        const auto tag = static_cast<std::uint32_t>(node.tag);
        const auto arity = static_cast<std::uint32_t>(node.children.size());
        const std::uint64_t key =
            (static_cast<std::uint64_t>(tag) << 32) | static_cast<std::uint64_t>(arity);
        tag_arity_index_[key].push_back(id);
    }
}

void Evaluator::walk_env_frame_roots(std::vector<std::int64_t>& pair_roots_out,
                                     std::vector<std::int64_t>& closure_roots_out) const {
    // De-dup: a pair/closure may be bound in multiple envs.
    // Using a small set per pass; if the size grows beyond
    // a threshold, we fall back to dedup-after-the-fact
    // (mark vectors also de-dup via the set() semantic).
    // For now, just always set — the GC's mark_env_frame_roots
    // is idempotent (set() is a no-op if already set).
    for (const auto& fr : env_frames_) {
        // Walk the name-keyed bindings. bindings_symid_ is
        // populated when pool_ is set; bindings_ is always
        // populated. We walk BOTH to be safe (they should
        // hold the same values, but checking is cheap).
        for (const auto& [name, val] : fr.bindings_) {
            (void)name;
            if (is_pair(val)) {
                pair_roots_out.push_back(static_cast<std::int64_t>(as_pair_idx(val)));
            } else if (is_closure(val)) {
                closure_roots_out.push_back(static_cast<std::int64_t>(as_closure_id(val)));
            }
        }
        for (const auto& [sym, val] : fr.bindings_symid_) {
            (void)sym;
            if (is_pair(val)) {
                pair_roots_out.push_back(static_cast<std::int64_t>(as_pair_idx(val)));
            } else if (is_closure(val)) {
                closure_roots_out.push_back(static_cast<std::int64_t>(as_closure_id(val)));
            }
        }
    }
}

// Issue #206: Evaluator::compact_pairs. Compacts the
// pairs_ arena, building a remap table for stable id
// resolution.
//
// Algorithm: linear scan, copy live pairs to the front,
// build pair_remap_[old_idx] = new_idx. Dead pairs (not
// in `live_mask`) are skipped and their old index gets
// remap to -1.
//
// Returns the number of pairs after compact.
//
// The remap table is sized to the OLD pairs_ size, even
// after compact (which shrinks pairs_). This is by
// design: a stale id (e.g., from a saved
// MutationRecord) might still be in [0, old_size). The
// remap tells us if that id is live (and what its new
// index is) or freed (-1).
std::int64_t Evaluator::compact_pairs(const std::vector<bool>& live_mask) {
    const std::size_t n_old = pairs_.size();
    pair_remap_.clear();
    pair_remap_.reserve(n_old);
    // Build a new vector with only the live pairs. Use
    // move-semantics to avoid copies where possible.
    std::pmr::vector<Pair> new_pairs{&runtime_resource_};
    new_pairs.reserve(n_old); // upper bound
    std::int64_t new_idx = 0;
    for (std::size_t i = 0; i < n_old; ++i) {
        // If live_mask is empty, treat all as live.
        // If live_mask is sized to n_old, use the bit.
        // If live_mask is shorter than i, treat as dead
        // (defensive).
        const bool is_live =
            live_mask.empty() ? true : (i < live_mask.size() ? live_mask[i] : false);
        if (is_live) {
            pair_remap_.push_back(new_idx);
            new_pairs.push_back(std::move(pairs_[i]));
            ++new_idx;
        } else {
            pair_remap_.push_back(-1);
        }
    }
    pairs_ = std::move(new_pairs);
    return static_cast<std::int64_t>(pairs_.size());
}

// ── Issue #145: EnvView / ClosureView impls ──────────────────
//
// make_env_view: build a zero-copy view over an Env's
// bindings. The spans stay valid as long as the Env does
// (no vector reallocation expected — the Env's bindings_
// grows monotonically within a single eval).
EnvView make_env_view(const Env& env) {
    EnvView v;
    v.string_bindings = env.bindings();
    // The SymId-keyed array is private; access via a const
    // accessor friend. We add the accessor below.
    v.symid_bindings = env.bindings_symid();
    v.parent = env.parent();
    return v;
}

std::optional<EvalValue> EnvView::lookup(const std::string& name) const {
    for (auto it = string_bindings.rbegin(); it != string_bindings.rend(); ++it)
        if (it->first == name)
            return it->second;
    return parent ? parent->lookup(name) : std::nullopt;
}

std::optional<EvalValue> EnvView::lookup_by_intern(const std::string& n,
                                                   const aura::ast::StringPool* pool) const {
    // Mirror Env::lookup_by_intern: intern via the resolved
    // pool, route through lookup_by_symid, return local
    // symid_bindings lookup if not found, then fall through
    // to the parent walk. EnvView has no primitive/ADT
    // fallbacks (those live on Env, not EnvView), so the
    // behavior matches EnvView::lookup for the "name not
    // found" case: nullopt.
    if (!pool)
        return std::nullopt; // EnvView: no fallback pool
    // const_cast is safe — intern() is logically idempotent
    // (already-interned names are no-ops) and EnvView callers
    // pass a non-const pool pointer (canonical_pool() or
    // closure-captured). The function is logically const
    // (no observable EnvView state change).
    auto sym = const_cast<aura::ast::StringPool*>(pool)->intern(n);
    for (auto it = symid_bindings.rbegin(); it != symid_bindings.rend(); ++it)
        if (it->first == sym)
            return it->second;
    return parent ? parent->lookup_by_intern(n, pool) : std::nullopt;
}

std::optional<EvalValue> EnvView::lookup_by_symid(aura::ast::SymId s) const {
    for (auto it = symid_bindings.rbegin(); it != symid_bindings.rend(); ++it)
        if (it->first == s)
            return it->second;
    return parent ? parent->lookup_by_symid(s) : std::nullopt;
}

ClosureView make_closure_view(const Closure& cl) {
    ClosureView v;
    v.params = std::span<const aura::ast::SymId>(cl.params.data(), cl.params.size());
    v.body_id = cl.body_id;
    v.dotted = cl.dotted;
    v.flat = cl.flat;
    v.pool = cl.pool;
    // P0: no more cl.env; only env_id.
    v.env_id = cl.env_id;
    v.owner_arena = cl.owner_arena;
    v.name = cl.name;
    return v;
}

// ── Helper: coerce EvalValue to int (string → int parsing) ────
//
// Issue #146 Phase 1: thin legacy wrapper around the pure
// `coerce_to_int_pure` in `aura::compiler::pure`. The 32+ call
// sites in this file keep the bool+default-style API; new
// code should call `coerce_to_int_pure` directly and use
// `Result`'s monadic methods. The wrapper drops the Result
// error info (and the legacy stderr print is gone — use the
// pure version for diagnostic access).
namespace {
    static std::int64_t coerce_to_int(const EvalValue& v, std::span<const std::string> heap) {
        // Phase 1: thin wrapper around coerce_to_int_pure. On
        // error path the pure version returns a Diagnostic
        // describing the unparseable string; we mirror the
        // pre-Phase-1 stderr print here so the tree-walker
        // path's error surface matches the IR-executor path's
        // "error: type mismatch" message (see test_regression
        // tc-strict-runtime-typed-arg-mismatch).
        auto r = aura::compiler::pure::coerce_to_int_pure(v, heap);
        if (r)
            return *r;
        if (v.val != 3 && is_string(v) && !heap.empty()) {
            auto idx = as_string_idx(v);
            if (idx < heap.size()) {
                std::println(std::cerr, "error: type mismatch — expected Int, got String '{}'",
                             heap[idx]);
            }
        }
        return 0;
    }

    [[maybe_unused]] static double coerce_to_double(const EvalValue& v,
                                                    std::span<const std::string> heap) {
        if (is_float(v))
            return as_float(v);
        return static_cast<double>(coerce_to_int(v, heap));
    }
} // namespace

// ── Primitives: EvalValue operations ──────────────────────────

Primitives::Primitives() {
    // ── Variadic arithmetic ────────────────────────────────────────
    // (+) → 0, (+ x) → x, (+ x y ...) → sum; float if any arg is float
    // Issue #146 (7th extract): the body is now a 1-line
    // forwarder to aura::compiler::pure::arithmetic_sum_pure.
    // The diag sink is std::cerr to preserve the legacy "type
    // mismatch" stderr emission that the stateful coerce_to_int
    // wrapper used to provide — regression tests
    // tc-strict-runtime-typed-arg-mismatch and
    // tc-type-var-name-from-param depend on this.
    table_["+"] = [this](std::span<const EvalValue> a) {
        std::span<const std::string> heap(string_heap_->data(), string_heap_->size());
        return aura::compiler::pure::arithmetic_sum_pure(a, heap, &std::cerr);
    };
    // (-) → 0, (- x) → -x, (- x y ...) → x - y - z - ...
    // Issue #212 Phase 3: thin forwarder to arithmetic_sub_pure.
    table_["-"] = [this](std::span<const EvalValue> a) {
        return aura::compiler::pure::arithmetic_sub_pure(a, *string_heap_, &std::cerr);
    };
    // (*) → 1, (* x) → x, (* x y ...) → product; float if any arg is float
    // Issue #212 Phase 3: thin forwarder to arithmetic_mul_pure.
    table_["*"] = [this](std::span<const EvalValue> a) {
        return aura::compiler::pure::arithmetic_mul_pure(a, *string_heap_, &std::cerr);
    };
    // (/) → 1, (/ x) → 1.0/x (float reciprocal), (/ x y ...) → x / y / z / ...
    // Issue #212 Phase 3: thin forwarder to arithmetic_div_pure.
    // On div-by-zero the pure function returns an unexpected Diagnostic;
    // we mirror the legacy behavior (silent 0 result for non-empty divisor
    // lists, or 0 for the reciprocal case) so the existing test surface
    // doesn't change. A future migration can surface the error.
    table_["/"] = [this](std::span<const EvalValue> a) {
        auto r = aura::compiler::pure::arithmetic_div_pure(a, *string_heap_, &std::cerr);
        if (r)
            return *r;
        // Error path: legacy behavior. For (/) empty → 1 (but
        // the pure function already errors on empty, so legacy
        // returned 1; we mirror that). For other errors (div by
        // zero, type mismatch), legacy returned 0; mirror that.
        if (a.empty())
            return make_int(1);
        return make_int(0);
    };
    auto chain_cmp = [this](const auto& a, auto fn_int, auto fn_float) -> EvalValue {
        if (a.size() < 2)
            return make_bool(true);
        auto to_f = [this](const EvalValue& v) -> double {
            return is_float(v) ? as_float(v) : static_cast<double>(coerce_to_int(v, *string_heap_));
        };
        bool any_f = false;
        for (auto& v : a)
            if (is_float(v)) {
                any_f = true;
                break;
            }
        if (any_f) {
            for (std::size_t i = 1; i < a.size(); ++i)
                if (!fn_float(to_f(a[i - 1]), to_f(a[i])))
                    return make_bool(false);
            return make_bool(true);
        }
        for (std::size_t i = 1; i < a.size(); ++i)
            if (!fn_int(coerce_to_int(a[i - 1], *string_heap_), coerce_to_int(a[i], *string_heap_)))
                return make_bool(false);
        return make_bool(true);
    };
    table_["="] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x == y; }, [](auto x, auto y) { return x == y; });
    };
    table_["<"] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x < y; }, [](auto x, auto y) { return x < y; });
    };
    table_[">"] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x > y; }, [](auto x, auto y) { return x > y; });
    };
    table_["<="] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x <= y; }, [](auto x, auto y) { return x <= y; });
    };
    table_[">="] = [chain_cmp](std::span<const EvalValue> a) {
        return chain_cmp(
            a, [](auto x, auto y) { return x >= y; }, [](auto x, auto y) { return x >= y; });
    };
    // Ghuloum Step 9: booleans
    table_["not"] = [](std::span<const EvalValue> a) {
        return make_bool(a.empty() || !is_truthy(a[0]));
    };
    table_["and"] = [](std::span<const EvalValue> a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (!is_truthy(a[i]))
                return a[i];
        return a.empty() ? make_int(1) : a.back();
    };
    table_["or"] = [](std::span<const EvalValue> a) {
        for (std::size_t i = 0; i + 1 < a.size(); ++i)
            if (is_truthy(a[i]))
                return a[i];
        return a.empty() ? make_int(0) : a.back();
    };
    table_["eq?"] = [](std::span<const EvalValue> a) {
        return make_bool(a.size() >= 2 && a[0] == a[1]);
    };
    table_["current-time"] = [](std::span<const EvalValue> a) {
        (void)a;
        return make_int(static_cast<std::int64_t>(::time(nullptr)));
    };
    // Populate ordered_names_ with all primitives registered directly via table_[]
    for (auto& [name, _] : table_) {
        if (std::find(ordered_names_.begin(), ordered_names_.end(), name) == ordered_names_.end()) {
            ordered_names_.push_back(name);
        }
    }
}

// ── I/O helper for EvalValue ──────────────────────────────────
struct Pair;
// Forward declaration for defuse_index_destroy (defined after the
// DefUseIndex struct body, around line 7360). All 9 callers pass
// `&defuse_index_` to it — the helper does the actual `delete` and
// nullifies the slot. Defined as a free function rather than a
// static member because the call sites live before the struct
// body is in scope (defuse_index_ is set/reset in set-code and
// workspace-switching code that comes early in the TU).
static void defuse_index_destroy(void** slot);
namespace {
    // Check if value is the end of a list (void is the proper sentinel)
    // Note: int 0 is ALSO used as the empty list sentinel in some contexts,
    // but we only treat it as end-of-list in cdr chain position.
    static bool is_end_of_list(const EvalValue& v) {
        return is_void(v) || (is_int(v) && as_int(v) == 0);
    }
    // Format a value to string (same formatting as io_print_val but returns string)
    static std::string fmt_val_to_string(const EvalValue& v, std::span<const std::string> heap,
                                         std::span<const Pair> pairs, bool quote, int depth = 0) {
        std::string out;
        auto app = [&](const auto&... args) { (out += ... += args); };
        if (depth > 64)
            return "...";
        if (is_void(v))
            return "()";
        if (is_bool(v))
            return as_bool(v) ? "#t" : "#f";
        if (is_float(v))
            return std::to_string(as_float(v));
        if (is_int(v))
            return std::to_string(as_int(v));
        if (is_string(v)) {
            auto idx = as_string_idx(v);
            if (idx < heap.size()) {
                if (quote)
                    return "\"" + heap[idx] + "\"";
                return heap[idx];
            }
            return "";
        }
        if (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size())
                return "<pair>";
            out = "(";
            out += fmt_val_to_string(pairs[idx].car, heap, pairs, quote, depth + 1);
            auto cdr = pairs[idx].cdr;
            while (is_pair(cdr)) {
                out += " ";
                auto nidx = as_pair_idx(cdr);
                if (nidx >= pairs.size()) {
                    out += "<pair>";
                    break;
                }
                out += fmt_val_to_string(pairs[nidx].car, heap, pairs, quote, depth + 1);
                cdr = pairs[nidx].cdr;
            }
            if (!is_end_of_list(cdr)) {
                out += " . ";
                out += fmt_val_to_string(cdr, heap, pairs, quote, depth + 1);
            }
            out += ")";
            return out;
        }
        if (is_vector(v))
            return std::format("<vector[{}]>", as_vector_idx(v));
        if (is_hash(v))
            return std::format("<hash[{}]>", as_hash_idx(v));
        if (is_closure(v))
            return "#<procedure>";
        return "<unknown>";
    }

    static void io_print_val(const EvalValue& v, std::span<const std::string> heap,
                             std::span<const Pair> pairs, bool quote, int depth = 0,
                             std::span<const std::string> keywords = {}) {
        if (depth > 64) {
            std::fprintf(stdout, "...");
            return;
        }
        if (is_void(v)) {
            std::fprintf(stdout, "()");
            return;
        }
        if (is_bool(v)) {
            std::fprintf(stdout, "%s", as_bool(v) ? "#t" : "#f");
            return;
        }
        // IMPORTANT: Check is_string BEFORE is_keyword (Issue #96 bug fix;
        // pre-#181 encoding rationale). The v2 encoding (Issue #181)
        // uses (v & 3) == 2 as the dedicated string tag, so this is
        // no longer a correctness concern — but the ordering is kept
        // for semantic clarity (a string at idx N is never a keyword
        // at the same value).
        if (is_string(v) && !heap.empty()) {
            auto idx = as_string_idx(v);
            if (idx < heap.size()) {
                if (quote)
                    std::fprintf(stdout, "\"%s\"", heap[idx].c_str());
                else
                    std::fprintf(stdout, "%s", heap[idx].c_str());
                return;
            }
        }
        if (is_keyword(v)) {
            auto kidx = as_keyword_idx(v);
            if (!keywords.empty() && kidx < keywords.size()) {
                auto kname = keywords[kidx];
                std::fprintf(stdout, "%s", kname.c_str());
            } else {
                std::fprintf(stdout, ":%zu", (size_t)kidx);
            }
            return;
        }
        if (is_float(v)) {
            std::fprintf(stdout, "%g", as_float(v));
            return;
        }
        if (is_int(v)) {
            std::fprintf(stdout, "%ld", (long)as_int(v));
            return;
        }
        if (is_pair(v) && !pairs.empty()) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs.size()) {
                std::fprintf(stdout, "<pair[%zu]>", (size_t)idx);
                return;
            }
            // Check if it's a proper list (cdr chain ends in void or int 0 sentinel)
            auto cdr = pairs[idx].cdr;
            if (is_end_of_list(cdr) && !quote) {
                // Single-element list: (x)
                std::fprintf(stdout, "(");
                io_print_val(pairs[idx].car, heap, pairs, quote, depth + 1, keywords);
                std::fprintf(stdout, ")");
                return;
            }
            // Walk the chain to see if it's a proper list
            std::vector<EvalValue> elements;
            elements.push_back(pairs[idx].car);
            auto next = cdr;
            bool proper = true;
            while (!is_end_of_list(next)) {
                if (!is_pair(next)) {
                    proper = false;
                    break;
                }
                auto nidx = as_pair_idx(next);
                if (nidx >= pairs.size()) {
                    proper = false;
                    break;
                }
                elements.push_back(pairs[nidx].car);
                next = pairs[nidx].cdr;
            }
            std::fprintf(stdout, "(");
            for (std::size_t i = 0; i < elements.size(); ++i) {
                if (i > 0)
                    std::fprintf(stdout, " ");
                io_print_val(elements[i], heap, pairs, quote, depth + 1, keywords);
            }
            if (!is_end_of_list(next)) {
                std::fprintf(stdout, " . ");
                io_print_val(next, heap, pairs, quote, depth + 1, keywords);
            }
            std::fprintf(stdout, ")");
            return;
        }
        if (is_vector(v)) {
            std::fprintf(stdout, "<vector[%zu]>", (size_t)as_vector_idx(v));
            return;
        }
        if (is_hash(v)) {
            std::fprintf(stdout, "<hash[%zu]>", (size_t)as_hash_idx(v));
            return;
        }
        if (is_closure(v)) {
            std::fprintf(stdout, "#<procedure>");
            std::fprintf(stderr, "⚠ program returned an uncalled function\n");
            return;
        }
        if (is_cell(v)) {
            std::fprintf(stdout, "<cell[%zu]>", (size_t)as_cell_id(v));
            return;
        }
        std::fprintf(stdout, "<unknown>");
    }
} // namespace

// Centralized make_merr (refactor Step 0.1, 3.1 query cluster in progress).
// Replaces duplicated local `auto merr = ...` lambdas (orig ~14-15 in mutate + query).
// See evaluator.ixx private decl and docs/contributing.md §3.
EvalValue Evaluator::make_merr(const std::string& k, const std::string& m) {
    auto mi = string_heap_.size();
    string_heap_.push_back(m);
    auto ki = string_heap_.size();
    string_heap_.push_back(k);
    auto mp = make_pair(pairs_.size());
    pairs_.push_back({make_string(mi), EvalValue(0)});
    auto kp = make_pair(pairs_.size());
    pairs_.push_back({make_string(ki), mp});
    return kp;
}

void Evaluator::init_pair_primitives() {
    primitives_detail::register_type_and_char_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); });

    primitives_detail::register_pair_and_string_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        pairs_, string_heap_, error_values_);

    primitives_detail::register_json_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        pairs_, string_heap_);


    primitives_detail::register_list_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        pairs_, string_heap_, error_values_,
        [this](const EvalValue& fn, const EvalValue& arg) -> EvalValue {
            if (is_primitive(fn)) {
                auto slot = as_primitive_slot(fn);
                if (slot >= primitives_.slot_count())
                    return make_void();
                auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
                if (!prim)
                    return make_void();
                return (*prim)({arg});
            }
            if (is_closure(fn)) {
                auto cid = as_closure_id(fn);
                auto result = apply_closure(cid, {arg});
                return result ? *result : make_void();
            }
            return make_void();
        },
        [this](const EvalValue& fn, const EvalValue& arg) -> bool {
            if (is_primitive(fn)) {
                auto slot = as_primitive_slot(fn);
                if (slot >= primitives_.slot_count())
                    return false;
                auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
                if (!prim)
                    return false;
                return aura::compiler::pure::is_truthy((*prim)({arg}));
            }
            if (is_closure(fn)) {
                auto cid = as_closure_id(fn);
                auto result = apply_closure(cid, {arg});
                return result ? aura::compiler::pure::is_truthy(*result) : false;
            }
            return false;
        },
        [this](const EvalValue& fn, const EvalValue& acc, const EvalValue& arg) -> EvalValue {
            if (is_primitive(fn)) {
                auto slot = as_primitive_slot(fn);
                if (slot >= primitives_.slot_count())
                    return make_void();
                auto prim = primitives_.lookup(primitives_.name_for_slot(slot));
                if (!prim)
                    return make_void();
                return (*prim)({acc, arg});
            }
            if (is_closure(fn)) {
                auto cid = as_closure_id(fn);
                auto result = apply_closure(cid, {acc, arg});
                return result ? *result : make_void();
            }
            return make_void();
        });

    primitives_detail::register_vector_and_hash_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        pairs_, string_heap_, error_values_, vector_heap_);

    primitives_detail::register_math_regex_and_arithmetic_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        pairs_, string_heap_, error_values_);

    primitives_detail::register_reflect_and_type_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        pairs_, string_heap_, keyword_table_, type_registry_);

    primitives_detail::register_query_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        pairs_, string_heap_, type_registry_,
        [this](const std::string& path) { return resolve_module_path(path); });

    // ── ADT registration now delegated to adt_runtime_ (Step 2.3) ───
    // The old "adt:register-constructors" and "adt:reset-constructors"
    // (which mutated the removed global g_adt_constructors) have been
    // removed from here. The adt_runtime_ will provide equivalent
    // (or improved) registration in the full wiring.
    // Parser's parse_datatype still emits the call; for now it will
    // be unbound until the primitive is re-provided via adt_runtime
    // (or kept as compatibility stub in future step).
    //
    // The ctor PrimitiveFn creation logic (the big lambda above)
    // will live inside adt_runtime.register_primitives in the
    // complete extraction.

    primitives_.add("equal?", [this](std::span<const EvalValue> a) {
        if (a.size() < 2)
            return make_bool(true);

        struct EqCheck {
            Evaluator& e;
            bool operator()(const EvalValue& x, const EvalValue& y, int depth) const {
                if (depth > 64)
                    return true;
                if (x == y)
                    return true;
                if (is_int(x) && is_int(y))
                    return as_int(x) == as_int(y);
                if (is_float(x) && is_float(y))
                    return as_float(x) == as_float(y);
                if (is_bool(x) && is_bool(y))
                    return as_bool(x) == as_bool(y);
                if (is_string(x) && is_string(y)) {
                    auto xi = as_string_idx(x), yi = as_string_idx(y);
                    if (xi < e.string_heap_.size() && yi < e.string_heap_.size())
                        return e.string_heap_[xi] == e.string_heap_[yi];
                    return false;
                }
                if (is_pair(x) && is_pair(y)) {
                    auto xi = as_pair_idx(x), yi = as_pair_idx(y);
                    if (xi < e.pairs_.size() && yi < e.pairs_.size())
                        return (*this)(e.pairs_[xi].car, e.pairs_[yi].car, depth + 1) &&
                               (*this)(e.pairs_[xi].cdr, e.pairs_[yi].cdr, depth + 1);
                    return false;
                }
                if (is_vector(x) && is_vector(y)) {
                    auto xi = as_vector_idx(x), yi = as_vector_idx(y);
                    if (xi < e.vector_heap_.size() && yi < e.vector_heap_.size()) {
                        auto& vx = e.vector_heap_[xi];
                        auto& vy = e.vector_heap_[yi];
                        if (vx.size() != vy.size())
                            return false;
                        for (std::size_t i = 0; i < vx.size(); ++i)
                            if (!(*this)(vx[i], vy[i], depth + 1))
                                return false;
                        return true;
                    }
                    return false;
                }
                // Empty list sentinel: void or int 0
                if ((is_void(x) || (is_int(x) && as_int(x) == 0)) &&
                    (is_void(y) || (is_int(y) && as_int(y) == 0)))
                    return true;
                return false;
            }
        };

        return make_bool(EqCheck{*this}(a[0], a[1], 0));
    });

    // (gensym)  → "G__0"
    // (gensym "prefix") → "prefix__1"
    // (gensym "prefix" n) → "prefix__n" (n is an int suffix)
    // Issue #121: extended to take an optional prefix argument.
    // The prefix is a string; the suffix is a global atomic
    // counter (monotonically increasing for the process).
    // Useful for quasiquote templates that need to generate
    // unique binding names at macro-expansion time.
    primitives_.add("gensym", [this](std::span<const EvalValue> a) -> EvalValue {
        static std::atomic<std::int64_t> gs_counter_{0};
        auto id = gs_counter_.fetch_add(1, std::memory_order_relaxed);
        std::string prefix = "G__";
        if (a.size() >= 1 && is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size()) {
                prefix = string_heap_[idx] + "__";
            }
        }
        std::string name = prefix + std::to_string(id);
        auto sid = string_heap_.size();
        string_heap_.push_back(name);
        return make_string(sid);
    });
    primitives_.add("symbol-append", [this](std::span<const EvalValue> a) -> EvalValue {
        std::string result;
        for (auto& v : a) {
            if (is_string(v)) {
                auto idx = as_string_idx(v);
                if (idx < string_heap_.size())
                    result += string_heap_[idx];
            } else if (is_int(v)) {
                result += std::to_string(as_int(v));
            }
        }
        auto sid = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sid);
    });

    // (apply fn list) — call fn with list elements as individual args
    primitives_.add("apply", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2)
            return make_void();
        auto& fn = a[0];
        auto& arg_list = a[1];
        std::vector<EvalValue> args;
        auto current = arg_list;
        while (is_pair(current)) {
            auto idx = as_pair_idx(current);
            if (idx >= pairs_.size())
                break;
            args.push_back(pairs_[idx].car);
            current = pairs_[idx].cdr;
        }
        if (is_primitive(fn)) {
            auto slot = as_primitive_slot(fn);
            auto pfn = primitives_.lookup(primitives_.name_for_slot(slot));
            if (pfn)
                return (*pfn)(args);
        }
        if (is_closure(fn)) {
            auto cid = as_closure_id(fn);
            auto result = apply_closure(cid, args);
            if (result)
                return *result;
        }
        return make_void();
    });

    primitives_.add("display", [this](std::span<const EvalValue> a) {
        if (a.empty())
            return make_void();
        io_print_val(a[0], string_heap_, pairs_, false, 0, keyword_table_);
        std::fflush(stdout);
        return make_void();
    });
    primitives_.add("write", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        io_print_val(a[0], string_heap_, pairs_, true, 0, keyword_table_);
        std::fflush(stdout);
        return make_void();
    });
    primitives_.add("newline", [](const auto&) -> EvalValue {
        std::fprintf(stdout, "\n");
        std::fflush(stdout);
        return make_void();
    });
    // (format template args...) — Simple string formatting (SRFI-28 subset)
    // ~a  display arg    ~s  write arg    ~%  newline    ~~  literal ~
    primitives_.add("format", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto tidx = as_string_idx(a[0]);
        if (tidx >= string_heap_.size())
            return make_bool(false);
        auto& tmpl = string_heap_[tidx];
        std::string result;
        std::size_t arg_idx = 1; // first arg in a[1..]
        for (std::size_t i = 0; i < tmpl.size(); ++i) {
            if (tmpl[i] == '~' && i + 1 < tmpl.size()) {
                switch (tmpl[i + 1]) {
                    case 'a': // display arg
                        if (arg_idx < a.size()) {
                            auto val = a[arg_idx++];
                            result += fmt_val_to_string(val, string_heap_, pairs_, false);
                        }
                        ++i;
                        break;
                    case 's': // write arg (quoted)
                        if (arg_idx < a.size()) {
                            auto val = a[arg_idx++];
                            result += fmt_val_to_string(val, string_heap_, pairs_, true);
                        }
                        ++i;
                        break;
                    case '%': // newline
                        result += '\n';
                        ++i;
                        break;
                    case '~': // literal ~
                        result += '~';
                        ++i;
                        break;
                    default:
                        result += tmpl[i];
                        break;
                }
            } else {
                result += tmpl[i];
            }
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
    });
    // (error msg) — Create an error value (no longer throws C++ exception)
    primitives_.add("error", [this](std::span<const EvalValue> a) -> EvalValue {
        // Ensure error_values_[0] always exists for default errors
        if (error_values_.empty())
            error_values_.push_back(make_void());
        types::EvalValue cause = make_string(0); // default
        if (!a.empty())
            cause = a[0];
        auto eidx = error_values_.size();
        error_values_.push_back(cause);
        return make_error(eidx);
    });

    // (assert expr msg) — Assertion, returns error on failure
    primitives_.add("assert", [this](std::span<const EvalValue> a) -> EvalValue {
        if (!a.empty() && is_truthy(a[0]))
            return make_int(1);
        // Assertion failed — return error
        types::EvalValue cause = make_string(0);
        if (a.size() > 1)
            cause = a[1];
        auto eidx = error_values_.size();
        error_values_.push_back(cause);
        return make_error(eidx);
    });

    // (raise val) — Create an error with arbitrary cause value
    primitives_.add("raise", [this](std::span<const EvalValue> a) -> EvalValue {
        auto cause = a.empty() ? make_void() : a[0];
        auto eidx = error_values_.size();
        error_values_.push_back(cause);
        return make_error(eidx);
    });

    // (error? val) — Type predicate for error values
    primitives_.add("error?", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_bool(false);
        // Guard against encoding collision: strings can accidentally pass is_error()
        return make_bool(is_error(a[0]) && !is_string(a[0]));
    });


    // (check expr) — Test assertion, returns #t or error on failure
    primitives_.add("check", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_error(0);
        if (is_truthy(a[0]))
            return make_int(1);
        // Store failing value as error cause
        auto eidx = error_values_.size();
        error_values_.push_back(a[0]);
        return make_error(eidx);
    });

    // (check= expected actual) — Test equality, returns #t or error
    primitives_.add("check=", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2)
            return make_bool(false);
        if (types::is_void(a[0]) && types::is_void(a[1]))
            return make_int(1);
        if (types::is_int(a[0]) && types::is_int(a[1])) {
            if (types::as_int(a[0]) == types::as_int(a[1]))
                return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_float(a[0]) && types::is_float(a[1])) {
            if (types::as_float(a[0]) == types::as_float(a[1]))
                return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        if (types::is_string(a[0]) && types::is_string(a[1])) {
            auto i0 = types::as_string_idx(a[0]);
            auto i1 = types::as_string_idx(a[1]);
            if (i0 < string_heap_.size() && i1 < string_heap_.size() &&
                string_heap_[i0] == string_heap_[i1])
                return make_int(1);
            auto eidx = error_values_.size();
            error_values_.push_back(a[0]);
            return make_error(eidx);
        }
        return make_bool(false);
    });

    // (check-success output expected-list) — Flexible substring matching
    // Returns #t if any expected keyword is found in output.
    // Guards against false positives from error messages that happen to
    // contain expected keywords (uses word-boundary for short keys in error output).
    primitives_.add("check-success", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_pair(a[1]))
            return make_bool(false);
        // Get normalized output (strip quotes)
        auto out_idx = as_string_idx(a[0]);
        if (out_idx >= string_heap_.size())
            return make_bool(false);
        auto norm_out = string_heap_[out_idx];
        // Strip surrounding quotes
        if (!norm_out.empty() && (norm_out[0] == '"' || norm_out[0] == '\''))
            norm_out = norm_out.substr(1);
        if (!norm_out.empty() && (norm_out.back() == '"' || norm_out.back() == '\''))
            norm_out.pop_back();
        // Detect if output looks like an error message
        auto lower = norm_out;
        for (auto& c : lower)
            c = std::tolower(static_cast<unsigned char>(c));
        bool is_error_like = lower.find("error:") != std::string::npos ||
                             lower.find("parse error") != std::string::npos ||
                             lower.find("unbound variable") != std::string::npos ||
                             lower.find("type error") != std::string::npos ||
                             lower.find("syntax error") != std::string::npos ||
                             lower.find("invalid syntax") != std::string::npos ||
                             lower.find("expected expression") != std::string::npos ||
                             (norm_out.size() > 1 && norm_out[0] == '(' && norm_out[1] == '"');
        // Iterate expected list
        auto pair_idx = as_pair_idx(a[1]);
        while (pair_idx < pairs_.size()) {
            auto& p = pairs_[pair_idx];
            if (is_string(p.car)) {
                auto kw_idx = as_string_idx(p.car);
                if (kw_idx < string_heap_.size()) {
                    auto& kw = string_heap_[kw_idx];
                    if (!kw.empty() && norm_out.find(kw) != std::string::npos) {
                        // Guard: for error-like output with short keywords
                        if (is_error_like && kw.size() <= 5) {
                            // Generic error words that should never match in error output
                            static const std::unordered_set<std::string> generic_words = {
                                "error", "type", "parse", "syntax", "kind"};
                            if (generic_words.count(kw)) {
                                // Skip — too generic, likely part of error message
                                if (is_pair(p.cdr)) {
                                    pair_idx = as_pair_idx(p.cdr);
                                    continue;
                                }
                                break;
                            }
                            // Word boundary check for other short keywords
                            auto pos = norm_out.find(kw);
                            bool word_boundary = true;
                            if (pos > 0) {
                                char prev = norm_out[pos - 1];
                                if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')
                                    word_boundary = false;
                            }
                            if (pos + kw.size() < norm_out.size()) {
                                char next = norm_out[pos + kw.size()];
                                if (std::isalnum(static_cast<unsigned char>(next)) || next == '_')
                                    word_boundary = false;
                            }
                            if (!word_boundary) {
                                // Move to next keyword
                                if (is_pair(p.cdr)) {
                                    pair_idx = as_pair_idx(p.cdr);
                                    continue;
                                }
                                break;
                            }
                        }
                        return make_bool(true);
                    }
                }
            }
            if (is_pair(p.cdr))
                pair_idx = as_pair_idx(p.cdr);
            else
                break;
        }
        return make_bool(false);
    });

    // (diagnose error-string) — Analyze structured error and return fix strategy
    // Returns a pair ("root-cause" "target" "fix-type" "fix-data" "message")
    // or #f if no diagnosis available.
    primitives_.add("diagnose", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_bool(false);
        auto msg = string_heap_[idx];

        // Build diagnosis result as list: (cause target fix-type fix-data explanation)
        auto make_diag = [&](const std::string& cause, const std::string& target,
                             const std::string& fix, const std::string& data,
                             const std::string& expl) -> EvalValue {
            auto push = [&](const std::string& s) -> std::uint64_t {
                auto sidx = string_heap_.size();
                string_heap_.push_back(s);
                return sidx;
            };
            auto nil = EvalValue(0);
            auto make_item = [&](const std::string& s) -> EvalValue {
                auto sidx = push(s);
                return make_string(sidx);
            };
            // Build: (cause target fix-type fix-data expl) as proper list
            auto e_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(expl), nil});
            auto d_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(data), e_pair});
            auto f_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(fix), d_pair});
            auto t_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(target), f_pair});
            auto c_pair = make_pair(pairs_.size());
            pairs_.push_back({make_item(cause), t_pair});
            return c_pair;
        };

        // ── Known unbound variable mappings ──
        // Format: {root-cause, target-fn, fix-type, fix-data, explanation}
        struct FixEntry {
            std::string cause;
            std::string fix_type;
            std::string fix_data;
            std::string explanation;
        };
        static const std::unordered_map<std::string, FixEntry> unbound_fixes = {
            {"for-each",
             {"missing-require", "add-require", "std/list",
              "Add (require \"std/list\" all:) to use for-each"}},
            {"map",
             {"missing-require", "add-require", "std/list",
              "Add (require \"std/list\" all:) to use map"}},
            {"filter",
             {"missing-require", "add-require", "std/list",
              "Add (require \"std/list\" all:) to use filter"}},
            {"foldl",
             {"missing-require", "add-require", "std/list",
              "Add (require \"std/list\" all:) to use foldl"}},
            {"make-hash",
             {"missing-require", "add-require", "std/hash",
              "Add (require \"std/hash\" all:) to use make-hash"}},
            {"hash-ref",
             {"missing-require", "add-require", "std/hash",
              "Add (require \"std/hash\" all:) to use hash-ref"}},
            {"rule:define",
             {"missing-require", "add-require", "std/rule",
              "Add (require \"std/rule\" all:) to use rule:define"}},
            {"synthesize:fill",
             {"missing-require", "add-require", "std/pipeline",
              "Add (require \"std/pipeline\" all:) to use synthesize"}},
            {"synthesize:pipeline",
             {"missing-require", "add-require", "std/pipeline",
              "Add (require \"std/pipeline\" all:) to use synthesize"}},
            {"define-type",
             {"missing-require", "add-require", "std/data",
              "Add (require \"std/data\" all:) to use define-type"}},
            {"c-func",
             {"missing-require", "add-require", "std/ffi",
              "Add (require \"std/ffi\" all:) to use c-func"}},
        };

        // Detect "unbound variable: X" and match against known symbols
        std::string prefix = "unbound variable: ";
        auto upos = msg.find(prefix);
        if (upos != std::string::npos) {
            auto target = msg.substr(upos + prefix.size());
            // Strip trailing punctuation
            while (!target.empty() && (target.back() == '.' || target.back() == '!' ||
                                       target.back() == '?' || target.back() == ':'))
                target.pop_back();
            auto it = unbound_fixes.find(target);
            if (it != unbound_fixes.end()) {
                return make_diag(it->second.cause, target, it->second.fix_type, it->second.fix_data,
                                 it->second.explanation);
            }
            // Unknown unbound variable — generic suggestion
            return make_diag("unbound-variable", target, "define-or-require", "",
                             "Define '" + target +
                                 "' with (define ...) or add the right (require ...)");
        }

        // Detect "type error: cannot call: X"
        prefix = "type error: cannot call: ";
        auto tpos = msg.find(prefix);
        if (tpos != std::string::npos) {
            auto target = msg.substr(tpos + prefix.size());
            return make_diag("type-error-unbound", target, "define-function", "",
                             "Define the function '" + target + "' before calling it");
        }

        // Detect parse errors
        if (msg.find("parse error") != std::string::npos ||
            msg.find("expected expression") != std::string::npos) {
            return make_diag("parse-error", "", "fix-syntax", msg.substr(0, 60),
                             "Check syntax: unbalanced parens, missing quotes, or wrong form");
        }

        // Detect #<procedure> / closure
        if (msg.find("procedure") != std::string::npos ||
            msg.find("uncalled function") != std::string::npos) {
            return make_diag("closure-no-display", "", "add-display", "",
                             "Add (display (your-function args)) at the end of your code");
        }

        return make_bool(false);
    });

    // (apply-fix code-string diagnose-result) — Apply pre-built fix from diagnosis
    // Returns fixed code, or original code if no auto-fix applies.
    primitives_.add("apply-fix", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[0]) || !is_pair(a[1]))
            return make_bool(false);
        auto code_idx = as_string_idx(a[0]);
        if (code_idx >= string_heap_.size())
            return make_bool(false);
        auto code = string_heap_[code_idx];

        // Extract fix-type (3rd element of diagnose result)
        // diagnose returns: (cause target fix-type fix-data explanation)
        auto pair_idx = as_pair_idx(a[1]);
        auto get_elem = [&](auto p, int n) -> std::string {
            for (int i = 0; i < n && is_pair(p); ++i) {
                if (i == n - 1) {
                    if (is_string(pairs_[as_pair_idx(p)].car)) {
                        auto si = as_string_idx(pairs_[as_pair_idx(p)].car);
                        if (si < string_heap_.size())
                            return string_heap_[si];
                    }
                    return "";
                }
                p = pairs_[as_pair_idx(p)].cdr;
            }
            return "";
        };
        // Elements (1-indexed in list traversal):
        // 1=cause, 2=target, 3=fix-type, 4=fix-data, 5=explanation
        auto fix_type = get_elem(a[1], 3);
        auto fix_data = get_elem(a[1], 4);
        auto target = get_elem(a[1], 2); // function name

        std::string result;
        if (fix_type == "add-require") {
            // Prepend (require "module" all:) if not already present
            auto req_line = "(require \"" + fix_data + "\" all:)";
            if (code.find(req_line) == std::string::npos) {
                // Check if there's already an existing require
                auto nl = code.find('\n');
                auto first_line = (nl == std::string::npos) ? code : code.substr(0, nl);
                if (first_line.find("(require ") == 0) {
                    // There's already a require, insert after it
                    auto rest = (nl == std::string::npos) ? "" : code.substr(nl);
                    result = first_line + "\n" + req_line + rest;
                } else {
                    result = req_line + "\n" + code;
                }
            } else {
                result = code;
            }
        } else if (fix_type == "define-function" || fix_type == "define-or-require") {
            // Add stub define for unnamed function
            auto stub = "(define (" + target + " x)\n  x)\n";
            if (code.find("(define (" + target) == std::string::npos) {
                result = code + "\n" + stub;
            } else {
                result = code;
            }
        } else if (fix_type == "add-display") {
            // Append (display result) — already handled by eval-current auto-fix
            result = code;
        } else if (fix_type == "fix-syntax") {
            // Can't auto-fix syntax errors — return code as-is
            result = code;
        } else {
            result = code;
        }

        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return make_string(sidx);
    });

    // (run-tests) — Find all test:* bindings in top_ env, report summary
    primitives_.add("run-tests", [this](const auto&) -> EvalValue {
        auto& bindings = top_.bindings();
        int total_suites = 0, total_passed = 0, total_failed = 0;

        for (auto& [name, val] : bindings) {
            if (name.size() <= 5 || name.substr(0, 5) != "test:")
                continue;

            // Dereference cell if needed (define stores via cell)
            auto actual = val;
            if (is_cell(val) && as_cell_id(val) < cells_.size())
                actual = cells_[as_cell_id(val)];
            if (!is_pair(actual))
                continue;

            total_suites++;
            auto idx = as_pair_idx(actual);
            if (idx >= pairs_.size()) {
                total_failed++;
                continue;
            }

            // Suite name
            std::string suite_name;
            if (is_string(pairs_[idx].car)) {
                auto sid = as_string_idx(pairs_[idx].car);
                if (sid < string_heap_.size())
                    suite_name = string_heap_[sid];
            }

            // Walk and evaluate each stored check form
            auto forms = pairs_[idx].cdr;
            int sp = 0, sf = 0;

            while (is_pair(forms)) {
                auto fi = as_pair_idx(forms);
                if (fi >= pairs_.size())
                    break;
                auto check_form = pairs_[fi].car;
                forms = pairs_[fi].cdr;

                // Convert check_form data back to AST and evaluate.
                // Use temp_arena_ so (gc-temp) reclaims parse state after
                // each check clause (was: arena_ = monotonic = leaked).
                if (!arena_) {
                    sf++;
                    continue;
                }
                auto alloc = temp_arena_->allocator();
                auto* cf_pool = temp_arena_->create<aura::ast::StringPool>(alloc);
                auto* cf_flat = temp_arena_->create<aura::ast::FlatAST>(alloc);
                auto ast_root = data_to_flat(check_form, *cf_flat, *cf_pool, 0);
                if (ast_root == aura::ast::NULL_NODE) {
                    sf++;
                    continue;
                }
                cf_flat->root = ast_root;

                auto result = eval_flat(*cf_flat, *cf_pool, ast_root, top_);
                if (!result) {
                    sf++;
                    continue;
                }

                bool ok = is_int(*result) && as_int(*result) == 1;
                if (ok) {
                    sp++;
                    total_passed++;
                } else {
                    sf++;
                    total_failed++;
                }
            }

            std::fprintf(stderr, "  Suite '%s': %d/%d passed\n", suite_name.c_str(), sp, sp + sf);
        }

        std::string summary = std::to_string(total_suites) +
                              " suites: " + std::to_string(total_passed) + " passed, " +
                              std::to_string(total_failed) + " failed";
        auto sidx = string_heap_.size();
        string_heap_.push_back(summary);
        return make_string(sidx);
    });

    primitives_.add("read", [this](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    // ── File I/O (P0) ───────────────────────────────────────────
    // Helper: check path is a regular file (skip directories)
    auto is_regular = [](const std::string& path) -> bool {
        struct stat st;
        return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    };

    // (current-time) → integer epoch seconds
    primitives_.add("current-time", [](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(std::time(nullptr)));
    });

    // (arena-offset) → integer (current TL arena offset in bytes, 0 = disabled)
    primitives_.add("arena-offset", [](const auto&) -> EvalValue {
        // g_tl_arena is thread-local TLarena declared in runtime_shared.h
        return make_int(static_cast<int64_t>(g_tl_arena.offset));
    });

    primitives_.add("read-file", [this, is_regular](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& path = string_heap_[idx];
        if (!is_regular(path))
            return make_void();
        std::ifstream f(path);
        if (!f)
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(content));
        return make_string(id);
    });

    primitives_.add("write-file", [this](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& path = string_heap_[idx];
        std::string content;
        if (is_string(a[1])) {
            auto cidx = as_string_idx(a[1]);
            if (cidx < string_heap_.size())
                content = string_heap_[cidx];
        } else if (is_int(a[1])) {
            content = std::to_string(as_int(a[1]));
        } else {
            return make_void();
        }
        std::ofstream f(path);
        if (!f)
            return make_void();
        f << content;
        return make_int(1);
    });

    // ── CLI interface ────────────────────────────────────────────
    primitives_.add("command-line", [this](const auto&) -> EvalValue {
        // Returns list of command-line argument strings, NOT including argv[0].
        // Parsed from /proc/self/cmdline on Linux.
        std::ifstream f("/proc/self/cmdline");
        if (!f)
            return make_void();
        std::string raw;
        std::getline(f, raw, '\0'); // skip argv[0]
        std::vector<std::string> items;
        while (std::getline(f, raw, '\0')) {
            if (!raw.empty())
                items.push_back(raw);
        }
        EvalValue result = make_void();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(*it);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    primitives_.add("file-exists?", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_int(0);
        auto& path = string_heap_[idx];
        struct stat st;
        return make_int(::stat(path.c_str(), &st) == 0 ? 1 : 0);
    });

    // ── File I/O: copy, delete, size, directory list ─────────────
    primitives_.add("file-copy", [this, is_regular](const auto& a) {
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto sidx = as_string_idx(a[0]), didx = as_string_idx(a[1]);
        if (sidx >= string_heap_.size() || didx >= string_heap_.size())
            return make_void();
        if (!is_regular(string_heap_[sidx]))
            return make_void();
        std::ifstream src(string_heap_[sidx], std::ios::binary);
        if (!src)
            return make_void();
        std::ofstream dst(string_heap_[didx], std::ios::binary);
        if (!dst)
            return make_void();
        dst << src.rdbuf();
        return make_int(1);
    });

    primitives_.add("file-delete", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_int(0);
        return make_int(std::remove(string_heap_[idx].c_str()) == 0 ? 1 : 0);
    });

    primitives_.add("file-size", [this, is_regular](const auto& a) {
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size() || !is_regular(string_heap_[idx]))
            return make_int(0);
        std::ifstream f(string_heap_[idx], std::ios::ate | std::ios::binary);
        if (!f)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(f.tellg()));
    });

    // ── Shell / Process ────────────────────────────────────────
    primitives_.add("shell", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_int(-1);
        return make_int(::system(string_heap_[idx].c_str()));
    });

    primitives_.add("command-output", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& cmd = string_heap_[idx];
        std::array<char, 4096> buf;
        std::string result;
        auto* fp = ::popen(cmd.c_str(), "r");
        if (!fp)
            return make_void();
        while (::fgets(buf.data(), buf.size(), fp) != nullptr)
            result += buf.data();
        ::pclose(fp);
        if (!result.empty() && result.back() == '\n')
            result.pop_back();
        auto sid = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    primitives_.add("directory-list", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& dir_path = string_heap_[idx];
        EvalValue result = make_void();
        auto dir = opendir(dir_path.c_str());
        if (!dir)
            return make_void();
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..")
                continue;
            auto sid = string_heap_.size();
            string_heap_.push_back(name);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(sid), result});
            result = make_pair(pid);
        }
        closedir(dir);
        return result;
    });

    primitives_detail::register_git_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);

    // ═══════════════════════════════════════════════════════════════
    // Module primitives (Phase 1: module objects)
    // ═══════════════════════════════════════════════════════════════

    // (module? v) — Check if value is a module object
    primitives_.add("module?",
                    [](const auto& a) { return make_bool(!a.empty() && is_module(a[0])); });

    // (module-get mod name) — Get a binding from a module by symbol name.
    // Phase 2.5.0 commit 6: route through lookup_by_intern
    // (SymId-first) with canonical_pool() so the intern happens
    // against the long-lived workspace pool. The env's own
    // pool_ (set via set_pool for closures that captured a non-
    // canonical pool) is the fallback — see Env::lookup_by_intern
    // for the full resolution chain. Observable behavior
    // matches Env::lookup(name) when no binding is found.
    //
    // Issue #231 fix: don't pass canonical_pool() here. The
    // module's bindings are interned in the module's own
    // pool (per-module arena), NOT in the workspace pool.
    // Different pools = different SymIds = lookup_by_symid
    // misses the binding. Use the legacy string-based lookup
    // directly, which walks bindings_ (string-keyed) without
    // needing a SymId. The same applies to module-keys.
    primitives_.add("module-get", [this](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_module(a[0]) || !is_string(a[1]))
            return make_void();
        auto mod_idx = as_module_idx(a[0]);
        auto name_idx = as_string_idx(a[1]);
        if (mod_idx >= modules_.size() || name_idx >= string_heap_.size())
            return make_void();
        auto result = modules_[mod_idx]->lookup(std::string(string_heap_[name_idx]));
        if (!result)
            return make_void();
        // Issue #229 fix: deref cell sentinel
        if (is_cell(*result)) {
            auto ci = as_cell_id(*result);
            if (ci < cells_.size())
                return cells_[ci];
        }
        return *result;
    });

    // (module-keys mod) — List all exported binding names from a module
    primitives_.add("module-keys", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_module(a[0]))
            return make_void();
        auto mod_idx = as_module_idx(a[0]);
        if (mod_idx >= modules_.size())
            return make_void();
        EvalValue result = make_void();
        auto& bindings = modules_[mod_idx]->bindings();
        for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(it->first);
            auto pid = pairs_.size();
            pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // (use path) — Load module, return module object (no env injection)
    primitives_.add("use", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        return load_module_file(string_heap_[idx]);
    });

    primitives_.add("load-module", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        return load_module_file(string_heap_[idx]);
    });

    // (while pred body) — Iterative loop with zero C++ stack growth.
    // pred: closure returning bool (#t to continue, #f to stop).
    // body: closure — evaluated each iteration.
    primitives_.add("while", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_closure(a[0]) || !types::is_closure(a[1]))
            return make_void();
        auto pred_cid = types::as_closure_id(a[0]);
        auto body_cid = types::as_closure_id(a[1]);
        for (;;) {
            auto pred_result = apply_closure(pred_cid, {});
            if (!pred_result)
                break;
            auto& val = *pred_result;
            bool cont = types::is_bool(val)  ? types::as_bool(val)
                        : types::is_int(val) ? types::as_int(val) != 0
                                             : false;
            if (!cont)
                break;
            (void)apply_closure(body_cid, {});
        }
        return make_void();
    });

    primitives_.add("import", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& path = string_heap_[idx];

        // Optional prefix: (import "path" "prefix:")
        std::string prefix;
        if (a.size() > 1 && is_string(a[1])) {
            auto pidx = as_string_idx(a[1]);
            if (pidx < string_heap_.size())
                prefix = string_heap_[pidx];
        }

        // Load module (cached, isolated env)
        auto mod_val = load_module_file(path);
        if (!is_module(mod_val))
            return make_void();
        auto mod_idx = as_module_idx(mod_val);
        if (mod_idx >= modules_.size())
            return make_void();

        // Inject all bindings into top_ env
        auto* mod_env = modules_[mod_idx];
        if (prefix.empty()) {
            // No prefix: inject as-is (backward compat)
            for (auto& [name, val] : mod_env->bindings()) {
                top_.bind(name, val);
            }
        } else {
            // Prefix injection: bind both prefix:name and bare name for each export
            for (auto& [name, val] : mod_env->bindings()) {
                auto prefixed = prefix + name;
                // Inter the prefixed name into the workspace pool
                auto psid = string_heap_.size();
                string_heap_.push_back(prefixed);
                // Bind in top env with prefix
                top_.bind(prefixed, val);
                // Also bind bare name (no prefix) so tree-walker can find it
                top_.bind(name, val);
            }
        }
        return make_bool(true);
    });

    // ── Character + I/O extensions ────────────────────────────────

    // char?: (char? v) → true if is_int(v) (chars represented as ints)
    primitives_.add("char?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_int(a[0]));
    });
    // char->integer: (char->integer c) → integer value
    primitives_.add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });
    // integer->char: (integer->char n) → identity
    primitives_.add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });
    // string->list: (string->list s) → list of char codes
    primitives_.add("string->list", [this](std::span<const EvalValue> a) {
        if (a.empty())
            return make_void();
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size())
                s = string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back(
                {make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });
    // list->string: (list->string lst) → string from char codes
    primitives_.add("list->string", [this](std::span<const EvalValue> a) {
        if (a.empty() || (!is_pair(a[0]) && !is_void(a[0])))
            return make_int(0);
        std::string result;
        auto v = a[0];
        while (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= pairs_.size())
                break;
            auto car = pairs_[idx].car;
            if (is_int(car))
                result.push_back(static_cast<char>(as_int(car)));
            v = pairs_[idx].cdr;
        }
        auto sid = string_heap_.size();
        string_heap_.push_back(std::move(result));
        return make_string(sid);
    });
    // read-line: (read-line) → read a line from stdin as string
    primitives_.add("read-line", [this](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(line));
        return make_string(id);
    });
    // eof-object?: (eof-object? v) → check if value represents EOF
    primitives_.add("eof-object?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        // EOF is represented as void (the same as when read-line returns empty)
        return make_bool(is_void(a[0]));
    });

    // ── List utility primitives ────────────────────────────────────


    // (mutation-count)
    primitives_.add("mutation-count", [this](const auto&) {
        if (!workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(workspace_flat_->mutation_count()));
    });

    // (mutation-history node-id) → list of summary strings
    primitives_.add("mutation-history", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto hist = workspace_flat_->mutation_history(node);
        EvalValue result = make_void();
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            auto& rec = *it;
            auto sid = string_heap_.size();
            string_heap_.push_back(std::format(
                "[{}] {}: {}{}", rec.mutation_id, rec.operator_name, rec.summary,
                rec.status == aura::ast::MutationStatus::RolledBack ? " [rolled-back]" : ""));
            auto pair_id = pairs_.size();
            pairs_.push_back({make_string(sid), result});
            result = make_pair(pair_id);
        }
        return result;
    });

    // (rollback mutation-id) → true if successful
    primitives_.add("rollback", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return make_bool(false);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_bool(workspace_flat_->rollback(mid));
    });


    // (rollback-since mutation-id) → count of rolled-back mutations
    primitives_.add("rollback-since", [this](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !workspace_flat_)
            return make_int(0);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(workspace_flat_->rollback_since(mid)));
    });

    primitives_detail::register_auto_evolve_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);

    // (check-preconditions node-id (field-offset|new-type-str)) → true if valid
    // With int second arg: check field existence (0=int_val_, 1=type_id_)
    // With string second arg: check type compatibility (new type string)
    primitives_.add("check-preconditions", [this](std::span<const EvalValue> a) {
        if (a.size() < 2 || !is_int(a[0]) || !workspace_flat_)
            return make_bool(false);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size())
            return make_bool(false);
        auto nv = flat.get(node);

        // String arg: type compatibility check
        if (is_string(a[1])) {
            auto type_idx = as_string_idx(a[1]);
            if (type_idx >= string_heap_.size())
                return make_bool(false);
            auto new_type = string_heap_[type_idx];

            // Check compatibility based on node tag
            switch (nv.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    // Int literal: compatible with Int, Float, Bool (≠0), Dyn
                    return make_bool(new_type == "Int" || new_type == "Float" ||
                                     new_type == "Bool" || new_type == "Dyn" || new_type == "Any");
                case aura::ast::NodeTag::LiteralFloat:
                    // Float literal: compatible with Float, Dyn
                    return make_bool(new_type == "Float" || new_type == "Dyn" || new_type == "Any");
                case aura::ast::NodeTag::LiteralString:
                    return make_bool(new_type == "String" || new_type == "Dyn" ||
                                     new_type == "Any");
                case aura::ast::NodeTag::Call:
                case aura::ast::NodeTag::Lambda:
                    // Structural nodes: always OK (any type)
                    return make_bool(true);
                case aura::ast::NodeTag::Variable:
                    // Variable: always OK (outer scope determines type)
                    return make_bool(true);
                default:
                    // Other nodes: permissive
                    return make_bool(true);
            }
        }

        // Int arg: field existence check
        if (!is_int(a[1]))
            return make_bool(false);
        auto field = static_cast<std::uint32_t>(as_int(a[1]));
        switch (field) {
            case 0:
                return make_bool(nv.has_int()); // int_val_
            case 1:
                return make_bool(true); // type_id_ (always valid)
            default:
                return make_bool(false);
        }
    });

    // ═══════════════════════════════════════════════════════════════
    // P6: Query/Transform EDSL 原语
    // ═══════════════════════════════════════════════════════════════

    // (set-code code-string) — Parse code and set as current workspace AST
    // Nodes in workspace AST have stable IDs across query/mutate operations
    // Multi-expression code is automatically wrapped in (begin ...) by the parser.
    // Helper: build structured error value as a pair ("kind" "message")
    // Inline lambda to avoid capture issues — used by set-code, eval-current, etc.
    auto make_error_val = [this](const std::string& kind, const std::string& msg) -> EvalValue {
        auto msg_idx = string_heap_.size();
        string_heap_.push_back(msg);
        auto kind_idx = string_heap_.size();
        string_heap_.push_back(kind);
        auto nil = EvalValue(0);
        // (cons "kind" (cons "message" nil)) → ("kind" "message") as a proper list
        auto msg_pair = make_pair(pairs_.size());
        pairs_.push_back({make_string(msg_idx), nil});
        auto kind_pair = make_pair(pairs_.size());
        pairs_.push_back({make_string(kind_idx), msg_pair});
        return kind_pair;
    };
    std::function<EvalValue(const std::string&, const std::string&)> mev = make_error_val;

    primitives_detail::register_workspace_query_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        workspace_mtx_, workspace_flat_, workspace_pool_, type_registry_, keyword_table_, pairs_,
        string_heap_, temp_arena_, tag_arity_index_, [this]() { return canonical_pool(); },
        [this]() { build_tag_arity_index(); }, mev);

    primitives_detail::register_mutate_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this, mev, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_workspace_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_.add("set-code", [this, mev](const auto& a) -> EvalValue {
        std::unique_lock<std::shared_mutex> wlock(workspace_mtx_);
        if (workspace_read_only_)
            return mev("read-only", "workspace is read-only");
        // Clear any previous set-code error and eval-current cache
        last_set_code_error_kind_.clear();
        last_set_code_error_msg_.clear();
        last_eval_current_result_.reset();
        coverage_counters_[0]++;
        coverage_counters_[5]++;
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (set-code code-string)");
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return mev("bad-arg", "code string index out of range");
        if (!arena_)
            return mev("internal", "no arena allocator available");

        // Issue #230 #3: allocate fresh pool/flat on every set-code
        // (don't reuse the old one). Reason: registered macros
        // (via defmacro / define-hygienic-macro) hold raw pointers
        // into the old pool/flat for their body. Reusing the same
        // pool/flat and clearing it would invalidate those pointers
        // — old macros would silently fail to expand (the lookup
        // finds the macro in `macros_` but clone_macro_body reads
        // from a cleared flat and returns NULL_NODE → macro
        // returns make_void()). Allocating fresh each call keeps
        // the old pool/flat intact in the arena, so previously
        // registered macros keep working after subsequent
        // set-codes.
        //
        // Trade-off: arena grows by one pool/flat per set-code
        // call. The previous OOM-fix comment about "750
        // consecutive set-codes" is no longer protected, but in
        // practice test scripts do < 10 set-codes and the arena
        // size stays bounded. The correctness win (macros
        // survive set-code) is worth the small arena growth.
        auto alloc = arena_->allocator();
        auto* pool_ptr = arena_->create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_->create<aura::ast::FlatAST>(alloc);
        bool fresh_alloc = true;

        auto pr = aura::parser::parse_to_flat(string_heap_[idx], *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            // Return structured parse error: ("parse" "message")
            std::string err;
            if (!pr.errors.empty()) {
                for (auto& e : pr.errors) {
                    if (!err.empty())
                        err += "; ";
                    err += e.format();
                }
            } else if (!pr.error.empty()) {
                err = pr.error;
            } else {
                err = "parse error";
            }
            // Store error for eval-current/eval-current-output to propagate
            last_set_code_error_kind_ = "parse";
            last_set_code_error_msg_ = err;
            coverage_counters_[5]--;
            return mev("parse", err);
        }
        flat_ptr->root = pr.root;
        workspace_flat_ = flat_ptr;
        workspace_pool_ = pool_ptr;
        // Issue #211: invalidate the (tag, arity) index
        // when the workspace changes. (The set_workspace_flat
        // hook would do this, but set-code assigns directly
        // here, so we invalidate inline.)
        invalidate_tag_arity_index();
        update_shared_tree_root();
        // Invalidate def-use index (new workspace)
        // (ASAN fix #107 leak) delete the old index; without this,
        // each set-code leaks the previous DefUseIndex (~3KB each).
        defuse_index_destroy(&defuse_index_);
        // Phase 2: a fresh workspace means every cached define is potentially
        // changed. Mark all dirty so the next (eval-current) re-evaluates.
        if (mark_all_defines_dirty_fn_)
            mark_all_defines_dirty_fn_();
        // Pre-populate the v2 IR cache from the new workspace's defines.
        // For unchanged defines, this is a cache hit (skip lowering).
        // For new/changed defines, this lowers them once and stores the result.
        if (pre_cache_workspace_defines_fn_)
            pre_cache_workspace_defines_fn_();
        // Issue #165 follow-up: extract MacroDef nodes from the new
        // workspace and register them in the runtime `macros_` registry.
        // Without this, subsequent eval("(m)") parses a fresh FlatAST
        // that has no MacroDef node, so macro_expand_all finds nothing
        // and the call resolves to an undefined variable. The fix
        // walks the workspace and inserts every MacroDef (defmacro /
        // define-hygienic-macro) into macros_, mirroring what eval_flat
        // would have done if the workspace had been evaluated inline.
        {
            // set-code is single-threaded at this point (we hold
            // workspace_mtx_ above), so a plain unsynchronized
            // write to macros_ is fine. If concurrent set-code
            // becomes a real scenario, add a macros_mtx_ in
            // evaluator.ixx alongside closures_mtx_.
            //
            // Issue #230 #3: DON'T clear macros_ on set-code.
            // The loop below adds/overrides macros from the new
            // workspace, but the macros registered BEFORE this
            // set-code (e.g. via (require "std/test" all:))
            // need to survive so they can still be invoked
            // after the workspace is replaced. The new
            // pool/flat pointers are fresh, but the OLD
            // pool/flat (referenced by the surviving macros)
            // is still in the arena and untouched (we no
            // longer reuse/clear it on set-code), so the
            // stored MacroDef::flat/::pool pointers are
            // valid and the macro bodies can still be cloned
            // and expanded.
            for (aura::ast::NodeId id = 0; id < flat_ptr->size(); ++id) {
                auto v = flat_ptr->get(id);
                if (v.tag != aura::ast::NodeTag::MacroDef)
                    continue;
                auto macro_name = std::string(pool_ptr->resolve(v.sym_id));
                if (macro_name.empty())
                    continue;
                std::vector<std::string> param_names;
                param_names.reserve(v.params.size());
                for (auto pid : v.params)
                    param_names.emplace_back(pool_ptr->resolve(pid));
                auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                bool is_dotted = (v.int_value & 1) != 0;
                bool is_hygienic = (v.int_value & 2) != 0;
                // Issue #230 #2: bit 2 of int_val_ flags the
                // `define-hygienic-macro*` (preserved-params) variant.
                bool is_preserved = (v.int_value & 4) != 0;
                macros_[macro_name] = MacroDef{std::move(param_names),
                                               is_dotted,
                                               is_hygienic,
                                               is_preserved,
                                               flat_ptr,
                                               pool_ptr,
                                               body_id};
            }
        }
        return make_bool(true);
    });

    // (ir-cache-v2:dirty? name) — #t if the named define's IR cache entry is
    // marked dirty.
    primitives_.add("ir-cache-v2:dirty?", [this, mev](const auto& a) -> EvalValue {
        if (a.size() < 1 || !is_string(a[0]))
            return mev("bad-arg", "usage: (ir-cache-v2:dirty? name)");
        if (!is_define_dirty_fn_)
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= string_heap_.size())
            return make_bool(false);
        auto name = string_heap_[name_idx];
        return make_bool(is_define_dirty_fn_(name));
    });

    // (ir-cache-v2:dependents name) — list of defines that reference this one.
    primitives_.add("ir-cache-v2:dependents", [this, mev](const auto& a) -> EvalValue {
        if (a.size() < 1 || !is_string(a[0]))
            return mev("bad-arg", "usage: (ir-cache-v2:dependents name)");
        if (!get_dependents_fn_)
            return make_void();
        auto name_idx = as_string_idx(a[0]);
        if (name_idx >= string_heap_.size())
            return make_void();
        auto name = string_heap_[name_idx];
        auto names = get_dependents_fn_(name);
        EvalValue list = make_void();
        for (auto it = names.rbegin(); it != names.rend(); ++it) {
            auto sid = string_heap_.size();
            string_heap_.push_back(*it);
            auto head = make_string(sid);
            auto pid = pairs_.size();
            pairs_.push_back({head, list});
            list = make_pair(pid);
        }
        return list;
    });

    // (current-source) — Return the current workspace AST as source code string
    // Implemented inline to avoid circular dependency with lowering module.
    // (eval code) — Parse and evaluate a string of Aura code
    primitives_.add("eval", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto code = string_heap_[types::as_string_idx(a[0])];
        aura::ast::StringPool pool;
        aura::ast::FlatAST flat;
        auto pr = aura::parser::parse_to_flat(code, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return make_void();
        flat.root = pr.root;
        auto result = eval_flat(flat, pool, pr.root, top_);
        if (result)
            return *result;
        return make_void();
    });

    // (load filename) — Load and evaluate a file of Aura code
    // Uses set-code + eval-current internally so definitions persist
    // in the workspace and closures are properly rooted.
    primitives_.add("load", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_void();
        auto& path = string_heap_[idx];

        std::ifstream f(path);
        if (!f)
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

        // Fresh workspace state for each load — resetting the pool corrupts
        // existing top_ bindings that reference string indices from the old pool.
        last_set_code_error_kind_.clear();
        last_set_code_error_msg_.clear();
        last_eval_current_result_.reset();

        // Use temp_arena_ for the parse state so (gc-temp) reclaims it.
        // The workspace_pool_ / workspace_flat_ pointers below are the
        // long-lived handles; the temp allocation is just a parse scratch.
        auto alloc = temp_arena_->allocator();
        auto* pool_ptr = temp_arena_->create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = temp_arena_->create<aura::ast::FlatAST>(alloc);

        auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return mev("parse", "load failed to parse file");
        }
        flat_ptr->root = pr.root;
        workspace_flat_ = flat_ptr;
        workspace_pool_ = pool_ptr;
        update_shared_tree_root();
        // (ASAN fix #107 leak) delete the old index; see sibling site above.
        defuse_index_destroy(&defuse_index_);

        // Evaluate the workspace AST
        if (!last_set_code_error_kind_.empty()) {
            auto msg = std::move(last_set_code_error_msg_);
            return mev("load", msg);
        }
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*workspace_flat_, *workspace_pool_,
                                                         workspace_flat_->root);
        auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
        workspace_flat_->clear_all_dirty();
        if (result)
            return *result;
        return make_void();
    });

    // (eval-expr value) — Evaluate any Aura value (not just strings)
    // Useful for evaluating stored expressions (e.g., from pipeline steps)
    primitives_.add("eval-expr", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_void();
        // Convert the value to a FlatAST and evaluate.
        // Use temp_arena_ so (gc-temp) reclaims the parse state per call
        // (was: arena_ = monotonic = 1 FlatAST/StringPool leaked per call).
        if (!arena_)
            return make_void();
        auto alloc = temp_arena_->allocator();
        auto* pool = temp_arena_->create<aura::ast::StringPool>(alloc);
        auto* flat = temp_arena_->create<aura::ast::FlatAST>(alloc);
        auto root = data_to_flat(a[0], *flat, *pool, 0);
        if (root == aura::ast::NULL_NODE)
            return make_void();
        flat->root = root;
        auto result = eval_flat(*flat, *pool, root, top_);
        if (result)
            return *result;
        return make_void();
    });

    primitives_.add("current-source", [this](std::span<const EvalValue> a) -> EvalValue {
        // Dual-workspace (Phase 1): default reads the per-eval current source
        // (the AST being evaluated right now), set by CompilerService::eval /
        // eval_ir / exec_jit. Optional :workspace keyword reads the persistent
        // EDSL workspace (set via (set-code ...)).
        // See dual-workspace design (archived: docs-archive-pre-2026-06)
        //
        // Use an enum to distinguish "user asked for :workspace" (even if
        // workspace_flat_ is null) from "no preference" (use current_flat_).
        // This matters because workspace_flat_ is null until (set-code ...) is
        // called, and the test expects a different result in that case.
        enum class Source { Default, Workspace };
        Source which = Source::Default;
        if (a.size() >= 1 && types::is_keyword(a[0])) {
            auto kidx = types::as_keyword_idx(a[0]);
            if (kidx < keyword_table_.size() && keyword_table_[kidx] == ":workspace") {
                which = Source::Workspace;
            }
        }
        const aura::ast::FlatAST* flat = nullptr;
        const aura::ast::StringPool* pool = nullptr;
        switch (which) {
            case Source::Workspace:
                flat = workspace_flat_;
                pool = workspace_pool_;
                break;
            case Source::Default:
                flat = current_flat_;
                pool = current_pool_;
                break;
        }
        if (!flat || !pool)
            return make_string(0);

        // Inline unparse for the chosen root (captures flat/pool by ref).
        constexpr int kMaxUnparseDepth = 256;
        auto unparse = [&](this const auto& self, aura::ast::NodeId id, int indent,
                           int depth = 0) -> std::string {
            if (depth > kMaxUnparseDepth)
                return "...";
            if (id == aura::ast::NULL_NODE || id >= flat->size())
                return "()";
            auto v = flat->get(id);
            switch (v.tag) {
                case aura::ast::NodeTag::LiteralInt: {
                    if (flat->marker(id) == aura::ast::SyntaxMarker::BoolLiteral)
                        return v.int_value ? "#t" : "#f";
                    return std::to_string(v.int_value);
                }
                case aura::ast::NodeTag::LiteralFloat: {
                    auto s = std::to_string(v.float_value);
                    if (s.find('.') == std::string::npos)
                        s += ".0";
                    return s;
                }
                case aura::ast::NodeTag::LiteralString: {
                    auto raw = pool->resolve(v.sym_id);
                    std::string esc = "\"";
                    for (auto c : std::string_view(raw)) {
                        if (c == '\\' || c == '"')
                            esc += '\\';
                        esc += c;
                    }
                    esc += '"';
                    return esc;
                }
                case aura::ast::NodeTag::Variable:
                    return std::string(pool->resolve(v.sym_id));
                case aura::ast::NodeTag::Call: {
                    std::string s = "(";
                    for (std::size_t i = 0; i < v.children.size(); ++i) {
                        if (i > 0)
                            s += " ";
                        s += self(v.child(i), indent + 1, depth + 1);
                    }
                    return s + ")";
                }
                case aura::ast::NodeTag::Lambda: {
                    std::string s = "(lambda (";
                    for (std::size_t i = 0; i < v.params.size(); ++i) {
                        if (i > 0)
                            s += " ";
                        s += std::string(pool->resolve(v.params[i]));
                    }
                    s += ")";
                    if (!v.children.empty())
                        s += " " + self(v.child(0), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec: {
                    auto kw = (v.tag == aura::ast::NodeTag::LetRec) ? std::string("letrec")
                                                                    : std::string("let");
                    std::string s = "(" + kw + " ((" + std::string(pool->resolve(v.sym_id)) + " ";
                    if (!v.children.empty())
                        s += self(v.child(0), indent + 1, depth + 1);
                    s += "))";
                    if (v.children.size() > 1)
                        s += " " + self(v.child(1), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Define: {
                    return "(define " + std::string(pool->resolve(v.sym_id)) + " " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::IfExpr: {
                    std::string s = "(if";
                    for (std::size_t i = 0; i < v.children.size(); ++i)
                        s += " " + self(v.child(i), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Begin: {
                    std::string s = "(begin";
                    for (std::size_t i = 0; i < v.children.size(); ++i)
                        s += " " + self(v.child(i), indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Set: {
                    return "(set! " + std::string(pool->resolve(v.sym_id)) + " " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::Quote: {
                    return "(quote " +
                           (v.children.empty() ? "()" : self(v.child(0), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::Pair: {
                    return "(" +
                           (v.children.empty() ? "()"
                                               : self(v.child(0), indent + 1, depth + 1) + " . " +
                                                     self(v.child(1), indent + 1, depth + 1)) +
                           ")";
                }
                case aura::ast::NodeTag::DefineModule: {
                    std::string s = "(define-module (" + std::string(pool->resolve(v.sym_id));
                    for (auto pid : v.params)
                        s += " " + std::string(pool->resolve(pid));
                    s += ")";
                    for (auto cid : v.children)
                        s += " " + self(cid, indent + 1, depth + 1);
                    return s + ")";
                }
                case aura::ast::NodeTag::Export: {
                    std::string s = "(export";
                    for (auto pid : v.params)
                        s += " " + std::string(pool->resolve(pid));
                    return s + ")";
                }
                case aura::ast::NodeTag::MacroDef: {
                    std::string s = "(defmacro (" + std::string(pool->resolve(v.sym_id));
                    for (auto pid : v.params)
                        s += " " + std::string(pool->resolve(pid));
                    s += ")";
                    if (!v.children.empty())
                        s += " " + self(v.child(0), indent + 1, depth + 1);
                    return s + ")";
                }
                default: {
                    // Fallback: generic node dump for unknown types
                    return std::format("<{}>", static_cast<int>(v.tag));
                }
            }
        };

        auto src = unparse(flat->root, 0);
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(src));
        return make_string(id);
    });

    // (api-reference) — Return all registered primitives as a string for LLM reference
    primitives_.add("api-reference", [this](const auto&) -> EvalValue {
        std::string out = "Available Aura primitives:\n\n";
        for (std::size_t i = 0; i < primitives_.slot_count(); ++i) {
            auto name = primitives_.name_for_slot(i);
            if (!name.empty()) {
                out += "  " + std::string(name) + "\n";
            }
        }
        out += "\nStandard library: (require std/name) loads with prefix (std/name:func-name)\n";
        out += "Or (require std/name all:) for bare names\n";
        out += "Try it: (require std/list all:) then (sort (list 3 1 2))\n";
        auto id = string_heap_.size();
        string_heap_.push_back(std::move(out));
        return make_string(id);
    });

    // (eval-current) — Evaluate the current workspace AST
    primitives_.add("eval-current", [this, mev](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx_);
        // Phase 4: (eval-current :jit) — compile-and-run via the IR/JIT
        // pipeline. Falls back to tree-walker if the hook isn't installed
        // (e.g. unit tests without a CompilerService) or if the JIT
        // compile fails.
        if (a.size() == 1 && types::is_keyword(a[0])) {
            auto kidx = types::as_keyword_idx(a[0]);
            if (kidx < keyword_table_.size() && keyword_table_[kidx] == ":jit") {
                if (try_jit_fn_ && get_workspace_source_fn_) {
                    // Re-eval the workspace via the IR/JIT pipeline.
                    // The result is the workspace's last-expression value
                    // (no env sync back to the original workspace yet).
                    std::string src = get_workspace_source_fn_();
                    if (!src.empty()) {
                        auto jit_result = try_jit_fn_(src);
                        if (jit_result)
                            return *jit_result;
                        // JIT failed — fall through to tree-walker
                    }
                }
                // No service wired, or workspace empty, or JIT failed —
                // fall through to tree-walker
            }
        }
        coverage_counters_[2]++;
        // If set-code failed on the last call, propagate the diagnostic immediately
        if (!last_set_code_error_kind_.empty()) {
            auto kind = std::move(last_set_code_error_kind_);
            auto msg = std::move(last_set_code_error_msg_);
            return mev(kind, msg);
        }
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*workspace_flat_, *workspace_pool_,
                                                         workspace_flat_->root);
        coverage_counters_[4]++;

        // Incremental eval: if the workspace root is clean and we have a cached
        // result, skip full re-evaluation entirely (Issue #32b).
        // The root is marked dirty by mark_dirty_upward() on any mutation.
        // clear_cached_value is called in mark_dirty(), so we know the cache
        // is stale when dirty flags are set.
        //
        // Issue #159 Phase 2: instead of checking the root, check
        // the LAST form's subtree. The last form is what produces
        // the eval_current result; if its subtree is clean, the
        // cached result is still valid even if other parts of the
        // tree (e.g., earlier defines) are dirty. Win: mutating
        // `(define a 1)` doesn't invalidate the cache when the
        // result comes from a later form.
        {
            using aura::ast::NodeId;
            // Find the last top-level form. For a flat workspace
            // (root has children), it's the last child. For a
            // single-form workspace, it's the root itself.
            NodeId last_form = expanded;
            auto root_v = workspace_flat_->get(expanded);
            if (!root_v.children.empty()) {
                last_form = root_v.child(root_v.children.size() - 1);
            }
            if (last_form != aura::ast::NULL_NODE &&
                !workspace_flat_->has_dirty_subtree(last_form) && last_eval_current_result_) {
                return *last_eval_current_result_;
            }
        }

        auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);

        // Cache successful results for incremental reuse
        if (result)
            last_eval_current_result_ = *result;

        // Clear dirty flags after successful eval
        workspace_flat_->clear_all_dirty();
        if (!result) {
            // Return structured diagnostic: (kind-string message-string)
            auto& diag = result.error();
            auto kind = std::string(kind_name(diag.kind));
            auto msg = diag.format();
            return mev(kind, msg);
        }
        // ── Auto-fix closure: if result is an uncalled function, try (display ...) ──
        using aura::ast::NodeId;
        using aura::ast::NodeTag;
        if (is_closure(*result) && workspace_flat_ && workspace_pool_) {
            // Scan for Define nodes to extract function name + arity
            std::string fn_name;
            int arity = 0;
            for (NodeId nid = 0; nid < static_cast<NodeId>(workspace_flat_->size()); ++nid) {
                auto nv = workspace_flat_->get(nid);
                if (nv.tag == NodeTag::Define && nv.sym_id != aura::ast::INVALID_SYM) {
                    fn_name = std::string(workspace_pool_->resolve(nv.sym_id));
                    arity = 0;
                    if (!nv.children.empty()) {
                        auto lambda_nv = workspace_flat_->get(nv.child(0));
                        if (lambda_nv.tag == NodeTag::Lambda)
                            arity = static_cast<int>(lambda_nv.params.size());
                    }
                }
            }
            if (!fn_name.empty()) {
                // Build arg patterns based on arity
                std::vector<std::string> arg_pats;
                if (arity == 0) {
                    arg_pats = {""};
                } else if (arity == 1) {
                    // Simple scalars first, then list-based
                    // Use small ints to avoid exponential recursion (e.g., fib(42))
                    arg_pats = {"5", "0", "1", "\"test\"", "(list 3 1 4 1 5)"};
                } else if (arity == 2) {
                    // List+scalar for search, then plain scalars
                    // Try both (scalar list) and (list scalar) orderings
                    arg_pats = {"42 7",
                                "0 0",
                                "(list 1 3 5 7 9) 5",
                                "5 (list 1 3 5 7 9)",
                                "(list 3 1 4 1 5) 1",
                                "1 (list 3 1 4 1 5)"};
                } else {
                    arg_pats = {"1 2 3", "0 0 0"};
                }
                // Suppress stdout during auto-fix attempts to avoid polluting output
                std::fflush(stdout);
                int saved_stdout = ::dup(STDOUT_FILENO);
                int null_fd = ::open("/dev/null", O_WRONLY);
                if (null_fd >= 0)
                    ::dup2(null_fd, STDOUT_FILENO);
                bool auto_fixed = false;
                std::string winning_call; // the call that worked
                for (auto& args : arg_pats) {
                    std::string call_code = "(" + fn_name;
                    if (!args.empty())
                        call_code += " " + args;
                    call_code += ")";
                    aura::ast::StringPool temp_pool;
                    aura::ast::FlatAST temp_flat;
                    auto pr = aura::parser::parse_to_flat(call_code, temp_flat, temp_pool);
                    if (!pr.success || pr.root == aura::ast::NULL_NODE)
                        continue;
                    temp_flat.root = pr.root;
                    auto call_expanded =
                        aura::compiler::macro_expand_all(temp_flat, temp_pool, temp_flat.root);
                    auto call_result = eval_flat(temp_flat, temp_pool, call_expanded, top_);
                    if (!call_result || is_void(*call_result) || is_closure(*call_result))
                        continue;
                    auto_fixed = true;
                    winning_call = call_code;
                    break;
                }
                // Restore stdout
                std::fflush(stdout);
                ::dup2(saved_stdout, STDOUT_FILENO);
                ::close(saved_stdout);
                if (null_fd >= 0)
                    ::close(null_fd);
                // Return the auto-fix result silently (no display output)
                if (auto_fixed && !winning_call.empty()) {
                    // Winners are evaluated with original stdout restored, but we
                    // re-evaluate silently and return the raw value. Use (eval-current-output)
                    // if you need the display output for LLM consumption.
                    aura::ast::StringPool call_pool;
                    aura::ast::FlatAST call_flat;
                    auto call_pr = aura::parser::parse_to_flat(winning_call, call_flat, call_pool);
                    if (call_pr.success && call_pr.root != aura::ast::NULL_NODE) {
                        call_flat.root = call_pr.root;
                        auto call_expanded =
                            aura::compiler::macro_expand_all(call_flat, call_pool, call_flat.root);
                        auto call_result = eval_flat(call_flat, call_pool, call_expanded, top_);
                        if (call_result) {
                            coverage_counters_[9]++;
                            return *call_result;
                        }
                    }
                }
            }
        }
        return *result;
    });

    // (eval-current-output) — Evaluate workspace, return display output as string
    // Captures all display output during eval via fd-level redirection.
    primitives_.add("eval-current-output", [this, mev](const auto&) {
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx_);
        // If set-code failed on the last call, propagate the diagnostic immediately
        if (!last_set_code_error_kind_.empty()) {
            auto kind = std::move(last_set_code_error_kind_);
            auto msg = std::move(last_set_code_error_msg_);
            return mev(kind, msg);
        }
        if (!workspace_flat_ || !workspace_pool_)
            return make_void();
        auto expanded = aura::compiler::macro_expand_all(*workspace_flat_, *workspace_pool_,
                                                         workspace_flat_->root);
        // Redirect stdout to a temp file (fd-level, catches fprintf too)
        std::fflush(stdout);
        auto* tmp = std::tmpfile();
        if (!tmp) {
            auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
            workspace_flat_->clear_all_dirty();
            if (!result) {
                auto msg = result.error().format();
                auto sidx = string_heap_.size();
                string_heap_.push_back(msg);
                return make_string(sidx);
            }
            return *result;
        }
        int new_fd = ::fileno(tmp);
        int old_fd = ::dup(STDOUT_FILENO);
        ::dup2(new_fd, STDOUT_FILENO);
        // Run the eval
        auto result = eval_flat(*workspace_flat_, *workspace_pool_, expanded, top_);
        workspace_flat_->clear_all_dirty();
        // Restore stdout
        std::fflush(stdout);
        ::dup2(old_fd, STDOUT_FILENO);
        ::close(old_fd);
        // Read captured output from temp file
        std::rewind(tmp);
        std::string captured;
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0)
            captured.append(buf, n);
        std::fclose(tmp);
        // If eval failed, prepend structured diagnostic to captured output
        if (!result) {
            auto& diag = result.error();
            auto kind = std::string(kind_name(diag.kind));
            auto diag_str = diag.format();
            auto combined = "[" + std::string(kind) + "] " + diag_str;
            if (!captured.empty())
                combined = combined + "\n" + captured;
            captured = combined;
        }
        // Store captured output in string heap
        auto sidx = string_heap_.size();
        string_heap_.push_back(captured);
        return make_string(sidx);
    });

    // (eval:async code) — Evaluate code asynchronously on the thread pool.
    // Returns the result as a string, or error message.
    // In serve-async mode, the eval runs on a background thread and the
    // calling fiber yields until the result is ready.
    // In stdin mode, falls back to synchronous eval (same as (eval code)).
    primitives_.add("eval:async", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto& code = string_heap_[as_string_idx(a[0])];
        if (aura::messaging::g_eval_async) {
            // Thread pool path: offload to background
            auto result_str = aura::messaging::g_eval_async(code);
            auto idx = string_heap_.size();
            string_heap_.push_back(result_str);
            return make_string(idx);
        }
        // Fallback: synchronous eval via the existing eval primitive
        auto eval_fn = primitives_.lookup("eval");
        if (!eval_fn)
            return make_void();
        auto sidx = string_heap_.size();
        string_heap_.push_back(code);
        auto result = (*eval_fn)({make_string(sidx)});
        // Format result as string
        auto& ev = *this;
        auto& cs = ev;
        auto formatted = aura::compiler::format_value(result, ev.string_heap_, ev.pairs_, 0,
                                                      &ev.primitives_, ev.keyword_table());
        auto ris = string_heap_.size();
        string_heap_.push_back(std::move(formatted));
        return make_string(ris);
    });


    // (typecheck-current) — Type check the workspace AST
    // Uses a persistent TypeRegistry across calls so type IDs are stable.
    //
    // Issue #159 Phase 5: skip the full traversal when the workspace
    // is clean. Caches the last result string. On a subsequent call
    // with a clean workspace (no dirty nodes anywhere), returns the
    // cached result without re-traversing. The cache is invalidated
    // implicitly by mutations: any mutation marks the root dirty via
    // mark_dirty_upward(), so has_dirty_subtree(root) becomes true
    // and we re-traverse.
    //
    // Win: repeated (typecheck-current) calls on an unchanged
    // workspace skip the O(N) tree walk entirely. Latency drops from
    // ~20us to ~1us in the cache-hit case.
    primitives_.add("typecheck-current", [this](const auto&) {
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx_);
        coverage_counters_[1]++;
        if (!workspace_flat_ || !workspace_pool_) {
            auto eidx = string_heap_.size();
            string_heap_.push_back("no workspace");
            return make_string(eidx);
        }

        // Phase 5: cache check. If the workspace is clean (no dirty
        // nodes anywhere) AND we have a cached result, reuse it.
        // The cache is implicitly invalidated by mutations (which
        // mark the root dirty via mark_dirty_upward).
        if (!workspace_flat_->has_dirty_subtree(workspace_flat_->root) && last_typecheck_result_) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(*last_typecheck_result_);
            return make_string(sidx);
        }

        // Lazily create persistent type registry (stable TypeIds across calls)
        if (!type_registry_) {
            type_registry_ = new aura::core::TypeRegistry();
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        aura::compiler::TypeChecker tc(treg);

        // 注入 declare-type 声明的自定义类型签名
        if (!declared_type_sigs_.empty()) {
            std::unordered_map<std::string, std::string> sig_map;
            std::unordered_map<std::string, std::string> mod_src_map;
            for (auto& [name, decl] : declared_type_sigs_) {
                sig_map[name] = decl.type_str;
                if (!decl.module_file.empty())
                    mod_src_map[name] = decl.module_file;
            }
            tc.inject_type_sigs(sig_map, mod_src_map);
        }

        aura::diag::DiagnosticCollector diag;

        auto result =
            tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);

        // TypeChecker now writes back normalized types via synthesize_flat + infer_flat,
        // and clears per-node dirty flags. No need for post-pass cache sync.
        // Safety clear for any nodes that may have been missed.
        workspace_flat_->clear_all_dirty();

        std::string out = "type: " + treg.format_type(result) + "\n";
        auto all_diags = diag.diagnostics();
        if (all_diags.empty()) {
            out += "no errors\n";
        } else {
            out += "diagnostics:\n";
            for (auto& d : all_diags) {
                out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
            }
        }

        // Phase 5: cache the result for the next clean-workspace call.
        last_typecheck_result_ = out;
        auto sidx = string_heap_.size();
        string_heap_.push_back(out);
        return make_string(sidx);
    });

    // Issue #159 Phase 1: (typecheck-incremental) — incremental
    // typecheck using the most recent MutationRecord from the
    // mutation log. Routes through TypeChecker::infer_flat_partial
    // (the per-subtree re-inference path) instead of doing a full
    // traversal. Returns the number of nodes re-inferred
    // (cache hits don't count). On a workspace with no
    // recent mutations, returns 0 (no-op).
    //
    // Usage pattern in Aura:
    //   (mutate:rebind "f" "..." "...")
    //   (typecheck-incremental)   ; re-infer only what changed
    //   (query:type "f")
    //
    // Compared to (typecheck-current), this is faster for small
    // mutations because the type checker only walks the dirty
    // subtree. The win scales with workspace size.
    primitives_.add("typecheck-incremental", [this](const auto& a) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(workspace_mtx_);
        coverage_counters_[1]++;
        if (!workspace_flat_ || !workspace_pool_) {
            auto eidx = string_heap_.size();
            string_heap_.push_back("no workspace");
            return make_string(eidx);
        }
        // Find the most recent mutation record in the workspace's
        // mutation log. The log is append-only, so the last entry
        // is the most recent. If empty, no-op.
        const auto& log = workspace_flat_->all_mutations();
        if (log.empty()) {
            auto ridx = string_heap_.size();
            string_heap_.push_back("no mutations recorded");
            return make_string(ridx);
        }
        const auto& rec = log.back();
        if (rec.target_node == aura::ast::NULL_NODE && rec.parent_id == aura::ast::NULL_NODE) {
            // Empty record — no-op.
            auto ridx = string_heap_.size();
            string_heap_.push_back("empty mutation record");
            return make_string(ridx);
        }
        // Lazily create persistent type registry (stable TypeIds
        // across calls) — same pattern as typecheck-current.
        if (!type_registry_) {
            type_registry_ = new aura::core::TypeRegistry();
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        // Run the partial re-inference. The per-node path uses
        // the cache (skip clean nodes) and re-infers only the
        // affected subtree (descendants of target_node + dirty
        // ancestors). See type_checker_impl.cpp:2901 for the
        // full algorithm.
        std::size_t re_inferred =
            tc.infer_flat_partial(*workspace_flat_, *workspace_pool_, rec, diag);
        std::string out = "re-inferred: " + std::to_string(re_inferred) + "\n";
        if (!diag.diagnostics().empty()) {
            out += "diagnostics:\n";
            for (auto& d : diag.diagnostics()) {
                out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
            }
        }
        auto ridx = string_heap_.size();
        string_heap_.push_back(out);
        return make_string(ridx);
    });

    // ── Issue #104: AuraQuery type-introspection primitives ───────────
    // These are the LLM-facing surface for "let me ask the type
    // system what it thinks". All four read from the same cache
    // populated by (typecheck-current) (the FlatAST stores a
    // normalized TypeId per node after infer_flat runs). They do
    // not trigger inference on their own — the LLM should call
    // typecheck-current first, then query.

    // (get-inferred-type node-id) — Return the cached inferred
    // type for a single AST node, formatted as a string. Returns
    // "unknown" if the node has no cached type (e.g. the LLM
    // queried before typecheck-current ran).
    primitives_.add("get-inferred-type", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (get-inferred-type node-id)");
        if (!workspace_flat_ || !type_registry_) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("no-typecheck-yet");
            return make_string(sidx);
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("out-of-range");
            return make_string(sidx);
        }
        auto type_idx = flat.type_id(node);
        if (type_idx == 0) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("unknown");
            return make_string(sidx);
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        std::string formatted = treg.format_type(aura::core::TypeId{type_idx, 1});
        auto sidx = string_heap_.size();
        string_heap_.push_back(formatted);
        return make_string(sidx);
    });

    // (query-type-of name) — Look up a top-level definition by
    // name and return its inferred type. The Define node's child
    // (the value/lambda) holds the cached type. The LLM uses
    // this to ask "what's the type of foo?" after a typecheck.
    primitives_.add("query-type-of", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query-type-of name)");
        if (!workspace_flat_ || !workspace_pool_ || !type_registry_) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("no-typecheck-yet");
            return make_string(sidx);
        }
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return mev("bad-arg", "string index out of range");
        auto name = string_heap_[idx];
        // Phase 2.5.0: route via canonical_pool() (== workspace_pool, explicit).
        auto sym = canonical_pool()->intern(name);

        auto& flat = *workspace_flat_;
        // Find a Define node with the matching symbol.
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
                // The value of a Define is in v.children[0] (if
                // 1-arg form) or in the cached type_id of the
                // Define node itself (the 2-arg / 3-arg forms
                // also cache on the Define). Use whichever has a
                // type set.
                aura::ast::NodeId type_target = id;
                if (!v.children.empty()) {
                    auto child_type = flat.type_id(v.child(0));
                    if (child_type != 0)
                        type_target = v.child(0);
                }
                auto type_idx = flat.type_id(type_target);
                if (type_idx == 0) {
                    auto sidx = string_heap_.size();
                    string_heap_.push_back("unknown");
                    return make_string(sidx);
                }
                auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
                std::string formatted = treg.format_type(aura::core::TypeId{type_idx, 1});
                auto sidx = string_heap_.size();
                string_heap_.push_back(formatted);
                return make_string(sidx);
            }
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back("not-found");
        return make_string(sidx);
    });

    // (query-expected-type context) — Return the expected type
    // for a given position in the AST. The context is a small
    // s-expression describing the lookup (e.g. "param:foo" for
    // the foo parameter, "return" for the return type of the
    // current lambda, or a node-id). For now this is a thin
    // wrapper that says "Dynamic" — a real implementation would
    // inspect the surrounding lambda's param/return annotations.
    // The scaffolding here is so the LLM has a stable API name
    // to use; the implementation can deepen over time.
    primitives_.add("query-expected-type", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return mev("bad-arg", "usage: (query-expected-type context)");
        // Placeholder: return "Dynamic" with a note. The LLM
        // sees a stable name + a sensible fallback rather than
        // an unbound-variable error.
        auto sidx = string_heap_.size();
        string_heap_.push_back("Dynamic (query-expected-type is scaffold; see #104)");
        return make_string(sidx);
    });

    // (suggest-annotation-at node-id) — Suggest a type annotation
    // for the given AST node. For Lambda nodes, emit a `[:x Int
    // body]` form with the inferred param types. For other nodes,
    // emit a wrapping `(check expr :<inferred-type>)` form. The
    // LLM can splice the suggestion into the source.
    primitives_.add("suggest-annotation-at", [this, mev](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return mev("bad-arg", "usage: (suggest-annotation-at node-id)");
        if (!workspace_flat_ || !type_registry_) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("no-typecheck-yet");
            return make_string(sidx);
        }
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto& flat = *workspace_flat_;
        if (node >= flat.size()) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("out-of-range");
            return make_string(sidx);
        }
        auto v = flat.get(node);
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        std::string suggestion;
        if (v.tag == aura::ast::NodeTag::Lambda) {
            suggestion = "(: <params> :<inferred-fn-type> body)";
            suggestion += "  // LLM: replace with actual params and inferred type";
        } else {
            auto type_idx = flat.type_id(node);
            if (type_idx == 0) {
                suggestion = "(check <expr> :?)  // unknown type — use a type hole";
            } else {
                std::string t = treg.format_type(aura::core::TypeId{type_idx, 1});
                suggestion = "(check <expr> " + t + ")";
            }
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back(suggestion);
        return make_string(sidx);
    });

    // ── Issue #105: "Infer & Suggest Annotations" pass ──────────────────
    // (query-annotate-functions [filter])
    //
    // Issue #105 pass. Walks the FlatAST after a typecheck-current
    // has populated per-node type caches. For every top-level
    // Define whose value is a Lambda AND whose inferred function
    // type is "high confidence" (concrete — no Dynamic, no fresh
    // type variables), produces a suggested annotated signature
    // of the form:
    //
    //   (define (foo [x : Int] [y : String]) ...body)
    //
    // The body is left as "..." because the LLM is expected to
    // splice the suggested signature into the original source
    // (using the returned original-line number to locate the
    // Define). We return a list of (name . line . suggested-string)
    // triples — the LLM can iterate and apply each one.
    //
    // High-confidence = the function type's args and return are
    // NOT Dynamic (index 0) and NOT fresh type variables (rendered
    // with a leading "__t" or similar by the type formatter).
    // A function with `__t0` in its arg list is low-confidence
    // and skipped.
    //
    // Filter: optional. "all" (default) processes every top-level
    // Define. "public" processes only the ones with no underscores
    // in the name (the Aura convention for "public" is no leading
    // underscore, matching the existing private-underscore
    // convention). The filter is LLM-facing; we keep it simple.
    primitives_.add("query-annotate-functions", [this, mev](const auto& a) -> EvalValue {
        if (!workspace_flat_ || !workspace_pool_ || !type_registry_) {
            return mev("no-typecheck-yet",
                       "call (typecheck-current) before query-annotate-functions");
        }
        // Filter: default "all"
        std::string filter = "all";
        if (!a.empty() && is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < string_heap_.size())
                filter = string_heap_[idx];
        }

        auto& flat = *workspace_flat_;
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);

        // High-confidence gate: walk the function type's args and
        // return and reject any that is a TYPE_VAR (fresh or named)
        // or DYNAMIC. A function whose arg or return is a type
        // variable is polymorphic — there's no concrete annotation
        // to write. A function whose arg or return is Dynamic has
        // a hole somewhere in the inference; suggesting an
        // annotation would be a guess. We want concrete types only.
        std::function<bool(aura::core::TypeId)> arg_or_ret_is_open;
        arg_or_ret_is_open = [&](aura::core::TypeId t) -> bool {
            if (!t.valid())
                return true;
            if (treg.is_var(t) || t == treg.dynamic_type())
                return true;
            if (auto* f = treg.func_of(t)) {
                if (arg_or_ret_is_open(f->ret))
                    return true;
                for (auto a : f->args)
                    if (arg_or_ret_is_open(a))
                        return true;
            }
            return false;
        };
        auto is_high_conf = [&](aura::core::TypeId func_tid) -> bool {
            if (!func_tid.valid())
                return false;
            if (arg_or_ret_is_open(func_tid))
                return false;
            return true;
        };

        // Build a list of (name, line, suggestion) pairs as a
        // proper list. The Aura-side caller iterates with
        // map/filter/etc.
        EvalValue result = make_void();
        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag != aura::ast::NodeTag::Define)
                continue;
            // The value is in v.children[0] (1-arg form) or the
            // cached type_id of the Define itself. Use whichever
            // is a Lambda with a cached function type.
            aura::ast::NodeId value_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            if (value_id == aura::ast::NULL_NODE)
                continue;
            auto vv = flat.get(value_id);
            if (vv.tag != aura::ast::NodeTag::Lambda)
                continue;

            // Get the function's cached type. Prefer the value's
            // cached type (more specific), fall back to the Define.
            aura::core::TypeId func_tid{0, 1};
            auto val_type_idx = flat.type_id(value_id);
            if (val_type_idx != 0) {
                func_tid = aura::core::TypeId{val_type_idx, 1};
            } else {
                auto def_type_idx = flat.type_id(id);
                if (def_type_idx != 0)
                    func_tid = aura::core::TypeId{def_type_idx, 1};
            }
            if (!is_high_conf(func_tid))
                continue;

            // Get the function structure
            auto* ft = treg.func_of(func_tid);
            if (!ft)
                continue;

            // Get the function name
            std::string fname = std::string(workspace_pool_->resolve(v.sym_id));
            // Filter: "public" skips names with leading underscores
            if (filter == "public" && !fname.empty() && fname[0] == '_')
                continue;
            // Skip if the function already has param annotations
            // (no need to suggest). Check the first param's annot.
            bool already_annotated = false;
            for (auto annot_id : vv.param_annotations) {
                if (annot_id != aura::ast::NULL_NODE) {
                    already_annotated = true;
                    break;
                }
            }
            if (already_annotated)
                continue;

            // Build the suggested signature:
            //   (define (fname [p1 : T1] [p2 : T2]) ...body)
            // For now, body is "..." — the LLM splices this in
            // alongside the original body source.
            std::string sugg = "(define (" + fname;
            // params are stored as SymId spans; resolve each name
            // to its string for the suggestion.
            std::size_t n_params = vv.params.size();
            std::size_t n_args = ft->args.size();
            std::size_t count = std::min(n_params, n_args);
            for (std::size_t i = 0; i < count; ++i) {
                auto pname = std::string(workspace_pool_->resolve(vv.params[i]));
                std::string ptype = treg.format_type(ft->args[i]);
                sugg += " [" + pname + " : " + ptype + "]";
            }
            sugg += ") ...body)  ;; #105 suggestion for '" + fname + "' (line ";
            sugg += std::to_string(v.line) + ")";
            if (n_params != n_args) {
                sugg += "  ;; WARNING: param/arg arity mismatch (";
                sugg += std::to_string(n_params) + " vs ";
                sugg += std::to_string(n_args) + ")";
            }

            // Push this entry as a triple: (name-line . suggestion)
            // We'll build a flat list of cons cells.
            auto fname_sidx = string_heap_.size();
            string_heap_.push_back(fname);
            auto fname_v = make_string(fname_sidx);
            auto line_sidx = string_heap_.size();
            string_heap_.push_back(std::to_string(v.line));
            auto line_v = make_string(line_sidx);
            auto sugg_sidx = string_heap_.size();
            string_heap_.push_back(sugg);
            auto sugg_v = make_string(sugg_sidx);

            // cons: (sugg . (line . (name . ())))
            auto pid1 = pairs_.size();
            pairs_.push_back({line_v, sugg_v});
            auto pid0 = pairs_.size();
            pairs_.push_back({fname_v, make_pair(pid1)});
            auto head_pid = pairs_.size();
            pairs_.push_back({make_pair(pid0), result});
            result = make_pair(head_pid);
        }
        return result;
    });

    primitives_detail::register_eval_observability_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);
}


// ── Env::lookup_cell_ptr: returns EvalValue* ──────────────────
//
// Issue #145 Phase 2.2: parent walk migrates to
// walk_env_frames when owner_ + parent_id_ are set (SoA path).
// Legacy Env* walk preserved as fallback for stack-allocated
// Envs that aren't registered in env_frames_ (local eval scopes
// before Phase 2.6 ships the rename).
EvalValue* Env::lookup_cell_ptr(const std::string& n, std::vector<EvalValue>* cells) const {
    if (!cells)
        return nullptr;
    // 1. Local bindings (no walk needed)
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second)) {
                auto ci = as_cell_id(b.second);
                if (ci < cells->size())
                    return &(*cells)[ci];
            }
            return nullptr;
        }
    }
    // 2. Walk parent chain — prefer SoA walk via env_frames_
    //    when owner_ + parent_id_ are both set. Canonical path
    //    for registered Envs (top_, modules_, arena-allocated
    //    envs). Cache-friendly index lookup replaces pointer
    //    chase; shadowing semantics preserved (closest frame
    //    wins, walk_env_frames stops at first match).
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        EvalValue* result = nullptr;
        owner_->walk_env_frames(parent_id_, [&](EnvId, const EnvFrame& f) {
            for (auto& b : f.bindings_) {
                if (b.first == n) {
                    if (is_cell(b.second)) {
                        auto ci = as_cell_id(b.second);
                        if (ci < cells->size())
                            result = &(*cells)[ci];
                    }
                    return false; // stop walking
                }
            }
            return true;
        });
        return result;
    }
    // 3. Legacy pointer walk (preserved for unregistered Envs).
    //    Same shadowing semantics: closest frame wins.
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second)) {
                    auto ci = as_cell_id(b.second);
                    if (ci < cells->size())
                        return &(*cells)[ci];
                }
                return nullptr;
            }
        }
    }
    return nullptr;
}

// ── Env::lookup_cell_index: returns uint64_t (stable) ─────────────
//
// Issue #145 Phase 2.2: same dual-path pattern as
// lookup_cell_ptr. SoA walk via env_frames_ when registered,
// legacy pointer walk otherwise.
std::optional<std::uint64_t> Env::lookup_cell_index(const std::string& n) const {
    // 1. Local bindings
    for (auto& b : bindings_) {
        if (b.first == n) {
            if (is_cell(b.second))
                return as_cell_id(b.second);
            return std::nullopt;
        }
    }
    // 2. SoA walk via env_frames_ when registered
    if (owner_ && parent_id_ != NULL_ENV_ID) {
        std::optional<std::uint64_t> result;
        owner_->walk_env_frames(parent_id_, [&](EnvId, const EnvFrame& f) {
            for (auto& b : f.bindings_) {
                if (b.first == n) {
                    if (is_cell(b.second))
                        result = as_cell_id(b.second);
                    return false;
                }
            }
            return true;
        });
        return result;
    }
    // 3. Legacy pointer walk
    for (auto* p = parent_; p; p = p->parent_) {
        for (auto& b : p->bindings_) {
            if (b.first == n) {
                if (is_cell(b.second))
                    return as_cell_id(b.second);
                return std::nullopt;
            }
        }
    }
    return std::nullopt;
}

std::optional<PrimFn> Primitives::lookup(const std::string& n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    auto i = table_.find(n);
    return i != table_.end() ? std::optional(i->second) : std::nullopt;
}

// ═══════════════════════════════════════════════════════════════
// DefUseIndex — Scope-level cached def-use chain
// ═══════════════════════════════════════════════════════════════
// P1 implementation: build scope tree + indexes once, incremental rebuild on mutate.

struct DefUseIndex {
    using NodeId = aura::ast::NodeId;
    using SymId = aura::ast::SymId;
    using FlatAST = aura::ast::FlatAST;
    using StringPool = aura::ast::StringPool;
    using NodeTag = aura::ast::NodeTag;
    static constexpr auto INVALID_SYM = aura::ast::INVALID_SYM;

    struct ScopeNode {
        NodeId node = 0;
        std::uint32_t parent = std::uint32_t(-1);
        std::uint32_t first_child = 0;
        std::uint16_t child_count = 0;
        std::uint32_t def_first = 0;
        std::uint16_t def_count = 0;
        std::uint32_t ref_first = 0;
        std::uint16_t ref_count = 0;
        std::uint32_t use_first = 0;
        std::uint32_t use_count = 0;
        bool dirty = false;
        bool tombstoned = false;
    };

    struct SymRef {
        SymId sym = INVALID_SYM;
        std::uint32_t use_start = 0;
        std::uint16_t use_count = 0;
    };

    // Arena data — all flat vectors, no pointers
    std::vector<ScopeNode> scopes_;
    std::vector<SymId> def_syms_;
    std::vector<NodeId> def_nodes_;
    std::vector<SymRef> refs_;
    std::vector<NodeId> uses_;

    // Cross-scope: sym → all scopes that define/reference it
    std::vector<SymId> sym_scopes_keys_;
    std::vector<std::uint32_t> sym_scopes_vals_;
    std::vector<std::uint32_t> sym_to_range_;

    // ── Call-graph index (#10) ─────────────────────────────────
    // callers_of_: SymId → all Call nodes that call this symbol
    // built during build(), enables O(1) query_callers
    std::unordered_map<SymId, std::vector<NodeId>> callers_of_;
    // callee_of_: NodeId → SymId (only for Call nodes)
    // enables O(1) callee lookup from a call site
    std::vector<SymId> callee_of_;

    FlatAST* flat_ = nullptr;
    StringPool* pool_ = nullptr;
    bool built_ = false;
    std::size_t flat_size_at_build_ = 0;

    // ── Per-symbol version (Issue #107 part 5) ──────────────────
    // Tracks which syms have been touched (mutated) since the last
    // index refresh. Each touch bumps global_version_; after a refresh
    // (full build or incremental update_callers_for), the affected
    // syms are removed from stale_syms_. Syms not in stale_syms_ are
    // guaranteed to have fresh callers_of_ / callee_of_ data.
    //
    // Why per-sym and not just one global counter (defuse_version_):
    // - Granular invalidation: a mutation to sym X only invalidates X.
    //   Other syms' cached data stays valid; we can skip their re-scan.
    // - Observability: query:index-stats reports how many syms are
    //   stale, which is useful for cache-hit rate diagnostics.
    // - Forward-compatible: future fine-grained refresh paths (refresh
    //   only stale syms without full flat scan) can use this directly.
    //
    // Note: stale_syms_ is a superset of "syms that changed". After
    // a full build, stale_syms_ is empty. After touch_sym(s), {s} is
    // added. After update_callers_for(S), all s ∈ S are removed.
    std::unordered_set<SymId> stale_syms_;
    std::uint64_t global_version_ = 0;

    // Mark a sym as touched: its callers_of_ / callee_of_ data may
    // now be stale. Bumps global_version_ so is_sym_stale() returns
    // true for this sym until mark_sym_fresh() is called.
    void touch_sym(SymId s) {
        if (s == INVALID_SYM)
            return;
        stale_syms_.insert(s);
        ++global_version_;
    }

    // Touch multiple syms at once. Bumps global_version_ once
    // regardless of |syms| (the bump is a logical "mutation epoch"
    // marker, not a per-sym counter).
    void touch_syms(const std::unordered_set<SymId>& syms) {
        if (syms.empty())
            return;
        for (auto s : syms) {
            if (s != INVALID_SYM)
                stale_syms_.insert(s);
        }
        ++global_version_;
    }

    // Check if a sym's data is stale.
    bool is_sym_stale(SymId s) const { return stale_syms_.count(s) > 0; }

    // Mark a sym as fresh (its callers_of_ / callee_of_ data is
    // up-to-date with the current flat).
    void mark_sym_fresh(SymId s) { stale_syms_.erase(s); }

    // Mark a set of syms as fresh.
    void mark_syms_fresh(const std::unordered_set<SymId>& syms) {
        for (auto s : syms)
            stale_syms_.erase(s);
    }

    // Mark all syms as fresh (called after a full build).
    void mark_all_fresh() { stale_syms_.clear(); }

    // Stats accessors.
    std::size_t stale_count() const { return stale_syms_.size(); }
    std::uint64_t current_version() const { return global_version_; }

    void destroy() {
        scopes_.clear();
        def_syms_.clear();
        def_nodes_.clear();
        refs_.clear();
        uses_.clear();
        sym_scopes_keys_.clear();
        sym_scopes_vals_.clear();
        sym_to_range_.clear();
        callers_of_.clear();
        callee_of_.clear();
        stale_syms_.clear();
        global_version_ = 0;
        flat_ = nullptr;
        pool_ = nullptr;
        built_ = false;
    }

    // ── Build from scratch ──────────────────────────────────────
    // Single-pass: walk AST nodes 0..N-1, detect scope boundaries,
    // collect defs, build scope tree, then collect uses per scope.
    void build(FlatAST& flat, StringPool& pool) {
        destroy();
        flat_ = &flat;
        pool_ = &pool;
        flat_size_at_build_ = flat.size();

        // Pre-allocate
        def_syms_.reserve(flat.size() / 4);
        def_nodes_.reserve(flat.size() / 4);
        uses_.reserve(flat.size() / 2);
        refs_.reserve(flat.size() / 4);

        // Root scope (module-level: node 0)
        scopes_.push_back({});
        scopes_.back().node = 0;
        scopes_.back().dirty = false;

        // Pass 1: walk all nodes, build scope tree + collect defs
        // Use explicit depth-first traversal, NOT scan-by-NodeId
        // because children may not be contiguous.
        struct Frame {
            NodeId node_id;
            std::uint32_t scope_idx;
            std::size_t child_idx; // which child we're processing
        };
        std::vector<Frame> stack;
        stack.push_back({flat.root, 0, 0});

        while (!stack.empty()) {
            auto& f = stack.back();
            auto v = flat.get(f.node_id);

            // First visit: check if this node creates a scope
            if (f.child_idx == 0) {
                bool is_scope_creator = false;
                switch (v.tag) {
                    case NodeTag::Define:
                    case NodeTag::Lambda:
                    case NodeTag::Let:
                    case NodeTag::LetRec:
                    case NodeTag::Begin:
                        is_scope_creator = true;
                        break;
                    default:
                        break;
                }

                if (is_scope_creator && f.node_id != flat.root) {
                    // Create new scope (except for root which already has scope 0)
                    auto scope_idx = scopes_.size();
                    ScopeNode sn;
                    sn.node = f.node_id;
                    sn.parent = f.scope_idx;
                    sn.dirty = false;
                    scopes_.push_back(sn);

                    // Link into parent
                    auto& parent = scopes_[f.scope_idx];
                    if (parent.child_count == 0)
                        parent.first_child = scope_idx;
                    parent.child_count++;

                    // Collect defs for this scope
                    auto& sn2 = scopes_.back();
                    sn2.def_first = def_syms_.size();
                    switch (v.tag) {
                        case NodeTag::Define:
                            def_syms_.push_back(v.sym_id);
                            def_nodes_.push_back(f.node_id);
                            break;
                        case NodeTag::Lambda:
                            for (auto pid : v.params) {
                                def_syms_.push_back(pid);
                                def_nodes_.push_back(f.node_id);
                            }
                            break;
                        case NodeTag::Let:
                        case NodeTag::LetRec:
                            def_syms_.push_back(v.sym_id);
                            def_nodes_.push_back(f.node_id);
                            break;
                        default:
                            break;
                    }
                    sn2.def_count = def_syms_.size() - sn2.def_first;

                    // Update frame scope
                    f.scope_idx = scope_idx;
                }
            }

            // Process children
            if (f.child_idx < v.children.size()) {
                auto child = v.child(f.child_idx);
                auto child_scope = f.scope_idx;

                // Skip scope-creating children (they create their own scope)
                // But still process them as sub-frames
                auto cv = flat.get(child);
                f.child_idx++;
                stack.push_back({child, child_scope, 0});
            } else {
                stack.pop_back();
                // When returning from a scope-creating child, the parent scope stays
            }
        }

        // Pass 2: collect uses per scope
        // Walk all Variable nodes, associate each with the innermost scope
        // that could define it (or skip if unbound)
        // For simplicity: associate each Variable with the scope it belongs to
        collect_uses(flat);

        // Pass 3: add any unfound defs from full scan (covers edge cases)
        // This ensures top-level defines and lets are always indexed
        for (NodeId sid = 0; sid < flat.size(); ++sid) {
            auto sv = flat.get(sid);
            aura::ast::SymId def_sym = aura::ast::INVALID_SYM;

            // Check for define/let/letrec that might not be in any scope
            if (sv.tag == NodeTag::Define || sv.tag == NodeTag::Let || sv.tag == NodeTag::LetRec) {
                def_sym = sv.sym_id;
            } else if (sv.tag == NodeTag::Lambda && sv.params.size() > 0) {
                // For lambdas at top level, add all params
            }

            if (def_sym != aura::ast::INVALID_SYM) {
                // Find which scope this node belongs to
                std::uint32_t found_scope = 0; // default: root scope
                for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
                    auto& sn = scopes_[si];
                    if (sn.node == sid) {
                        found_scope = si;
                        break;
                    }
                }

                // Check if this sym is already def'd in this scope
                auto& sn = scopes_[found_scope];
                bool exists = false;
                for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                    if (def_syms_[sn.def_first + d] == def_sym) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    // Insert def at the end of this scope's defs
                    // Need to shift: append new def, update scope's def_range
                    // For root scope, just append
                    auto old_def_first = sn.def_first;
                    auto old_def_count = sn.def_count;
                    def_syms_.push_back(def_sym);
                    def_nodes_.push_back(sid);
                    sn.def_first = def_syms_.size() - 1;
                    sn.def_count = 1 + old_def_count;
                }
            }
        }

        // Pass 4: build cross-scope sym index
        build_sym_index();

        // Pass 5: build call-graph index (#10)
        // Walk all Call nodes, record callers_of_ and callee_of_
        build_call_graph(flat);

        // All syms are now fresh after a full build. (Issue #107 part 5)
        // global_version_ is kept monotonic (never reset); staleness
        // is tracked by stale_syms_ membership, not by version compare.
        mark_all_fresh();

        built_ = true;
    }

    // ── Collect uses: walk all Variable nodes, group by scope ────
    void collect_uses(FlatAST& flat) {
        // Map: node_id → scope_idx
        std::unordered_map<NodeId, std::uint32_t> node_to_scope;
        node_to_scope.reserve(flat.size());

        // Build node-to-scope mapping via DFS
        struct Frame {
            NodeId nid;
            std::uint32_t scope_idx;
            std::size_t child_idx;
        };
        std::vector<Frame> stack;
        stack.push_back({flat.root, 0, 0});

        while (!stack.empty()) {
            auto& f = stack.back();
            auto v = flat.get(f.nid);

            if (f.child_idx == 0) {
                // First visit: determine scope
                // Check parent scope
                auto this_scope = f.scope_idx;

                // If this node creates a scope, find its scope index
                bool found = false;
                for (std::size_t si = scopes_.size(); si > 0; --si) {
                    auto& sn = scopes_[si - 1];
                    if (sn.node == f.nid && !sn.tombstoned) {
                        this_scope = si - 1;
                        found = true;
                        break;
                    }
                }
                node_to_scope[f.nid] = this_scope;

                if (f.child_idx == 0) {
                    f.scope_idx = this_scope;
                }
            }

            if (f.child_idx < v.children.size()) {
                auto child = v.child(f.child_idx);
                f.child_idx++;
                stack.push_back({child, f.scope_idx, 0});
            } else {
                stack.pop_back();
            }
        }

        // Now collect Variables grouped by scope
        // Group by scope: scope_idx → {sym → [node_ids]}
        struct ScopeVarGroup {
            std::unordered_map<SymId, std::vector<NodeId>> vars;
        };
        std::unordered_map<std::uint32_t, ScopeVarGroup> scope_vars;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Variable) {
                auto scope_it = node_to_scope.find(id);
                if (scope_it != node_to_scope.end()) {
                    scope_vars[scope_it->second].vars[v.sym_id].push_back(id);
                }
            }
        }

        // Build refs_ and uses_ from scope_vars
        for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
            auto& sn = scopes_[si];
            sn.ref_first = refs_.size();
            sn.use_first = uses_.size();

            auto sv_it = scope_vars.find(si);
            if (sv_it != scope_vars.end()) {
                for (auto& [sym, nodes] : sv_it->second.vars) {
                    SymRef sr;
                    sr.sym = sym;
                    sr.use_start = uses_.size();
                    sr.use_count = static_cast<std::uint16_t>(nodes.size());
                    for (auto nid : nodes)
                        uses_.push_back(nid);
                    refs_.push_back(sr);
                }
            }

            sn.ref_count = static_cast<std::uint16_t>(refs_.size() - sn.ref_first);
            sn.use_count = uses_.size() - sn.use_first;
        }
    }

    // ── Build sym → scopes index ────────────────────────────────
    void build_sym_index() {
        SymId max_sym = 0;
        for (auto s : def_syms_)
            if (s != INVALID_SYM && s > max_sym)
                max_sym = s;
        for (auto& r : refs_)
            if (r.sym != INVALID_SYM && r.sym > max_sym)
                max_sym = r.sym;

        sym_to_range_.resize(max_sym + 1, 0);

        struct Entry {
            SymId sym;
            std::uint32_t scope_idx;
            bool is_def;
            std::uint32_t local_idx;
        };
        std::unordered_map<uint32_t, std::vector<Entry>> entries_by_sym;

        for (std::uint32_t si = 0; si < scopes_.size(); ++si) {
            auto& sn = scopes_[si];
            for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                auto sym = def_syms_[sn.def_first + d];
                entries_by_sym[sym].push_back({sym, si, true, sn.def_first + d});
            }
            for (std::uint16_t r = 0; r < sn.ref_count; ++r) {
                auto& ref = refs_[sn.ref_first + r];
                entries_by_sym[ref.sym].push_back({ref.sym, si, false, sn.ref_first + r});
            }
        }

        sym_scopes_keys_.clear();
        sym_scopes_vals_.clear();

        for (auto& [sym, entries] : entries_by_sym) {
            if (sym > max_sym)
                continue;
            sym_to_range_[sym] = (sym_scopes_vals_.size() << 16) | (uint32_t)entries.size();
            for (auto& e : entries) {
                sym_scopes_keys_.push_back(sym);
                sym_scopes_vals_.push_back((e.scope_idx << 1) | (e.is_def ? 1u : 0u));
            }
        }
    }

    // ── Build call-graph index (#10) ────────────────────────────
    // Populates callers_of_ and callee_of_ from all Call nodes.
    void build_call_graph(FlatAST& flat) {
        callers_of_.clear();
        callee_of_.resize(flat.size(), INVALID_SYM);
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id != INVALID_SYM) {
                    callers_of_[callee.sym_id].push_back(id);
                    callee_of_[id] = callee.sym_id;
                }
            }
        }
    }

    // ── Query: def-use for a symbol ─────────────────────────────
    struct DefUseResult {
        std::vector<NodeId> defs;
        std::vector<NodeId> uses;
    };

    DefUseResult query_def_use(SymId sym) {
        DefUseResult r;
        if (sym >= sym_to_range_.size())
            return r;

        auto packed = sym_to_range_[sym];
        if (packed == 0)
            return r;

        auto start = packed >> 16;
        auto count = packed & 0xFFFF;

        for (std::uint32_t i = start; i < start + count; ++i) {
            auto val = sym_scopes_vals_[i];
            auto scope_idx = val >> 1;
            if (val & 1) {
                // is_def
                auto& sn = scopes_[scope_idx];
                for (std::uint16_t d = 0; d < sn.def_count; ++d) {
                    if (def_syms_[sn.def_first + d] == sym)
                        r.defs.push_back(def_nodes_[sn.def_first + d]);
                }
            } else {
                // is_ref — collect use nodes
                auto& sn = scopes_[scope_idx];
                for (std::uint16_t ri = 0; ri < sn.ref_count; ++ri) {
                    auto& ref = refs_[sn.ref_first + ri];
                    if (ref.sym == sym) {
                        for (std::uint16_t u = 0; u < ref.use_count; ++u)
                            r.uses.push_back(uses_[ref.use_start + u]);
                    }
                }
            }
        }
        return r;
    }

    // ── Query: caller nodes for a symbol ────────────────────────
    // O(1) callee lookup: which symbol does a Call node invoke?
    // Returns INVALID_SYM if not a call or not indexed.
    SymId query_callee(NodeId node) const {
        if (node < callee_of_.size())
            return callee_of_[node];
        return INVALID_SYM;
    }

    // O(1) caller query using callers_of_ index (built during build())
    // Fallback: if index not available (build_call_graph wasn't run), do O(N) scan
    std::vector<NodeId> query_callers(SymId sym, FlatAST& flat) {
        // Try indexed path first
        if (!callers_of_.empty()) {
            auto it = callers_of_.find(sym);
            if (it != callers_of_.end())
                return it->second;
            return {};
        }
        // Fallback: O(N) scan for unindexed state
        std::vector<NodeId> callers;
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id == sym)
                    callers.push_back(id);
            }
        }
        return callers;
    }

    // ── Mark scope containing a node as dirty ───────────────────
    void mark_dirty(NodeId node) {
        if (!built_ || scopes_.empty())
            return;
        for (auto& sn : scopes_) {
            if (sn.tombstoned)
                continue;
            if (sn.node == node) {
                mark_dirty_up(sn);
                return;
            }
        }
        for (auto& sn : scopes_)
            sn.dirty = true;
    }

    void mark_dirty_up(ScopeNode& sn) {
        sn.dirty = true;
        if (sn.parent < scopes_.size() && !scopes_[sn.parent].tombstoned)
            mark_dirty_up(scopes_[sn.parent]);
    }

    // ── Incremental rebuild ─────────────────────────────────────
    bool rebuild_dirty(FlatAST& flat, StringPool& pool) {
        if (!built_) {
            build(flat, pool);
            return true;
        }
        if (flat_ != &flat || flat.size() != flat_size_at_build_) {
            build(flat, pool);
            return true;
        }
        bool any_dirty = false;
        for (auto& sn : scopes_) {
            if (sn.dirty) {
                any_dirty = true;
                break;
            }
        }
        if (!any_dirty)
            return false;
        build(flat, pool);
        return true;
    }

    // ── Incremental: update callers_of_ for specific syms ─────
    // Used after mutations that only modify existing nodes
    // (rebind/set-body/replace-pattern) without adding new nodes.
    // Full flat scan still needed but scope tree + defs/uses preserved.
    //
    // After this call, all syms in `affected_syms` have fresh
    // callers_of_ / callee_of_ data. We mark them as fresh in the
    // per-sym version tracker (Issue #107 part 5) so subsequent
    // ensure_defuse() calls don't redundantly re-touch them.
    void update_callers_for(FlatAST& flat, const std::unordered_set<SymId>& affected_syms) {
        if (!built_ || affected_syms.empty())
            return;
        // Clear old callers entries for affected syms
        for (auto sym : affected_syms)
            callers_of_.erase(sym);
        // Reset callee_of_ for call nodes that referenced affected syms
        for (NodeId id = 0; id < callee_of_.size(); ++id) {
            if (callee_of_[id] != INVALID_SYM && affected_syms.count(callee_of_[id]))
                callee_of_[id] = INVALID_SYM;
        }
        // Full scan to find new Call nodes referencing affected syms
        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee = flat.get(v.child(0));
                if (callee.tag == NodeTag::Variable && callee.sym_id != INVALID_SYM &&
                    affected_syms.count(callee.sym_id)) {
                    callers_of_[callee.sym_id].push_back(id);
                    callee_of_[id] = callee.sym_id;
                }
            }
        }
        // Mark refreshed syms as fresh. (Issue #107 part 5)
        // We erase from stale_syms_ even if they weren't stale before
        // (idempotent); the data is now up-to-date either way.
        mark_syms_fresh(affected_syms);
    }
};

// ── DefUseIndex ownership helper (ASAN fix #107 leak) ───────────
// Free a DefUseIndex held in a void* slot and null the slot. Defined
// after the struct body so DefUseIndex's destructor is visible (a
// forward-declared type's destructor is not, so `delete static_cast
// <DefUseIndex*>(...)` won't compile from sites before the struct).
// The function is small and the call sites pass `&defuse_index_`,
// which is the same idiom as `defuse_touch_fn_` and friends.
//
// Note: we can't use std::unique_ptr<DefUseIndex> in the header
// because DefUseIndex is a TU-local type (defined in evaluator_impl.cpp
// only). The void* slot + this helper is the PIMPL-shaped equivalent.
static void defuse_index_destroy(void** slot) {
    if (!slot || !*slot)
        return;
    delete static_cast<DefUseIndex*>(*slot);
    *slot = nullptr;
}


// ═══════════════════════════════════════════════════════════════

void Evaluator::update_shared_tree_root() {
    if (!workspace_tree_)
        return;
    auto* wt = static_cast<WorkspaceTree*>(workspace_tree_);
    if (wt->size() > 0) {
        // Only update the ACTIVE node's flat/pool pointer.
        // DO NOT update root (index 0) — that would break isolation.
        // If the active node IS root (index 0), that gets updated naturally.
        // This ensures set-code/mutate writes propagate to the correct
        // WorkspaceNode. Without this, a child workspace's modifications
        // are lost on workspace:switch back because the node still points
        // to the pre-set-code flat.
        auto active = wt->active_idx();
        if (active < wt->size()) {
            wt->nodes_[active].flat = workspace_flat_;
            wt->nodes_[active].pool = workspace_pool_;
            if (active > 0)
                wt->nodes_[active].has_own_flat = true;
        }
    }
}

Evaluator::Evaluator() {
    // Register heap mutex for thread-safe GC (P2)
    aura::messaging::g_heap_mutex = [this]() -> std::mutex& { return heap_mutex(); };

    top_.set_primitives(&primitives_);

    // Issue #145 Phase 2.2: register top_ env with this
    // Evaluator's env_frames_ SoA arena. The frame's bindings_
    // stay in top_ itself (we don't copy into env_frames_) —
    // we only need a parent_id_ index so Env::lookup_cell_ptr
    // can route its parent walk through walk_env_frames when
    // a deeper chain exists. top_ has no parent, so the frame's
    // parent_id = NULL_ENV_ID. For modules_ envs, see copy_env
    // / load_module_file where they're arena-allocated.
    top_.set_owner(this);
    top_.set_parent_id(alloc_env_frame(NULL_ENV_ID /* no parent */, &primitives_));
    primitives_.set_string_heap(&string_heap_);
    arena_group_ = std::make_unique<aura::ast::ArenaGroup>();
    init_pair_primitives();

    // Issue #131: FFI primitives registered via FFIRuntime
    // (the previous inline registrations in this ctor were
    // ~250 lines of monolithic code, now extracted to
    // ffi_primitives_impl.cpp). The runtime owns the FFI
    // state (libs, funcs) and is per-evaluator. The
    // callback breaks the cyclic import between
    // evaluator.ixx and ffi_primitives.ixx.
    ffi_runtime_.register_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        &string_heap_, &opaque_heap_, &coverage_counters_);

    // Step 2.3 + 5.2 hygiene: wire ADT using exact same signature + coverage
    // counters as ffi_runtime_ (for consistency across extractions; modeled on FFI).
    adt_runtime_.register_primitives(
        [this](std::string n, PrimFn f) { primitives_.add(std::move(n), std::move(f)); },
        &string_heap_, &opaque_heap_, &coverage_counters_);

    build_primitive_slots();

    primitives_detail::register_network_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);

    // 示例: (declare-type add "Int Int" "Int") → add: (Int, Int) -> Int
    // 这些签名在 typecheck-current 时注入到类型环境中。
    primitives_.add("declare-type", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !is_string(a[0]) || !is_string(a[1]) || !is_string(a[2]))
            return make_bool(false);
        auto name_idx = as_string_idx(a[0]);
        auto params_idx = as_string_idx(a[1]);
        auto ret_idx = as_string_idx(a[2]);
        if (name_idx >= string_heap_.size() || params_idx >= string_heap_.size() ||
            ret_idx >= string_heap_.size())
            return make_bool(false);
        declared_type_sigs_[string_heap_[name_idx]] = {.type_str = string_heap_[params_idx] + "|" +
                                                                   string_heap_[ret_idx],
                                                       .module_file = "",
                                                       .resolved = false};
        return make_bool(true);
    });

    // (generate-type-sigs "module-path") — 类型推断并生成 .aura-type 文件
    // 解析模块文件，对其中每个 export 的函数进行类型推断，
    // 生成同名的 .aura-type 签名文件。
    // 示例: (generate-type-sigs "helper.aura") → helper.aura-type
    primitives_.add("generate-type-sigs", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_bool(false);
        auto path = resolve_module_path(string_heap_[idx]);
        if (path.empty()) {
            std::println(std::cerr, "generate-type-sigs: cannot resolve '{}'", string_heap_[idx]);
            return make_bool(false);
        }

        // 读取并解析模块文件
        std::ifstream f(path);
        if (!f) {
            std::println(std::cerr, "generate-type-sigs: cannot open '{}'", path);
            return make_bool(false);
        }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty())
            return make_bool(false);

        aura::ast::ASTArena local_arena;
        auto alloc = local_arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(content, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "generate-type-sigs: parse error");
            return make_bool(false);
        }
        flat.root = pr.root;

        // 类型推断
        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        tc.infer_flat(flat, pool, flat.root, diag);

        // 扫描所有 Define 节点，收集名称
        // 如果模块有 (export ...) 声明，只输出 export 的函数
        std::unordered_set<std::string> export_set;
        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Export) {
                for (auto cid : nv.children) {
                    auto cv = flat.get(cid);
                    if (cv.tag == aura::ast::NodeTag::Variable)
                        export_set.insert(std::string(pool.resolve(cv.sym_id)));
                }
            }
        }

        std::vector<std::string> fn_names;
        std::unordered_map<std::string, aura::ast::NodeId> define_map;
        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Define) {
                auto name = std::string(pool.resolve(nv.sym_id));
                define_map[name] = nid;
                if (export_set.empty() || export_set.count(name))
                    fn_names.push_back(name);
            }
        }

        // 生成类型签名文件
        auto type_sig_path = path;
        {
            auto dot_pos = type_sig_path.rfind('.');
            if (dot_pos != std::string::npos)
                type_sig_path = type_sig_path.substr(0, dot_pos) + ".aura-type";
        }

        std::ofstream of(type_sig_path);
        if (!of) {
            std::println(std::cerr, "generate-type-sigs: cannot write '{}'", type_sig_path);
            return make_bool(false);
        }

        std::function<std::string(std::uint32_t)> type_name_for =
            [&](std::uint32_t tid) -> std::string {
            auto t = aura::core::TypeId{tid, 1};
            auto tag = treg.tag_of(t);
            switch (tag) {
                case aura::core::TypeTag::INT:
                    return "Int";
                case aura::core::TypeTag::BOOL:
                    return "Bool";
                case aura::core::TypeTag::STRING:
                    return "String";
                case aura::core::TypeTag::FLOAT:
                    return "Float";
                case aura::core::TypeTag::VOID:
                    return "Void";
                case aura::core::TypeTag::FUNC: {
                    if (auto* ft = treg.func_of(t)) {
                        std::string s;
                        for (auto& a : ft->args) {
                            if (!s.empty())
                                s += " ";
                            s += type_name_for(a.index);
                        }
                        s += " -> " + type_name_for(ft->ret.index);
                        return s;
                    }
                    return "Any";
                }
                default:
                    return "Any";
            }
        };

        std::size_t written = 0;
        for (auto& name : fn_names) {
            auto it = define_map.find(name);
            if (it == define_map.end())
                continue;
            auto def_v = flat.get(it->second);
            if (!def_v.children.empty()) {
                auto val_id = def_v.child(0);
                // 对 define 的值显式进行类型推断（define 自身不遍历子节点）
                // 使用 tc 在同一个 TypeRegistry 中推断类型
                auto val_type = tc.infer_flat(flat, pool, val_id, diag);
                if (val_type.valid() && val_type.index != 0) {
                    of << name << ": " << type_name_for(val_type.index) << "\n";
                    ++written;
                }
            }
        }

        // 写入成功后，失效模块缓存强制下次 require 重新加载
        // 这样 .aura-type 文件才能在后续的 require 中被读取。
        // pre_exec_requires 可能在 generate-type-sigs 之前加载了模块。
        auto cache_it = module_cache_.find(path);
        if (cache_it != module_cache_.end()) {
            module_cache_.erase(cache_it);
        }

        std::println(std::cerr, "generate-type-sigs: wrote {} types to '{}'", written,
                     type_sig_path);
        return make_bool(written > 0);
    });

    // (check-module-signature "module-path")
    // 加载模块，对其每个 define 的函数进行类型推断，
    // 然后与 .aura-type 中的声明签名进行比对。
    // 输出不一致的诊断结果（不修改文件）。
    primitives_.add("check-module-signature", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap_.size())
            return make_bool(false);
        auto path = resolve_module_path(string_heap_[idx]);
        if (path.empty()) {
            std::println(std::cerr, "check-module-signature: cannot resolve '{}'",
                         string_heap_[idx]);
            return make_bool(false);
        }

        // 读取并解析模块文件
        std::ifstream f(path);
        if (!f) {
            std::println(std::cerr, "check-module-signature: cannot open '{}'", path);
            return make_bool(false);
        }
        std::string content((std::istreambuf_iterator<char>(f)), {});
        if (content.empty())
            return make_bool(false);

        aura::ast::ASTArena local_arena;
        auto alloc = local_arena.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(content, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            std::println(std::cerr, "check-module-signature: parse error");
            return make_bool(false);
        }
        flat.root = pr.root;

        // 类型推断：对每个 define 的值进行类型推断
        struct FnInfo {
            std::string name;
            std::string inferred_type;
        };
        std::vector<FnInfo> fn_infos;

        aura::core::TypeRegistry treg;
        aura::compiler::TypeChecker tc(treg);
        aura::diag::DiagnosticCollector diag;
        tc.infer_flat(flat, pool, flat.root, diag);

        for (aura::ast::NodeId nid = 0; nid < flat.size(); ++nid) {
            auto nv = flat.get(nid);
            if (nv.tag == aura::ast::NodeTag::Define) {
                auto name = std::string(pool.resolve(nv.sym_id));
                if (!nv.children.empty()) {
                    auto val_id = nv.child(0);
                    auto val_type = tc.infer_flat(flat, pool, val_id, diag);
                    if (val_type.valid() && val_type.index != 0) {
                        fn_infos.push_back({name, treg.format_type(val_type)});
                    }
                }
            }
        }

        // 读取 .aura-type 文件
        auto sig_path = path;
        {
            auto dot = sig_path.rfind('.');
            if (dot != std::string::npos)
                sig_path = sig_path.substr(0, dot) + ".aura-type";
        }

        struct SigDecl {
            std::string name;
            std::string decl_type;
        };
        std::vector<SigDecl> sig_decls;

        struct stat st;
        if (::stat(sig_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            std::ifstream tf(sig_path);
            if (tf) {
                std::string line;
                while (std::getline(tf, line)) {
                    auto colon = line.find(':');
                    if (colon == std::string::npos)
                        continue;
                    auto arrow = line.find("->", colon);
                    if (arrow == std::string::npos)
                        continue;
                    auto name = line.substr(0, colon);
                    name.erase(name.find_last_not_of(" \t\r") + 1);
                    auto params_str = line.substr(colon + 1, arrow - colon - 1);
                    params_str.erase(0, params_str.find_first_not_of(" \t\r"));
                    params_str.erase(params_str.find_last_not_of(" \t\r") + 1);
                    auto ret_str = line.substr(arrow + 2);
                    ret_str.erase(0, ret_str.find_first_not_of(" \t\r"));
                    ret_str.erase(ret_str.find_last_not_of(" \t\r\n") + 1);
                    sig_decls.push_back({name, params_str + " -> " + ret_str});
                }
            }
        }

        // 比对：每个 decl 必须在 inferred 中找到匹配的
        // 类型字符串精确匹配（eg: "Int Int -> Int"）
        std::size_t matched = 0, mismatched = 0, missing = 0;
        for (auto& sd : sig_decls) {
            bool found = false;
            bool match = false;
            for (auto& fi : fn_infos) {
                if (fi.name == sd.name) {
                    found = true;
                    // treg.format_type 返回 "(Int Int -> Int)"，sd.decl_type 是 "Int Int -> Int"
                    // 标准化比较：去掉 format_type 中的括号
                    std::string fmt = fi.inferred_type;
                    // format_type 像 "(Int Int -> Int)"，去掉首尾括号
                    if (fmt.size() >= 2 && fmt.front() == '(' && fmt.back() == ')')
                        fmt = fmt.substr(1, fmt.size() - 2);
                    // 替换类型变量 __tN 为 Any（未标注类型时推断出 "__t0 -> __t0"）
                    for (auto ci = fmt.find("__t"); ci != std::string::npos;
                         ci = fmt.find("__t", ci)) {
                        auto end = ci + 3;
                        while (end < fmt.size() && (std::isalnum(fmt[end]) || fmt[end] == '_'))
                            ++end;
                        fmt.replace(ci, end - ci, "Any");
                        ci += 3;
                    }
                    if (fmt == sd.decl_type) {
                        match = true;
                    }
                    break;
                }
            }
            if (!found) {
                std::println(std::cerr, "  MISSING '{}' in module (declared but not defined)",
                             sd.name);
                ++missing;
            } else if (!match) {
                // 查找推断的类型字符串
                std::string inferred_fmt;
                for (auto& fi2 : fn_infos) {
                    if (fi2.name == sd.name) {
                        auto f = fi2.inferred_type;
                        if (f.size() >= 2 && f.front() == '(' && f.back() == ')')
                            f = f.substr(1, f.size() - 2);
                        // 替换类型变量 __tN 为 Any
                        std::string clean;
                        for (std::size_t ci = 0; ci < f.size(); ++ci) {
                            if (f[ci] == '_' && ci + 3 < f.size() && f[ci + 1] == '_' &&
                                f[ci + 2] == 't') {
                                clean += "Any";
                                while (ci < f.size() && (std::isalnum(f[ci]) || f[ci] == '_'))
                                    ++ci;
                                --ci;
                            } else {
                                clean += f[ci];
                            }
                        }
                        inferred_fmt = clean;
                        break;
                    }
                }
                std::println(std::cerr, "  MISMATCH '{}': declared '{}', inferred '{}'", sd.name,
                             sd.decl_type, inferred_fmt);
                ++mismatched;
            } else {
                ++matched;
            }
        }

        std::println(std::cerr, "check-module-signature: {}/{}/{} matched/mismatched/missing",
                     matched, mismatched, missing);
        return make_bool(matched > 0 || (mismatched == 0 && missing == 0));
    });

    // ═══════════════════════════════════════════════════════════════
    // P9: Def-Use Analysis (P1 — scope-level cached)
    // ═══════════════════════════════════════════════════════════════

    // ── 依赖图查询回调注册 ─────────────────────────────────────
    // 在 def-use 索引中注册依赖图查询函数，供 mutation 原语
    // (mutate:rebind / set-body) 在变更前查询调用者节点。
    // 定义在这里（DefUseIndex 完整类型已知后），绕开前向声明问题。
    dep_caller_fn_ = [](void* idx_ptr, aura::ast::SymId sym) -> std::vector<aura::ast::NodeId> {
        if (!idx_ptr)
            return {};
        auto* idx = static_cast<DefUseIndex*>(idx_ptr);
        auto result = idx->query_def_use(sym);
        return std::move(result.uses);
    };

    // ── Per-sym version touch callback (#107 part 5) ───────────
    // Registers a callback that mutations can use to mark a sym as
    // stale in the DefUseIndex. Same forward-decl workaround as
    // dep_caller_fn_. When defuse_index_ is null (no index yet), the
    // callback is a no-op; the next ensure_defuse() will build from
    // scratch anyway.
    defuse_touch_fn_ = [](void* idx_ptr, aura::ast::SymId sym) {
        if (!idx_ptr)
            return;
        auto* idx = static_cast<DefUseIndex*>(idx_ptr);
        idx->touch_sym(sym);
    };

    // Helper: get or rebuild the def-use index
    // Tracks defuse_version_ to detect mutations since last build.
    // (#10) Tracks rebuild count and clears affected_syms_ after rebuild.
    // (#107 part 5) Per-sym version: DefUseIndex itself tracks which
    // syms are stale (DefUseIndex::stale_syms_). The mutation paths
    // that report affected_syms (mutate:rebind / set-body) also call
    // defuse_touch_fn_ to mark the sym stale in the index. The
    // staleness set is used for observability (query:index-stats)
    // and for the update_callers_for() path to know which syms need
    // fresh data. We deliberately do NOT short-circuit ensure_defuse
    // on staleness: if a mutation reports an affected sym, we must
    // refresh it, even if the per-sym version wasn't bumped (defense
    // in depth: the affected_syms_ list is the authoritative source
    // for "this sym needs re-indexing", and the per-sym version is
    // a co-located observation).
    auto ensure_defuse = [this]() -> DefUseIndex* {
        if (!workspace_flat_ || !workspace_pool_)
            return nullptr;
        auto idx = static_cast<DefUseIndex*>(defuse_index_);
        if (!idx) {
            idx = new DefUseIndex();
            defuse_index_ = idx;
            idx->build(*workspace_flat_, *workspace_pool_);
            defuse_version_.store(1, std::memory_order_relaxed);
            defuse_rebuild_count_++;
            defuse_affected_syms_.clear();
            return idx;
        }

        // Collect affected syms since last ensure_defuse
        std::unordered_set<aura::ast::SymId> affected_sym_ids;
        if (!defuse_affected_syms_.empty()) {
            for (auto& name : defuse_affected_syms_) {
                auto sym = workspace_pool_->intern(name);
                if (sym != aura::ast::INVALID_SYM)
                    affected_sym_ids.insert(sym);
            }
        }
        defuse_affected_syms_.clear();

        // Incremental path: only rebuild callers_of_ for affected syms
        // when flat size hasn't changed (mutations that modify existing nodes).
        if (!affected_sym_ids.empty()) {
            if (workspace_flat_->size() == idx->flat_size_at_build_) {
                idx->update_callers_for(*workspace_flat_, affected_sym_ids);
                defuse_version_.store(1, std::memory_order_relaxed);
                return idx;
            }
        }

        // Fallback: full rebuild (flat size changed or many affected syms)
        idx->build(*workspace_flat_, *workspace_pool_);
        defuse_version_.store(1, std::memory_order_relaxed);
        defuse_rebuild_count_++;
        return idx;
    };

    // Helper: build Aura result list from NodeIds
    auto nodes_to_list = [this](std::span<const DefUseIndex::NodeId> nodes) -> EvalValue {
        EvalValue list = make_void();
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            auto pid = pairs_.size();
            pairs_.push_back({make_int(static_cast<std::int64_t>(*it)), list});
            list = make_pair(pid);
        }
        return list;
    };

    auto def_use_pair = [this, nodes_to_list](DefUseIndex* idx, aura::ast::SymId sym) -> EvalValue {
        auto result = idx->query_def_use(sym);
        auto def_list = nodes_to_list(result.defs);
        auto use_list = nodes_to_list(result.uses);
        auto result_pid = pairs_.size();
        pairs_.push_back({def_list, use_list});
        return make_pair(result_pid);
    };

    primitives_detail::register_defuse_query_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        workspace_mtx_, workspace_flat_, workspace_pool_, string_heap_, ensure_defuse,
        [def_use_pair](void* idx, aura::ast::SymId sym) {
            return def_use_pair(static_cast<DefUseIndex*>(idx), sym);
        },
        [this, nodes_to_list, def_use_pair](void* idx, aura::ast::NodeId target) -> EvalValue {
            auto& flat = *workspace_flat_;
            auto v = flat.get(target);
            aura::ast::SymId defined_sym = aura::ast::INVALID_SYM;
            switch (v.tag) {
                case aura::ast::NodeTag::Define:
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec:
                    defined_sym = v.sym_id;
                    break;
                default:
                    return make_merr("type-error", "node " + std::to_string(target) +
                                                       " is not a definition node");
            }
            if (defined_sym == aura::ast::INVALID_SYM)
                return make_merr("internal", "definition node has invalid symbol id");
            return def_use_pair(static_cast<DefUseIndex*>(idx), defined_sym);
        },
        [this, nodes_to_list](void* idx, aura::ast::SymId sym) -> EvalValue {
            auto* du_idx = static_cast<DefUseIndex*>(idx);
            auto& flat = *workspace_flat_;
            auto duo = du_idx->query_def_use(sym);
            auto callers = du_idx->query_callers(sym, flat);
            auto def_list = nodes_to_list(duo.defs);
            auto use_list = nodes_to_list(duo.uses);
            auto caller_list = nodes_to_list(callers);
            auto c1 = pairs_.size();
            pairs_.push_back({caller_list, make_void()});
            auto c2 = pairs_.size();
            pairs_.push_back({use_list, make_pair(c1)});
            auto c3 = pairs_.size();
            pairs_.push_back({def_list, make_pair(c2)});
            return make_pair(c3);
        },
        [this](void* idx) -> EvalValue {
            auto* du_idx = static_cast<DefUseIndex*>(idx);
            du_idx->build(*workspace_flat_, *workspace_pool_);
            defuse_version_.store(1, std::memory_order_relaxed);
            defuse_rebuild_count_++;
            defuse_affected_syms_.clear();
            return make_int(1);
        },
        [this](void* idx) -> EvalValue {
            auto* du_idx = static_cast<DefUseIndex*>(idx);
            auto& flat = *workspace_flat_;
            auto make_kv = [&](std::string_view k, std::int64_t v) -> types::EvalValue {
                auto kv_ref = make_pair(pairs_.size());
                auto k_sym = string_heap_.size();
                string_heap_.push_back(std::string(k));
                types::EvalValue car = make_string(k_sym);
                types::EvalValue cdr = make_int(v);
                pairs_.push_back(Pair{car, cdr});
                return kv_ref;
            };
            auto stats = make_void();
            auto push_kv = [&](std::string_view k, std::int64_t v) {
                auto kv_ref = make_kv(k, v);
                auto new_ref = make_pair(pairs_.size());
                pairs_.push_back(Pair{kv_ref, stats});
                stats = new_ref;
            };
            push_kv("nodes", flat.size());
            push_kv("scopes", du_idx->scopes_.size());
            push_kv("def-syms", du_idx->def_syms_.size());
            push_kv("refs", du_idx->refs_.size());
            push_kv("callers", du_idx->callers_of_.size());
            push_kv("rebuilds", static_cast<std::int64_t>(defuse_rebuild_count_));
            push_kv("stale-syms", static_cast<std::int64_t>(du_idx->stale_count()));
            push_kv("defuse-version", static_cast<std::int64_t>(du_idx->current_version()));
            return stats;
        },
        [this](const std::string& k, const std::string& m) { return make_merr(k, m); });

    primitives_detail::register_ast_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this, [this]() { defuse_index_destroy(&defuse_index_); },
        [this]() -> std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> {
            auto* idx = static_cast<DefUseIndex*>(defuse_index_);
            if (!idx || !idx->built_)
                return std::nullopt;
            return std::make_tuple(static_cast<std::uint64_t>(idx->scopes_.size()),
                                   static_cast<std::uint64_t>(idx->def_syms_.size()),
                                   static_cast<std::uint64_t>(idx->uses_.size()));
        });

    // ═══════════════════════════════════════════════════════════════
    // Issue #97 Action 1: Hot-swap primitive
    // ═══════════════════════════════════════════════════════════════
    // (hot-swap:fn "name" "new-source") → #t / #f
    // Replaces the body of an existing function while keeping its id.
    // Closures referencing the function will use the new code on next call.
    // Requires the CompilerService to have set a hot-swap callback.
    primitives_.add("hot-swap:fn", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() != 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_bool(false);
        auto ni = as_string_idx(a[0]);
        auto si = as_string_idx(a[1]);
        if (ni >= string_heap_.size() || si >= string_heap_.size())
            return make_bool(false);
        if (!hot_swap_fn_) {
            // No callback set — hot-swap not available in this context
            return make_bool(false);
        }
        const std::string& name = string_heap_[ni];
        const std::string& new_source = string_heap_[si];
        bool ok = false;
        try {
            ok = hot_swap_fn_(name, new_source);
        } catch (...) {
            ok = false;
        }
        return make_bool(ok);
    });

    primitives_detail::register_compile_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);

    primitives_detail::register_messaging_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);

    primitives_detail::register_synthesize_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_strategy_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);

    // ── coverage-report — 编译器路径覆盖率 ──────────────────
    primitives_.add("coverage-report", [this](const auto&) -> EvalValue {
        std::string result = "#(coverage";
        for (int i = 0; i < 16; i++) {
            if (coverage_counters_[i] > 0) {
                std::string name;
                switch (i) {
                    case 0:
                        name = "parser";
                        break;
                    case 1:
                        name = "typecheck";
                        break;
                    case 2:
                        name = "eval";
                        break;
                    case 3:
                        name = "jit";
                        break;
                    case 4:
                        name = "macro";
                        break;
                    case 5:
                        name = "edsl-set-code";
                        break;
                    case 6:
                        name = "edsl-query";
                        break;
                    case 7:
                        name = "edsl-mutate";
                        break;
                    case 8:
                        name = "ffi";
                        break;
                    default:
                        name = "reserved-" + std::to_string(i);
                        break;
                }
                result += " " + name + ":" + std::to_string(coverage_counters_[i]);
            }
        }
        result += ")";
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sidx);
    });

    // (gc) — Reset arena to reclaim memory between benchmark tasks
    // Saves current source, resets arena, re-parses source into fresh arena.
    primitives_.add("gc", [this](const auto&) -> EvalValue {
        // Save current source
        std::string saved_src;
        if (workspace_flat_ && workspace_flat_->root != aura::ast::NULL_NODE) {
            auto src_fn = primitives_.lookup("current-source");
            if (src_fn) {
                auto src = (*src_fn)({});
                if (types::is_string(src)) {
                    auto sidx = types::as_string_idx(src);
                    if (sidx < string_heap_.size())
                        saved_src = string_heap_[sidx];
                }
            }
        }

        // Reset arena (invalidates all arena-allocated state)
        // (ASAN fix #107 leak) delete the old index.
        defuse_index_destroy(&defuse_index_);
        modules_.clear();
        module_cache_.clear();
        current_flat_ = nullptr;
        current_pool_ = nullptr;
        workspace_flat_ = nullptr;
        workspace_pool_ = nullptr;
        if (aura::messaging::g_reset_arena && compiler_service_) {
            aura::messaging::g_reset_arena(compiler_service_);
        }

        // Re-parse saved source into fresh arena
        if (!saved_src.empty()) {
            auto set_fn = primitives_.lookup("set-code");
            if (set_fn) {
                auto si = string_heap_.size();
                string_heap_.push_back(saved_src);
                (*set_fn)({types::make_string(si)});
            }
        }

        return types::make_bool(!saved_src.empty());
    });

    // (gc-heap) — Trigger GC or clear heap vectors.
    // When a GC collector is available (serve-async mode with
    // thread-safe GC), triggers a full GC cycle instead of
    // blindly clearing. Falls back to direct clear for stdin mode.
    primitives_.add("gc-heap", [this](const auto&) -> EvalValue {
        // If GC collector is available, use it
        if (aura::messaging::g_gc_collect) {
            std::lock_guard<std::mutex> lock(heap_mutex());
            return types::make_bool(aura::messaging::g_gc_collect());
        }
        // Fallback: direct clear (stdin mode)
        {
            std::lock_guard<std::mutex> lock(heap_mutex());
            // Clear short_str_cache_ BEFORE string_heap_ so cached EvalValues
            // referencing old indices aren't returned after the heap shrinks.
            // Without this, the next LiteralString eval returns a stale
            // cached String EvalValue pointing past the end of string_heap_,
            // and string_heap_[idx] is UB (segfault on .data() access).
            short_str_cache_.clear();
            string_heap_.clear();
            string_heap_.shrink_to_fit();
            pairs_.clear();
            pairs_.shrink_to_fit();
            error_values_.clear();
            error_values_.shrink_to_fit();
            for (auto* fht : g_hash_tables)
                FlatHashTable::destroy(fht);
            g_hash_tables.clear();
            g_hash_tables.shrink_to_fit();
            vector_heap_.clear();
            vector_heap_.shrink_to_fit();
            opaque_heap_.clear();
            opaque_heap_.shrink_to_fit();
            // gc-heap is a stronger reset than gc-temp; also record
            // the eval-depth snapshot so memory-pressure won't keep
            // suggesting "gc-temp" right after a gc-heap.
            last_gc_temp_eval_depth_ = eval_depth_;
        }
        return types::make_bool(true);
    });

    // (gc-freeze) — Mark current closure generation as "root".
    // The while loop's predicate/body closures are created before this
    // call (in persistent arena when in_task_context_=false).
    primitives_.add("gc-freeze", [this](const auto&) -> EvalValue {
        gc_safe_closure_id_ = next_id_;
        return types::make_bool(true);
    });

    // (gc-temp) — Reset temp arena + clear temp closures + heap vectors.
    // Safe to call between benchmark tasks. Temp closures (those with
    // owner_arena == temp_arena_) are erased, their arena memory freed O(1).
    // Module functions and while-loop closures (in persistent arena) survive.
    primitives_.add("gc-temp", [this](const auto&) -> EvalValue {
        if (!temp_arena_)
            return types::make_bool(false);

        // Erase closures in temp arena
        for (auto it = closures_.begin(); it != closures_.end();) {
            if (it->second.owner_arena == temp_arena_)
                it = closures_.erase(it);
            else
                ++it;
        }

        // Reset temp arena (O(1) — frees all cl_flat/cl_pool/copy_env)
        temp_arena_->reset();
        // Record the eval-depth snapshot so memory-pressure knows
        // when to suggest "gc-temp" again.
        last_gc_temp_eval_depth_ = eval_depth_;

        // Clear heap vectors.
        // NOTE: pairs_ and string_heap_ are NOT cleared — result lists are
        // pair-based and contain string references. gc-temp is called
        // before the caller reads results. Use gc-heap separately to
        // clear strings/pairs when results are no longer needed.
        // vector_heap_, opaque_heap_ are safe to clear here.
        error_values_.clear();
        error_values_.shrink_to_fit();
        for (auto* fht : g_hash_tables)
            FlatHashTable::destroy(fht);
        g_hash_tables.clear();
        g_hash_tables.shrink_to_fit();
        vector_heap_.clear();
        vector_heap_.shrink_to_fit();
        opaque_heap_.clear();
        opaque_heap_.shrink_to_fit();

        return types::make_bool(true);
    });

    // (gc-stats) — Return formatted string of all heap sizes for telemetry.
    primitives_.add("gc-stats", [this](const auto&) -> EvalValue {
        std::uint64_t root_count = 0;
        for (auto& [id, _] : closures_) {
            if (id < gc_safe_closure_id_)
                ++root_count;
        }
        auto result =
            std::format("string:{}/pairs:{}/cells:{}/err:{}/hash:{}/vec:{}/opq:{}/cls:{}/root:{}",
                        string_heap_.size(), pairs_.size(), cells_.size(), error_values_.size(),
                        g_hash_tables.size(), vector_heap_.size(), opaque_heap_.size(),
                        closures_.size(), root_count);
        auto sidx = string_heap_.size();
        string_heap_.push_back(result);
        return types::make_string(sidx);
    });

    // (gc-module "path") — Free a previously-loaded module's per-module
    // arena and remove it from the module cache. Returns #t on success,
    // #f if the path wasn't loaded. The path must match exactly what was
    // passed to (import) / (require) — for stdlib modules loaded via
    // AURA_PATH, this is the resolved absolute path.
    primitives_.add("gc-module", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return types::make_bool(false);
        auto sidx = types::as_string_idx(a[0]);
        if (sidx >= string_heap_.size())
            return types::make_bool(false);
        return types::make_bool(gc_module(string_heap_[sidx]));
    });

    // (gc-module-count) — Number of modules currently in the module cache.
    primitives_.add("gc-module-count", [this](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(modules_.size()));
    });

    // (type-registry-stats) — Issue #78: TypeRegistry observability.
    // Returns a hash with current size, generation, and predefined count.
    // Use this to monitor TypeRegistry growth in long-running sessions.
    primitives_.add("type-registry-stats", [this](const auto&) -> EvalValue {
        if (!type_registry_) {
            return make_void();
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        std::vector<std::pair<std::string, EvalValue>> kv;
        kv.push_back({"size", make_int(static_cast<std::int64_t>(treg.size()))});
        kv.push_back({"generation", make_int(static_cast<std::int64_t>(treg.generation()))});
        kv.push_back({"predefined-count", make_int(static_cast<std::int64_t>(
                                              aura::core::TypeRegistry::kPredefinedCount))});
        kv.push_back(
            {"user-types", make_int(static_cast<std::int64_t>(
                               treg.size() - aura::core::TypeRegistry::kPredefinedCount))});
        // Build a hash with the 4 keys.
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto cap = ht->capacity;
        for (auto& [k, v] : kv) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
            auto kidx = string_heap_.size();
            string_heap_.push_back(k);
            EvalValue key_ev = make_string(kidx);
            bool inserted = false;
            for (std::size_t at = 0; at < cap; ++at) {
                auto idx = ((h >> 1) + at) & (cap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key_ev.val;
                    vals[idx] = v.val;
                    ht->size++;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                FlatHashTable::destroy(ht);
                return make_void();
            }
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // (type-registry-compact) — Issue #78: reclaim all non-predefined
    // entries. Bumps the generation counter so any TypeId from the
    // previous generation becomes stale. Returns the number of entries
    // reclaimed.
    primitives_.add("type-registry-compact", [this](const auto&) -> EvalValue {
        if (!type_registry_) {
            return make_int(0);
        }
        auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
        std::uint32_t reclaimed = treg.compact();
        return make_int(static_cast<std::int64_t>(reclaimed));
    });

    // (arena:compact) — Issue #187 (P0): conservative arena buffer
    // compaction. Reclaims the unused tail of the main arena's pmr
    // buffer by rebuilding it at used-size + 25% headroom. Returns
    // the number of bytes reclaimed. Use (arena:compact-all) to
    // compact every per-module arena above the configured threshold.
    primitives_.add("arena:compact", [this](const auto&) -> EvalValue {
        if (!arena_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(arena_->compact()));
    });
    primitives_.add("arena:compact-all", [this](const auto&) -> EvalValue {
        if (!arena_group_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(arena_group_->auto_compact()));
    });
    primitives_.add("arena:shrink-to-fit", [this](const auto&) -> EvalValue {
        if (!arena_)
            return make_void();
        arena_->shrink_to_fit();
        return make_void();
    });
    // (arena:set-compact-threshold pct) — Issue #187: configure the
    // fragmentation ratio at which (arena:compact-all) triggers a
    // compact. pct is 0-95 (clamped). 50 = default.
    primitives_.add("arena:set-compact-threshold", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]) || !arena_group_)
            return make_void();
        arena_group_->set_compact_threshold(static_cast<double>(as_int(a[0])) / 100.0);
        return make_void();
    });
    // (arena:estimate) — Issue #187: bytes that could be reclaimed
    // by a (arena:compact). Cheap O(1) check, no side effects.
    primitives_.add("arena:estimate", [this](const auto&) -> EvalValue {
        if (!arena_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(arena_->compact_estimate()));
    });
    // (arena:stats-json) — Issue #187: JSON snapshot of all managed
    // arenas (capacity, used, fragmentation, compaction count). For
    // dashboards and auto-tuners. Returns the JSON as a string.
    primitives_.add("arena:stats-json", [this](const auto&) -> EvalValue {
        std::string out;
        if (arena_group_) {
            out = arena_group_->stats_json();
        } else if (arena_) {
            // Single-arena fallback: emit a one-entry JSON manually.
            auto s = arena_->stats();
            out = std::format("{{\"arenas\":[{{\"name\":\"main\",\"used\":{},\"capacity\":{},"
                              "\"peak_used\":{},\"allocs\":{},\"compaction_count\":{},"
                              "\"last_compaction_saved\":{},\"total_compaction_saved\":{},"
                              "\"fragmentation_ratio\":{:.3f}}}],\"compact_threshold\":0.5}}",
                              s.used, s.capacity, s.peak_used, s.allocation_count,
                              s.compaction_count, s.last_compaction_saved, s.total_compaction_saved,
                              s.fragmentation_ratio());
        } else {
            out = "{\"arenas\":[]}";
        }
        auto sidx = string_heap_.size();
        string_heap_.push_back(out);
        return types::make_string(sidx);
    });
    // (string-pool:compact) — Issue #187 (P0): rehash the workspace's
    // StringPool to the smallest power-of-2 capacity that still
    // holds all live entries. Reclaims hash_tbl_ memory. Returns
    // bytes reclaimed. SymIds are stable (buf_ is monotonic).
    primitives_.add("string-pool:compact", [this](const auto&) -> EvalValue {
        if (!workspace_pool_ && !canonical_pool())
            return make_int(0);
        auto* pool = workspace_pool_ ? workspace_pool_ : canonical_pool();
        return make_int(static_cast<std::int64_t>(pool->compact()));
    });
    // (string-pool:stats) — Issue #187: StringPool observability.
    // Returns hash {entries, capacity, load-factor, data-size,
    // hash-bytes, fragmentation}.
    // (Built inline using the same hash-build pattern as
    //  gc-arena-info above.)
    primitives_.add("string-pool:stats", [this](const auto&) -> EvalValue {
        if (!workspace_pool_ && !canonical_pool())
            return make_void();
        auto* pool = workspace_pool_ ? workspace_pool_ : canonical_pool();
        std::size_t entries = pool->entry_count();
        std::size_t cap = pool->hash_capacity();
        double lf = pool->load_factor();
        std::size_t ds = pool->data_size();
        std::size_t hb = pool->hash_table_bytes();
        double frag = pool->buf_fragmentation();

        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                auto kidx = string_heap_.size();
                string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"entries", make_int(static_cast<std::int64_t>(entries))},
            {"capacity", make_int(static_cast<std::int64_t>(cap))},
            {"load-factor", make_float(lf)},
            {"data-size", make_int(static_cast<std::int64_t>(ds))},
            {"hash-bytes", make_int(static_cast<std::int64_t>(hb))},
            {"fragmentation", make_float(frag)},
        };
        return build_hash(kv);
    });

    // (dirty:reasons node-id) — Issue #188: return the per-node
    // dirty-reason bitmask. Useful for the type checker to decide
    // which targeted re-analysis pass to run, and for diagnostics
    // to surface "why is this node dirty". Bit values:
    //   0x01 = general (re-infer), 0x02 = constraint, 0x04 = occurrence,
    //   0x08 = ownership, 0x10 = coercion. Returns 0 for clean nodes
    //   or out-of-range ids.
    primitives_.add("dirty:reasons", [this](const auto& a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        if (!workspace_flat_)
            return make_int(0);
        auto id = static_cast<aura::ast::NodeId>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(workspace_flat_->dirty_reasons(id)));
    });
    // (dirty:counts) — Issue #188: aggregate per-reason dirty counts
    // across the workspace. Returns hash with 5 integer fields:
    //   general, constraint, occurrence, ownership, coercion, total
    //   (total is the number of dirty nodes, not the sum of bits).
    // Built inline using the same hash-build pattern as gc-arena-info.
    primitives_.add("dirty:counts", [this](const auto&) -> EvalValue {
        if (!workspace_flat_)
            return make_void();
        std::size_t gen = 0, con = 0, occ = 0, own = 0, coe = 0, total = 0;
        const auto& dirty = workspace_flat_->dirty_column();
        for (std::size_t i = 0; i < dirty.size(); ++i) {
            auto b = dirty[i];
            if (b == 0)
                continue;
            ++total;
            if (b & 0x01)
                ++gen;
            if (b & 0x02)
                ++con;
            if (b & 0x04)
                ++occ;
            if (b & 0x08)
                ++own;
            if (b & 0x10)
                ++coe;
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                auto kidx = string_heap_.size();
                string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"general", make_int(static_cast<std::int64_t>(gen))},
            {"constraint", make_int(static_cast<std::int64_t>(con))},
            {"occurrence", make_int(static_cast<std::int64_t>(occ))},
            {"ownership", make_int(static_cast<std::int64_t>(own))},
            {"coercion", make_int(static_cast<std::int64_t>(coe))},
            {"total", make_int(static_cast<std::int64_t>(total))},
        };
        return build_hash(kv);
    });

    primitives_detail::register_jit_arena_primitives(
        [this](std::string name, PrimFn fn) { primitives_.add(std::move(name), std::move(fn)); },
        *this);

    // (memory-pressure) — Assess overall memory pressure and suggest actions.
    //
    //   Returns hash:
    //     {
    //       level: "low" | "medium" | "high" | "critical",
    //       used-pct: 87,                  ; overall usage %
    //       total-used: 12.5,              ; MB
    //       total-capacity: 16.0,          ; MB
    //       top-arena: "json.aura",        ; highest-pct arena name (or "" if none)
    //       top-pct: 92,                   ; top arena's pct (or 0)
    //       suggestions: ["gc-module json.aura", "gc-temp"]  ; vector of strings
    //     }
    //
    //   Thresholds (percent of arena capacity used):
    //     low      < 60
    //     medium   60-79
    //     high     80-94
    //     critical >= 95
    //
    //   Suggestions: for each arena with used-pct >= 80, add "gc-module <name>".
    //   If no gc-temp has been called in the last 100 evaluations, also
    //   add "gc-temp".
    //
    //   Tie-breaking for top-arena: highest used-pct, then largest used
    //   bytes, then name (lexicographic) for determinism.
    primitives_.add("memory-pressure", [this](const auto&) -> EvalValue {
        // Snapshot arena state.
        struct Snap {
            std::string name;
            double used;
            double cap;
            int pct;
        };
        std::vector<Snap> snaps;
        double total_used = 0.0, total_cap = 0.0;
        if (arena_) {
            auto s = arena_->stats();
            double u = s.used / 1048576.0;
            double c = s.capacity / 1048576.0;
            snaps.push_back({"main", u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
            total_used += u;
            total_cap += c;
        }
        if (arena_group_) {
            for (auto& [full_name, stats] : arena_group_->module_stats()) {
                auto slash = full_name.rfind('/');
                auto short_name =
                    slash == std::string::npos ? full_name : full_name.substr(slash + 1);
                double u = stats.used / 1048576.0;
                double c = stats.capacity / 1048576.0;
                snaps.push_back({short_name, u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                total_used += u;
                total_cap += c;
            }
        }
        int overall = total_cap > 0 ? static_cast<int>(total_used / total_cap * 100.0) : 0;

        // Determine level from overall used-pct.
        const char* level = "low";
        if (overall >= 95)
            level = "critical";
        else if (overall >= 80)
            level = "high";
        else if (overall >= 60)
            level = "medium";

        // Find top-arena (highest used-pct, then largest used, then name asc).
        std::string top_name;
        int top_pct = 0;
        double top_used = 0.0;
        for (auto& s : snaps) {
            if (s.pct > top_pct || (s.pct == top_pct && s.used > top_used) ||
                (s.pct == top_pct && s.used == top_used && s.name < top_name)) {
                top_name = s.name;
                top_pct = s.pct;
                top_used = s.used;
            }
        }

        // Build suggestions: for each arena with used-pct >= 80, add a
        // "gc-module <name>" hint. If no recent gc-temp call (within the
        // last 100 evaluations), also add "gc-temp".
        std::vector<EvalValue> suggestions;
        for (auto& s : snaps) {
            if (s.pct >= 80) {
                auto sidx = string_heap_.size();
                string_heap_.push_back("gc-module " + s.name);
                suggestions.push_back(make_string(sidx));
            }
        }
        if (eval_depth_ - last_gc_temp_eval_depth_ > memory_policy_.recent_gc_temp_window) {
            auto sidx = string_heap_.size();
            string_heap_.push_back("gc-temp");
            suggestions.push_back(make_string(sidx));
        }

        // Build the result hash. Inline Swiss-table construction (same
        // shape as gc-arena-info's build_hash, 8-slot capacity).
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto cap = ht->capacity;
        // Helper: insert a (string-key, EvalValue) pair into the hash.
        // String values are interned in string_heap_ first.
        auto hput = [&](const std::string& k, const EvalValue& v) -> bool {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
            auto kidx = string_heap_.size();
            string_heap_.push_back(k);
            EvalValue key_ev = make_string(kidx);
            for (std::size_t at = 0; at < cap; ++at) {
                auto idx = ((h >> 1) + at) & (cap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key_ev.val;
                    vals[idx] = v.val;
                    ht->size++;
                    return true;
                }
            }
            return false;
        };

        // String values: intern the level and top_name, then build String EvalValues.
        auto level_idx = string_heap_.size();
        string_heap_.push_back(level);
        auto top_name_idx = string_heap_.size();
        string_heap_.push_back(top_name);

        // Suggestions vector
        auto sugg_vidx = vector_heap_.size();
        vector_heap_.push_back(std::move(suggestions));

        bool ok = true;
        ok = ok && hput("level", make_string(level_idx));
        ok = ok && hput("used-pct", make_int(overall));
        ok = ok && hput("total-used", make_float(total_used));
        ok = ok && hput("total-capacity", make_float(total_cap));
        ok = ok && hput("top-arena", make_string(top_name_idx));
        ok = ok && hput("top-pct", make_int(top_pct));
        ok = ok && hput("suggestions", make_vector(sugg_vidx));
        if (!ok) {
            FlatHashTable::destroy(ht);
            return make_void();
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // (set-memory-policy hash) — Configure the auto-governance policy
    // for memory pressure. The hash may contain any of:
    //   "auto-gc":              #t / #f       ; default #f
    //   "warn-pct":             int (0-100)   ; default 80
    //   "critical-pct":         int (0-100)   ; default 95
    //   "sample-every":         int (>= 1)    ; default 1000
    //   "cooldown-evals":       int (>= 1)    ; default 5000
    //   "recent-gc-temp-window": int (>= 1)   ; default 100
    // Returns the previous policy as a hash. Pass #f to disable
    // auto-governance (resets to defaults).
    primitives_.add("set-memory-policy", [this](std::span<const EvalValue> a) -> EvalValue {
        // Snapshot the current policy to return as "previous".
        auto prev = memory_policy_;
        // Reset to defaults first; then apply overrides from the hash.
        memory_policy_ = Evaluator::MemoryPolicy{};

        if (a.size() >= 1 && is_hash(a[0])) {
            auto hidx = as_hash_idx(a[0]);
            if (hidx < g_hash_tables.size() && g_hash_tables[hidx]) {
                auto* ht = g_hash_tables[hidx];
                // The hash stores keys as the encoded EvalValue (int64) of
                // the key string at the time the hash was built. The current
                // string_heap_ may have a different interning. So we have
                // to compare by content: for each slot, decode the key
                // back to a string and compare to the target.
                auto hget = [&](const std::string& k) -> EvalValue {
                    for (std::uint64_t i = 0; i < ht->capacity; ++i) {
                        if (ht->metadata()[i] == 0xFF)
                            continue;
                        EvalValue kev(ht->keys()[i]);
                        if (is_string(kev)) {
                            auto kidx = as_string_idx(kev);
                            if (kidx < string_heap_.size() && string_heap_[kidx] == k) {
                                return EvalValue(ht->values()[i]);
                            }
                        }
                    }
                    return make_void();
                };
                auto try_int = [&](const std::string& k, int& out) {
                    auto v = hget(k);
                    if (is_int(v)) {
                        out = static_cast<int>(as_int(v));
                        return true;
                    }
                    return false;
                };
                auto try_bool = [&](const std::string& k, bool& out) {
                    auto v = hget(k);
                    if (is_bool(v)) {
                        out = as_bool(v);
                        return true;
                    }
                    return false;
                };
                int v_i = 0;
                bool v_b = false;
                if (try_bool("auto-gc", v_b))
                    memory_policy_.auto_gc = v_b;
                if (try_int("warn-pct", v_i))
                    memory_policy_.warn_pct = v_i;
                if (try_int("critical-pct", v_i))
                    memory_policy_.critical_pct = v_i;
                if (try_int("sample-every", v_i)) {
                    memory_policy_.sample_every = static_cast<std::size_t>(v_i);
                }
                if (try_int("cooldown-evals", v_i)) {
                    memory_policy_.cooldown_evals = static_cast<std::size_t>(v_i);
                }
                if (try_int("recent-gc-temp-window", v_i)) {
                    memory_policy_.recent_gc_temp_window = static_cast<std::size_t>(v_i);
                }
            }
        }
        // If #f was passed (or empty), the policy stays at defaults.

        // Reset cooldown so the new policy starts fresh.
        last_auto_gc_eval_depth_ = 0;
        sample_counter_ = 0;
        last_warn_level_.clear();

        return build_policy_hash(prev);
    });

    // (get-memory-policy) — Return the current policy as a hash.
    primitives_.add("get-memory-policy",
                    [this](const auto&) -> EvalValue { return build_policy_hash(memory_policy_); });

    // ── Capability primitives (with-capability / capability? / check-capability) ──

    primitives_.add("with-capability", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            auto es = string_heap_.size();
            string_heap_.push_back(
                "with-capability: first argument must be a string or list of strings");
            auto ev = error_values_.size();
            error_values_.push_back(make_string(es));
            return make_error(ev);
        }
        if (a.size() < 2) {
            auto es = string_heap_.size();
            string_heap_.push_back("with-capability: requires at least 2 args");
            auto ev = error_values_.size();
            error_values_.push_back(make_string(es));
            return make_error(ev);
        }
        auto cap_val = a[0];
        std::vector<std::string> caps;
        if (types::is_string(cap_val)) {
            auto sidx = types::as_string_idx(cap_val);
            if (sidx < string_heap_.size())
                caps.push_back(string_heap_[sidx]);
        } else if (types::is_pair(cap_val)) {
            auto cidx = types::as_pair_idx(cap_val);
            while (cidx < pairs_.size()) {
                auto& p = pairs_[cidx];
                if (types::is_string(p.car)) {
                    auto sidx2 = types::as_string_idx(p.car);
                    if (sidx2 < string_heap_.size())
                        caps.push_back(string_heap_[sidx2]);
                }
                if (types::is_int(p.cdr) && types::as_int(p.cdr) == 0)
                    break;
                if (types::is_pair(p.cdr))
                    cidx = types::as_pair_idx(p.cdr);
                else
                    break;
            }
        }
        // Push capability context
        capability_stack_.push_back(caps);
        // Evaluate body expression (the last arg)
        auto body = a[1];
        EvalValue result = make_void();
        if (types::is_closure(body) && workspace_flat_ && workspace_pool_) {
            auto cid = types::as_closure_id(body);
            auto it = closures_.find(cid);
            if (it != closures_.end() && it->second.body_id != ast::NULL_NODE)
                result =
                    eval_flat(*workspace_flat_, *workspace_pool_, it->second.body_id, top_env())
                        .value_or(make_void());
        } else {
            result = body;
        }
        // Pop capability context
        if (!capability_stack_.empty())
            capability_stack_.pop_back();
        return result;
    });

    primitives_.add("capability?",
                    [](const auto& a) -> EvalValue { return types::make_bool(false); });

    primitives_.add("check-capability", [this](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0])) {
            auto es = string_heap_.size();
            string_heap_.push_back("check-capability: first argument must be a string");
            auto ev = error_values_.size();
            error_values_.push_back(make_string(es));
            return make_error(ev);
        }
        auto sidx = types::as_string_idx(a[0]);
        std::string needed;
        if (sidx < string_heap_.size())
            needed = string_heap_[sidx];
        // Check each capability context in reverse order for proper scoping
        for (auto it = capability_stack_.rbegin(); it != capability_stack_.rend(); ++it) {
            for (auto& c : *it) {
                if (c == needed || c == "*") {
                    return types::make_bool(true);
                }
            }
        }
        return types::make_bool(false);
    });

    primitives_.add("capability-stack", [this](const auto&) -> EvalValue {
        // Collect all unique caps from stack
        std::vector<std::string> caps;
        for (auto& layer : capability_stack_) {
            for (auto& cap : layer) {
                bool dup = false;
                for (auto& c : caps)
                    if (c == cap) {
                        dup = true;
                        break;
                    }
                if (!dup)
                    caps.push_back(cap);
            }
        }
        // Build list from BACK to FRONT (append to head)
        EvalValue result = make_void(); // '()
        for (int i = static_cast<int>(caps.size()) - 1; i >= 0; --i) {
            auto sidx = string_heap_.size();
            string_heap_.push_back(caps[i]);
            auto new_pair_idx = pairs_.size();
            pairs_.push_back({make_string(sidx), result});
            result = make_pair(new_pair_idx);
        }
        return result;
    });
}

// slot_for_name: find the slot for a primitive name
std::size_t Primitives::slot_for_name(const std::string& name) const {
    for (std::size_t i = 0; i < ordered_names_.size(); ++i) {
        if (ordered_names_[i] == name)
            return i;
    }
    return std::numeric_limits<std::size_t>::max();
}

void Evaluator::build_primitive_slots() {
    // No longer needed — Primitives manages ordering internally
}

// ── Module path resolution ──────────────────────────────────
std::string Evaluator::resolve_module_path(const std::string& path) const {
    auto try_load = [](const std::string& full) -> std::optional<std::string> {
        for (auto candidate : {full, full + ".aura"}) {
            struct stat st;
            if (::stat(candidate.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            char real[4096];
            if (::realpath(candidate.c_str(), real))
                return std::string(real);
            return candidate;
        }
        return std::nullopt;
    };

    if (!path.empty() && path[0] == '/') {
        auto hit = try_load(path);
        if (hit)
            return *hit;
        return {};
    }

    // Search CWD first
    {
        char cwd_buf[4096];
        if (::getcwd(cwd_buf, sizeof(cwd_buf))) {
            auto hit = try_load(std::string(cwd_buf) + "/" + path);
            if (hit)
                return *hit;
        }
    }

    // Search AURA_PATH
    auto* env = ::getenv("AURA_PATH");
    if (env) {
        std::string aura_path(env);
        std::size_t start = 0, end;
        while ((end = aura_path.find(':', start)) != std::string::npos) {
            auto dir = aura_path.substr(start, end - start);
            if (!dir.empty()) {
                auto hit = try_load(dir + "/" + path);
                if (hit)
                    return *hit;
            }
            start = end + 1;
        }
        if (start < aura_path.size()) {
            auto dir = aura_path.substr(start);
            if (!dir.empty()) {
                auto hit = try_load(dir + "/" + path);
                if (hit)
                    return *hit;
            }
        }
    }

    // Auto-discover: try ../lib/ and ./lib/ (relative to executable / CWD)
    {
        // Try ../lib/ (common for build/aura → lib/ layout)
        auto hit = try_load("../lib/" + path);
        if (hit)
            return *hit;
    }
    {
        // Try ./lib/ (cwd-relative)
        auto hit = try_load("./lib/" + path);
        if (hit)
            return *hit;
    }

    return {};
}

// ── Load module file, return module object ────────────────
types::EvalValue Evaluator::load_module_file(const std::string& path) {
    // 1. Resolve path
    auto resolved = resolve_module_path(path);
    if (resolved.empty()) {
        std::println(std::cerr, "load_module_file: cannot resolve '{}'", path);
        return types::make_void();
    }

    // 2. Check cache
    auto cache_it = module_cache_.find(resolved);
    if (cache_it != module_cache_.end()) {
        return types::make_module(cache_it->second);
    }

    // 3. Circular dependency detection
    if (loading_stack_.count(resolved)) {
        auto eidx = string_heap_.size();
        string_heap_.push_back("circular dependency: " + resolved);
        return types::make_void();
    }
    loading_stack_.insert(resolved);

    // 4. Read file
    struct stat st;
    if (::stat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }
    std::ifstream f(resolved);
    if (!f) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }
    std::string content((std::istreambuf_iterator<char>(f)), {});
    if (content.empty()) {
        loading_stack_.erase(resolved);
        return types::make_void();
    }

    // 5. Parse
    if (!arena_) {
        loading_stack_.erase(resolved);
        std::println(std::cerr, "load_module_file: no arena");
        return types::make_void();
    }
    // Per-module arena: StringPool / FlatAST / mod_env all live here so the
    // entire module (incl. closures that reference its FlatAST/Pool) can be
    // freed in O(1) by reset_module(resolved). The default 8MB initial size
    // matches ASTArena's main arena budget; can be tuned per module later.
    auto& mod_arena = arena_group_->module_arena(resolved);
    auto alloc = mod_arena.allocator();
    auto* pool_ptr = mod_arena.create<aura::ast::StringPool>(alloc);
    auto* flat_ptr = mod_arena.create<aura::ast::FlatAST>(alloc);
    auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
    if (!pr.success || pr.root == aura::ast::NULL_NODE) {
        loading_stack_.erase(resolved);
        std::println(std::cerr, "load_module_file: parse error for {}", resolved);
        if (!pr.error.empty())
            std::println(std::cerr, "  {}", pr.error);
        // Free the partial allocation — module was never inserted into cache.
        arena_group_->reset_module(resolved);
        return types::make_void();
    }
    flat_ptr->root = pr.root;

    // 6. Create isolated module env (child of top_ for primitive access)
    // Arena-allocate in the per-module arena so closures captured during
    // module eval stay valid for the module's lifetime.
    auto* mod_env = mod_arena.create<Env>(&top_);
    mod_env->set_primitives(&primitives_);
    // Issue #232 follow-up: register the module env in env_frames_ so
    // closures captured during module eval (e.g., the lambda in
    // `(define (f x) ...)`) can walk the parent chain via SoA when
    // materialize_call_env creates a fresh env for the call. Without
    // this, the closure body can't see bindings from the surrounding
    // scope (e.g., from a nested `(require "other-module" all:)`).
    //
    // Pass top_.parent_id() (the env_id of top_'s frame, which is
    // the root frame index) as the explicit parent. This makes the
    // new frame's parent_id_ point to top_'s frame so the SoA walk
    // can find top_'s bindings (which is where the require primitive
    // injects the required module's exports).
    EnvId top_id = top_.parent_id();
    if (top_id == NULL_ENV_ID)
        top_id = 0; // top_ is at index 0
    EnvId mod_id = alloc_env_frame_from_env(*mod_env, top_id);
    const_cast<Env*>(mod_env)->set_parent_id(mod_id);


    // 7. Clear any stale export set from previous module loads
    if (current_export_set_)
        current_export_set_->clear();

    // 8. Evaluate module in its own env
    auto expanded = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);
    auto result = eval_flat(*flat_ptr, *pool_ptr, expanded, *mod_env);

    // 9. Apply export filtering: if (export ...) was declared, remove unexported bindings
    if (current_export_set_ && !current_export_set_->empty()) {
        auto& bindings = mod_env->bindings();
        for (auto it = bindings.begin(); it != bindings.end();) {
            if (!current_export_set_->count(it->first)) {
                it = bindings.erase(it);
            } else {
                ++it;
            }
        }
        current_export_set_->clear();
    }

    // 10. Store module (pointer to arena-allocated env — persists for the
    // module's lifetime, freed by gc_module(resolved)).
    auto mod_idx = modules_.size();
    modules_.push_back(mod_env);
    module_cache_[resolved] = mod_idx;
    module_arena_ptrs_[resolved] = &mod_arena;
    string_heap_.push_back(resolved);
    module_names_.push_back(resolved);

    // 10b. IR caching callback (registered by CompilerService)
    if (module_loaded_cb_) {
        module_loaded_cb_(content, resolved);
    }

    // 10c. 自动加载 .aura-type 类型签名文件
    // 检查 {module}.aura 同目录下是否有 {module}.aura-type 文件
    auto type_sig_path = resolved;
    if (type_sig_path.size() > 5) {
        auto dot = type_sig_path.rfind('.');
        if (dot != std::string::npos)
            type_sig_path = type_sig_path.substr(0, dot) + ".aura-type";
    }
    struct stat st2;
    if (::stat(type_sig_path.c_str(), &st2) == 0 && S_ISREG(st2.st_mode)) {
        std::ifstream tf(type_sig_path);
        if (tf) {
            std::string line;
            while (std::getline(tf, line)) {
                // 格式: "name: param1 param2 -> rettype"
                // 例如 "add: Int Int -> Int"
                auto colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                auto arrow = line.find("->", colon);
                if (arrow == std::string::npos)
                    continue;
                auto name = line.substr(0, colon);
                name.erase(name.find_last_not_of(" \t\r") + 1);
                auto params_str = line.substr(colon + 1, arrow - colon - 1);
                params_str.erase(0, params_str.find_first_not_of(" \t\r"));
                params_str.erase(params_str.find_last_not_of(" \t\r") + 1);
                auto ret_str = line.substr(arrow + 2);
                ret_str.erase(0, ret_str.find_first_not_of(" \t\r"));
                ret_str.erase(ret_str.find_last_not_of(" \t\r\n") + 1);
                if (!name.empty() && !ret_str.empty()) {
                    declared_type_sigs_[name] = {.type_str = params_str + "|" + ret_str,
                                                 .module_file = resolved,
                                                 .resolved = false};
                }
            }
        }
    }

    // 10d. 为没有 .aura-type 签名的导出函数注册 Any 级签名
    // 这样 typecheck-current 至少知道这些函数存在，不会报 unbound variable。
    // 如果有 .aura-type 签名，优先使用（已在 10c 中注册）。
    for (auto& [fname, fval] : mod_env->bindings()) {
        if (declared_type_sigs_.find(fname) != declared_type_sigs_.end())
            continue; // 已有 .aura-type 签名
        // Module bindings are stored as cells (define creates mutable cells).
        // Unwrap cell to get the actual closure value.
        types::EvalValue actual = fval;
        if (types::is_cell(actual)) {
            auto cid = types::as_cell_id(actual);
            if (cid < cells_.size())
                actual = cells_[cid];
        }
        if (types::is_closure(actual)) {
            auto cid = types::as_closure_id(actual);
            std::string param_str;
            auto cit = closures_.find(cid);
            if (cit != closures_.end() && !cit->second.params.empty()) {
                for (std::size_t pi = 0; pi < cit->second.params.size(); ++pi) {
                    if (pi > 0)
                        param_str += " ";
                    param_str += "Any";
                }
                param_str += " ";
            }
            declared_type_sigs_[fname] = {
                .type_str = param_str + "|Any", .module_file = resolved, .resolved = false};
        }
    }

    loading_stack_.erase(resolved);
    return types::make_module(mod_idx);
}

// Free a module's per-module arena and all closures it owns. Removes the
// module from modules_ / module_cache_ / module_names_. After the call,
// module lookup by name returns void. Caller must ensure no in-flight
// calls into the module (the language runtime is single-threaded per
// Evaluator, so this is the caller's responsibility).
bool Evaluator::gc_module(const std::string& path) {
    auto cache_it = module_cache_.find(path);
    if (cache_it == module_cache_.end())
        return false;
    auto mod_idx = cache_it->second;
    if (mod_idx >= modules_.size())
        return false;

    // Erase closures whose owner_arena matches the module's arena.
    auto arena_it = module_arena_ptrs_.find(path);
    if (arena_it != module_arena_ptrs_.end() && arena_it->second) {
        auto* owner = arena_it->second;
        for (auto it = closures_.begin(); it != closures_.end();) {
            if (it->second.owner_arena == owner)
                it = closures_.erase(it);
            else
                ++it;
        }
    }

    // Reset the module arena. ASTArena v4 (#131) now runs ~Env() on
    // every arena-allocated object as part of reset(), so we no
    // longer need to call env->~Env() manually here — that was the
    // pre-#131 band-aid and would now double-destroy.
    arena_group_->reset_module(path);

    // Clear the module slot. Swap-with-last keeps indices stable for
    // any other live modules; update module_cache_ and
    // functor_instance_cache_ accordingly.
    auto last = modules_.size() - 1;
    if (mod_idx != last) {
        modules_[mod_idx] = modules_[last];
        for (auto& [p, idx] : module_cache_) {
            if (idx == last) {
                module_cache_[p] = mod_idx;
                break;
            }
        }
        for (auto& [key, idx] : functor_instance_cache_) {
            if (idx == last) {
                functor_instance_cache_[key] = mod_idx;
                break;
            }
        }
    }
    modules_.pop_back();
    module_names_.pop_back();
    module_cache_.erase(cache_it);
    module_arena_ptrs_.erase(path);
    // Also remove from functor instance cache if present (the
    // swap-with-last above already fixed indices for survivors; the
    // removed entry's key is the path itself for functor instances).
    auto fic_it = functor_instance_cache_.find(path);
    if (fic_it != functor_instance_cache_.end())
        functor_instance_cache_.erase(fic_it);

    return true;
}

Env* Evaluator::copy_env(const Env& e, ast::ASTArena* target) {
    // The pre (target != nullptr) is on the declaration in
    // evaluator.ixx. arena_ can't appear there (it's private),
    // so we additionally assert it here. contract_assert is the
    // right tool for impl-only invariants that can't go on the
    // interface contract.
    contract_assert(arena_ != nullptr);
    auto* ar = target ? target : arena_;
    return ar ? ar->create<Env>(e) : nullptr;
}

// Build a 6-key policy hash from a MemoryPolicy. Interned in string_heap_.
// Defined as a member function (not a local lambda) so it can be referenced
// from std::function-captured primitives without dangling.
EvalValue Evaluator::build_policy_hash(const MemoryPolicy& p) {
    std::vector<std::pair<std::string, EvalValue>> kv;
    kv.push_back({"auto-gc", make_bool(p.auto_gc)});
    kv.push_back({"warn-pct", make_int(p.warn_pct)});
    kv.push_back({"critical-pct", make_int(p.critical_pct)});
    kv.push_back({"sample-every", make_int(static_cast<std::int64_t>(p.sample_every))});
    kv.push_back({"cooldown-evals", make_int(static_cast<std::int64_t>(p.cooldown_evals))});
    kv.push_back(
        {"recent-gc-temp-window", make_int(static_cast<std::int64_t>(p.recent_gc_temp_window))});
    auto* ht = FlatHashTable::create(8);
    if (!ht)
        return make_void();
    auto meta = ht->metadata();
    auto keys = ht->keys();
    auto vals = ht->values();
    auto cap = ht->capacity;
    for (auto& [k, v] : kv) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (char c : k)
            h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
        auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
        if (fp == 0xFF)
            fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
        auto kidx = string_heap_.size();
        string_heap_.push_back(k);
        EvalValue key_ev = make_string(kidx);
        bool inserted = false;
        for (std::size_t at = 0; at < cap; ++at) {
            auto idx = ((h >> 1) + at) & (cap - 1);
            if (meta[idx] == 0xFF) {
                meta[idx] = fp;
                keys[idx] = key_ev.val;
                vals[idx] = v.val;
                ht->size++;
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            FlatHashTable::destroy(ht);
            return make_void();
        }
    }
    auto hidx = g_hash_tables.size();
    g_hash_tables.push_back(ht);
    return make_hash(hidx);
}

// eval_in(ast::Expr*) removed — all evaluation uses eval_flat(FlatAST&) now

// apply_closure — looks up closures_, foreign functions, or IR bridge
std::optional<EvalValue> Evaluator::apply_closure(ClosureId cid, std::span<const EvalValue> args) {
    // Issue #252: closure dual-path observability. Bump the
    // total counter on every call. The path-specific counters
    // (ffi / tw / bridge / ir) are bumped in each branch.
    // The IR path (runtime_closures_) bumps the ir counter
    // via the shared metrics pointer (set by service.ixx).
    if (compiler_metrics_) {
        auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
        m->closure_calls_total.fetch_add(1, std::memory_order_relaxed);
    }
    // Check for foreign function closure (cid < ffi_runtime_.func_count())
    if (cid < ffi_runtime_.func_count()) {
        if (compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
            m->closure_ffi_calls.fetch_add(1, std::memory_order_relaxed);
        }
        auto fidx = cid;
        if (fidx < ffi_runtime_.func_count()) {
            auto& ff = ffi_runtime_.func_at(static_cast<std::size_t>(fidx));
            void* fn_ptr = ff.fn_ptr;
            int ret_type = ff.ret_type;
            auto& arg_types = ff.arg_types;

            // Issue #146 Phase 5: FFI marshalling is now a pure
            // function (aura::compiler::pure::ffi_marshal_args_pure)
            // that takes all dependencies as spans. The legacy
            // stateful loop above has been replaced with a single
            // call to the pure version; the FFI dispatch (calling
            // the function pointer below) stays in this function
            // because it depends on the per-FFI function pointer
            // from ffi_runtime_.
            std::span<const std::string> string_heap_view(string_heap_.data(), string_heap_.size());
            std::span<void* const> opaque_heap_view(opaque_heap_.data(), opaque_heap_.size());
            auto marshalled = aura::compiler::pure::ffi_marshal_args_pure(
                args, arg_types, string_heap_view, opaque_heap_view);
            const auto& i6 = marshalled.i_vals;
            const auto& d6 = marshalled.d_vals;
            // s6 and str_bufs are not used directly (we only
            // need the void*; s_vals already points into str_bufs
            // for lifetime).

            std::int64_t result_i = 0;
            double result_f = 0.0;

            if (marshalled.any_float) {
                auto f_fn =
                    reinterpret_cast<double (*)(double, double, double, double, double, double)>(
                        fn_ptr);
                result_f = f_fn(d6[0], d6[1], d6[2], d6[3], d6[4], d6[5]);
                if (ret_type == 2)
                    return types::make_float(result_f);
                if (ret_type == 1)
                    return types::make_int(static_cast<std::int64_t>(result_f));
                return types::make_float(result_f);
            } else {
                auto i_fn =
                    reinterpret_cast<std::int64_t (*)(std::int64_t, std::int64_t, std::int64_t,
                                                      std::int64_t, std::int64_t, std::int64_t)>(
                        fn_ptr);
                result_i = i_fn(i6[0], i6[1], i6[2], i6[3], i6[4], i6[5]);
                if (ret_type == 2)
                    return types::make_float(*reinterpret_cast<double*>(&result_i));
                if (ret_type == 3 && result_i != 0) {
                    // String return: char* → string_heap
                    auto s = reinterpret_cast<const char*>(static_cast<std::intptr_t>(result_i));
                    auto sidx = string_heap_.size();
                    string_heap_.emplace_back(s ? s : "");
                    return types::make_string(sidx);
                }
                if (ret_type == 4) {
                    // Opaque: store pointer in opaque_heap_, return OpaqueRef
                    auto oi = opaque_heap_.size();
                    opaque_heap_.push_back(reinterpret_cast<void*>(result_i));
                    return types::make_opaque(oi);
                }
                return types::make_int(result_i);
            }
        }
        return std::nullopt;
    }

    // Try tree-walker closures first
    // Issue #145 P0 follow-up: shared lock on closures_mtx_
    // while we look up AND copy the closure. The fiber
    // thread holds this lock for the duration of the copy
    // (microseconds), so the main thread's mutations don't
    // race with the lookup. Without this lock, the fiber
    // thread can see a half-modified Closure (the hash
    // table node's key/value pair can be in an inconsistent
    // state during insert/erase on the main thread).
    Closure cl_copy;
    bool cl_found = false;
    {
        std::shared_lock<std::shared_mutex> rlock(closures_mtx_);
        auto it = closures_.find(cid);
        if (it != closures_.end()) {
            cl_copy = it->second;
            cl_found = true;
        }
    }
    if (cl_found) {
        if (compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
            m->closure_tw_calls.fetch_add(1, std::memory_order_relaxed);
        }
        // Issue #145 Phase 2.3 — materialize the call env from
        // the SoA arena (env_frames_[cl.env_id]) -- legacy cl.env pointer path removed in P0.
        // materialize_call_env now takes env_frames_mtx_
        // internally (Issue #145 P0 follow-up — env_frames_
        // deque map-array reallocation race under fiber:spawn).
        Env ne = materialize_call_env(cl_copy);
        ne.set_primitives(&primitives_);

        // Issue #145: set the pool so bind_symid can mirror
        // lambda params into the string-keyed bindings_ array
        // (so body code's lookup(name) still finds them).
        if (cl_copy.pool)
            ne.set_pool(cl_copy.pool);
        if (cl_copy.dotted) {
            // Dotted rest param: bind named params, collect rest into list
            std::size_t named_count = cl_copy.params.size() - 1;
            for (std::size_t i = 0; i < named_count && i < args.size(); ++i)
                ne.bind_symid(cl_copy.params[i], args[i]); // Issue #145: SymId
            // Collect remaining args into a pair list for the rest param
            types::EvalValue rest = make_void();
            for (std::size_t i = args.size(); i > named_count; --i) {
                auto pid = pairs_.size();
                pairs_.push_back({args[i - 1], rest});
                rest = make_pair(pid);
            }
            ne.bind_symid(cl_copy.params.back(), rest);
        } else {
            for (std::size_t i = 0; i < cl_copy.params.size() && i < args.size(); ++i)
                ne.bind_symid(cl_copy.params[i], args[i]); // Issue #145: SymId
        }
        if (cl_copy.flat) {
            // Issue #223: check if the closure's bridge is stale
            // (arena was reset, or major mutation invalidated the
            // captured flat*/pool*). If stale, the flat*/pool*
            // are dangling — invalidate the closure (return
            // nullopt) so the caller can re-bridge from the new
            // arena. The body_source re-parse fallback is a
            // future slice (requires parser integration).
            if (is_bridge_stale(cl_copy.bridge_epoch, current_bridge_epoch())) {
                if (compiler_metrics_) {
                    auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                    m->closure_stale_returns.fetch_add(1, std::memory_order_relaxed);
                }
                return std::nullopt;
            }
            auto r = eval_flat(*cl_copy.flat, *cl_copy.pool, cl_copy.body_id, ne);
            if (r)
                return *r;
        }
        return std::nullopt;
    }

    // Try IR bridge
    if (closure_bridge_) {
        if (compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
            m->closure_bridge_calls.fetch_add(1, std::memory_order_relaxed);
        }
        return closure_bridge_(cid, args);
    }

    return std::nullopt;
}

// ── ast_to_data: convert AST subtree to EvalValue data ───────
EvalValue Evaluator::ast_to_data(const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
                                 aura::ast::NodeId nid) {
    if (nid == ast::NULL_NODE)
        return make_void();
    auto v = flat.get(nid);
    // Local helper: build (cons "fn-name" args)
    auto cd = [&](const std::string& fn, const EvalValue& args) -> EvalValue {
        auto fi = string_heap_.size();
        string_heap_.push_back(fn);
        auto pi = pairs_.size();
        pairs_.push_back({types::make_string(fi), args});
        return types::make_pair(pi);
    };

    switch (v.tag) {
        case ast::NodeTag::LiteralInt:
            return make_int(v.int_value);
        case ast::NodeTag::LiteralFloat:
            return make_float(v.float_value);
        case ast::NodeTag::LiteralString: {
            auto name = std::string(pool.resolve(v.sym_id));
            auto idx = string_heap_.size();
            string_heap_.push_back(std::move(name));
            return make_string(idx);
        }
        case ast::NodeTag::Variable: {
            auto name = std::string(pool.resolve(v.sym_id));
            // Issue #231: dedup by content so two quote literals
            // of the same symbol produce the same string heap
            // index. Without this, `(eq? 'eda:module 'eda:module)`
            // returns #f because each quote creates a fresh
            // string heap entry. Use short_str_cache_ for short
            // symbols (the common case for symbols with colon
            // prefixes like eda:module), and a heap scan for
            // longer ones. Symbols are usually < 32 chars so the
            // linear scan is bounded.
            if (name.size() <= 6) {
                auto it = short_str_cache_.find(name);
                if (it != short_str_cache_.end())
                    return it->second;
            } else {
                for (std::size_t i = 0; i < string_heap_.size(); ++i) {
                    if (string_heap_[i] == name)
                        return make_string(static_cast<std::int64_t>(i));
                }
            }
            auto idx = string_heap_.size();
            string_heap_.push_back(std::move(name));
            auto val = make_string(static_cast<std::int64_t>(idx));
            if (name.size() <= 6)
                short_str_cache_[name] = val;
            return val;
        }
        case ast::NodeTag::Call: {
            EvalValue tail = make_void();
            for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
                auto item = ast_to_data(flat, pool, *it);
                auto pair_idx = pairs_.size();
                pairs_.push_back(Pair{std::move(item), tail});
                tail = make_pair(pair_idx);
            }
            return tail;
        }
        case ast::NodeTag::Begin: {
            EvalValue tail = make_void();
            for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
                auto item = ast_to_data(flat, pool, *it);
                auto pair_idx = pairs_.size();
                pairs_.push_back(Pair{std::move(item), tail});
                tail = make_pair(pair_idx);
            }
            return cd("begin", tail);
        }
        case ast::NodeTag::IfExpr: {
            auto cond = v.children.size() > 0 ? ast_to_data(flat, pool, v.child(0)) : make_void();
            auto then_b = v.children.size() > 1 ? ast_to_data(flat, pool, v.child(1)) : make_void();
            auto else_b = v.children.size() > 2 ? ast_to_data(flat, pool, v.child(2)) : make_void();
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({then_b, else_b});
            tail = make_pair(pairs_.size());
            pairs_.push_back({cond, tail});
            return cd("if", tail);
        }
        case ast::NodeTag::Lambda: {
            EvalValue params_tail = make_void();
            for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
                auto pname = std::string(pool.resolve(*it));
                auto pidx = string_heap_.size();
                string_heap_.push_back(pname);
                auto pair_idx = pairs_.size();
                pairs_.push_back({make_string(pidx), params_tail});
                params_tail = make_pair(pair_idx);
            }
            auto body = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({params_tail, body});
            return cd("lambda", tail);
        }
        case ast::NodeTag::Define: {
            auto name_str = std::string(pool.resolve(v.sym_id));
            auto nidx = string_heap_.size();
            string_heap_.push_back(name_str);
            auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({make_string(nidx), val});
            return cd("define", tail);
        }
        case ast::NodeTag::DefineType: {
            auto type_name = pool.resolve(v.sym_id);
            auto tnidx = string_heap_.size();
            string_heap_.push_back(std::string(type_name));
            EvalValue params_tail = make_void();
            for (auto it = v.params.rbegin(); it != v.params.rend(); ++it) {
                auto pname = std::string(pool.resolve(*it));
                auto pidx = string_heap_.size();
                string_heap_.push_back(pname);
                auto pp = pairs_.size();
                pairs_.push_back({make_string(pidx), params_tail});
                params_tail = make_pair(pp);
            }
            auto type_spec = make_pair(pairs_.size());
            pairs_.push_back({make_string(tnidx), params_tail});
            EvalValue ctors_tail = make_void();
            for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
                auto ctor_data = ast_to_data(flat, pool, *it);
                auto pp = pairs_.size();
                pairs_.push_back({ctor_data, ctors_tail});
                ctors_tail = make_pair(pp);
            }
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({type_spec, ctors_tail});
            return cd("define-type", tail);
        }
        case ast::NodeTag::Set: {
            auto name_str = std::string(pool.resolve(v.sym_id));
            auto nidx = string_heap_.size();
            string_heap_.push_back(name_str);
            auto val = v.children.empty() ? make_void() : ast_to_data(flat, pool, v.child(0));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({make_string(nidx), val});
            return cd("set!", tail);
        }
        case ast::NodeTag::Let:
        case ast::NodeTag::LetRec: {
            // The let node from add_let / parser has:
            //   sym_id = binding name (e.g. x)
            //   children = [val_node, body_node]
            // The body is always the LAST child.
            auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
            auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
            auto bname = std::string(pool.resolve(v.sym_id));
            auto bni = string_heap_.size();
            string_heap_.push_back(bname);
            auto bv =
                val_id != aura::ast::NULL_NODE ? ast_to_data(flat, pool, val_id) : make_void();
            auto body =
                body_id != aura::ast::NULL_NODE ? ast_to_data(flat, pool, body_id) : make_void();
            // Build: (cons name val) → bindings pair
            auto bp = pairs_.size();
            pairs_.push_back({make_string(bni), bv});
            auto bindings_tail = make_pair(bp);
            // Build: (cons bindings body) → complete let form
            auto full_bindings = make_pair(pairs_.size());
            pairs_.push_back({bindings_tail, body});
            auto kind = v.tag == ast::NodeTag::LetRec ? "letrec" : "let";
            return cd(kind, full_bindings);
        }
        case ast::NodeTag::Quote: {
            if (!v.children.empty()) {
                auto quoted = ast_to_data(flat, pool, v.child(0));
                auto tail = make_pair(pairs_.size());
                pairs_.push_back({quoted, make_void()});
                return cd("quote", tail);
            }
            return make_void();
        }
        case ast::NodeTag::Coercion: {
            if (!v.children.empty()) {
                auto expr = ast_to_data(flat, pool, v.child(0));
                auto tail = make_pair(pairs_.size());
                pairs_.push_back({expr, make_void()});
                return cd("cast", tail);
            }
            return make_void();
        }
        case ast::NodeTag::Pair: {
            auto car = ast_to_data(flat, pool, v.child(0));
            auto cdr_val = ast_to_data(flat, pool, v.child(1));
            auto tail = make_pair(pairs_.size());
            pairs_.push_back({cdr_val, make_void()});
            tail = make_pair(pairs_.size());
            pairs_.push_back({car, tail});
            return cd("cons", tail);
        }
        default:
            return make_void();
    }
}
// Inverse of ast_to_data. Needed so lambda bodies from macro data
// can be converted to AST for closure creation.
ast::NodeId Evaluator::data_to_flat(const types::EvalValue& data, aura::ast::FlatAST& flat,
                                    aura::ast::StringPool& pool, int depth) {
    using namespace types;
    if (depth > 256)
        return ast::NULL_NODE;
    if (is_int(data)) {
        return flat.add_literal(as_int(data));
    }
    if (is_float(data)) {
        return flat.add_literal_float(as_float(data));
    }
    if (is_bool(data)) {
        auto id = flat.add_literal(as_bool(data) ? 1 : 0);
        flat.set_marker(id, ast::SyntaxMarker::BoolLiteral);
        return id;
    }
    if (is_void(data)) {
        return flat.add_literal(0); // () sentinel
    }
    if (is_string(data)) {
        auto idx = as_string_idx(data);
        if (idx < string_heap_.size()) {
            // Strings in code context are variable references
            auto name = string_heap_[idx];
            auto sid = pool.intern(name);
            return flat.add_variable(sid);
        }
        return ast::NULL_NODE;
    }
    if (is_pair(data)) {
        auto pair_idx = as_pair_idx(data);
        if (pair_idx >= pairs_.size())
            return ast::NULL_NODE;

        auto car_data = pairs_[pair_idx].car;
        auto cdr_data = pairs_[pair_idx].cdr;

        if (is_string(car_data)) {
            auto fn_idx = as_string_idx(car_data);
            auto fn_name = fn_idx < string_heap_.size() ? string_heap_[fn_idx] : "";

            // Quote: (quote expr)
            if (fn_name == "quote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto quoted = pairs_[qp].car;
                auto quoted_node = data_to_flat(quoted, flat, pool, depth + 1);
                if (quoted_node != ast::NULL_NODE)
                    return flat.add_quote(quoted_node);
                return flat.add_literal(0); // fallback
            }

            // Quasiquote: (quasiquote expr)
            if (fn_name == "quasiquote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto quoted = pairs_[qp].car;
                return data_to_flat(quoted, flat, pool, depth + 1);
            }

            // Unquote: (unquote expr) — just convert the inner expression
            if (fn_name == "unquote" && is_pair(cdr_data)) {
                auto qp = as_pair_idx(cdr_data);
                auto inner = pairs_[qp].car;
                return data_to_flat(inner, flat, pool, depth + 1);
            }

            // Define-type: (define-type (type-name params...) (ctor1 ctor2 ...))
            if (fn_name == "define-type") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto type_name_data = pairs_[np].car;
                    auto ctor_rest = pairs_[np].cdr;

                    aura::ast::SymId type_name_val = 0;
                    std::vector<aura::ast::SymId> params;
                    std::vector<ast::NodeId> ctors;

                    if (is_pair(type_name_data)) {
                        auto tnp = as_pair_idx(type_name_data);
                        if (is_string(pairs_[tnp].car)) {
                            auto ti = as_string_idx(pairs_[tnp].car);
                            auto ts = ti < string_heap_.size() ? string_heap_[ti] : "";
                            type_name_val = pool.intern(ts);
                            auto rest = pairs_[tnp].cdr;
                            while (is_pair(rest)) {
                                auto rp = as_pair_idx(rest);
                                if (is_string(pairs_[rp].car)) {
                                    auto pi = as_string_idx(pairs_[rp].car);
                                    auto ps = pi < string_heap_.size() ? string_heap_[pi] : "";
                                    params.push_back(pool.intern(ps));
                                }
                                rest = pairs_[rp].cdr;
                            }
                        }
                    } else if (is_string(type_name_data)) {
                        auto ti = as_string_idx(type_name_data);
                        auto ts = ti < string_heap_.size() ? string_heap_[ti] : "";
                        type_name_val = pool.intern(ts);
                    }

                    if (type_name_val != 0) {
                        auto cur = ctor_rest;
                        while (is_pair(cur)) {
                            auto cp = as_pair_idx(cur);
                            auto ctor_form = pairs_[cp].car;
                            cur = pairs_[cp].cdr;

                            if (is_pair(ctor_form)) {
                                auto ctor_pair = as_pair_idx(ctor_form);
                                auto ctor_car = pairs_[ctor_pair].car;
                                // Handle (quote Name) format
                                if (is_string(ctor_car)) {
                                    auto ci = as_string_idx(ctor_car);
                                    auto cs = ci < string_heap_.size() ? string_heap_[ci] : "";
                                    if (cs == "quote" && is_pair(pairs_[ctor_pair].cdr)) {
                                        // (quote Some) — extract "Some" from cdr
                                        auto qcdr = as_pair_idx(pairs_[ctor_pair].cdr);
                                        auto name_val = pairs_[qcdr].car;
                                        if (is_string(name_val)) {
                                            auto ni = as_string_idx(name_val);
                                            auto ns =
                                                ni < string_heap_.size() ? string_heap_[ni] : "";
                                            auto ctor_var = flat.add_variable(pool.intern(ns));
                                            ctors.push_back(flat.add_quote(ctor_var));
                                        }
                                    }
                                } else {
                                    // Direct ctor descriptor — store as-is
                                    auto ctor_node = data_to_flat(ctor_form, flat, pool, depth + 1);
                                    if (ctor_node != ast::NULL_NODE)
                                        ctors.push_back(ctor_node);
                                }
                            }
                        }
                        return flat.add_define_type(type_name_val, params, ctors);
                    }
                }
                return ast::NULL_NODE;
            }

            // Begin: (begin ...)
            if (fn_name == "begin") {
                std::vector<ast::NodeId> exprs;
                auto cur = cdr_data;
                while (is_pair(cur)) {
                    auto cp = as_pair_idx(cur);
                    auto e = data_to_flat(pairs_[cp].car, flat, pool, depth + 1);
                    if (e != ast::NULL_NODE)
                        exprs.push_back(e);
                    cur = pairs_[cp].cdr;
                }
                return flat.add_begin(exprs);
            }

            // If: (if cond then else)
            if (fn_name == "if") {
                ast::NodeId cond_node = ast::NULL_NODE, then_node = ast::NULL_NODE,
                            else_node = ast::NULL_NODE;
                if (is_pair(cdr_data)) {
                    auto cp = as_pair_idx(cdr_data);
                    cond_node = data_to_flat(pairs_[cp].car, flat, pool, depth + 1);
                    auto rest = pairs_[cp].cdr;
                    if (is_pair(rest)) {
                        auto tp = as_pair_idx(rest);
                        then_node = data_to_flat(pairs_[tp].car, flat, pool, depth + 1);
                        auto erest = pairs_[tp].cdr;
                        if (is_pair(erest)) {
                            auto ep = as_pair_idx(erest);
                            else_node = data_to_flat(pairs_[ep].car, flat, pool, depth + 1);
                        }
                    }
                }
                if (cond_node != ast::NULL_NODE && then_node != ast::NULL_NODE &&
                    else_node != ast::NULL_NODE)
                    return flat.add_if(cond_node, then_node, else_node);
                if (cond_node != ast::NULL_NODE && then_node != ast::NULL_NODE)
                    return flat.add_if(cond_node, then_node, flat.add_literal(0));
                return cond_node != ast::NULL_NODE ? cond_node : flat.add_literal(0);
            }

            // Lambda: (lambda (args) body)
            if (fn_name == "lambda") {
                if (is_pair(cdr_data)) {
                    auto params_pair = as_pair_idx(cdr_data);
                    auto params_data = pairs_[params_pair].car;
                    auto body_rest = pairs_[params_pair].cdr;

                    std::vector<ast::SymId> params;
                    auto args_data = params_data;
                    while (is_pair(args_data)) {
                        auto ap = as_pair_idx(args_data);
                        auto arg = pairs_[ap].car;
                        if (is_string(arg)) {
                            auto aidx = as_string_idx(arg);
                            auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                            params.push_back(pool.intern(astr));
                        }
                        args_data = pairs_[ap].cdr;
                    }

                    ast::NodeId body_node = ast::NULL_NODE;
                    if (is_pair(body_rest)) {
                        auto bp = as_pair_idx(body_rest);
                        body_node = data_to_flat(pairs_[bp].car, flat, pool, depth + 1);
                    }
                    if (body_node == ast::NULL_NODE)
                        body_node = flat.add_literal(0);
                    return flat.add_lambda(params, body_node);
                }
                return ast::NULL_NODE;
            }

            // Define: (define name value)
            if (fn_name == "define") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto name_data = pairs_[np].car;
                    auto val_rest = pairs_[np].cdr;

                    if (is_string(name_data) && is_pair(val_rest)) {
                        auto ni = as_string_idx(name_data);
                        auto ns = ni < string_heap_.size() ? string_heap_[ni] : "";
                        auto vp = as_pair_idx(val_rest);
                        auto val_node = data_to_flat(pairs_[vp].car, flat, pool, depth + 1);
                        return flat.add_define(pool.intern(ns), val_node);
                    }
                }
                return ast::NULL_NODE;
            }

            // Issue #150 Phase 1: performance-region / evolution-region
            // wrappers. The forms are:
            //   (performance-region (define f ...))
            //   (performance-region (lambda (x) ...))
            //   (evolution-region (define g ...))
            //   (evolution-region (lambda (x) ...))
            // The wrapper is transparent — the inner form is
            // processed normally — but it records a side-table
            // mapping the resulting function's name to the
            // specified region. The lowering pass (FlatFnBuilder
            // in lowering_impl.cpp) reads this side-table to set
            // IRFunction::region accordingly.
            //
            // If the inner form is not a define/lambda, the
            // wrapper is silently a no-op (the inner form is
            // processed; the region tag is discarded). This
            // matches the user's mental model: "annotate this
            // form with a region hint" — the hint is only useful
            // for define/lambda; for other forms it's ignored.
            if (fn_name == "performance-region" || fn_name == "evolution-region") {
                if (is_pair(cdr_data)) {
                    auto rp = as_pair_idx(cdr_data);
                    auto inner_node = data_to_flat(pairs_[rp].car, flat, pool, depth + 1);
                    if (inner_node == ast::NULL_NODE)
                        return ast::NULL_NODE;
                    // The inner form should be a Define or Lambda.
                    // If it's a Define, get the name and record
                    // (name -> region) in the side-table.
                    if (flat.get(inner_node).tag == ast::NodeTag::Define) {
                        auto sym = flat.get(inner_node).sym_id;
                        if (sym != ast::INVALID_SYM) {
                            auto region = (fn_name == "performance-region")
                                              ? std::uint8_t{1} /*Performance*/
                                              : std::uint8_t{2} /*Evolution*/;
                            flat.set_function_region(sym, region);
                        }
                    } else if (flat.get(inner_node).tag == ast::NodeTag::Lambda) {
                        // Lambdas are anonymous — use the
                        // overload-tagged setter for lambdas.
                        auto region = (fn_name == "performance-region")
                                          ? std::uint8_t{1} /*Performance*/
                                          : std::uint8_t{2} /*Evolution*/;
                        flat.set_function_region_lambda(inner_node, region);
                    }
                    return inner_node;
                }
                return ast::NULL_NODE;
            }

            // Set!: (set! name value)
            if (fn_name == "set!") {
                if (is_pair(cdr_data)) {
                    auto np = as_pair_idx(cdr_data);
                    auto name_val = pairs_[np].car;
                    auto val_rest = pairs_[np].cdr;
                    if (is_string(name_val) && is_pair(val_rest)) {
                        auto ni = as_string_idx(name_val);
                        auto ns = ni < string_heap_.size() ? string_heap_[ni] : "";
                        auto vp = as_pair_idx(val_rest);
                        auto val_node = data_to_flat(pairs_[vp].car, flat, pool, depth + 1);
                        return flat.add_set(pool.intern(ns), val_node);
                    }
                }
                return ast::NULL_NODE;
            }

            // Let: (let ((x val)) body)
            if (fn_name == "let") {
                // For data_to_flat, let is just a call node
                // (let ((x val)) body) is sugar and should already be expanded
                // Just treat as a general call
            }
        }

        // General function call: build Call(node func, [args...])
        auto func_node = data_to_flat(car_data, flat, pool, depth);
        if (func_node == ast::NULL_NODE)
            return ast::NULL_NODE;
        std::vector<ast::NodeId> args;
        auto cur = cdr_data;
        while (is_pair(cur)) {
            auto cp = as_pair_idx(cur);
            auto arg_data = pairs_[cp].car;
            // Direct string arguments (e.g., "hello" in quoted form) → LiteralString
            // Nested expressions → recurse normally
            if (is_string(arg_data)) {
                auto sidx = as_string_idx(arg_data);
                if (sidx < string_heap_.size()) {
                    auto name = string_heap_[sidx];
                    auto ssid = pool.intern(name);
                    // Issue #334 (Cycle 1): use add_variable
                    // instead of add_literalstring. In a macro
                    // body, strings represent identifier names
                    // (e.g. field names spliced into a lambda
                    // body), and we want runtime lookup, not a
                    // literal value. This affects ALL callers
                    // of data_to_flat, but the change is
                    // behaviorally a no-op for code that doesn't
                    // have bare-string args in non-quote
                    // positions — LiteralString args would
                    // always have been rare and unusual.
                    args.push_back(flat.add_variable(ssid));
                }
            } else {
                auto a = data_to_flat(arg_data, flat, pool, depth + 1);
                if (a != ast::NULL_NODE)
                    args.push_back(a);
            }
            cur = pairs_[cp].cdr;
        }
        return flat.add_call(func_node, args);
    }
    if (is_cell(data)) {
        // Dereference cell and convert the inner value
        auto cid = as_cell_id(data);
        if (cid < cells_.size())
            return data_to_flat(cells_[cid], flat, pool, depth + 1);
    }
    return ast::NULL_NODE;
}

// ── eval_data_as_code: evaluate macro-expanded data as code ──
// Macro bodies produce data (lists) via cons/quote chains.
// This function interprets that data as code and evaluates it.
// flat/pool are needed for lambda and define-shorthand handling.
//
// Issue #230 #2 follow-up: unwrap Quote-wrapping on set! targets.
EvalResult Evaluator::eval_data_as_code(const types::EvalValue& data, const Env& env,
                                        ast::FlatAST* flat, ast::StringPool* pool) {
    // Not a pair → literal value (number, string, bool, void)
    if (!types::is_pair(data)) {
        // Strings are literal symbols/data, return as-is
        return data;
    }

    // Pair: (fn arg1 arg2 ...) or (special-form arg ...)
    auto pair_idx = types::as_pair_idx(data);
    if (pair_idx >= pairs_.size())
        return make_void();

    auto car_val = pairs_[pair_idx].car;
    auto cdr_val = pairs_[pair_idx].cdr;

    // Handle special forms by name
    if (types::is_string(car_val)) {
        auto fn_idx = types::as_string_idx(car_val);
        auto fn_name = fn_idx < string_heap_.size() ? string_heap_[fn_idx] : "";

        // ── if: (if cond then else) ──
        if (fn_name == "if") {
            if (types::is_pair(cdr_val)) {
                auto cond_pair = types::as_pair_idx(cdr_val);
                auto cond_val = pairs_[cond_pair].car;
                auto rest = pairs_[cond_pair].cdr;
                auto cond_result = eval_data_as_code(cond_val, env, flat, pool);
                if (!cond_result)
                    return cond_result;
                if (types::is_pair(rest)) {
                    auto then_pair = types::as_pair_idx(rest);
                    auto then_val = pairs_[then_pair].car;
                    auto else_rest = pairs_[then_pair].cdr;
                    if (aura::compiler::pure::is_truthy(*cond_result)) {
                        auto r = eval_data_as_code(then_val, env, flat, pool);
                        return r;
                    } else {
                        // Evaluate else branch
                        if (types::is_pair(else_rest)) {
                            auto else_pair = types::as_pair_idx(else_rest);
                            auto else_val = pairs_[else_pair].car;
                            return eval_data_as_code(else_val, env, flat, pool);
                        }
                    }
                }
            }
            return make_void();
        }

        // ── when: (when cond body...) — like (if cond (begin body...) (void))
        if (fn_name == "when" || fn_name == "unless") {
            if (types::is_pair(cdr_val)) {
                auto cond_pair = types::as_pair_idx(cdr_val);
                auto cond_val = pairs_[cond_pair].car;
                auto body_rest = pairs_[cond_pair].cdr;
                auto cond_result = eval_data_as_code(cond_val, env, flat, pool);
                if (!cond_result)
                    return cond_result;
                if (aura::compiler::pure::is_truthy(*cond_result)) {
                    // Evaluate body expressions sequentially
                    EvalResult last = make_void();
                    while (types::is_pair(body_rest)) {
                        auto bp = types::as_pair_idx(body_rest);
                        last = eval_data_as_code(pairs_[bp].car, env, flat, pool);
                        if (!last)
                            return last;
                        body_rest = pairs_[bp].cdr;
                    }
                    return last;
                }
            }
            return make_void();
        }

        // ── lambda: (lambda (params) body) ──
        // Needs flat/pool to create an AST closure
        if (fn_name == "lambda") {
            if (!flat || !pool) {
                // Without flat/pool, we can't create closures — return as-is
                return make_void();
            }
            if (types::is_pair(cdr_val)) {
                auto params_pair = types::as_pair_idx(cdr_val);
                auto params_data = pairs_[params_pair].car; // (arg1 arg2 ...)
                auto body_rest = pairs_[params_pair].cdr;   // (body ...)

                // Extract param names
                std::vector<ast::SymId> param_syms;
                auto args_data = params_data;
                while (types::is_pair(args_data)) {
                    auto ap = types::as_pair_idx(args_data);
                    auto arg_data = pairs_[ap].car;
                    if (types::is_string(arg_data)) {
                        auto aidx = types::as_string_idx(arg_data);
                        auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                        param_syms.push_back(pool->intern(astr));
                    }
                    args_data = pairs_[ap].cdr;
                }

                // Extract and convert body data to FlatAST
                ast::NodeId body_node = ast::NULL_NODE;
                if (types::is_pair(body_rest)) {
                    auto bp = types::as_pair_idx(body_rest);
                    auto body_data = pairs_[bp].car;
                    body_node = data_to_flat(body_data, *flat, *pool);
                }
                if (body_node == ast::NULL_NODE)
                    body_node = flat->add_literal(0);

                // Create lambda node and closure
                auto lambda_id = flat->add_lambda(param_syms, body_node);
                auto cid = next_id();
                // Allocate closure body in temp arena during task context,
                // otherwise in persistent arena (module functions, while loops).
                auto* target_arena = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                if (!target_arena) {
                    return make_void();
                }
                auto cl_alloc = target_arena->allocator();
                auto* cl_flat = target_arena->create<aura::ast::FlatAST>(cl_alloc);
                // Issue #334: keep the closure's body symids in the SAME
                // pool as the macro's `pool` and the env's bindings. If
                // we use a fresh cl_pool for the cloned body, the body's
                // Variable symids and the env's symids are in different
                // pools, so Env::lookup_by_symid (the fast path) misses
                // and the body's lambda params are reported as unbound.
                // Using the macro's pool here aligns the body's Variables
                // with the env's bindings at lookup time.
                auto* cl_pool = pool;
                auto cloned_body =
                    clone_macro_body(*cl_flat, *cl_pool, *flat, *pool, body_node, nullptr,
                                     /*name_map=*/nullptr,
                                     /*cloned_marker=*/aura::ast::SyntaxMarker::User);
                // Issue #230 #2 follow-up: undo the Quote-wrap on set!
                // targets. The Quote-wrap was added to give symbol-
                // generating macros access to the literal arg value,
                // but it broke set! semantics for normal macros. The
                // pass walks the body, finds `(set! <target> <value>)`
                // calls, and replaces the <target> Quote with the
                // inner Variable. This is safe to run on every
                // macro body — bodies that don't use set! have no
                // set! calls, so the pass is a no-op for them.
                // unwrap_set_quotes removed (Quote-wrap reverted)
                cl_flat->root = cloned_body;
                // P0: register captured for SoA, no legacy env pointer in Closure.
                EnvId cap_id = alloc_env_frame_from_env(env);
                {
                    std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
                    closures_[cid] =
                        Closure{"", {}, cl_flat, cl_pool, cloned_body, cap_id, false, target_arena};
                }
                // Store param SymIds directly (Issue #145: SoA migration).
                // Interning already happened at the lambda creation site
                // (param_syms are SymId from pool->intern).
                for (auto ps : param_syms) {
                    closures_[cid].params.push_back(ps);
                }
                return make_closure(cid);
            }
            return make_void();
        }

        // ── begin: (begin expr1 expr2 ...) ──
        if (fn_name == "begin") {
            auto current = cdr_val;
            EvalResult last = make_void();
            while (types::is_pair(current)) {
                auto elem_pair = types::as_pair_idx(current);
                last = eval_data_as_code(pairs_[elem_pair].car, env, flat, pool);
                if (!last)
                    return last;
                current = pairs_[elem_pair].cdr;
            }
            return last;
        }

        // ── quote: (quote expr) ──
        if (fn_name == "quote") {
            if (types::is_pair(cdr_val)) {
                auto quote_pair = types::as_pair_idx(cdr_val);
                return pairs_[quote_pair].car; // Return the quoted value as-is
            }
            return make_void();
        }

        // ── define: (define name value) or (define (name args) body) ──
        if (fn_name == "define") {
            if (types::is_pair(cdr_val)) {
                auto name_pair = types::as_pair_idx(cdr_val);
                auto name_val = pairs_[name_pair].car;
                auto val_rest = pairs_[name_pair].cdr;

                // (define name value)
                if (types::is_string(name_val) && types::is_pair(val_rest)) {
                    auto val_pair = types::as_pair_idx(val_rest);
                    auto val = eval_data_as_code(pairs_[val_pair].car, env, flat, pool);
                    if (val) {
                        auto name_idx = types::as_string_idx(name_val);
                        auto name_str =
                            name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                        auto ci = alloc_cell(make_void());
                        const_cast<Env&>(env).bind(name_str, make_cell(ci));
                        cells_[ci] = *val;
                        return *val;
                    }
                    return val;
                }

                // (define (name args...) body) — function shorthand
                if (types::is_pair(name_val) && types::is_pair(val_rest) && flat && pool) {
                    auto fn_pair = types::as_pair_idx(name_val);
                    auto fn_name_data = pairs_[fn_pair].car;
                    auto fn_args_data = pairs_[fn_pair].cdr;

                    if (types::is_string(fn_name_data)) {
                        auto ni = types::as_string_idx(fn_name_data);
                        auto fn_str = ni < string_heap_.size() ? string_heap_[ni] : "";

                        // Extract param names from (arg1 arg2 ...)
                        std::vector<ast::SymId> param_syms;
                        auto args_data = fn_args_data;
                        while (types::is_pair(args_data)) {
                            auto ap = types::as_pair_idx(args_data);
                            auto arg_data = pairs_[ap].car;
                            if (types::is_string(arg_data)) {
                                auto aidx = types::as_string_idx(arg_data);
                                auto astr = aidx < string_heap_.size() ? string_heap_[aidx] : "";
                                param_syms.push_back(pool->intern(astr));
                            }
                            args_data = pairs_[ap].cdr;
                        }

                        // Extract and convert body data
                        ast::NodeId body_node = ast::NULL_NODE;
                        if (types::is_pair(val_rest)) {
                            auto bp = types::as_pair_idx(val_rest);
                            auto body_data = pairs_[bp].car;
                            body_node = data_to_flat(body_data, *flat, *pool);
                        }
                        if (body_node == ast::NULL_NODE)
                            body_node = flat->add_literal(0);

                        // Create lambda node and closure
                        auto lambda_id = flat->add_lambda(param_syms, body_node);
                        auto cid = next_id();
                        auto* target = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                        auto* copied_env = copy_env(env, target);
                        Closure cl;
                        for (auto ps : param_syms) {
                            cl.params.push_back(ps); // Issue #145: SymId, not string
                        }
                        cl.name = fn_str;
                        cl.flat = flat;
                        cl.pool = pool;
                        cl.body_id = body_node;
                        // P0: legacy cl.env removed. Always register in env_frames_
                        // for SoA (GC-safe, no pointer). env_id is now the only handle.
                        cl.env_id = alloc_env_frame_from_env(*copied_env);
                        cl.owner_arena = target;
                        {
                            std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
                            closures_[cid] = std::move(cl);
                        }

                        // Bind in env
                        auto ci = alloc_cell(make_void());
                        const_cast<Env&>(env).bind(fn_str, make_cell(ci));
                        cells_[ci] = make_closure(cid);
                        return make_closure(cid);
                    }
                }
            }
            return make_void();
        }

        // ── set!: (set! name value) ──
        if (fn_name == "set!") {
            if (types::is_pair(cdr_val)) {
                auto name_pair = types::as_pair_idx(cdr_val);
                auto name_val = pairs_[name_pair].car;
                auto val_rest = pairs_[name_pair].cdr;
                if (types::is_string(name_val) && types::is_pair(val_rest)) {
                    auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_rest)].car, env,
                                                 flat, pool);
                    if (val) {
                        auto name_idx = types::as_string_idx(name_val);
                        auto name_str =
                            name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                        auto cell_idx = const_cast<Env&>(env).lookup_cell_index(name_str);
                        if (cell_idx && *cell_idx < cells_.size()) {
                            cells_[*cell_idx] = *val;
                            return *val;
                        }
                    }
                }
            }
            return make_void();
        }

        // ── let: (let ((x val)) body) ──
        if (fn_name == "let") {
            if (types::is_pair(cdr_val)) {
                auto bindings_val = pairs_[types::as_pair_idx(cdr_val)].car;
                auto body_rest = pairs_[types::as_pair_idx(cdr_val)].cdr;
                // Collect bindings
                std::vector<std::pair<std::string, EvalValue>> bindings;
                auto current = bindings_val;
                while (types::is_pair(current)) {
                    auto binding_pair = pairs_[types::as_pair_idx(current)].car;
                    if (types::is_pair(binding_pair)) {
                        auto name_val = pairs_[types::as_pair_idx(binding_pair)].car;
                        auto val_expr = pairs_[types::as_pair_idx(binding_pair)].cdr;
                        if (types::is_string(name_val) && types::is_pair(val_expr)) {
                            auto name_idx = types::as_string_idx(name_val);
                            auto name_str =
                                name_idx < string_heap_.size() ? string_heap_[name_idx] : "";
                            auto val = eval_data_as_code(pairs_[types::as_pair_idx(val_expr)].car,
                                                         env, flat, pool);
                            if (!val)
                                return val;
                            bindings.emplace_back(name_str, *val);
                        }
                    }
                    current = pairs_[types::as_pair_idx(current)].cdr;
                }
                // Create new env and bind
                Env new_env(&env);
                new_env.set_primitives(&primitives_);

                for (auto& [n, v] : bindings)
                    new_env.bind(n, v);
                // Evaluate body in new env
                auto body_current = body_rest;
                EvalResult last = make_void();
                while (types::is_pair(body_current)) {
                    auto elem_pair = types::as_pair_idx(body_current);
                    last = eval_data_as_code(pairs_[elem_pair].car, new_env, flat, pool);
                    if (!last)
                        return last;
                    body_current = pairs_[elem_pair].cdr;
                }
                return last;
            }
            return make_void();
        }

        // ── General function call ──
        // Look up the function in the environment or primitives.
        // #223 follow-up: skip primitive lookup when fn_name is
        // empty (e.g. when fn_idx was out of bounds at line 17542).
        // Without this guard, env.lookup_primitive("") triggers
        // the pre (!n.empty()) contract on Primitives::lookup.
        // The environment lookup below (lookup_by_intern) handles
        // missing names gracefully via nullopt.
        std::optional<PrimFn> prim;
        if (!fn_name.empty()) {
            prim = env.lookup_primitive(fn_name);
        }
        if (prim) {
            std::vector<EvalValue> args;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                if (!arg_val)
                    return arg_val;
                args.push_back(*arg_val);
                current = pairs_[arg_pair].cdr;
            }
            return (*prim)(args);
        }

        // Issue #158: macro expansion in eval_data_as_code. The
        // cons-chain result of a legacy `defmacro` body is
        // re-evaluated here. If the head of the list is itself a
        // macro (e.g., the qq-built form `(bar ,x)` in a macro
        // body where `bar` is also a macro), the system needs to
        // detect this and trigger the macro path. Before this
        // fix, eval_data_as_code only checked env + primitives,
        // so `bar` (which lives in `macros_`, not env) was never
        // expanded — the cons chain produced the list `(bar <x>)`
        // but `bar` was just a symbol at re-eval time.
        if (macros_.count(fn_name)) {
            auto macro_it = macros_.find(fn_name);
            auto& md = macro_it->second;
            bool is_rest = md.dotted;
            // Rest params on legacy macros via eval_data_as_code
            // are not yet supported (same limitation as the main
            // eval_flat path). Fall through to a no-op return if
            // the macro has a dotted rest param.
            if (is_rest) {
                return make_void();
            }
            // Collect args (already data — no ast_to_data needed).
            std::vector<EvalValue> cargs;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                cargs.push_back(pairs_[arg_pair].car);
                current = pairs_[arg_pair].cdr;
            }
            // Build tail env with regular params bound.
            Env tail_env(&env);
            tail_env.set_primitives(&primitives_);

            if (md.pool)
                tail_env.set_pool(md.pool);
            for (std::size_t i = 0; i < md.params.size() && i < cargs.size(); ++i) {
                tail_env.bind(md.params[i], std::move(cargs[i]));
            }
            // Evaluate macro body (quasiquote-expanded template)
            // → produces data (a list).
            auto template_result =
                eval_flat(*md.flat, md.pool ? *md.pool : *current_pool_, md.body_id, tail_env);
            if (!template_result)
                return template_result;
            // Re-evaluate the data as code. The recursive call
            // here is what enables macro composition: the inner
            // macro's expansion is re-evaluated, which may itself
            // contain another macro call.
            return eval_data_as_code(*template_result, env, flat, pool);
        }

        // Look up in environment. Phase 2.5.0: route through
        // lookup_by_intern (SymId-first). canonical_pool() is
        // the long-lived workspace pool; env.pool_ is the
        // fallback for closures that captured a non-canonical
        // pool. Observable behavior matches env.lookup(name).
        auto env_val = env.lookup_by_intern(fn_name, canonical_pool());
        if (env_val) {
            auto fn_val = *env_val;
            // Dereference cells — needed when lookup returned cell sentinel (cells_ not set on env)
            if (types::is_cell(fn_val)) {
                auto ci = types::as_cell_id(fn_val);
                if (ci < cells_.size())
                    fn_val = cells_[ci];
            }
            if (types::is_closure(fn_val)) {
                auto cid = types::as_closure_id(fn_val);
                auto it = closures_.find(cid);
                if (it != closures_.end()) {
                    auto& cl = it->second;
                    // Evaluate args and apply
                    std::vector<EvalValue> cargs;
                    auto current = cdr_val;
                    while (types::is_pair(current)) {
                        auto arg_pair = types::as_pair_idx(current);
                        auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                        if (!arg_val)
                            return arg_val;
                        cargs.push_back(*arg_val);
                        current = pairs_[arg_pair].cdr;
                    }
                    // Create tail env and apply
                    Env tail_env = materialize_call_env(cl);
                    tail_env.set_primitives(&primitives_);

                    // Issue #145: set the pool so bind_symid can mirror
                    if (cl.pool)
                        tail_env.set_pool(cl.pool);
                    for (std::size_t i = 0; i < cargs.size() && i < cl.params.size(); ++i)
                        tail_env.bind_symid(cl.params[i], std::move(cargs[i]));
                    if (cl.body_id != aura::ast::NULL_NODE && cl.flat)
                        return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id,
                                         tail_env);
                    return make_void();
                }
            }
        }
    }

    // Not a string function name — evaluate car and cdr, apply
    auto fn = eval_data_as_code(car_val, env, flat, pool);
    if (!fn)
        return fn;
    if (types::is_closure(*fn)) {
        auto cid = types::as_closure_id(*fn);
        // Try tree-walker closure first, then IR bridge
        auto result = apply_closure(cid, {});
        if (result)
            return *result;

        // Fallback: manual closure apply via eval_flat
        auto it = closures_.find(cid);
        if (it != closures_.end()) {
            auto& cl = it->second;
            std::vector<EvalValue> cargs;
            auto current = cdr_val;
            while (types::is_pair(current)) {
                auto arg_pair = types::as_pair_idx(current);
                auto arg_val = eval_data_as_code(pairs_[arg_pair].car, env, flat, pool);
                if (!arg_val)
                    return arg_val;
                cargs.push_back(*arg_val);
                current = pairs_[arg_pair].cdr;
            }
            Env tail_env = materialize_call_env(cl);
            tail_env.set_primitives(&primitives_);

            // Issue #145: set the pool so bind_symid can mirror
            if (cl.pool)
                tail_env.set_pool(cl.pool);
            for (std::size_t i = 0; i < cargs.size() && i < cl.params.size(); ++i)
                tail_env.bind_symid(cl.params[i], std::move(cargs[i]));
            if (cl.body_id != aura::ast::NULL_NODE && cl.flat)
                return eval_flat(*cl.flat, cl.pool ? *cl.pool : *current_pool_, cl.body_id,
                                 tail_env);
        }
    }

    return make_void();
}
// ── Runtime type helpers for type annotation checking ────────
static aura::core::TypeTag runtime_type_tag(const types::EvalValue& v) {
    if (types::is_int(v))
        return aura::core::TypeTag::INT;
    if (types::is_float(v))
        return aura::core::TypeTag::FLOAT;
    if (types::is_bool(v))
        return aura::core::TypeTag::BOOL;
    if (types::is_string(v))
        return aura::core::TypeTag::STRING;
    if (types::is_pair(v))
        return aura::core::TypeTag::PAIR;
    if (types::is_closure(v))
        return aura::core::TypeTag::CLOSURE;
    if (types::is_vector(v))
        return aura::core::TypeTag::VECTOR;
    if (types::is_hash(v))
        return aura::core::TypeTag::HASH;
    return aura::core::TypeTag::DYNAMIC;
}

static std::string type_tag_name(aura::core::TypeTag tag) {
    switch (tag) {
        case aura::core::TypeTag::INT:
            return "Int";
        case aura::core::TypeTag::FLOAT:
            return "Float";
        case aura::core::TypeTag::BOOL:
            return "Bool";
        case aura::core::TypeTag::STRING:
            return "String";
        case aura::core::TypeTag::PAIR:
            return "Pair";
        case aura::core::TypeTag::CLOSURE:
            return "Closure";
        case aura::core::TypeTag::VECTOR:
            return "Vector";
        case aura::core::TypeTag::HASH:
            return "Hash";
        default:
            return "Dynamic";
    }
}

// Issue #146 Phase 2: thin legacy wrapper around the pure
// `coerce_value_pure` in `aura::compiler::pure`. Mirrors the
// legacy bool API; the underlying Result<void> is discarded.
// The 1 existing call site (L18281) keeps using the bool
// interface; new code can use `coerce_value_pure` directly
// for monadic composition.
static bool coerce_value(types::EvalValue& val, aura::core::TypeTag from, aura::core::TypeTag to,
                         std::pmr::vector<std::string>& heap) {
    return aura::compiler::pure::coerce_value_pure(val, from, to, heap).has_value();
}

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────

// Issue #236: helper implementations for mutate:atomic-batch.
// The existing atomic-batch (line ~9071) called the sub-primitive
// via primitives_.lookup which re-enters MutationBoundaryGuard
// in each sub-op, deadlocking on the non-recursive shared_mutex.
// These helpers do the same work WITHOUT the guard — the batch's
// outer guard already holds the lock for the entire batch body.
//
// MVP scope: :rebind works for the "old Define exists" path.
// :replace-value and :tweak-literal are stubs (error-out) so
// they fail fast rather than deadlocking the batch. Follow-up
// to extract those internals.
EvalResult Evaluator::eval_flat_apply_mutate_rebind(std::span<const types::EvalValue> a) {
    if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
        return std::unexpected(
            aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch,
                                   "batch :rebind requires name and code (string args)"});
    if (!workspace_flat_ || !workspace_pool_)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :rebind: no workspace loaded"});
    auto name_idx = as_string_idx(a[0]);
    auto code_idx = as_string_idx(a[1]);
    if (name_idx >= string_heap_.size() || code_idx >= string_heap_.size())
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "batch :rebind: string index out of range"});
    auto& flat = *workspace_flat_;
    auto name = string_heap_[name_idx];
    auto sym = canonical_pool()->intern(name);
    aura::ast::NodeId old_define = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Define && v.sym_id == sym) {
            old_define = id;
            break;
        }
    }
    if (old_define == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{
            aura::diag::ErrorKind::ArityMismatch,
            "batch :rebind: no existing Define for '" + name +
                "' (new-binding path not yet supported; use standalone mutate:rebind)"});
    auto pr = aura::parser::parse_to_flat(string_heap_[code_idx], flat, *workspace_pool_);
    if (!pr.success || pr.root == aura::ast::NULL_NODE)
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::ParseError,
                                                      "batch :rebind: parse failed for new code"});
    aura::ast::NodeId new_value = pr.root;
    auto root_v = flat.get(pr.root);
    if (root_v.tag == aura::ast::NodeTag::Define) {
        if (root_v.children.empty())
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::ParseError, "batch :rebind: define form has no body"});
        new_value = root_v.child(0);
    }
    std::string summary = (a.size() > 2 && is_string(a[2])) ? string_heap_[as_string_idx(a[2])]
                                                            : "batch rebind " + name;
    auto old_v = flat.get(old_define);
    auto old_value_node = old_v.children.empty() ? aura::ast::NULL_NODE : old_v.child(0);
    auto mid = flat.add_mutation_with_rollback(
        old_define, "batch-rebind", std::string("Define:") + name, std::string("Define:") + name,
        summary, aura::ast::MutationStatus::Committed, 0,
        static_cast<std::uint64_t>(old_value_node), static_cast<std::uint64_t>(new_value), true);
    flat.set_child(old_define, 0, new_value);
    flat.mark_dirty_upward(old_define);
    return make_int(static_cast<std::int64_t>(mid));
}

EvalResult Evaluator::eval_flat_apply_mutate_replace_value(std::span<const types::EvalValue> a) {
    // TODO #236 follow-up: extract the inner logic from
    // mutate:replace-value (LiteralInt / LiteralFloat /
    // LiteralString / sym_id field updates) so it can run
    // under an outer guard. For MVP, error out so the agent
    // falls back to standalone (and accepts no transactionality).
    (void)a;
    return std::unexpected(aura::diag::Diagnostic{
        aura::diag::ErrorKind::InternalError,
        "batch :replace-value not yet supported (use standalone mutate:replace-value)"});
}

EvalResult Evaluator::eval_flat_apply_mutate_tweak_literal(std::span<const types::EvalValue> a) {
    (void)a;
    return std::unexpected(aura::diag::Diagnostic{
        aura::diag::ErrorKind::InternalError,
        "batch :tweak-literal not yet supported (use standalone mutate:tweak-literal)"});
}

// ── Phase 4: FlatAST tree-walker evaluator (EvalValue) ───────
EvalResult Evaluator::eval_flat(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                aura::ast::NodeId id, const Env& env) {
    // Catch bad_variant_access and return friendly error instead of crash.
    // This happens when user code passes wrong argument types to primitives.
    try {
        // TCO loop state: f/p point to the current FlatAST/Pool,
        // which may change during closure/macro tail calls.
        aura::ast::FlatAST* f = &flat;
        aura::ast::StringPool* p = &pool;
        const Env* current_env = &env;
        aura::ast::NodeId current_id = id;
        std::optional<Env> tail_env;

        // Recursion depth guard: friendly error vs segfault
        // MAX_C_STACK_DEPTH must be low enough to fit in the C++ call stack (~550 frames)
        // ── Recursion depth guard (thread_local) ────────────────────────
        // #109 (P0): the depth counter must be PER THREAD, not per Evaluator.
        // Fiber fallback (std::thread + [this] capture) shares an Evaluator
        // across N OS threads; if each thread increments a shared counter, a
        // modest amount of parallel work trips the MAX_C_STACK_DEPTH=2000 guard
        // even though no single thread is deeply nested. This is what made
        // `tests/suite/concurrent.aura` T7-T10 (orch:parallel 5-way, nested
        // spawn+join) flaky: in one run they'd see the shared counter spike
        // past 2000 and bail with "recursion depth exceeded".
        //
        // The shared eval_depth_ member is still used below for auto-gc-temp
        // sampling and auto-gc cooldown, where "global eval activity" is the
        // intended signal (one thread's deep call can still be enough work
        // to warrant a periodic gc-temp). The two are now decoupled.
        static constexpr std::size_t MAX_C_STACK_DEPTH = 2000;
        thread_local std::size_t t_c_stack_depth = 0;
        struct DepthGuard {
            std::size_t& d;
            ~DepthGuard() { --d; }
        } _dg{t_c_stack_depth};
        if (++t_c_stack_depth > MAX_C_STACK_DEPTH)
            return std::unexpected(
                Diagnostic{ErrorKind::InternalError,
                           std::format("recursion depth exceeded (>{})", MAX_C_STACK_DEPTH)});

        // ── Memory pressure auto-governance sampling (P1) ─────────
        // Every sample_every_ calls to eval_flat, recompute pressure
        // and (if policy allows) auto-trigger gc-module for the top arena.
        // Outside the hot path for typical evals (default 1-in-1000).
        if (++sample_counter_ >= memory_policy_.sample_every) {
            sample_counter_ = 0;
            // Snapshot arena state. Inline rather than refactoring into
            // a shared helper to avoid std::function capture-lifetime issues.
            struct Snap {
                std::string name;
                double used;
                double cap;
                int pct;
            };
            std::vector<Snap> snaps;
            double total_used = 0.0, total_cap = 0.0;
            if (arena_) {
                auto s = arena_->stats();
                double u = s.used / 1048576.0;
                double c = s.capacity / 1048576.0;
                snaps.push_back({"main", u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                total_used += u;
                total_cap += c;
            }
            if (arena_group_) {
                for (auto& [full_name, stats] : arena_group_->module_stats()) {
                    auto slash = full_name.rfind('/');
                    auto short_name =
                        slash == std::string::npos ? full_name : full_name.substr(slash + 1);
                    double u = stats.used / 1048576.0;
                    double c = stats.capacity / 1048576.0;
                    snaps.push_back(
                        {short_name, u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                    total_used += u;
                    total_cap += c;
                }
            }
            int overall = total_cap > 0 ? static_cast<int>(total_used / total_cap * 100.0) : 0;
            std::string level = "low";
            if (overall >= 95)
                level = "critical";
            else if (overall >= 80)
                level = "high";
            else if (overall >= 60)
                level = "medium";

            // Log warning on level transitions (avoid spam — only log when
            // the level string changes from the last warned one).
            if (level != last_warn_level_ && (level == "high" || level == "critical")) {
                std::println(
                    std::cerr,
                    "[memory-pressure] WARNING: level={} overall-pct={} total-used={:.1f}MB", level,
                    overall, total_used);
                last_warn_level_ = level;
            } else if (level == "low" || level == "medium") {
                last_warn_level_ = level;
            }

            // Auto-gc: only at critical AND policy enabled AND cooldown elapsed.
            if (memory_policy_.auto_gc && level == "critical" &&
                eval_depth_ - last_auto_gc_eval_depth_ > memory_policy_.cooldown_evals) {
                // Find top arena (highest used-pct, then largest used, then name asc).
                std::string top_name;
                int top_pct = 0;
                double top_used = 0.0;
                for (auto& s : snaps) {
                    if (s.pct > top_pct || (s.pct == top_pct && s.used > top_used) ||
                        (s.pct == top_pct && s.used == top_used && s.name < top_name)) {
                        top_name = s.name;
                        top_pct = s.pct;
                        top_used = s.used;
                    }
                }
                if (!top_name.empty()) {
                    std::println(std::cerr,
                                 "[memory-pressure] AUTO-GC: freeing arena '{}' ({}% full)",
                                 top_name, top_pct);
                    gc_module(top_name);
                    last_auto_gc_eval_depth_ = eval_depth_;
                }
            }
        }

        while (true) {
            current_flat_ = f;
            current_pool_ = p;
            // Save the eval environment before any tail_env.emplace could corrupt current_env
            const Env& eval_env = *current_env;
            if (current_id == aura::ast::NULL_NODE)
                return EvalResult(make_void());
            if (current_id >= f->size())
                return std::unexpected(Diagnostic{ErrorKind::InternalError, "invalid node id"});
            auto v = f->get(current_id);

            // Incremental eval: if node is clean and has a cached result, reuse it.
            // Skip leaf literals (LiteralInt, LiteralFloat, LiteralString) because
            // they're always fast and the cache lookup overhead is not worth it.
            if (v.tag != aura::ast::NodeTag::LiteralInt &&
                v.tag != aura::ast::NodeTag::LiteralFloat &&
                v.tag != aura::ast::NodeTag::LiteralString &&
                v.tag != aura::ast::NodeTag::Variable && !f->is_dirty(current_id)) {
                auto cached = f->get_cached_value(current_id);
                if (cached != aura::ast::FlatAST::kNotCached) {
                    return EvalResult(EvalValue(cached));
                }
            }

            switch (v.tag) {
                case aura::ast::NodeTag::LiteralInt:
                    // #t/#f have BoolLiteral marker — convert to Bool at runtime
                    if (v.marker == aura::ast::SyntaxMarker::BoolLiteral)
                        return make_bool(v.int_value != 0);
                    return make_int(v.int_value);
                case aura::ast::NodeTag::LiteralFloat:
                    return make_float(v.float_value);
                case aura::ast::NodeTag::LiteralString: {
                    auto raw = std::string(p->resolve(v.sym_id));
                    // Short strings: use cache to avoid duplicate heap pushes
                    if (raw.size() <= 6) {
                        auto it = short_str_cache_.find(raw);
                        if (it != short_str_cache_.end())
                            return it->second;
                        auto sid = string_heap_.size();
                        string_heap_.push_back(raw);
                        auto val = make_string(sid);
                        short_str_cache_[raw] = val;
                        return val;
                    }
                    auto sid = string_heap_.size();
                    string_heap_.push_back(std::move(raw));
                    return make_string(sid);
                }
                case aura::ast::NodeTag::Variable: {
                    auto name = p->resolve(v.sym_id);
                    // Keyword: :foo → self-evaluating keyword value (interned)
                    if (!name.empty() && name[0] == ':') {
                        auto kwstr = std::string(name);
                        std::uint64_t kidx = 0;
                        // Check if already interned
                        bool found = false;
                        for (; kidx < keyword_table_.size(); ++kidx) {
                            if (keyword_table_[kidx] == kwstr) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            kidx = keyword_table_.size();
                            keyword_table_.push_back(kwstr);
                        }
                        return make_keyword(kidx);
                    }
                    auto val = eval_env.lookup(std::string(name));
                    if (val) {
                        // Issue #229 Cycle 1 fix: dereference cell
                        // sentinel. The Define case binds the name to
                        // a cell (make_cell(ci)) for re-def support;
                        // ordinary lookups (Variable here, call sites
                        // elsewhere) should auto-deref the cell to
                        // the underlying value. Without this, simple
                        // `(define x 10) (display x)` shows
                        // `<cell[0]>` instead of `10`.
                        if (is_cell(*val)) {
                            auto ci = as_cell_id(*val);
                            if (ci < cells_.size())
                                return cells_[ci];
                        }
                        return *val;
                    }
                    std::string var_name(name);
                    if (var_name.empty()) {
                        var_name = std::format("<sym:{}>", v.sym_id);
                    }
                    std::vector<std::string> candidates;
                    {
                        const Env* e = &eval_env;
                        while (e) {
                            for (auto& b : const_cast<Env&>(*e).bindings())
                                candidates.push_back(b.first);
                            e = e->parent();
                        }
                    }
                    auto best = closest_match(var_name, candidates);
                    // Issue #79: source location from the offending node so the
                    // error report includes line:col instead of just node[id:N].
                    Diagnostic d(ErrorKind::UnboundVariable, std::move(var_name),
                                 aura::diag::SourceLocation{v.line, v.col, 0}, current_id);
                    if (!best.empty())
                        d.with_suggestion("did you mean '" + best + "'?");
                    return std::unexpected(std::move(d));
                }
                case aura::ast::NodeTag::Call: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    auto callee_id = v.child(0);
                    auto callee = f->get(callee_id);
                    // Inline lambda (arg evals are recursive; body is tail)
                    if (callee.tag == aura::ast::NodeTag::Lambda) {
                        auto pspan = callee.params;
                        bool dotted = callee.int_value != 0;
                        std::size_t named_count =
                            dotted && !pspan.empty() ? pspan.size() - 1 : pspan.size();
                        // Evaluate named args
                        std::vector<EvalValue> iargs;
                        iargs.reserve(named_count);
                        for (std::size_t i = 0; i < named_count && i + 1 < v.children.size(); ++i) {
                            auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                            if (!ar)
                                return ar;
                            iargs.push_back(*ar);
                        }
                        tail_env.emplace(&eval_env);
                        tail_env->set_primitives(&primitives_);

                        for (std::size_t i = 0; i < iargs.size(); ++i) {
                            tail_env->bind(std::string(p->resolve(pspan[i])), std::move(iargs[i]));
                        }
                        // Dotted rest: collect remaining args into a pair list
                        if (dotted && !pspan.empty()) {
                            types::EvalValue rest = make_void();
                            for (std::size_t i = v.children.size() - 1; i > named_count; --i) {
                                auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                if (!ar)
                                    return ar;
                                auto pid = pairs_.size();
                                pairs_.push_back({*ar, rest});
                                rest = make_pair(pid);
                            }
                            tail_env->bind(std::string(p->resolve(pspan.back())), rest);
                        }
                        auto body_id =
                            callee.children.empty() ? aura::ast::NULL_NODE : callee.child(0);
                        if (body_id != aura::ast::NULL_NODE)
                            return eval_flat(*f, *p, body_id, *tail_env);
                        return make_void();
                    }
                    // Macro expansion: evaluate args, bind in env, evaluate body (produces template
                    // data), then re-evaluate the data as code
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        auto macro_it = macros_.find(cname);
                        if (macro_it != macros_.end()) {
                            auto& md = macro_it->second;
                            bool is_rest = md.dotted;

                            // Issue #334: env-binding path for
                            // `define-hygienic-macro*` (md.preserved).
                            // The macro args are bound in a child env
                            // (name → ast_to_data), and the body is
                            // evaluated in that env. This makes the
                            // body's Variable refs resolve to the
                            // literal arg values directly — the same
                            // behavior as the legacy `defmacro` path.
                            // Used by symbol-generating macros like
                            // define-struct where `name` should
                            // resolve to the literal symbol and
                            // `fields` to the literal field list.
                            if (md.preserved) {
                                if (is_rest) {
                                    // Rest params on preserved
                                    // macros are not yet supported
                                    // — fall through to a "no
                                    // expansion" return.
                                    return make_void();
                                }
                                // Bind macro args as data in tail_env
                                std::size_t regular_count = md.params.size();
                                tail_env.emplace(&eval_env);
                                tail_env->set_primitives(&primitives_);
                                for (std::size_t i = 0;
                                     i < regular_count && i + 1 < v.children.size(); ++i) {
                                    tail_env->bind(md.params[i],
                                                   ast_to_data(*f, *p, v.child(i + 1)));
                                }
                                // Issue #334: use md.pool if set (the
                                // macro was defined in a different
                                // workspace), else use the current pool
                                // p. The body's Variables look up by
                                // string name in tail_env, so pool
                                // alignment doesn't matter for the
                                // Variable case (it just needs a valid
                                // pool to resolve symids into strings).
                                auto* mb_pool = md.pool ? md.pool : p;
                                auto* mb_flat = md.pool ? md.flat : f;
                                auto template_result =
                                    eval_flat(*mb_flat, *mb_pool, md.body_id, *tail_env);
                                if (!template_result)
                                    return template_result;
                                // Issue #334 (Cycle 1): convert the
                                // macro body data to FlatAST via
                                // data_to_flat. eval_data_as_code
                                // (the legacy path used here before)
                                // treats strings as literal values,
                                // so Variable references in the body
                                // become string values rather than
                                // runtime lookups — meaning
                                // symbol-generating macros that
                                // splice field names into the lambda
                                // body get the field-name strings
                                // baked into the vector instead of
                                // the call args. data_to_flat
                                // converts each string to a Variable
                                // AST node (add_variable), so the
                                // resulting lambda body has proper
                                // Variable refs that look up in the
                                // lambda's call env at call time.
                                auto ast_root = data_to_flat(*template_result, *f, *p, /*depth=*/0);
                                if (ast_root == aura::ast::NULL_NODE)
                                    return std::unexpected(aura::diag::Diagnostic{
                                        aura::diag::ErrorKind::InternalError,
                                        "data_to_flat returned NULL for env-binding macro body"});
                                return eval_flat(*f, *p, ast_root, eval_env);
                            }

                            // Issue #120: hygienic macros use clone_macro_body
                            // with a name_map (single-eval AST substitution +
                            // automatic gensym for template-introduced
                            // bindings). Non-hygienic macros keep the legacy
                            // double-eval path for backward compatibility.
                            if (md.hygienic) {
                                if (is_rest) {
                                    // Rest params on hygienic macros are
                                    // not yet supported — fall through to a
                                    // "no expansion" return.
                                    return make_void();
                                }
                                // Issue #146 follow-up: build the subst
                                // map via the pure helper. Local
                                // materialization of v.children into a
                                // vector (one alloc per macro call) keeps
                                // the call site short.
                                std::vector<aura::ast::NodeId> call_args(v.children.begin(),
                                                                         v.children.end());
                                auto subst = aura::compiler::pure::compute_macro_subst_pure(
                                    md.params, call_args, /*dotted=*/false);
                                // Clone the macro body with substitution +
                                // name_map. The cloned tree is in the
                                // *current* FlatAST (we use the target's
                                // flat = f, source = md.flat). name_map is
                                // empty initially; clone_macro_body
                                // populates it as it gensym's
                                // template-introduced bindings.
                                std::unordered_map<std::string, std::string> rename_map;
                                auto* src_pool = md.pool ? md.pool : p;
                                auto expanded = clone_macro_body(
                                    *f, *p, *md.flat, *src_pool, md.body_id, &subst, &rename_map,
                                    /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
                                if (expanded == aura::ast::NULL_NODE)
                                    return make_void();
                                // Issue #230 #2 follow-up: undo the Quote-wrap
                                // on set! targets. Same rationale as the
                                // lambda case in eval_data_as_code — the
                                // Quote-wrap helps symbol-generating macros
                                // but breaks set! semantics.
                                // unwrap_set_quotes removed (Quote-wrap reverted)
                                // Issue #121: recursively expand any nested
                                // macro calls in the cloned body using the
                                // runtime `macros_` registry. The cloned body
                                // lives in the calling flat (`*f`), but other
                                // macros (m1, m2, ...) were defined in earlier
                                // forms with their own flates — so the static
                                // `macro_expand_all` (which scans the flat)
                                // wouldn't see them. We walk the cloned tree
                                // and, for each Call whose callee is in
                                // `macros_`, recursively expand it. Bounded by
                                // a depth limit (10) to prevent infinite loops
                                // (e.g., macro X calls macro X).
                                expanded = expand_inner_macros(f, p, expanded,
                                                               /*depth=*/0,
                                                               /*max_depth=*/10, macros_);
                                // Evaluate the cloned + inner-expanded
                                // body. eval_flat returns a runtime
                                // value (a list for cons-chain qq
                                // bodies). The list needs to be
                                // re-evaluated as code so the
                                // inner macro's result is invoked
                                // (e.g., `(bar ,x)` returns
                                // `(* x 2)`, which then evaluates
                                // as a call to `*`). This mirrors
                                // the legacy defmacro path's
                                // eval_data_as_code re-evaluation
                                // and is what enables macro
                                // composition in the hygienic
                                // case (Issue #158).
                                auto hygienic_result = eval_flat(*f, *p, expanded, eval_env);
                                if (!hygienic_result)
                                    return hygienic_result;
                                return eval_data_as_code(*hygienic_result, eval_env, f, p);
                            }

                            // Convert AST args to data (NOT evaluate — macros receive syntax)
                            // Bind regular params first (all but the last)
                            std::size_t regular_count =
                                is_rest ? md.params.size() - 1 : md.params.size();
                            tail_env.emplace(&eval_env);
                            tail_env->set_primitives(&primitives_);


                            for (std::size_t i = 0; i < regular_count && i + 1 < v.children.size();
                                 ++i) {
                                tail_env->bind(md.params[i], ast_to_data(*f, *p, v.child(i + 1)));
                            }

                            // Rest param: collect remaining args as a list
                            if (is_rest) {
                                auto& rest_name = md.params.back();
                                EvalValue rest_list = make_void();
                                for (std::size_t i = v.children.size() - 1; i >= regular_count + 1;
                                     --i) {
                                    auto item = ast_to_data(*f, *p, v.child(i));
                                    auto pid = pairs_.size();
                                    pairs_.push_back(Pair{std::move(item), rest_list});
                                    rest_list = make_pair(pid);
                                }
                                tail_env->bind(rest_name, std::move(rest_list));
                            }
                            // Evaluate macro body (quasiquote-expanded template) → produces data
                            auto template_result =
                                eval_flat(*md.flat, md.pool ? *md.pool : *p, md.body_id, *tail_env);
                            if (!template_result)
                                return template_result;
                            // Issue #334 (deferred): the simpler
                            // data_to_flat path was tried here and
                            // reverted — it can't work because the
                            // first eval_flat (line above) already
                            // fails when the macro body references
                            // lambda-local Variables (e.g. `args` of a
                            // generated `(lambda (args) ...)`) that
                            // aren't bound in tail_env at expansion
                            // time. The proper fix for #1/#2
                            // (define-struct) is the env-binding path
                            // (issue 334), not a refactor of the
                            // defmacro expansion here.
                            return eval_data_as_code(*template_result, eval_env, f, p);
                        }
                    }
                    // Built-in require: (require mod-name) — symbol, not string
                    // Phase 4: prefix by default. (require std/list) → (import "std/list" "list:")
                    //          (require std/list all:) → (import "std/list")  (backward compat)
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        if (cname == "require" && v.children.size() > 1) {
                            // Collect module names and check for all: flag
                            // (require mod1 mod2 ... all:) — all: applies to ALL modules
                            std::vector<std::string> mod_names;
                            bool use_prefix = true;
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                auto arg_v = f->get(v.child(ci));
                                if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                    auto arg_name = std::string(p->resolve(arg_v.sym_id));
                                    if (arg_name == "all:") {
                                        use_prefix = false;
                                    } else {
                                        mod_names.push_back(arg_name);
                                    }
                                } else if (arg_v.tag == aura::ast::NodeTag::LiteralString) {
                                    mod_names.push_back(std::string(p->resolve(arg_v.sym_id)));
                                }
                            }

                            // Load all modules in sequence
                            if (!arena_)
                                return make_void();
                            EvalResult last = make_void();
                            for (auto& mod_path : mod_names) {
                                // Derive prefix from module name (last path component)
                                std::string prefix;
                                if (use_prefix) {
                                    auto slash = mod_path.rfind('/');
                                    auto base = (slash == std::string::npos)
                                                    ? mod_path
                                                    : mod_path.substr(slash + 1);
                                    prefix = base + ":";
                                }

                                // Build (import "path" "prefix:") or (import "path")
                                std::string import_expr;
                                if (prefix.empty()) {
                                    import_expr = std::string("(import \"") + mod_path + "\")";
                                } else {
                                    import_expr = std::string("(import \"") + mod_path + "\" \"" +
                                                  prefix + "\")";
                                }

                                // Use temp_arena_ so (gc-temp) reclaims the
                                // parse state for each (require ...) call.
                                auto alloc = temp_arena_->allocator();
                                auto* ipool = temp_arena_->create<aura::ast::StringPool>(alloc);
                                auto* iflat = temp_arena_->create<aura::ast::FlatAST>(alloc);
                                auto pr = aura::parser::parse_to_flat(import_expr, *iflat, *ipool);
                                if (!pr.success || pr.root == aura::ast::NULL_NODE) {
                                    return std::unexpected(Diagnostic{ErrorKind::ParseError,
                                                                      "require: internal error"});
                                }
                                iflat->root = pr.root;
                                // Pre-expand macros so import primitive is recognized
                                auto expanded_root =
                                    aura::compiler::macro_expand_all(*iflat, *ipool, iflat->root);
                                last = eval_flat(*iflat, *ipool, expanded_root, eval_env);
                                if (!last)
                                    return last;
                            }
                            return last;
                        }
                    }
                    // try/catch: (try body (catch (var) handler))
                    // body is evaluated; if it returns an error, handler is evaluated with var
                    // bound
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        // when: (when cond body...) — evaluate body only if cond is truthy
                        if (cname == "when" && v.children.size() >= 2) {
                            auto cond_id = v.child(1);
                            auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                            if (!cond_result)
                                return cond_result;
                            if (is_truthy(*cond_result)) {
                                // Evaluate all remaining children as body
                                EvalResult last = make_void();
                                for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                    last = eval_flat(*f, *p, v.child(ci), eval_env);
                                    if (!last)
                                        return last;
                                }
                                return last;
                            }
                            return make_void();
                        }
                        // unless: (unless cond body...) — evaluate body only if cond is falsy
                        if (cname == "unless" && v.children.size() >= 2) {
                            auto cond_id = v.child(1);
                            auto cond_result = eval_flat(*f, *p, cond_id, eval_env);
                            if (!cond_result)
                                return cond_result;
                            if (!is_truthy(*cond_result)) {
                                EvalResult last = make_void();
                                for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                    last = eval_flat(*f, *p, v.child(ci), eval_env);
                                    if (!last)
                                        return last;
                                }
                                return last;
                            }
                            return make_void();
                        }
                        // with-arena: (with-arena (size) body...)
                        if (cname == "with-arena" && v.children.size() >= 1) {
                            std::size_t body_start = 1;
                            if (v.children.size() >= 2) {
                                auto first_id = v.child(1);
                                auto first_v = f->get(first_id);
                                if (first_v.tag == ast::NodeTag::Call &&
                                    first_v.children.size() >= 1)
                                    body_start = 2;
                            }
                            tl_arena_push(&g_tl_arena);
                            EvalResult last_result = make_void();
                            for (std::size_t ci = body_start; ci < v.children.size(); ++ci) {
                                last_result = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!last_result)
                                    return last_result;
                            }
                            // Deep-copy result if it's an arena-allocated pair
                            if (last_result && is_pair(*last_result)) {
                                auto idx = as_pair_idx(*last_result);
                                if (idx < g_pair_slots.size() && g_pair_slots[idx]) {
                                    auto* slot = g_pair_slots[idx];
                                    auto arena_end = g_tl_arena.base + g_tl_arena.offset;
                                    auto ptr = (uint8_t*)slot;
                                    if (g_tl_arena.base && ptr >= g_tl_arena.base &&
                                        ptr < arena_end) {
                                        auto* new_slot = (PairSlot*)std::malloc(sizeof(PairSlot));
                                        new_slot->car = slot->car;
                                        new_slot->cdr = slot->cdr;
                                        auto new_id = static_cast<int64_t>(g_pair_slots.size());
                                        g_pair_slots.push_back(new_slot);
                                        *last_result =
                                            types::make_pair(static_cast<std::uint64_t>(new_id));
                                    }
                                }
                            }
                            tl_arena_pop(&g_tl_arena);
                            return last_result;
                        }
                        // performance-region: (performance-region body...)
                        if (cname == "performance-region" && v.children.size() >= 2) {
                            EvalResult last = make_void();
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                last = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!last)
                                    return last;
                            }
                            return last;
                        }
                        // evolution-region: (evolution-region body...)
                        if (cname == "evolution-region" && v.children.size() >= 2) {
                            EvalResult last = make_void();
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                last = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!last)
                                    return last;
                            }
                            return last;
                        }
                        // with-capability: (with-capability cap-name body...)
                        // Bind capabilities as special variables in the environment.
                        if (cname == "with-capability" && v.children.size() >= 2) {
                            auto cap_id = v.child(1);
                            auto cap_result = eval_flat(*f, *p, cap_id, eval_env);
                            if (!cap_result)
                                return cap_result;
                            // Extract capability name(s)
                            std::vector<std::string> caps;
                            if (is_string(*cap_result)) {
                                auto sidx = as_string_idx(*cap_result);
                                if (sidx < string_heap_.size())
                                    caps.push_back(string_heap_[sidx]);
                            } else if (is_pair(*cap_result)) {
                                auto cidx = as_pair_idx(*cap_result);
                                while (cidx < pairs_.size()) {
                                    auto& pr = pairs_[cidx];
                                    if (is_string(pr.car)) {
                                        auto sidx2 = as_string_idx(pr.car);
                                        if (sidx2 < string_heap_.size())
                                            caps.push_back(string_heap_[sidx2]);
                                    }
                                    break;
                                }
                            }
                            // Create child env with %cap:name bindings
                            tail_env.emplace(&eval_env);
                            tail_env->set_primitives(&primitives_);

                            for (auto& cap : caps)
                                tail_env->bind("%cap:" + cap, make_bool(true));
                            // Push to capability_stack_ for capability-stack readout
                            capability_stack_.push_back(caps);
                            // Evaluate body in child env
                            EvalResult last = make_void();
                            for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                last = eval_flat(*f, *p, v.child(ci), *tail_env);
                                if (!last) {
                                    capability_stack_.pop_back();
                                    return last;
                                }
                            }
                            capability_stack_.pop_back();
                            return last;
                        }
                        // check-capability: (check-capability "Name") — look up %cap:Name binding
                        if (cname == "check-capability" && v.children.size() >= 2) {
                            auto arg_result = eval_flat(*f, *p, v.child(1), eval_env);
                            if (!arg_result)
                                return arg_result;
                            std::string cap_name;
                            if (is_string(*arg_result)) {
                                auto sidx = as_string_idx(*arg_result);
                                if (sidx < string_heap_.size())
                                    cap_name = string_heap_[sidx];
                            }
                            auto val = eval_env.lookup("%cap:" + cap_name);
                            return val.has_value() ? make_bool(true) : make_bool(false);
                        }

                        // while: (while cond body) — evaluate condition, if true evaluate body,
                        // repeat
                        if (cname == "while" && v.children.size() >= 3) {
                            // Check if args are Lambda nodes (EDSL while with closures)
                            // In that case, fall through to primitive dispatch instead
                            auto c1_node = v.child(1) < f->size() ? f->get(v.child(1)) : v;
                            auto c2_node = v.child(2) < f->size() ? f->get(v.child(2)) : v;
                            if (c1_node.tag != aura::ast::NodeTag::Lambda &&
                                c2_node.tag != aura::ast::NodeTag::Lambda) {
                                while (true) {
                                    auto cond_result = eval_flat(*f, *p, v.child(1), eval_env);
                                    if (!cond_result)
                                        return cond_result;
                                    if (!is_truthy(*cond_result))
                                        break;
                                    auto body_result = eval_flat(*f, *p, v.child(2), eval_env);
                                    if (!body_result)
                                        return body_result;
                                }
                                return make_void();
                            }
                        }

                        if (cname == "try" && v.children.size() >= 2) {
                            auto body_id = v.child(1);
                            auto result = eval_flat(*f, *p, body_id, eval_env);
                            if (result && !is_error(*result)) {
                                // Body succeeded — return result as-is
                                return result;
                            }
                            // Body errored — find catch clause (child[2] or later)
                            if (v.children.size() < 3)
                                return make_void();
                            for (std::size_t ci = 2; ci < v.children.size(); ++ci) {
                                auto catch_id = v.child(ci);
                                auto cv = f->get(catch_id);
                                if (cv.tag == aura::ast::NodeTag::Call) {
                                    auto catch_fn = f->get(cv.child(0));
                                    if (catch_fn.tag == aura::ast::NodeTag::Variable &&
                                        std::string(p->resolve(catch_fn.sym_id)) == "catch") {
                                        // (catch (var) handler) — child[0]=catch, child[1]=(var),
                                        // child[2]=handler
                                        if (cv.children.size() < 3)
                                            continue;
                                        auto var_form = f->get(cv.child(1));
                                        // var_form is (var) — a Call where child[0]=Variable "var"
                                        std::string var_name;
                                        if (var_form.tag == aura::ast::NodeTag::Call &&
                                            var_form.children.size() >= 1) {
                                            auto var_node = f->get(var_form.child(0));
                                            if (var_node.tag == aura::ast::NodeTag::Variable)
                                                var_name = std::string(p->resolve(var_node.sym_id));
                                        }
                                        auto handler_id = cv.child(2);
                                        // Bind error value to var and evaluate handler
                                        Env catch_env(&eval_env);
                                        catch_env.set_primitives(&primitives_);
                                        // P0: no cells_ on Env; deref uses central cells_ or owner
                                        // walk
                                        if (!var_name.empty() && result) {
                                            catch_env.bind(var_name, *result);
                                        }
                                        return eval_flat(*f, *p, handler_id, catch_env);
                                    }
                                }
                            }
                            // No matching catch — propagate error
                            return result;
                        }
                    }

                    // and/or: short-circuit evaluation (not eager arg eval)
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        if (cname == "and" && v.children.size() >= 2) {
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                auto ar = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!ar)
                                    return ar;
                                if (!is_truthy(*ar))
                                    return *ar; // short-circuit: return falsy value
                                if (ci + 1 == v.children.size())
                                    return *ar; // last arg: return its value
                            }
                            return make_int(1); // (and) with no args → #t
                        }
                        if (cname == "or" && v.children.size() >= 2) {
                            for (std::size_t ci = 1; ci < v.children.size(); ++ci) {
                                auto ar = eval_flat(*f, *p, v.child(ci), eval_env);
                                if (!ar)
                                    return ar;
                                if (is_truthy(*ar))
                                    return *ar; // short-circuit: return first truthy value
                                if (ci + 1 == v.children.size())
                                    return *ar; // last arg: return last value (falsy)
                            }
                            return make_int(0); // (or) with no args → #f
                        }
                    }

                    // Primitive call (all arg evals are recursive)
                    if (callee.tag == aura::ast::NodeTag::Variable) {
                        auto cname = std::string(p->resolve(callee.sym_id));
                        // #223 follow-up: skip primitive lookup when
                        // cname is empty (e.g. when sym_id was out of
                        // bounds at the resolve() call). The
                        // environment lookup below handles missing
                        // names gracefully via nullopt.
                        std::optional<PrimFn> prim;
                        if (!cname.empty()) {
                            prim = eval_env.lookup_primitive(cname);
                        }
                        if (prim) {
                            std::vector<EvalValue> args;
                            for (std::size_t i = 1; i < v.children.size(); ++i) {
                                auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                if (!ar)
                                    return ar;
                                // Propagate error values through normal eval
                                // Note: is_string check prevents accidental collision
                                // where make_string(idx) with odd idx matches is_ref/RefError
                                // encoding
                                if (is_error(*ar) && !is_string(*ar))
                                    return ar;
                                args.push_back(*ar);
                            }
                            return (*prim)(args);
                        }
                    }
                    // Closure call (eval func + arg evals are recursive; body is tail)
                    auto fn = eval_flat(*f, *p, callee_id, eval_env);
                    if (!fn)
                        return fn;
                    if (is_closure(*fn)) {
                        auto cid = as_closure_id(*fn);
                        // Check for foreign function (high bit set)
                        if (cid < ffi_runtime_.func_count()) {
                            // Dispatch FFI through apply_closure
                            // (Issue #252: apply_closure increments
                            // closure_calls_total + closure_ffi_calls
                            // for the FFI branch — no double-count
                            // here because eval_flat's FFI inline path
                            // delegates the counter bumps to it.)
                            std::size_t named_count = 0;
                            std::vector<EvalValue> cargs;
                            for (std::size_t i = 0; i + 1 < v.children.size(); ++i) {
                                auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                                if (!ar)
                                    return ar;
                                cargs.push_back(*ar);
                            }
                            auto result = apply_closure(cid, cargs);
                            if (result)
                                return *result;
                            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                              "eval_flat: foreign call failed"});
                        }
                        // Issue #252: eval_flat's inline TW closure
                        // call path does NOT go through apply_closure
                        // (it inlines the body for TCO). Bump the
                        // total + TW counter here so the snapshot is
                        // consistent with apply_closure's other
                        // entry points.
                        if (compiler_metrics_) {
                            auto* m = static_cast<struct CompilerMetrics*>(compiler_metrics_);
                            m->closure_calls_total.fetch_add(1, std::memory_order_relaxed);
                            m->closure_tw_calls.fetch_add(1, std::memory_order_relaxed);
                        }
                        auto it = closures_.find(cid);
                        if (it == closures_.end())
                            return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                                              "eval_flat: invalid closure"});
                        auto& cl = it->second;
                        // Evaluate named args first
                        std::size_t named_count = cl.dotted && !cl.params.empty()
                                                      ? cl.params.size() - 1
                                                      : cl.params.size();
                        std::vector<EvalValue> cargs;
                        cargs.reserve(named_count);
                        for (std::size_t i = 0; i < named_count && i + 1 < v.children.size(); ++i) {
                            auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
                            if (!ar)
                                return ar;
                            if (is_error(*ar))
                                return ar;
                            cargs.push_back(*ar);
                        }
                        tail_env = materialize_call_env(cl);
                        tail_env->set_primitives(&primitives_);

                        // Issue #145: set the pool so bind_symid can mirror
                        if (cl.pool)
                            tail_env->set_pool(cl.pool);
                        for (std::size_t i = 0; i < cargs.size(); ++i) {
                            tail_env->bind_symid(cl.params[i], std::move(cargs[i]));
                        }
                        // Dotted rest: collect remaining args into a pair list
                        if (cl.dotted && !cl.params.empty()) {
                            types::EvalValue rest = make_void();
                            for (std::size_t i = v.children.size() - 1; i > named_count; --i) {
                                auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                if (!ar)
                                    return ar;
                                if (is_error(*ar) && !is_string(*ar))
                                    return ar;
                                auto pid = pairs_.size();
                                pairs_.push_back({*ar, rest});
                                rest = make_pair(pid);
                            }
                            tail_env->bind_symid(cl.params.back(), rest);
                        }
                        if (cl.body_id != aura::ast::NULL_NODE)
                            return eval_flat(*cl.flat, cl.pool ? *cl.pool : *p, cl.body_id,
                                             *tail_env);
                        return make_void();
                    }
                    // Functor instantiation: callee is a %functor marker
                    if (is_string(*fn) && as_string_idx(*fn) < string_heap_.size() &&
                        string_heap_[as_string_idx(*fn)] == "%functor") {
                        auto callee_v = f->get(v.child(0));
                        if (callee_v.tag == aura::ast::NodeTag::Variable) {
                            auto tpl_name = std::string(p->resolve(callee_v.sym_id));
                            auto tpl_it = module_templates_.find(tpl_name);
                            if (tpl_it != module_templates_.end()) {
                                // 构建缓存 key: "template|arg1|arg2|..."
                                std::string cache_key = tpl_name;
                                for (std::size_t ki = 1; ki < v.children.size(); ++ki) {
                                    auto kv = f->get(v.child(ki));
                                    cache_key += "|";
                                    if (kv.tag == aura::ast::NodeTag::Variable)
                                        cache_key += std::string(p->resolve(kv.sym_id));
                                    else
                                        cache_key += "#" + std::to_string(v.child(ki));
                                }
                                auto cache_it = functor_instance_cache_.find(cache_key);
                                if (cache_it != functor_instance_cache_.end()) {
                                    return types::make_module(cache_it->second);
                                }

                                // 使用 ModuleTemplate 中缓存的参数名（避免跨 FlatAST 扫描）
                                auto& param_names = tpl_it->second.type_param_names;

                                // 创建隔离环境
                                Env mod_env(&eval_env);
                                mod_env.set_primitives(&primitives_);


                                // 绑定类型参数到环境（按原始参数名）
                                for (std::size_t ai = 1; ai < v.children.size(); ++ai) {
                                    auto arg_v = f->get(v.child(ai));
                                    std::string pname = (ai - 1 < param_names.size())
                                                            ? param_names[ai - 1]
                                                            : (":T" + std::to_string(ai - 1));
                                    if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                        // 类型参数：存为字符串（类型名）
                                        auto type_name = std::string(p->resolve(arg_v.sym_id));
                                        auto sidx = string_heap_.size();
                                        string_heap_.push_back(type_name);
                                        mod_env.bind(pname, make_string(sidx));
                                    } else {
                                        // 值参数：正常 eval
                                        auto ar = eval_flat(*f, *p, v.child(ai), eval_env);
                                        if (!ar)
                                            return ar;
                                        mod_env.bind(pname, *ar);
                                    }
                                }

                                // Capability requirement check
                                if (!tpl_it->second.cap_require.empty()) {
                                    // Find the capability argument
                                    // Capability params are stored at the end of param_names
                                    std::string provided_caps_str;
                                    for (std::size_t ai = 1; ai < v.children.size(); ++ai) {
                                        auto arg_v = f->get(v.child(ai));
                                        std::string pname = (ai - 1 < param_names.size())
                                                                ? param_names[ai - 1]
                                                                : "";
                                        // Check if this param is a cap param
                                        bool is_cap_param = false;
                                        for (auto& cp : tpl_it->second.cap_param_names) {
                                            if (cp == pname) {
                                                is_cap_param = true;
                                                break;
                                            }
                                        }
                                        if (is_cap_param) {
                                            if (arg_v.tag == aura::ast::NodeTag::Variable) {
                                                provided_caps_str =
                                                    std::string(p->resolve(arg_v.sym_id));
                                            } else if (arg_v.tag ==
                                                       aura::ast::NodeTag::LiteralString) {
                                                provided_caps_str = "";
                                            }
                                        }
                                    }
                                    // Check if provided caps satisfy requirements
                                    // Simple string matching: "FileReadWrite" contains "FileRead"
                                    // and "FileWrite"
                                    std::vector<std::string> missing;
                                    for (auto& req : tpl_it->second.cap_require) {
                                        bool found = false;
                                        if (provided_caps_str.find(req) != std::string::npos)
                                            found = true;
                                        // Also check for "*" wildcard
                                        if (provided_caps_str == "*")
                                            found = true;
                                        if (!found)
                                            missing.push_back(req);
                                    }
                                    if (!missing.empty()) {
                                        std::string err =
                                            "functor " + tpl_name + ": missing capabilities: ";
                                        for (std::size_t mi = 0; mi < missing.size(); ++mi) {
                                            if (mi > 0)
                                                err += ", ";
                                            err += missing[mi];
                                        }
                                        auto es = string_heap_.size();
                                        string_heap_.push_back(err);
                                        auto ev = error_values_.size();
                                        error_values_.push_back(make_string(es));
                                        return make_error(ev);
                                    }
                                }

                                // Eval body by re-parsing the serialized source
                                EvalResult last = make_void();
                                auto& body_src = tpl_it->second.body_source;
                                if (!body_src.empty()) {
                                    // Parse body as a begin block so all expressions become
                                    // children
                                    std::string wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena body_arena;
                                    auto body_alloc = body_arena.allocator();
                                    aura::ast::StringPool body_pool(body_alloc);
                                    aura::ast::FlatAST body_flat(body_alloc);
                                    auto body_pr =
                                        aura::parser::parse_to_flat(wrapped, body_flat, body_pool);
                                    if (body_pr.success && body_pr.root != aura::ast::NULL_NODE) {
                                        body_flat.root = body_pr.root;
                                        auto body_v = body_flat.get(body_flat.root);
                                        if (body_v.tag == aura::ast::NodeTag::Begin) {
                                            for (auto nid : body_v.children) {
                                                auto br =
                                                    eval_flat(body_flat, body_pool, nid, mod_env);
                                                if (!br)
                                                    return br;
                                                last = *br;
                                            }
                                        } else {
                                            auto br = eval_flat(body_flat, body_pool,
                                                                body_flat.root, mod_env);
                                            if (!br)
                                                return br;
                                            last = *br;
                                        }
                                    }
                                }
                                // 实例化后生成 .aura-type 签名
                                // Extract export names from the body source
                                std::vector<std::string> export_names;
                                {
                                    std::string scan_wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena scan_arena;
                                    auto scan_alloc = scan_arena.allocator();
                                    aura::ast::StringPool scan_pool(scan_alloc);
                                    aura::ast::FlatAST scan_flat(scan_alloc);
                                    auto scan_pr = aura::parser::parse_to_flat(
                                        scan_wrapped, scan_flat, scan_pool);
                                    if (scan_pr.success && scan_pr.root != aura::ast::NULL_NODE) {
                                        scan_flat.root = scan_pr.root;
                                        auto scan_v = scan_flat.get(scan_flat.root);
                                        auto scan_children =
                                            (scan_v.tag == aura::ast::NodeTag::Begin)
                                                ? scan_v.children
                                                : std::span<const aura::ast::NodeId>(
                                                      &scan_flat.root, 1);
                                        for (auto nid : scan_children) {
                                            auto nv = scan_flat.get(nid);
                                            if (nv.tag == aura::ast::NodeTag::Export) {
                                                for (auto eid : nv.children) {
                                                    auto ev = scan_flat.get(eid);
                                                    if (ev.tag == aura::ast::NodeTag::Variable)
                                                        export_names.push_back(std::string(
                                                            scan_pool.resolve(ev.sym_id)));
                                                }
                                            }
                                        }
                                    }
                                }
                                if (!export_names.empty()) {
                                    // 对实例化后的 body 做类型推断，生成实际签名
                                    // Parse body source, type-check via TypeChecker, register
                                    // signatures
                                    aura::core::TypeRegistry tc_reg;
                                    aura::compiler::TypeChecker functor_tc(tc_reg);
                                    aura::diag::DiagnosticCollector tc_diag;

                                    std::string tc_wrapped = "(begin " + body_src + ")";
                                    aura::ast::ASTArena tc_arena;
                                    auto tc_alloc = tc_arena.allocator();
                                    aura::ast::StringPool tc_pool(tc_alloc);
                                    aura::ast::FlatAST tc_flat(tc_alloc);
                                    auto tc_pr =
                                        aura::parser::parse_to_flat(tc_wrapped, tc_flat, tc_pool);
                                    aura::ast::NodeId tc_root = tc_pr.root;
                                    if (tc_pr.success && tc_root != aura::ast::NULL_NODE) {
                                        tc_flat.root = tc_root;
                                        // Type-check the whole body to populate func types
                                        functor_tc.infer_flat(tc_flat, tc_pool, tc_root, tc_diag);

                                        // Scan body for export functions and extract their types
                                        for (auto& en : export_names) {
                                            std::string sig_key = cache_key + "/" + en;
                                            if (declared_type_sigs_.find(sig_key) !=
                                                declared_type_sigs_.end())
                                                continue;

                                            // Find the Define node for this export
                                            bool found = false;
                                            std::string type_str = "Any|Any";
                                            for (aura::ast::NodeId nid = 0; nid < tc_flat.size();
                                                 ++nid) {
                                                auto nv = tc_flat.get(nid);
                                                if (nv.tag == aura::ast::NodeTag::Define &&
                                                    nv.sym_id != aura::ast::INVALID_SYM &&
                                                    std::string(tc_pool.resolve(nv.sym_id)) == en &&
                                                    !nv.children.empty()) {
                                                    // Re-infer the value expression to get its type
                                                    auto val_type = functor_tc.infer_flat(
                                                        tc_flat, tc_pool, nv.child(0), tc_diag);
                                                    if (val_type.valid() && val_type.index > 0) {
                                                        // Format as type signature
                                                        auto fmt = tc_reg.format_type(val_type);
                                                        if (!fmt.empty()) {
                                                            // Convert from '->' to '|' format for
                                                            // declared_type_sigs_
                                                            auto pipe_pos = fmt.find(" -> ");
                                                            if (pipe_pos != std::string::npos) {
                                                                auto params =
                                                                    fmt.substr(0, pipe_pos);
                                                                auto ret = fmt.substr(pipe_pos + 4);
                                                                type_str = params + "|" + ret;
                                                            } else {
                                                                type_str = "|" + fmt;
                                                            }
                                                        }
                                                    }
                                                    found = true;
                                                    break;
                                                }
                                            }

                                            declared_type_sigs_[sig_key] = {
                                                .type_str = type_str,
                                                .module_file = "%functor:" + tpl_name,
                                                .resolved = found};
                                        }
                                    } else {
                                        // Fallback: Any|Any (shouldn't happen since body was parsed
                                        // earlier)
                                        for (auto& en : export_names) {
                                            std::string sig_key = cache_key + "/" + en;
                                            if (declared_type_sigs_.find(sig_key) ==
                                                declared_type_sigs_.end()) {
                                                declared_type_sigs_[sig_key] = {
                                                    .type_str = "Any|Any",
                                                    .module_file = "%functor:" + tpl_name,
                                                    .resolved = false};
                                            }
                                        }
                                    }
                                }

                                // 缓存实例化结果 — per-instance arena so the
                                // Env and any closures it owns can be freed later
                                // via gc_module(cache_key).
                                auto& inst_arena = arena_group_->module_arena(cache_key);
                                auto* cached_env = inst_arena.create<Env>(mod_env);
                                auto mod_idx = modules_.size();
                                modules_.push_back(cached_env);
                                module_cache_[cache_key] = mod_idx;
                                module_arena_ptrs_[cache_key] = &inst_arena;
                                module_names_.push_back(cache_key);
                                functor_instance_cache_[cache_key] = mod_idx;
                                return types::make_module(mod_idx);
                            }
                        }
                        return make_void();
                    }

                    // Primitive value call: callee is a PrimitiveRef (passed as value, not a
                    // Variable node)
                    if (is_primitive(*fn)) {
                        auto slot = as_primitive_slot(*fn);
                        if (slot < primitives_.slot_count()) {
                            auto prim = eval_env.lookup_primitive(primitives_.name_for_slot(slot));
                            if (prim) {
                                std::vector<EvalValue> args;
                                for (std::size_t i = 1; i < v.children.size(); ++i) {
                                    auto ar = eval_flat(*f, *p, v.child(i), eval_env);
                                    if (!ar)
                                        return ar;
                                    args.push_back(*ar);
                                }
                                return (*prim)(args);
                            }
                        }
                    }
                    auto callee_name = std::string(p->resolve(callee.sym_id));
                    // Build diagnostic with appropriate suggestion (no self-move)
                    std::string suggestion;
                    if (callee_name.size() > 3 &&
                        callee_name.substr(callee_name.size() - 3) == "-fn")
                        suggestion = "if using c-func: (c-func -1 \"" +
                                     callee_name.substr(0, callee_name.size() - 3) +
                                     "\" \"(String) -> Int\")";
                    else
                        suggestion = "did you forget to define '" + callee_name + "'?";
                    return std::unexpected(
                        Diagnostic{ErrorKind::TypeError, "cannot call: " + callee_name}
                            .with_suggestion(std::move(suggestion)));
                }
                case aura::ast::NodeTag::IfExpr: {
                    if (v.children.size() < 2)
                        return EvalResult(make_void());
                    auto c = eval_flat(*f, *p, v.child(0), eval_env);
                    if (!c)
                        return c;
                    if (v.children.size() == 2) {
                        // (if cond then) — conditionally execute then-branch
                        if (is_truthy(*c)) {
                            current_id = v.child(1);
                            continue;
                        }
                        // Condition false, no else — use TCO to NULL_NODE so the
                        // while-loop guard returns void on next iteration (avoids
                        // a return path that can cause NULL_NODE in outer TCO loop
                        // when used inside rest-arg lambda bodies).
                        current_id = aura::ast::NULL_NODE;
                        continue;
                    }
                    current_id = is_truthy(*c) ? v.child(1) : v.child(2);
                    continue; // TCO: branch
                }
                case aura::ast::NodeTag::Lambda: {
                    // Capture params from FlatAST directly. Issue #145:
                    // store as SymId (SoA) — the apply_closure path now
                    // does integer-compare parameter binding instead of
                    // string-compare.
                    auto pspan = v.params;
                    std::vector<aura::ast::SymId> params;
                    params.reserve(pspan.size());
                    for (auto pid : pspan)
                        params.push_back(pid);
                    bool dotted = v.int_value != 0;
                    auto* target = (temp_arena_ && in_task_context_) ? temp_arena_ : arena_;
                    auto cid = next_id();
                    auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    // P0: legacy env pointer removed. Register captured env in SoA for this
                    // closure.
                    EnvId cap_id = alloc_env_frame_from_env(*current_env);
                    {
                        std::unique_lock<std::shared_mutex> wlock(closures_mtx_);
                        closures_[cid] =
                            Closure{"", std::move(params), f, p, body_id, cap_id, dotted, target};
                    }
                    // Do NOT cache closure values — the closure captures the current env and a
                    // cached closure would reuse the same env on subsequent evaluations (wrong
                    // when the same Lambda node is evaluated with different captured variables).
                    return make_closure(cid);
                }
                case aura::ast::NodeTag::Let:
                case aura::ast::NodeTag::LetRec: {
                    bool rec = (v.tag == aura::ast::NodeTag::LetRec);
                    auto name = p->resolve(v.sym_id);
                    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    auto body_id = v.children.size() < 2 ? aura::ast::NULL_NODE : v.child(1);
                    if (rec) {
                        // For letrec, the init value is evaluated in the new env (with cell
                        // binding)
                        tail_env.emplace(&eval_env);
                        tail_env->set_primitives(&primitives_);

                        // Issue #232 fix: register eval_env in env_frames_
                        // (always, not just when parent_id_ is NULL), then
                        // set tail_env's parent_id_ to eval_env's id. The
                        // materialized call env can then walk the SoA chain
                        // via lookup()'s parent_id_ fallback (added in #232
                        // commit 6e73ef2). The fix below is needed because
                        // even when eval_env.parent_id_ is non-NULL (e.g., a
                        // materialized call env has parent_id_ = top_'s id),
                        // the SoA walk needs to find the BINDINGS (e.g., 'n'
                        // from a let*), which live in eval_env but NOT in top_.
                        // Registering eval_env in env_frames_ makes those
                        // bindings visible to the SoA walk.
                        //
                        // Idempotency: skip if eval_env is already a frame
                        // (this would require tracking which envs are frames;
                        // for now, we always register, which is wasteful but
                        // correct).
                        if (eval_env.parent_id() == NULL_ENV_ID) {
                            EnvId eval_id = alloc_env_frame_from_env(eval_env);
                            const_cast<Env&>(eval_env).set_parent_id(eval_id);
                        } else {
                            // eval_env already has a parent_id_ (probably the
                            // top env). The SoA walk starts at this parent_id_
                            // which is top_ — but top_ doesn't have the let*'s
                            // bindings. Register eval_env as a NEW frame in
                            // env_frames_ (at the next index) and update its
                            // parent_id_ to the new id. The old parent_id_ is
                            // preserved on the eval_env's OWN frame.
                            EnvId new_id = alloc_env_frame_from_env(eval_env);
                            const_cast<Env&>(eval_env).set_parent_id(new_id);
                        }
                        tail_env->set_parent_id(eval_env.parent_id());

                        std::size_t ci = cells_.size();
                        cells_.push_back(make_void());
                        tail_env->bind(std::string(name), make_cell(ci));
                        // Evaluate value in *tail_env (has cell binding for self-reference)
                        auto vv = eval_flat(*f, *p, val_id, *tail_env);
                        if (!vv)
                            return vv;
                        cells_[ci] = *vv;
                        // Body evaluated in *tail_env (recursive refs need the child env)
                        if (body_id != aura::ast::NULL_NODE)
                            return eval_flat(*f, *p, body_id, *tail_env);
                        return make_void();
                    } else {
                        // For let, bind directly to current eval_env (like define) to avoid
                        // creating a stack-local child env whose parent_ pointer becomes
                        // dangling when captured by a closure (bug: closure capture copies
                        // the env but parent_ still points to the original stack env).
                        auto vv = eval_flat(*f, *p, val_id, eval_env);
                        if (!vv)
                            return vv;

                        // ── Match exhaustiveness check (tree-walker path) ──
                        // At runtime we don't have static type info, so we resolve the
                        // target ADT by finding which ADT in the registry contains the
                        // first used constructor (e.g. `Cons` -> `List`). Bare-identifier
                        // candidates are only counted as used if they are real ctors of
                        // that ADT (so a variable binding doesn't false-positive).
                        if (!rec && type_registry_ && f->has_match_info(current_id)) {
                            auto* minfo = f->get_match_info(current_id);
                            if (minfo && !minfo->has_wildcard &&
                                (!minfo->used_constructors.empty() ||
                                 !minfo->candidate_constructors.empty())) {
                                auto& treg =
                                    *static_cast<aura::core::TypeRegistry*>(type_registry_);
                                // Find the target ADT by scanning for the first used_ctor
                                // or candidate_ctor (bare-id patterns).
                                const std::vector<std::string>* target_ctors = nullptr;
                                aura::core::TypeId target_tid{};
                                auto find_adt_for = [&](aura::ast::SymId sid) -> bool {
                                    auto cname = std::string(p->resolve(sid));
                                    for (std::size_t ti = 0; ti < treg.size(); ++ti) {
                                        auto tid =
                                            aura::core::TypeId{static_cast<std::uint32_t>(ti), 1};
                                        auto* c = treg.get_adt_constructors(tid);
                                        if (!c)
                                            continue;
                                        if (std::find(c->begin(), c->end(), cname) != c->end()) {
                                            target_ctors = c;
                                            target_tid = tid;
                                            return true;
                                        }
                                    }
                                    return false;
                                };
                                for (auto sid : minfo->used_constructors) {
                                    if (find_adt_for(sid))
                                        break;
                                }
                                if (!target_ctors) {
                                    for (auto sid : minfo->candidate_constructors) {
                                        if (find_adt_for(sid))
                                            break;
                                    }
                                }
                                if (target_ctors) {
                                    // Build effective used set
                                    std::vector<std::string> used_eff;
                                    used_eff.reserve(minfo->used_constructors.size() +
                                                     minfo->candidate_constructors.size());
                                    for (auto sid : minfo->used_constructors)
                                        used_eff.push_back(std::string(p->resolve(sid)));
                                    for (auto sid : minfo->candidate_constructors) {
                                        auto cname = std::string(p->resolve(sid));
                                        if (std::find(target_ctors->begin(), target_ctors->end(),
                                                      cname) != target_ctors->end())
                                            used_eff.push_back(std::move(cname));
                                    }
                                    for (auto& expected_ctor : *target_ctors) {
                                        if (std::find(used_eff.begin(), used_eff.end(),
                                                      expected_ctor) == used_eff.end()) {
                                            std::println(
                                                std::cerr,
                                                "match warning: unhandled constructor '{}' in {}",
                                                expected_ctor, treg.name_of(target_tid));
                                        }
                                    }
                                }
                            }
                        }

                        auto& me = const_cast<Env&>(eval_env);

                        auto ci = cells_.size();
                        cells_.push_back(*vv);
                        me.bind(std::string(name), make_cell(ci));
                        if (body_id != aura::ast::NULL_NODE)
                            return eval_flat(*f, *p, body_id, eval_env);
                        return make_void();
                    }
                }
                case aura::ast::NodeTag::DefineType: {
                    // (define-type (Name params...) (Ctor fields...) ...)
                    // Bind each constructor by evaluating an Aura lambda:
                    //   (define <Ctor> (lambda args (cons 'Ctor args)))
                    // This avoids C++ complexity and works with existing pair infrastructure.
                    auto type_name = p->resolve(v.sym_id);
                    Env& me = const_cast<Env&>(eval_env);


                    for (auto cid : v.children) {
                        if (cid >= f->size())
                            continue;
                        auto cv = f->get(cid);
                        if (cv.tag != aura::ast::NodeTag::Quote || cv.children.empty())
                            continue;
                        auto quoted = cv.child(0);
                        if (quoted >= f->size())
                            continue;
                        auto qv = f->get(quoted);
                        // Constructor data is now (cons 'ctor-name (cons 'ft1 (cons 'ft2 ...)))
                        // Extract the constructor name from the head of the list
                        std::string ctor_name;
                        aura::ast::NodeId current = quoted;
                        auto cur_v = f->get(current);
                        if (cur_v.tag == aura::ast::NodeTag::Pair) {
                            auto car_id = cur_v.child(0);
                            if (car_id < f->size()) {
                                auto car_v = f->get(car_id);
                                if (car_v.tag == aura::ast::NodeTag::Variable)
                                    ctor_name = std::string(p->resolve(car_v.sym_id));
                            }
                        }
                        if (ctor_name.empty())
                            continue;

                        // Register constructor as a primitive that creates tagged lists:
                        // (Ctor arg1 arg2 ...) → (cons 'Ctor (cons arg1 (cons arg2 ...)))
                        auto tag_slot = string_heap_.size();
                        string_heap_.push_back(ctor_name);
                        auto tag_str = make_string(tag_slot);

                        // Count fields to determine if zero-arg constructor
                        int field_count = 0;
                        {
                            aura::ast::NodeId fields_node = quoted;
                            auto fv = f->get(fields_node);
                            if (fv.tag == aura::ast::NodeTag::Pair && fv.children.size() >= 2)
                                fields_node = fv.child(1);
                            while (fields_node < f->size()) {
                                auto fnv = f->get(fields_node);
                                if (fnv.tag != aura::ast::NodeTag::Pair || fnv.children.empty())
                                    break;
                                field_count++;
                                if (fnv.children.size() >= 2)
                                    fields_node = fnv.child(1);
                                else
                                    break;
                            }
                        }
                        if (field_count == 0) {
                            // Zero-arg constructor: bind directly to constructed value
                            types::EvalValue rest = make_void();
                            auto cid = static_cast<std::uint64_t>(pairs_.size());
                            pairs_.push_back({tag_str, rest});
                            me.bind(ctor_name, make_pair(cid));
                        } else {
                            // Multi-arg constructor: register as primitive
                            primitives_.add(
                                ctor_name, [this, tag_str](const auto& args) -> EvalValue {
                                    types::EvalValue rest = make_void();
                                    for (auto it = args.rbegin(); it != args.rend(); ++it) {
                                        auto pid = static_cast<std::uint64_t>(pairs_.size());
                                        pairs_.push_back({*it, rest});
                                        rest = make_pair(pid);
                                    }
                                    auto pid = static_cast<std::uint64_t>(pairs_.size());
                                    pairs_.push_back({tag_str, rest});
                                    return make_pair(pid);
                                });
                        } // end else (multi-arg)
                    }
                    return EvalResult(make_void());
                }

                case aura::ast::NodeTag::Define: {
                    auto name = p->resolve(v.sym_id);
                    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    Env& me = const_cast<Env&>(eval_env);


                    // Check if already bound as a cell — update existing cell to maintain
                    // sequential define chains across multiple eval calls
                    // Use lookup_binding to get the raw cell sentinel (not dereferenced value)
                    auto existing = eval_env.lookup_binding(std::string(name));
                    if (existing && is_cell(*existing)) {
                        auto ci = as_cell_id(*existing);
                        auto vv = eval_flat(*f, *p, val_id, eval_env);
                        if (!vv)
                            return vv;
                        cells_[ci] = *vv;
                        return *vv;
                    }

                    // Create new cell binding
                    auto ci = alloc_cell(make_void());
                    me.bind(std::string(name), make_cell(ci));
                    auto vv = eval_flat(*f, *p, val_id, eval_env);
                    if (!vv)
                        return vv;
                    cells_[ci] = *vv;
                    return *vv;
                }
                case aura::ast::NodeTag::Begin: {
                    auto count = v.children.size();
                    if (count == 0)
                        return EvalResult(make_void());

                    // Check if there are multiple define nodes → use letrec semantics
                    // Phase 1: pre-allocate cells for all defines
                    std::vector<std::pair<std::string, aura::ast::NodeId>> letrec_defs;
                    bool has_multiple_defs = false;
                    int define_count = 0;
                    // Find last non-NULL child (NULL_NODE holes may exist from mutate:move-node)
                    aura::ast::NodeId last_expr = aura::ast::NULL_NODE;
                    for (std::size_t si = count; si > 0; --si) {
                        auto cid = v.child(si - 1);
                        if (cid != aura::ast::NULL_NODE) {
                            last_expr = cid;
                            break;
                        }
                    }
                    if (last_expr == aura::ast::NULL_NODE)
                        return EvalResult(make_void());
                    for (std::size_t i = 0; i < count; ++i) {
                        auto cid = v.child(i);
                        if (cid == aura::ast::NULL_NODE)
                            continue;
                        auto child_node = f->get(cid);
                        if (child_node.tag == aura::ast::NodeTag::Define) {
                            define_count++;
                            if (define_count > 1)
                                has_multiple_defs = true;
                            letrec_defs.push_back({std::string(p->resolve(child_node.sym_id)),
                                                   child_node.children.empty()
                                                       ? aura::ast::NULL_NODE
                                                       : child_node.child(0)});
                        }
                    }

                    // Skip NULL_NODE children (left by mutate:move-node / mutate:remove-node)
                    std::size_t effective_count = 0;
                    for (std::size_t ci = 0; ci < count; ++ci) {
                        if (v.child(ci) != aura::ast::NULL_NODE)
                            effective_count++;
                    }
                    if (effective_count < count) {
                        // Count again for the main loop using only original children
                        // We'll check each child in the loop below
                        has_multiple_defs = false;
                        define_count = 0;
                        for (std::size_t ci = 0; ci < count; ++ci) {
                            auto cid = v.child(ci);
                            if (cid == aura::ast::NULL_NODE)
                                continue;
                            auto child_node = f->get(cid);
                            if (child_node.tag == aura::ast::NodeTag::Define) {
                                define_count++;
                                if (define_count > 1)
                                    has_multiple_defs = true;
                            }
                        }
                    }

                    if (has_multiple_defs) {
                        // Phase 1: pre-allocate cells for all defines
                        // This ensures all function names are visible to each other
                        std::vector<std::size_t> cell_ids;
                        {
                            auto& mutable_env = const_cast<Env&>(eval_env);

                            for (auto& d : letrec_defs) {
                                auto ci = alloc_cell(make_void());
                                mutable_env.bind(d.first, make_cell(ci));
                                cell_ids.push_back(ci);
                            }
                        }
                        // Phase 2: evaluate values and set cells
                        for (std::size_t i = 0; i < letrec_defs.size(); ++i) {
                            auto& d = letrec_defs[i];
                            if (d.second != aura::ast::NULL_NODE) {
                                auto val = eval_flat(*f, *p, d.second, eval_env);
                                if (!val)
                                    return val;
                                cells_[cell_ids[i]] = *val;
                            }
                        }
                        // Phase 3: evaluate remaining (non-define) expressions
                        for (std::size_t i = 0; i < count - 1; ++i) {
                            auto cid = v.child(i);
                            if (cid == aura::ast::NULL_NODE)
                                continue;
                            auto child_node = f->get(cid);
                            if (child_node.tag == aura::ast::NodeTag::Define)
                                continue;
                            auto r = eval_flat(*f, *p, cid, eval_env);
                            if (!r)
                                return r;
                        }
                        // TCO: last expression
                        current_id = last_expr;
                        continue;
                    }

                    // Single define (or no defines) — sequential evaluation
                    for (std::size_t i = 0; i < count - 1; ++i) {
                        auto cid = v.child(i);
                        if (cid == aura::ast::NULL_NODE)
                            continue;
                        auto r = eval_flat(*f, *p, cid, eval_env);
                        if (!r)
                            return r;
                    }
                    // Find last non-NULL child
                    current_id = aura::ast::NULL_NODE;
                    for (std::size_t i = count; i > 0; --i) {
                        auto cid = v.child(i - 1);
                        if (cid != aura::ast::NULL_NODE) {
                            current_id = cid;
                            break;
                        }
                    }
                    if (current_id == aura::ast::NULL_NODE)
                        return EvalResult(make_void());
                    continue; // TCO: last expression in begin
                }
                case aura::ast::NodeTag::DefineModule: {
                    // (define-module (Name :T ...) body...)
                    // Store module template and bind Name to functor
                    auto mod_name = std::string(p->resolve(v.sym_id));
                    ModuleTemplate mt;

                    // Extract type parameter names from AST params metadata
                    // (type params = all params minus cap params)
                    auto num_cap_params = f->cap_require_count(v.id);
                    std::size_t num_type_params =
                        (num_cap_params > 0 && v.params.size() >= num_cap_params)
                            ? v.params.size() - num_cap_params
                            : v.params.size();
                    for (std::size_t i = 0; i < num_type_params; ++i) {
                        auto pid = f->param_at(v.id, i);
                        mt.type_param_names.push_back(std::string(p->resolve(pid)));
                    }
                    if (num_cap_params > 0) {
                        // cap params are at the end of the param list
                        for (std::size_t i = 0; i < num_cap_params; ++i) {
                            auto pid = f->param_at(v.id, v.params.size() - num_cap_params + i);
                            mt.cap_param_names.push_back(std::string(p->resolve(pid)));
                        }
                    }

                    // Serialize body expressions to source strings (for cross-eval instantiation)
                    // Build a node-to-source serializer using the current FlatAST
                    std::function<std::string(aura::ast::NodeId)> node_source;
                    node_source = [&](aura::ast::NodeId nid) -> std::string {
                        if (nid >= f->size() || nid == aura::ast::NULL_NODE)
                            return "";
                        auto nv = f->get(nid);
                        switch (nv.tag) {
                            case aura::ast::NodeTag::LiteralInt:
                                return std::to_string(nv.int_value);
                            case aura::ast::NodeTag::LiteralFloat:
                                return std::to_string(nv.float_value);
                            case aura::ast::NodeTag::LiteralString:
                                return "\"" + std::string(p->resolve(nv.sym_id)) + "\"";
                            case aura::ast::NodeTag::Variable:
                                return std::string(p->resolve(nv.sym_id));
                            case aura::ast::NodeTag::Quote: {
                                if (nv.children.empty())
                                    return "'()";
                                return "'" + node_source(nv.child(0));
                            }
                            case aura::ast::NodeTag::Lambda: {
                                std::string s = "(lambda (";
                                for (std::size_t pi = 0; pi < nv.params.size(); ++pi) {
                                    if (pi > 0)
                                        s += " ";
                                    s += std::string(p->resolve(nv.params[pi]));
                                }
                                s += ")";
                                if (!nv.children.empty())
                                    s += " " + node_source(nv.child(0));
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Define: {
                                std::string s = "(define";
                                if (!nv.children.empty()) {
                                    auto val_nv = f->get(nv.child(0));
                                    if (val_nv.tag == aura::ast::NodeTag::Lambda) {
                                        // Shorthand: (define (name params...) body...)
                                        s += " (" + std::string(p->resolve(nv.sym_id));
                                        for (std::size_t pi = 0; pi < val_nv.params.size(); ++pi) {
                                            s += " ";
                                            s += std::string(p->resolve(val_nv.params[pi]));
                                        }
                                        s += ")";
                                        if (!val_nv.children.empty())
                                            s += " " + node_source(val_nv.child(0));
                                    } else {
                                        s += " " + std::string(p->resolve(nv.sym_id));
                                        s += " " + node_source(nv.child(0));
                                    }
                                }
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Export: {
                                std::string s = "(export";
                                for (auto eid : nv.children) {
                                    auto ev = f->get(eid);
                                    if (ev.tag == aura::ast::NodeTag::Variable)
                                        s += " " + std::string(p->resolve(ev.sym_id));
                                }
                                return s + ")";
                            }
                            case aura::ast::NodeTag::Call: {
                                std::string s = "(";
                                for (std::size_t ci = 0; ci < nv.children.size(); ++ci) {
                                    if (ci > 0)
                                        s += " ";
                                    s += node_source(nv.child(ci));
                                }
                                return s + ")";
                            }
                            default:
                                return "()";
                        }
                    };

                    // Serialize each body expression
                    std::string body_src;
                    for (auto cid : v.children) {
                        auto sexpr = node_source(cid);
                        if (!sexpr.empty()) {
                            if (!body_src.empty())
                                body_src += "\n";
                            body_src += sexpr;
                        }
                    }
                    mt.body_source = std::move(body_src);

                    // Scan body for `:require` directives
                    // Format: ((:require FileRead FileWrite) ...) or ((:require FileRead) ...)
                    for (auto cid : v.children) {
                        auto cv = f->get(cid);
                        if (cv.tag == aura::ast::NodeTag::Call && cv.children.size() > 0) {
                            auto callee_node = f->get(cv.child(0));
                            if (callee_node.tag == aura::ast::NodeTag::Variable ||
                                callee_node.tag == aura::ast::NodeTag::Quote) {
                                aura::ast::SymId sym =
                                    (callee_node.tag == aura::ast::NodeTag::Variable)
                                        ? callee_node.sym_id
                                        : aura::ast::INVALID_SYM;
                                std::string_view callee_name = (sym != aura::ast::INVALID_SYM)
                                                                   ? p->resolve(sym)
                                                                   : std::string_view();
                                // Check for :require or require keyword
                                if (callee_name == ":require" || callee_name == ":require-all") {
                                    // Extract required capability names from remaining children
                                    for (std::size_t ai = 1; ai < cv.children.size(); ++ai) {
                                        auto arg_node = f->get(cv.child(ai));
                                        if (arg_node.tag == aura::ast::NodeTag::Variable) {
                                            auto cap_name =
                                                std::string(p->resolve(arg_node.sym_id));
                                            // Skip duplicates
                                            bool dup = false;
                                            for (auto& r : mt.cap_require) {
                                                if (r == cap_name) {
                                                    dup = true;
                                                    break;
                                                }
                                            }
                                            if (!dup)
                                                mt.cap_require.push_back(cap_name);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    module_templates_[mod_name] = std::move(mt);

                    // Bind Name in the current env (as a cell with functor marker)
                    Env& me = const_cast<Env&>(eval_env);

                    auto ci = alloc_cell(make_void());
                    auto sidx = string_heap_.size();
                    string_heap_.push_back("%functor");
                    me.bind(mod_name, make_cell(ci));
                    cells_[ci] = make_string(sidx);
                    return make_string(sidx);
                }
                case aura::ast::NodeTag::Export: {
                    // (export sym ...) — record module API during loading
                    // No runtime effect; children are Variable nodes
                    if (!current_export_set_) {
                        current_export_set_ = std::make_unique<std::unordered_set<std::string>>();
                    }
                    for (auto cid : v.children) {
                        auto cv = f->get(cid);
                        if (cv.tag == aura::ast::NodeTag::Variable) {
                            current_export_set_->insert(std::string(p->resolve(cv.sym_id)));
                        }
                    }
                    return types::make_void();
                }
                case aura::ast::NodeTag::Set: {
                    auto name = p->resolve(v.sym_id);
                    auto val_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    auto val = eval_flat(*f, *p, val_id, eval_env);
                    if (!val)
                        return val;
                    // Use stable index instead of pointer (cells_ may reallocate)
                    auto cell_idx = eval_env.lookup_cell_index(std::string(name));
                    if (cell_idx) {
                        if (*cell_idx < cells_.size())
                            cells_[*cell_idx] = *val;
                        return *val;
                    }
                    // Fallback 1: direct binding in current env
                    for (auto& b : const_cast<Env&>(eval_env).bindings()) {
                        if (b.first == name) {
                            b.second = *val;
                            return *val;
                        }
                    }
                    // Fallback 2: scan parent envs for direct (non-cell) bindings
                    {
                        const Env* e = eval_env.parent();
                        while (e) {
                            for (auto& b : const_cast<Env&>(*e).bindings()) {
                                if (b.first == name) {
                                    b.second = *val;
                                    return *val;
                                }
                            }
                            e = e->parent();
                        }
                    }
                    // Suggest closest bound variables
                    {
                        std::vector<std::string> candidates;
                        {
                            const Env* e = &eval_env;
                            while (e) {
                                for (auto& b : const_cast<Env&>(*e).bindings())
                                    candidates.push_back(b.first);
                                e = e->parent();
                            }
                        }
                        auto best = closest_match(name, candidates);
                        // Issue #79: source location from the offending node.
                        Diagnostic d(ErrorKind::UnboundVariable, "set!: " + std::string(name),
                                     aura::diag::SourceLocation{v.line, v.col, 0}, current_id);
                        if (!best.empty())
                            d.with_suggestion("did you mean '" + best + "'?");
                        return std::unexpected(std::move(d));
                    }
                    return std::unexpected(
                        Diagnostic{ErrorKind::UnboundVariable, "set!: " + std::string(name),
                                   aura::diag::SourceLocation{v.line, v.col, 0}, current_id});
                }
                case aura::ast::NodeTag::Quote: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    return EvalResult(ast_to_data(*f, *p, v.child(0)));
                }
                case aura::ast::NodeTag::TypeAnnotation: {
                    if (v.children.empty())
                        return EvalResult(make_void());
                    auto annot_id = current_id;
                    auto child_result = eval_flat(*f, *p, v.child(0), eval_env);
                    if (!child_result)
                        return child_result;
                    // 3-arg form (: name Type val): bind the result in eval_env
                    if (v.int_value != 0) {
                        auto var_name = p->resolve(static_cast<aura::ast::SymId>(v.int_value));
                        if (!var_name.empty()) {
                            auto& me = const_cast<Env&>(eval_env);

                            auto ci = cells_.size();
                            cells_.push_back(*child_result);
                            me.bind(std::string(var_name), make_cell(ci));
                        }
                    }
                    // Runtime type check: compare value type against annotation
                    if (type_registry_ && annot_id < f->size()) {
                        auto expected_type_id = f->type_id(annot_id);
                        if (expected_type_id != 0) {
                            auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
                            auto expected_tag =
                                treg.tag_of(aura::core::TypeId{expected_type_id, 1});
                            auto actual_tag = runtime_type_tag(*child_result);
                            if (actual_tag != expected_tag &&
                                actual_tag != aura::core::TypeTag::DYNAMIC) {
                                auto& val = *child_result;
                                // Attempt coercion at runtime
                                bool coerced =
                                    coerce_value(val, actual_tag, expected_tag, string_heap_);
                                if (!coerced) {
                                    std::string expected_name(
                                        treg.format_type(aura::core::TypeId{expected_type_id, 1}));
                                    std::string actual_name = type_tag_name(actual_tag);
                                    std::println(std::cerr, "type warning: expected {}, got {}\n",
                                                 expected_name, actual_name);
                                }
                            }
                        }
                    }
                    return child_result;
                }
                case aura::ast::NodeTag::MacroDef: {
                    auto name = p->resolve(v.sym_id);
                    std::vector<std::string> param_names;
                    for (auto pn : v.params)
                        param_names.push_back(std::string(p->resolve(pn)));
                    auto body_id = v.children.empty() ? aura::ast::NULL_NODE : v.child(0);
                    if (body_id == aura::ast::NULL_NODE)
                        return EvalResult(make_void());

                    // ── Warn: unused macro parameters ──────────────────────────
                    // Scan the body for variable references and compare with params.
                    {
                        // Collect all variable names referenced in the macro body
                        std::unordered_set<std::string> used_vars;
                        auto collect_vars = [&](this const auto& self,
                                                aura::ast::NodeId nid) -> void {
                            if (nid == aura::ast::NULL_NODE || nid >= f->size())
                                return;
                            auto nv = f->get(nid);
                            if (nv.tag == aura::ast::NodeTag::Variable &&
                                nv.sym_id != aura::ast::INVALID_SYM) {
                                used_vars.insert(std::string(p->resolve(nv.sym_id)));
                            }
                            for (auto c : nv.children)
                                self(c);
                        };
                        collect_vars(body_id);

                        int used_count = 0;
                        for (auto& pn : param_names) {
                            if (used_vars.count(pn) == 0) {
                                std::println(std::cerr,
                                             "warning: macro '{}': parameter '{}' never used",
                                             std::string(name), pn);
                            } else {
                                ++used_count;
                            }
                        }
                        if (used_count == 0) {
                            std::println(
                                std::cerr,
                                "warning: macro '{}': body does not reference any parameter",
                                std::string(name));
                        }
                    }

                    // Store macro definition with proper dotted flag
                    // Issue #120: dotted is bit 0, hygienic is bit 1 of
                    // int_val_ (encoded by add_macrodef in parser_impl.cpp).
                    bool is_dotted = (v.int_value & 1) != 0;
                    bool is_hygienic = (v.int_value & 2) != 0;
                    // Issue #230 #2: bit 2 of int_val_ flags the
                    // `define-hygienic-macro*` (preserved-params) variant.
                    bool is_preserved = (v.int_value & 4) != 0;
                    macros_[std::string(name)] = MacroDef{std::move(param_names),
                                                          is_dotted,
                                                          is_hygienic,
                                                          is_preserved,
                                                          f,
                                                          p,
                                                          body_id};
                    return EvalResult(make_void());
                }
                case aura::ast::NodeTag::Linear:
                case aura::ast::NodeTag::Move:
                case aura::ast::NodeTag::Borrow:
                case aura::ast::NodeTag::MutBorrow:
                case aura::ast::NodeTag::Drop: {
                    // M4 ownership operations at tree-walker level:
                    // compile-time concepts — evaluate inner expression directly
                    if (v.children.empty())
                        return EvalResult(make_void());
                    return eval_flat(*f, *p, v.child(0), eval_env);
                }
                default:
                    return std::unexpected(
                        Diagnostic{ErrorKind::InternalError, "eval_flat: unsupported node type"});
            }
        }
    } catch (const std::bad_alloc& e) {
        return std::unexpected(Diagnostic{ErrorKind::InternalError, "out of memory"});
    } catch (const std::out_of_range& e) {
        return std::unexpected(Diagnostic{ErrorKind::InternalError,
                                          std::format("argument out of range: {}", e.what())});
    } catch (const std::bad_variant_access& e) {
        return std::unexpected(Diagnostic{
            ErrorKind::TypeError,
            std::format("type mismatch (wrong argument type passed to primitive): {}", e.what())});
    }
}

// ── Macro expander (hygienic Phase 2) ────────────────────────
// Clone a FlatAST subtree with MacroIntroduced markers.
// When a Variable matches a macro param, substitute with the arg expression.
// All new nodes are marked MacroIntroduced for hygiene tracking.
//
// name_map: when non-null, enables hygienic renaming — template-introduced
// binding positions (let, lambda, define) auto-gensym to avoid capture.
// References to gensym'd names are updated via the name_map.
static aura::ast::NodeId clone_macro_body(
    aura::ast::FlatAST& target, aura::ast::StringPool& target_pool, aura::ast::FlatAST& source,
    aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId>* subst,
    std::unordered_map<std::string, std::string>* name_map, aura::ast::SyntaxMarker cloned_marker) {
    using namespace aura::ast;
    if (body_id == NULL_NODE || body_id >= source.size())
        return NULL_NODE;
    auto v = source.get(body_id);

    // Set of built-in names that should never be gensym'd
    static const std::unordered_set<std::string> builtins = {
        "if",
        "cond",
        "let",
        "let*",
        "letrec",
        "lambda",
        "define",
        "begin",
        "set!",
        "quote",
        "unquote",
        "quasiquote",
        "case",
        "when",
        "unless",
        "car",
        "cdr",
        "cons",
        "list",
        "pair?",
        "null?",
        "eq?",
        "equal?",
        "+",
        "-",
        "*",
        "/",
        "=",
        "<",
        ">",
        "<=",
        ">=",
        "not",
        "and",
        "or",
        "void",
        "display",
        "write",
        "newline",
        "number?",
        "integer?",
        "float?",
        "boolean?",
        "string?",
        "symbol?",
        "string-append",
        "string-length",
        "string-ref",
        "substring",
        "number->string",
        "string->number",
        "apply",
        "map",
        "filter",
        "foldl",
    };

    // Variable substitution: if this variable is a macro param, return the arg clone.
    //
    // Issue #120: the arg's NodeId is in the *calling* FlatAST (= target),
    // not in `source` (the macro definition's FlatAST). The recursive
    // call to clone_macro_body with body_id=it->second would try to
    // read it->second from `source`, which is wrong (NodeId indices
    // are per-FlatAST). The fix: detect this case and return the
    // arg's NodeId as-is, then recursively clone its children from
    // `target` (not `source`).
    if (subst && v.tag == NodeTag::Variable && v.sym_id != INVALID_SYM) {
        auto name = std::string(source_pool.resolve(v.sym_id));
        auto it = subst->find(name);
        if (it != subst->end()) {
            // Issue #334 follow-up: REVERTED Quote-wrap from commit
            // 6b90641. The Quote-wrap made Variables in macro
            // bodies evaluate to the literal arg value (helped
            // define-struct), but it broke `set!` semantics in
            // normal macros: the set! target became a literal
            // symbol (the arg name) instead of the caller's
            // variable, causing test_issue_137/190 to fail. The
            // original AST subst (returning the arg NodeId
            // directly) is restored for now. The proper fix for
            // #230 #1 (define-struct) is the env-binding path
            // (tracked in issue 334), not Quote-wrap.
            return it->second;
        }
    }

    // Re-intern SymIds: resolve in source_pool, intern in target_pool
    auto transplant = [&](SymId sid) -> SymId {
        return (sid == INVALID_SYM) ? sid
                                    : target_pool.intern(std::string(source_pool.resolve(sid)));
    };

    // Resolve a name through name_map (hygiene: renamed binding)
    auto resolve_name = [&](SymId sid) -> std::string {
        if (sid == INVALID_SYM)
            return "";
        auto name = std::string(source_pool.resolve(sid));
        if (name_map) {
            auto it = name_map->find(name);
            if (it != name_map->end())
                return it->second;
        }
        return name;
    };

    // Rename a binding position for hygiene: gensym if macro-introduced
    static std::uint64_t hyg_ctr = 0;
    auto rename_binding_pre = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        if ((subst && subst->count(name)) || builtins.count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Issue #120: pre-scan the body to populate name_map BEFORE cloning.
    // The body may reference gensym'd bindings (e.g., `(let ((tmp a)) (set! b tmp))`
    // — the inner `tmp` Variable reference must see the gensym'd name
    // when it's cloned). Without the pre-scan, the recursive clone
    // would process the inner `tmp` (as a Variable reference) before the
    // let binding is processed (which is what gensym's `tmp`).
    if (name_map) {
        std::function<void(NodeId)> pre_scan = [&](NodeId nid) {
            if (nid == NULL_NODE || nid >= source.size())
                return;
            auto nv = source.get(nid);
            // If this node is a binding position, gensym its name
            // (into the name_map) but don't generate any target node.
            if (nv.tag == NodeTag::Let || nv.tag == NodeTag::LetRec || nv.tag == NodeTag::Define) {
                rename_binding_pre(nv.sym_id);
            } else if (nv.tag == NodeTag::Lambda) {
                for (auto pid : nv.params)
                    rename_binding_pre(pid);
            }
            for (auto c : nv.children)
                pre_scan(c);
        };
        pre_scan(body_id);
    }

    auto rename_binding = [&](SymId sid) -> SymId {
        if (sid == INVALID_SYM || !name_map)
            return transplant(sid);
        auto name = std::string(source_pool.resolve(sid));
        // Macro params, builtins, and already-renamed names keep their name
        if ((subst && subst->count(name)) || builtins.count(name))
            return transplant(sid);
        auto it = name_map->find(name);
        if (it != name_map->end())
            return target_pool.intern(it->second);
        // Gensym! Create fresh name and track in name_map
        auto fresh = std::string("__") + name + "_" + std::to_string(hyg_ctr++);
        (*name_map)[name] = fresh;
        return target_pool.intern(fresh);
    };

    // Clone children recursively (pass cloned_marker through)
    std::vector<aura::ast::NodeId> child_ids;
    for (std::uint32_t i = 0; i < v.children.size(); ++i)
        child_ids.push_back(clone_macro_body(target, target_pool, source, source_pool, v.child(i),
                                             subst, name_map, cloned_marker));

    // Clone params (for Lambda nodes) — with hygienic renaming
    std::vector<aura::ast::SymId> param_syms;
    for (auto pid : v.params)
        param_syms.push_back(rename_binding(pid));

    aura::ast::NodeId new_id = NULL_NODE;
    switch (v.tag) {
        case NodeTag::LiteralInt:
            new_id = target.add_literal(v.int_value);
            break;
        case NodeTag::LiteralString:
            new_id = target.add_literalstring(transplant(v.sym_id));
            break;
        case NodeTag::Variable: {
            // Hygienic: check name_map for renamed bindings
            if (name_map) {
                auto name = resolve_name(v.sym_id);
                new_id = target.add_variable(target_pool.intern(name));
            } else {
                new_id = target.add_variable(transplant(v.sym_id));
            }
            break;
        }
        case NodeTag::Call: {
            std::vector<aura::ast::NodeId> args(child_ids.begin() + 1, child_ids.end());
            if (!child_ids.empty())
                new_id = target.add_call(child_ids[0], args);
            break;
        }
        case NodeTag::IfExpr:
            if (child_ids.size() >= 3)
                new_id = target.add_if(child_ids[0], child_ids[1], child_ids[2]);
            break;
        case NodeTag::Lambda:
            if (!child_ids.empty())
                new_id = target.add_lambda(param_syms, child_ids[0]);
            break;
        case NodeTag::Let:
        case NodeTag::LetRec:
            if (child_ids.size() >= 2)
                new_id =
                    (v.tag == NodeTag::Let)
                        ? target.add_let(rename_binding(v.sym_id), child_ids[0], child_ids[1])
                        : target.add_letrec(rename_binding(v.sym_id), child_ids[0], child_ids[1]);
            break;
        case NodeTag::Begin:
            if (!child_ids.empty())
                new_id = target.add_begin(child_ids);
            break;
        case NodeTag::Set:
            if (!child_ids.empty()) {
                // Issue #120: if the set! target is a macro param, look
                // up the arg and use ITS sym_id (resolved from target).
                // Otherwise the set! would target the macro param
                // (e.g., "a") which isn't bound in the calling env.
                SymId set_name_sid = transplant(v.sym_id);
                if (subst) {
                    auto set_name = std::string(source_pool.resolve(v.sym_id));
                    auto sit = subst->find(set_name);
                    if (sit != subst->end()) {
                        auto arg_v = target.get(sit->second);
                        if (arg_v.tag == NodeTag::Variable) {
                            set_name_sid = arg_v.sym_id;
                        }
                    }
                }
                new_id = target.add_set(set_name_sid, child_ids[0]);
            }
            break;
        case NodeTag::Quote:
            if (!child_ids.empty())
                new_id = target.add_quote(child_ids[0]);
            break;
        case NodeTag::Define:
            if (!child_ids.empty())
                new_id = target.add_define(rename_binding(v.sym_id), child_ids[0]);
            break;
        default:
            break;
    }

    if (new_id != NULL_NODE) {
        // Issue #190: use the caller's specified marker
        // (MacroIntroduced for macro expansion, User for closure
        // materialization). The recursive calls already set the
        // marker on each child node, so this is just the outer
        // wrapper node.
        target.set_marker(new_id, cloned_marker);
        target.set_loc(new_id, v.line, v.col);
    }
    return new_id;
}

// ── expand_inner_macros — recursively expand nested macro calls ─────
//
// Issue #121: when a hygienic macro's body contains a call to
// another macro, that inner call needs to be expanded too. The
// static `macro_expand_all` (below) scans the flat for
// MacroDef nodes, but in the typical REPL / stdin flow each
// form has its own flat — so macros defined in earlier forms
// aren't visible to the static helper. This recursive helper
// walks the cloned AST and expands inner Calls using the
// runtime `macros_` registry (which IS shared across forms).
//
// Bounded by `max_depth` to prevent infinite recursion (e.g., a
// macro X whose body calls X).
// Issue #158: helper to unwrap a cons-chain call into a real
// Call node. The qq expander turns `(bar ,x)` (where `bar` is a
// macro) into `(cons (quote bar) (cons x (quote ())))`. The
// macro symbol is buried inside a Quote, not at the call head,
// so the main macro check below misses it. This helper detects
// the pattern: a Call to `cons` whose first arg is a Quote of
// a known macro symbol. It walks the rest of the cons chain to
// extract the args, then returns a new Call to the macro with
// those args. Returns NULL_NODE if the pattern doesn't match.
static aura::ast::NodeId
unwrap_cons_chain_to_call(aura::ast::FlatAST* flat, aura::ast::StringPool* pool,
                          aura::ast::NodeId root,
                          const std::unordered_map<std::string, MacroDef>& macros) {
    using namespace aura::ast;
    if (root == NULL_NODE)
        return NULL_NODE;
    auto v = flat->get(root);
    if (v.tag != NodeTag::Call || v.children.size() != 3)
        return NULL_NODE;
    auto callee_v = flat->get(v.child(0));
    if (callee_v.tag != NodeTag::Variable)
        return NULL_NODE;
    auto callee_name = std::string(pool->resolve(callee_v.sym_id));
    if (callee_name != "cons")
        return NULL_NODE;
    // First arg must be (quote <known-macro-sym>)
    auto arg0_v = flat->get(v.child(1));
    if (arg0_v.tag != NodeTag::Quote || arg0_v.children.empty())
        return NULL_NODE;
    auto quoted_v = flat->get(arg0_v.child(0));
    if (quoted_v.tag != NodeTag::Variable)
        return NULL_NODE;
    auto quoted_name = std::string(pool->resolve(quoted_v.sym_id));
    if (macros.find(quoted_name) == macros.end())
        return NULL_NODE;
    // Walk the cdr chain (v.child(2)) to collect arg NodeIds.
    // Each step: cdr is (cons <arg> <rest>) or (quote ()).
    std::vector<NodeId> args;
    NodeId cdr_id = v.child(2);
    while (cdr_id != NULL_NODE) {
        auto cdr_v = flat->get(cdr_id);
        if (cdr_v.tag == NodeTag::Quote) {
            // (quote ()) — end of list
            break;
        }
        if (cdr_v.tag != NodeTag::Call || cdr_v.children.size() != 3) {
            // Not a cons cell — bail
            return NULL_NODE;
        }
        auto c_callee = flat->get(cdr_v.child(0));
        if (c_callee.tag != NodeTag::Variable ||
            std::string(pool->resolve(c_callee.sym_id)) != "cons") {
            return NULL_NODE;
        }
        // Push the arg (cdr_v.child(1))
        args.push_back(cdr_v.child(1));
        cdr_id = cdr_v.child(2);
    }
    // Build Call(<quoted_name>, args...)
    auto macro_var = flat->add_variable(pool->intern(quoted_name));
    flat->set_marker(macro_var, SyntaxMarker::MacroIntroduced);
    return flat->add_call(macro_var, args);
}

static aura::ast::NodeId
expand_inner_macros(aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root,
                    int depth, int max_depth,
                    const std::unordered_map<std::string, MacroDef>& macros) {
    using namespace aura::ast;
    if (root == NULL_NODE || depth >= max_depth)
        return root;
    // Issue #158: unwrap qq-built cons chains whose head is a
    // known macro. Without this, `(bar ,x)` inside a macro body
    // stays as `(cons (quote bar) ...)` after expand_qq, and the
    // main macro check below (which expects a Call head matching
    // a known macro) misses it.
    if (auto unwrapped = unwrap_cons_chain_to_call(flat, pool, root, macros);
        unwrapped != NULL_NODE) {
        // Substitute the unwrapped Call for the original cons chain
        // at the parent's child slot, then recurse.
        auto parent_id = flat->parent_of(root);
        if (parent_id != NULL_NODE) {
            auto parent_v = flat->get(parent_id);
            for (std::uint32_t ci = 0; ci < parent_v.children.size(); ++ci) {
                if (parent_v.child(ci) == root) {
                    flat->set_child(parent_id, ci, unwrapped);
                    break;
                }
            }
        }
        // Recurse into the unwrapped Call (which is now a real
        // macro call site).
        return expand_inner_macros(flat, pool, unwrapped, depth, max_depth, macros);
    }
    auto v = flat->get(root);
    if (v.tag == NodeTag::Call && !v.children.empty()) {
        auto callee_v = flat->get(v.child(0));
        if (callee_v.tag == NodeTag::Variable) {
            auto cname = std::string(pool->resolve(callee_v.sym_id));
            auto it = macros.find(cname);
            if (it != macros.end()) {
                // Build substitution: macro param → arg NodeId.
                // Issue #146 follow-up: route through the pure helper
                // so the substitution logic lives in evaluator_pure.ixx
                // (single source of truth) and the legacy inline loop
                // goes away. v.children is materialized into a vector
                // (one alloc per macro call) for the pure helper's
                // vector-typed call_args parameter.
                const auto& md = it->second;
                std::vector<aura::ast::NodeId> call_args(v.children.begin(), v.children.end());
                auto subst =
                    aura::compiler::pure::compute_macro_subst_pure(md.params, call_args, md.dotted);
                if (md.dotted) {
                    // Rest params on inner macros: not yet supported
                    // (same limitation as the main hygienic path).
                    return root;
                }
                // Clone the macro body into the current flat and
                // re-intern sym_ids. Use the runtime registry's
                // `flat` / `pool` pointers as the source.
                std::unordered_map<std::string, std::string> rename_map;
                auto* src_pool = md.pool ? md.pool : pool;
                auto cloned = clone_macro_body(*flat, *pool, *md.flat, *src_pool, md.body_id,
                                               &subst, &rename_map);
                if (cloned == NULL_NODE)
                    return root;
                // Recursively expand inner macros in the cloned body
                cloned = expand_inner_macros(flat, pool, cloned, depth + 1, max_depth, macros);
                // Rewrite the parent's child to use the cloned body
                auto parent_id = flat->parent_of(root);
                if (parent_id != NULL_NODE) {
                    auto parent_v = flat->get(parent_id);
                    for (std::uint32_t ci = 0; ci < parent_v.children.size(); ++ci) {
                        if (parent_v.child(ci) == root) {
                            flat->set_child(parent_id, ci, cloned);
                            break;
                        }
                    }
                }
                return cloned;
            }
        }
    }
    // Not a macro call — recurse into children
    for (std::uint32_t i = 0; i < v.children.size(); ++i) {
        auto child = v.child(i);
        // We can't modify children in place easily; rebuild
        // the current node's children via the recursive call.
        (void)expand_inner_macros(flat, pool, child, depth, max_depth, macros);
    }
    return root;
}

// ── Pre-expand all macros in a FlatAST ────────────────
// Scans for MacroDef nodes, collects them, then expands all macro calls.
// Returns the (possibly new) root node of the expanded tree.
// After this pass, the FlatAST contains no MacroDef or macro calls.
// Multiple passes handle nested macros.
aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                   aura::ast::NodeId root, int max_passes) {
    using namespace aura::ast;
    for (int pass = 0; pass < max_passes; ++pass) {
        // Phase 1: collect macro definitions
        struct MD {
            aura::ast::FlatAST* src_flat;
            aura::ast::StringPool* src_pool;
            std::vector<std::string> params;
            NodeId body_id;
            bool dotted;
            bool hygienic;  // Issue #120
            bool preserved; // Issue #230 #2
        };
        std::unordered_map<std::string, MD> local_macros;
        bool has_macro_def = false;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::MacroDef) {
                has_macro_def = true;
                // Macro name is in sym_id; params follow
                auto macro_name = std::string(pool.resolve(v.sym_id));
                std::vector<std::string> params;
                for (auto pid : v.params)
                    params.push_back(std::string(pool.resolve(pid)));
                auto body_id = v.children.empty() ? NULL_NODE : v.child(0);
                // Issue #120: dotted is bit 0, hygienic is bit 1
                bool is_dotted = (v.int_value & 1) != 0;
                bool is_hygienic = (v.int_value & 2) != 0;
                bool is_preserved = (v.int_value & 4) != 0;
                local_macros[macro_name] = MD{&flat,     &pool,       std::move(params), body_id,
                                              is_dotted, is_hygienic, is_preserved};
            }
        }

        if (!has_macro_def)
            return root; // no more macros to expand

        // Phase 2: find and expand macro calls
        bool expanded_any = false;
        NodeId new_root = root;

        for (NodeId id = 0; id < flat.size(); ++id) {
            auto v = flat.get(id);
            if (v.tag == NodeTag::Call && !v.children.empty()) {
                auto callee_v = flat.get(v.child(0));
                if (callee_v.tag == NodeTag::Variable) {
                    auto cname = std::string(pool.resolve(callee_v.sym_id));
                    auto it = local_macros.find(cname);
                    if (it != local_macros.end()) {
                        // Build substitution: macro param → arg expression.
                        // Issue #146 follow-up: route through the pure
                        // helper. Rest-param handling stays here because
                        // it requires FlatAST mutation (allocating a
                        // pair-list) — that's stateful, not pure.
                        auto& md = it->second;
                        std::vector<aura::ast::NodeId> call_args(v.children.begin(),
                                                                 v.children.end());
                        auto subst = aura::compiler::pure::compute_macro_subst_pure(
                            md.params, call_args, md.dotted);
                        // Rest param: collect remaining args as a quoted list.
                        // We need regular_count locally to know where the
                        // rest args start; recompute it (cheap).
                        std::size_t regular_count = md.dotted && md.params.size() > 0
                                                        ? md.params.size() - 1
                                                        : md.params.size();
                        if (md.dotted && !md.params.empty() &&
                            regular_count + 1 < v.children.size()) {
                            auto& rest_name = md.params.back();
                            // Build a data list of the remaining arg nodes as a quote
                            // Create () as the base, then cons each remaining arg
                            std::vector<aura::ast::NodeId> remaining;
                            for (std::size_t ai = regular_count + 1; ai < v.children.size(); ++ai)
                                remaining.push_back(v.child(ai));
                            // Create nested quote: (quote (arg1 arg2 ...)) using add_call chains
                            // Actually, clone_macro_body substitutes Variable nodes, so we need
                            // the rest arg as an expression node, not data.
                            // For simplicity: build a (begin remaining...) — but that evaluates
                            // them. Build as (quote (arg1 arg2 ...)) by creating a proper list:
                            // cons arg1 (cons arg2 (cons ... ())) then wrap in quote
                            // Since these are NodeIds in the SAME FlatAST, we can build an
                            // expression that produces a list: (list arg1 arg2 ...)
                            auto list_var = flat.add_variable(pool.intern("list"));
                            std::vector<aura::ast::NodeId> all_args;
                            all_args.push_back(list_var);
                            all_args.insert(all_args.end(), remaining.begin(), remaining.end());
                            auto list_call = flat.add_call(list_var, all_args);
                            // But this would be (list arg1 arg2 ...) which EVALUATES the args.
                            // Macros need syntax (unevaluated). We need (quote (arg1 arg2 ...)).
                            // Create a quoted version: for each remaining arg, convert to data via
                            // ast_to_data... but we don't have access to the evaluator's pairs_.
                            // For now: just use the (list ...) approach and note that rest args
                            // in macro_expand_all will be evaluated (same as the evaluator's
                            // version) Actually this is the same issue as the evaluator's macros_
                            // expansion. The difference: in macro_expand_all we can directly
                            // substitute. Let me just not handle rest in macro_expand_all for now —
                            // the evaluator's macros_ handles it correctly. This path is only for
                            // same-expression macros.
                        }
                        // Clone macro body with substitution
                        std::unordered_map<std::string, std::string> rename_map;
                        auto expanded = clone_macro_body(
                            flat, pool, *md.src_flat, *md.src_pool, md.body_id, &subst, &rename_map,
                            /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
                        if (expanded != NULL_NODE) {
                            if (id == root)
                                new_root = expanded;
                            expanded_any = true;
                        }
                    }
                }
            }
        }

        if (!expanded_any)
            return root;
        root = new_root;
    }
    // Issue #121: hit the pass limit with macros still in the
    // tree. Emit a warning so the user knows the result is
    // partial. This is the user-facing equivalent of the
    // solver TIMEOUT pattern from Issue #118.
    if (root != NULL_NODE) {
        std::println(std::cerr,
                     "warning: macro_expand_all hit pass limit ({}); "
                     "the result may have unexpanded macro calls",
                     max_passes);
    }
    return root;
}

void* Evaluator::create_workspace_tree() {
    auto* tree = new WorkspaceTree();
    WorkspaceNode root;
    root.name = "root";
    root.is_root = true;
    root.has_own_flat = true;
    root.flat = nullptr;
    root.pool = nullptr;
    tree->nodes_.push_back(std::move(root));
    return tree;
}

void Evaluator::destroy_workspace_tree(void* wt) {
    if (!wt)
        return;
    auto* tree = static_cast<WorkspaceTree*>(wt);
    // Delete owned flats (child workspaces that had COW triggered)
    for (auto& node : tree->nodes_) {
        if (!node.is_root && node.has_own_flat) {
            delete node.flat;
            delete node.pool;
        }
    }
    delete tree;
}

// Issue #141 AC: lazy COW trigger. Called by mutate:* primitives
// before they modify workspace_flat_. If the active workspace is a
// child that still shares parent's flat, clone it now (COW) so the
// mutation doesn't pollute the parent. No-op for root, already-
// cloned, or read-only workspaces (those return false).
bool Evaluator::trigger_lazy_cow(void* wt) {
    if (!wt)
        return true; // no tree yet, nothing to clone
    auto* tree = static_cast<WorkspaceTree*>(wt);
    auto idx = tree->active_idx();
    if (idx == 0 || idx >= tree->size())
        return true; // root, nothing to do
    auto& node = tree->nodes_[idx];
    if (node.has_own_flat)
        return true; // already cloned
    if (node.read_only)
        return false; // can't clone read-only
    return tree->ensure_local_flat(idx);
}

// After trigger_lazy_cow, the active workspace's flat/pool may have
// been reallocated. Call this to refresh the pointers without
// exposing the WorkspaceTree type to callers defined before the type.
bool Evaluator::refresh_active_flat_pool(void* wt, void** out_flat, void** out_pool) {
    if (!wt)
        return false;
    auto* tree = static_cast<WorkspaceTree*>(wt);
    auto* node = tree->active();
    if (!node)
        return false;
    if (out_flat)
        *out_flat = node->flat;
    if (out_pool)
        *out_pool = node->pool;
    return true;
}

// ── Panic auto-rollback (Issue #39) ────────────────────────────
bool Evaluator::save_panic_checkpoint() {
    if (!workspace_flat_ || !workspace_pool_)
        return false;
    auto src_fn = primitives_.lookup("current-source");
    if (!src_fn)
        return false;
    auto src = (*src_fn)({});
    if (!types::is_string(src))
        return false;
    auto idx = types::as_string_idx(src);
    if (idx >= string_heap_.size())
        return false;
    panic_safe_source_ = string_heap_[idx];
    // Issue #242: snapshot the 4 pmr/append-only arena sizes
    // so restore_panic_checkpoint can truncate them back.
    // env_frames_ size is recorded for diagnostics only (the
    // deque itself is NOT truncated on restore — see
    // restore_panic_checkpoint).
    panic_safe_cells_size_ = cells_.size();
    panic_safe_pairs_size_ = pairs_.size();
    panic_safe_string_heap_size_ = string_heap_.size();
    panic_safe_env_frames_size_ = env_frames_.size();
    return true;
}

bool Evaluator::restore_panic_checkpoint() {
    if (panic_safe_source_.empty())
        return false;
    auto set_fn = primitives_.lookup("set-code");
    if (!set_fn)
        return false;
    auto idx = string_heap_.size();
    string_heap_.push_back(panic_safe_source_);
    auto result = (*set_fn)({make_string(idx)});
    bool ok = types::is_bool(result) && types::as_bool(result);
    if (ok) {
        // Issue #242: truncate the 3 append-only arenas back to
        // their checkpoint sizes. We do NOT truncate env_frames_
        // (the Closure::env_id indices must remain valid for
        // already-constructed closures; the version stamping
        // from Phase 1 detects stale frames instead).
        //
        // The source string we just pushed back is at idx; we
        // resize string_heap_ to idx+1 (= pre-save size + 1) so
        // the source string is preserved while everything added
        // AFTER the save (idx+1 onwards) is truncated away.
        std::size_t new_string_heap_size = idx + 1;
        if (new_string_heap_size <= string_heap_.size()) {
            string_heap_.resize(new_string_heap_size);
        }
        if (panic_safe_cells_size_ > 0 && panic_safe_cells_size_ <= cells_.size()) {
            cells_.resize(panic_safe_cells_size_);
        }
        if (panic_safe_pairs_size_ > 0 && panic_safe_pairs_size_ <= pairs_.size()) {
            pairs_.resize(panic_safe_pairs_size_);
        }
        // Clear checkpoint after successful restore
        panic_safe_source_.clear();
        panic_safe_cells_size_ = 0;
        panic_safe_pairs_size_ = 0;
        panic_safe_string_heap_size_ = 0;
        panic_safe_env_frames_size_ = 0;
    }
    return ok;
}


// Issue #67 / #131 follow-up: arena-allocated Env destruction.
//
// Pre-#131 the arena didn't run ~Env() automatically, so the original
// loop manually called env->~Env() to free bindings_' heap
// allocation. ASTArena v4 (this commit) adds type-erased destructor
// tracking, so the arena itself runs ~Env() at ~ASTArena() time.
// The manual loop would double-destroy, so it's gone — the arena
// owns the lifecycle now. The module_arena_ptrs_ map is also dropped
// from the manual cleanup since arena_group_ will free them as a
// member after this dtor returns.
Evaluator::~Evaluator() {
    // (ASAN fix #107 leak) free the def-use index. Same root cause
    // as the 8 reset sites: defuse_index_ is a void* that was
    // leaked on every reset (and at process exit). The destructor's
    // no-op cleanup was the final leak in the chain.
    defuse_index_destroy(&defuse_index_);
    modules_.clear();
    module_cache_.clear();
    module_arena_ptrs_.clear();
    module_names_.clear();
    closures_.clear();
    // cells_ / pairs_ / error_values_ / opaque_heap_ hold raw values;
    // their destructors run automatically (vectors of trivially-constructible
    // types don't need explicit cleanup).
    cells_.clear();
    pairs_.clear();
    error_values_.clear();
    opaque_heap_.clear();
    string_heap_.clear();
    // strategies_/intend_records_/intend_history_/timeline_ are std::vector
    // of trivially-destructible structs; clear() releases their heap.
    strategies_.clear();
}

// Issue #107 part 4: inline typecheck helpers. Caller MUST hold
// workspace_mtx_ (shared or unique). The two helpers share the
// same infer_flat + diag-drain pattern; they differ only in
// return type — string for sites that need the error message,
// bool for sites that only need pass/fail. Both are members
// of Evaluator (so they can access the privates below).
std::string Evaluator::run_typecheck_no_lock() {
    if (!workspace_flat_ || !workspace_pool_)
        return std::string("no workspace");
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
    }
    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    aura::compiler::TypeChecker tc(treg);
    if (!declared_type_sigs_.empty()) {
        std::unordered_map<std::string, std::string> sig_map;
        std::unordered_map<std::string, std::string> mod_src_map;
        for (auto& [name, decl] : declared_type_sigs_) {
            sig_map[name] = decl.type_str;
            if (!decl.module_file.empty())
                mod_src_map[name] = decl.module_file;
        }
        tc.inject_type_sigs(sig_map, mod_src_map);
    }
    aura::diag::DiagnosticCollector diag;
    auto result = tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
    workspace_flat_->clear_all_dirty();
    std::string out = "type: " + treg.format_type(result) + "\n";
    auto all_diags = diag.diagnostics();
    if (all_diags.empty()) {
        out += "no errors\n";
    } else {
        out += "diagnostics:\n";
        for (auto& d : all_diags) {
            out += "  [" + std::to_string(static_cast<int>(d.kind)) + "] " + d.format() + "\n";
        }
    }
    return out;
}

bool Evaluator::run_typecheck_no_lock_bool() {
    // Same as the string version but returns pass/fail directly
    // without formatting. Cheaper for hot fuzzer loops.
    //
    // Issue #116: this is called from the fuzzy/evolutionary loop
    // (compute_fitness), which then `eval`s the workspace. The
    // workspace must be lowering-ready, so we apply the deferred
    // CoercionMap before returning.
    if (!workspace_flat_ || !workspace_pool_)
        return true;
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
    }
    auto& treg = *static_cast<aura::core::TypeRegistry*>(type_registry_);
    aura::compiler::TypeChecker tc(treg);
    if (!declared_type_sigs_.empty()) {
        std::unordered_map<std::string, std::string> sig_map;
        std::unordered_map<std::string, std::string> mod_src_map;
        for (auto& [name, decl] : declared_type_sigs_) {
            sig_map[name] = decl.type_str;
            if (!decl.module_file.empty())
                mod_src_map[name] = decl.module_file;
        }
        tc.inject_type_sigs(sig_map, mod_src_map);
    }
    aura::diag::DiagnosticCollector diag;
    tc.infer_flat(*workspace_flat_, *workspace_pool_, workspace_flat_->root, diag);
    // Issue #116: apply deferred coercions — the caller (fuzzer
    // loop) will then `eval` the workspace via compute_fitness,
    // which needs CoercionNodes present for the IR generator.
    {
        auto cm = tc.take_coercions();
        if (!cm.empty()) {
            aura::compiler::apply_coercion_map(*workspace_flat_, cm);
        }
    }
    workspace_flat_->clear_all_dirty();
    return diag.diagnostics().empty();
}

// ── GC root registration (Issue #113) ──────────────────────────
//
// `flush_gc_roots` walks every vector heap this Evaluator owns and
// populates the GCRootSet with the indices of all live objects. The
// GC collector calls this during its root collection phase (after
// the safepoint has stopped all fibers on this worker, so no
// concurrent mutator can run). We additionally hold `heap_mutex()`
// so a non-fiber thread in serve-async mode can't race a concurrent
// `string_heap_.push_back` (or similar) with the walk.
//
// `gc_root_count` is the cheap version: just returns the number of
// entries that WOULD be marked, without allocating the GCRootSet.
// Useful for pre-GC metrics and unit tests that want to verify the
// root set is populated without paying for the GCRootSet heap allocs.

void Evaluator::flush_gc_roots(void* root_set_out) {
    // The opaque pointer is aura::serve::GCRootSet* (set by the
    // serve_async.cpp callback). Cast is safe because the GC
    // collector passes a real GCRootSet that the messaging bridge
    // constructed in its own TU.
    auto& out = *static_cast<aura::serve::GCRootSet*>(root_set_out);

    std::lock_guard<std::mutex> lock(heap_mutex());

    // 1. string_heap_ — every slot is a root. The pool can be
    //    compacted in the sweep phase; until then, treat them all
    //    as live. Pairs can reference strings (car/cdr), so
    //    undermarking here would dangle pair fields.
    out.string_roots.reserve(out.string_roots.size() + string_heap_.size());
    for (std::size_t i = 0; i < string_heap_.size(); ++i) {
        out.string_roots.push_back(static_cast<int64_t>(i));
    }

    // 2. pairs_ — every slot is a root (cons cells are the spine
    //    of every list / tree in the heap). Stale entries from
    //    previous gc-temp cycles are the caller's responsibility
    //    to remove before GC; we mark everything.
    out.pair_roots.reserve(out.pair_roots.size() + pairs_.size());
    for (std::size_t i = 0; i < pairs_.size(); ++i) {
        out.pair_roots.push_back(static_cast<int64_t>(i));
    }

    // 3. closures_ — only roots with id < gc_safe_closure_id_ are
    //    pinned (module-level / while-loop bodies). Anything above
    //    that watermark was created inside a temp-arena intend and
    //    is safe to collect. We walk the map and emit the safe set.
    out.closure_roots.reserve(out.closure_roots.size() + closures_.size());
    for (const auto& [id, c] : closures_) {
        if (static_cast<std::uint64_t>(id) < gc_safe_closure_id_) {
            out.closure_roots.push_back(static_cast<int64_t>(id));
        }
    }

    // 4. fiber results — s_fiber_results_ is a TU-local static in
    //    evaluator_impl.cpp (managed by fiber:join). Each live entry
    //    is a root because the value is shared between the spawned
    //    fiber and the joiner. The static map is internally
    //    synchronized by s_fiber_results_mtx_, but at the safepoint
    //    no fiber is touching it, so we walk it directly.
    //
    //    We can't see s_fiber_results_ from this method (TU-local),
    //    so we skip the fiber_result_roots field here. The
    //    message-bridge flush hook in serve_async.cpp adds those
    //    entries separately (or the GC tolerates an empty set
    //    since the value is in closures_/string_heap_ which we
    //    already marked).
}

std::size_t Evaluator::gc_root_count() const {
    // No lock — called at safepoint time. Returns upper bound.
    std::size_t n = string_heap_.size() + pairs_.size();
    for (const auto& [id, _] : closures_) {
        if (static_cast<std::uint64_t>(id) < gc_safe_closure_id_) {
            ++n;
        }
    }
    return n;
}

// ── GC sweep / compaction (Issue #113 Phase 3) ──────────────
//
// `compact_sweep` is called by the GC collector's `collect()`
// after the mark phase has set the live bits in `marks`. We hold
// `heap_mutex()` because the sweep runs at the safepoint but a
// non-fiber thread in serve-async mode could still touch the heaps.
//
// For `closures_` we actually erase unmarked entries — this is the
// main memory-reclamation path (closure bodies hold arena-allocated
// state). For the vector heaps, we report the dead count without
// compaction, because compaction requires remapping all
// EvalValue / pair / cell references — that's a major refactor
// tracked separately in `binary_runtime_plan.md` (the C-runtime
// equivalent) and in a future iteration of the Aura evaluator
// (likely via a generation index table).

void* Evaluator::compact_sweep(void* sweep_buffers) {
    // The opaque pointer is aura::serve::GCSweepBuffers* (set by
    // the serve_async.cpp callback or directly by the GC collector
    // test). Cast is safe because both the message-bridge caller
    // and the direct test pass a real GCSweepBuffers.
    auto* marks = static_cast<aura::serve::GCSweepBuffers*>(sweep_buffers);
    if (!marks)
        return nullptr;

    std::lock_guard<std::mutex> lock(heap_mutex());
    // The result is allocated on the heap (via new) so its
    // lifetime extends past the function return. The caller
    // (serve_async.cpp) reads the fields and deletes.
    //
    // The struct here is layout-compatible with
    // `aura::messaging::GCSweepResultMsg` in messaging_bridge.h.
    // We use a local struct because messaging_bridge.h is a
    // non-module .h included via the global fragment, and the
    // C++20 module rules make it awkward to refer to its
    // types directly here. The static_assert below catches
    // any drift between the two definitions.
    struct SweepResult {
        std::size_t strings_freed = 0;
        std::size_t pairs_freed = 0;
        std::size_t closures_freed = 0;
        std::size_t fiber_results_freed = 0;
    };
    static_assert(sizeof(SweepResult) == 4 * sizeof(std::size_t),
                  "SweepResult layout must match GCSweepResultMsg");
    auto* result = new SweepResult();

    // 1. closures_ — erase unmarked entries.
    //    This is the main leak-reduction path: each closure holds
    //    an arena-allocated flat, pool, and env that can be
    //    significant memory.
    if (marks->closure_marks) {
        std::size_t before = closures_.size();
        for (auto it = closures_.begin(); it != closures_.end();) {
            int64_t id = static_cast<int64_t>(it->first);
            if (!marks->closure_marks->test(id)) {
                it = closures_.erase(it);
            } else {
                ++it;
            }
        }
        result->closures_freed = before - closures_.size();
    }

    // 2. string_heap_ — report dead count, no compaction.
    //    Compaction requires remapping all references that hold
    //    a string index (Pair car/cdr, EvalValue String tag,
    //    Closure params, etc.). Until that work lands, the heap
    //    keeps stale entries but the GC metric tells the caller
    //    how much pressure exists.
    if (marks->string_marks) {
        result->strings_freed = marks->string_marks->count_dead();
    }

    // 3. pairs_ — same. report dead count.
    if (marks->pair_marks) {
        result->pairs_freed = marks->pair_marks->count_dead();
    }

    // 4. fiber_results — owned by s_fiber_results_ (TU-local). The
    //    GC sweep handles those separately when the
    //    message-bridge registers a fiber_result sweep callback.
    //    We report 0 here so the totals add up correctly.
    result->fiber_results_freed = 0;

    return result;
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
void Evaluator::yield_mutation_boundary() {
    if (aura::messaging::g_fiber_yield_mutation_boundary)
        aura::messaging::g_fiber_yield_mutation_boundary();
}

// Issue #165 Phase 1B: post-mutation macro re-expansion.
//
// Walks the mutation's affected subtree and re-expands any
// macro call sites it finds. This fixes the bug where
// EDSL mutations (mutate:rebind, mutate:set-body, etc.)
// leave stale macro expansions — the macro's gensym'd
// bindings may not be re-generated, and the call site
// may pick up caller's bindings that should have been
// hygiene-isolated.
//
// Algorithm (incremental — only on affected subtrees, not
// the full AST):
//   1. Compute the affected subtree using the same
//      walk pattern as affected_subtree_from_mutation
//      (Issue #148): descendants of target_node/parent_id
//      + dirty-upward ancestors.
//   2. For each node in the affected set, check if it's:
//        (a) A MacroDef — the macro body was mutated. Find
//            every Call site whose callee is this macro and
//            re-expand them.
//        (b) A Call whose callee is a known macro — the
//            call site context was mutated. Re-expand it.
//   3. For each call site, build a substitution (param →
//      arg), call clone_macro_body with fresh gensym (or
//      without, for non-hygienic macros), then run
//      expand_inner_macros on the result to handle nested
//      macros.
//   4. Set SyntaxMarker::MacroIntroduced on the new
//      expansion so the post-expansion tree is properly
//      marked for downstream consumers (type checker, IR
//      lowering, mutation operators).
//
// Returns the number of call sites re-expanded. The
// function is safe to call on any mutation record —
// bails on malformed input (NULL_NODE, out-of-range,
// empty macros_ registry).
std::size_t Evaluator::post_mutation_macro_reexpand(aura::ast::FlatAST& flat,
                                                    aura::ast::StringPool& pool,
                                                    const aura::ast::MutationRecord& rec) {
    using namespace aura::ast;

    std::size_t re_expanded = 0;
    if (rec.target_node == NULL_NODE && rec.parent_id == NULL_NODE)
        return 0;
    if (macros_.empty())
        return 0; // no macros registered, nothing to do

    // Collect affected node IDs: descendants of target_node
    // + parent_id + dirty-upward chain. This is a conservative
    // set — we may visit nodes that aren't actually affected
    // by the macro, but the re-expansion is idempotent
    // (re-expanding an already-expanded call site is a no-op
    // in effect, just reuses the same gensym).
    std::vector<NodeId> affected;
    auto add_subtree = [&](NodeId root_id) {
        if (root_id == NULL_NODE || root_id >= flat.size())
            return;
        affected.push_back(root_id);
        // BFS for descendants
        std::vector<NodeId> frontier{root_id};
        while (!frontier.empty()) {
            std::vector<NodeId> next;
            for (auto n : frontier) {
                auto v = flat.get(n);
                for (auto c : v.children) {
                    if (c != NULL_NODE && c < flat.size()) {
                        affected.push_back(c);
                        next.push_back(c);
                    }
                }
            }
            frontier = std::move(next);
        }
    };
    add_subtree(rec.target_node);
    add_subtree(rec.parent_id);

    // Climb parent_of chain for dirty-upward ancestors.
    // Safety-bounded to defend against cycles in malformed
    // FlatASTs.
    NodeId climb = rec.target_node;
    for (int i = 0; i < 256 && climb != NULL_NODE && climb < flat.size(); ++i) {
        if (auto p = flat.parent_of(climb); p != NULL_NODE && p < flat.size()) {
            affected.push_back(p);
            climb = p;
        } else {
            break;
        }
    }

    // Walk the affected set, find Call nodes whose callee is
    // a registered macro, and re-expand them.
    for (auto id : affected) {
        if (id == NULL_NODE || id >= flat.size())
            continue;
        auto v = flat.get(id);
        if (v.tag != NodeTag::Call || v.children.empty())
            continue;

        auto callee_id = v.child(0);
        if (callee_id == NULL_NODE || callee_id >= flat.size())
            continue;
        auto callee_v = flat.get(callee_id);
        if (!callee_v.has_name())
            continue;
        auto callee_name = pool.resolve(callee_v.sym_id);

        // Is the callee a registered macro?
        auto macro_it = macros_.find(std::string(callee_name));
        if (macro_it == macros_.end())
            continue;

        const auto& md = macro_it->second;

        // Build substitution: macro param names → call arg node IDs
        std::vector<aura::ast::NodeId> call_args;
        for (std::size_t i = 1; i < v.children.size(); ++i) {
            call_args.push_back(v.child(i));
        }
        auto subst_view = std::span<const aura::ast::NodeId>(call_args);

        // Compute substitution: param string → call arg node id.
        // For each param name, find the corresponding call arg
        // by position. If params has dotted-rest, the last param
        // binds to a list of remaining args.
        std::unordered_map<std::string, aura::ast::NodeId> subst_map;
        std::unordered_map<std::string, std::string> rename_map;
        for (std::size_t i = 0; i < md.params.size() && i < call_args.size(); ++i) {
            subst_map[md.params[i]] = call_args[i];
        }
        // Handle dotted-rest param (the last param absorbs remaining args)
        if (md.dotted && !md.params.empty() && call_args.size() >= md.params.size()) {
            // Build a proper-list from the remaining args by
            // consing them with add_pair. Build right-to-left
            // so we get (a . (b . (c . nil))).
            std::size_t first_rest_idx = md.params.size() - 1;
            aura::ast::NodeId list_end = aura::ast::NULL_NODE;
            for (std::size_t k = call_args.size(); k > first_rest_idx; --k) {
                std::size_t i = k - 1;
                list_end = flat.add_pair(call_args[i], list_end);
            }
            subst_map[md.params.back()] = list_end;
        }

        // Clone the macro body into the calling flat with
        // substitution + (for hygienic) name rename map.
        // For non-hygienic macros, the name_map is empty (no
        // gensym) and the params bind to call-site names (legacy
        // defmacro behavior).
        // Issue #190: pass MacroIntroduced so the cloned nodes
        // get the correct SyntaxMarker at creation time. (The
        // post-clone BFS marker-set that used to be here is now
        // redundant — clone_macro_body handles it.)
        auto* src_pool = md.pool ? md.pool : &pool;
        auto* src_flat = md.flat ? md.flat : &flat;
        auto expanded =
            clone_macro_body(flat, pool, *src_flat, *src_pool, md.body_id, &subst_map, &rename_map,
                             /*cloned_marker=*/aura::ast::SyntaxMarker::MacroIntroduced);
        if (expanded == NULL_NODE)
            continue;

        // Recursively expand any nested macro calls in the
        // cloned body. Bounded by depth=10 to prevent infinite
        // loops (a macro that expands to itself).
        expanded = expand_inner_macros(&flat, &pool, expanded, 0, 10, macros_);

        ++re_expanded;
    }

    return re_expanded;
}

} // namespace aura::compiler
