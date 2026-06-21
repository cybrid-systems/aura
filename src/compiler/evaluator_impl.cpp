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
    Evaluator& ev);
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
void register_memory_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                                std::function<void()> destroy_defuse_index);
void register_policy_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_eval_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                              std::function<EvalValue(const std::string&, const std::string&)> mev,
                              std::function<void()> destroy_defuse_index);
void register_type_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_hot_swap_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_diagnostic_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_module_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_file_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_runtime_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_test_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_misc_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_control_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_char_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_mutation_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
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

struct MacroDef;
aura::ast::NodeId
clone_macro_body(aura::ast::FlatAST& target, aura::ast::StringPool& target_pool,
                 aura::ast::FlatAST& source, aura::ast::StringPool& source_pool,
                 aura::ast::NodeId body_id,
                 const std::unordered_map<std::string, aura::ast::NodeId>* subst = nullptr,
                 std::unordered_map<std::string, std::string>* name_map = nullptr,
                 aura::ast::SyntaxMarker cloned_marker = aura::ast::SyntaxMarker::User);
aura::ast::NodeId
expand_inner_macros(aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root,
                    int depth, int max_depth,
                    const std::unordered_map<std::string, MacroDef>& macros);


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
void defuse_index_destroy(void** slot);

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
    register_all_primitives();
}

void Evaluator::register_adt_ctor(const std::string& ctor_name, types::EvalValue tag_str,
                                  int field_count) {
    const auto slot = primitives_.slot_count();
    auto body = [this, tag_str](const auto& args) -> EvalValue {
        types::EvalValue rest = make_void();
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            auto pid = static_cast<std::uint64_t>(pairs_.size());
            pairs_.push_back({*it, rest});
            rest = make_pair(pid);
        }
        auto pid = static_cast<std::uint64_t>(pairs_.size());
        pairs_.push_back({tag_str, rest});
        return make_pair(pid);
    };
    adt_runtime_.register_dynamic_ctor(prim_registrar(), ctor_name, std::move(body), field_count,
                                       slot);
}

types::EvalValue Evaluator::make_adt_zero_arg_ctor(types::EvalValue tag_str) {
    types::EvalValue rest = make_void();
    auto cid = static_cast<std::uint64_t>(pairs_.size());
    pairs_.push_back({tag_str, rest});
    return make_pair(cid);
}



std::optional<PrimFn> Primitives::lookup(const std::string& n) const {
    // The pre (!n.empty()) is on the declaration in evaluator.ixx.
    auto i = table_.find(n);
    return i != table_.end() ? std::optional(i->second) : std::nullopt;
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
        prim_registrar(),
        &string_heap_, &opaque_heap_, &coverage_counters_);

    // Step 2.3 + 5.2 hygiene: wire ADT using exact same signature + coverage
    // counters as ffi_runtime_ (for consistency across extractions; modeled on FFI).
    adt_runtime_.register_primitives(
        prim_registrar(),
        &string_heap_, &opaque_heap_, &coverage_counters_);

    build_primitive_slots();

    primitives_detail::register_network_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_type_primitives(
        prim_registrar(),
        *this);

    install_defuse_subsystem();

    // Issue #97 Action 1: Hot-swap primitive
    primitives_detail::register_hot_swap_primitives(
        prim_registrar(),
        *this);


    primitives_detail::register_compile_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_messaging_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_synthesize_primitives(
        prim_registrar(),
        *this, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_strategy_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_memory_primitives(
        prim_registrar(),
        *this, [this]() { defuse_index_destroy(&defuse_index_); });


    primitives_detail::register_jit_arena_primitives(
        prim_registrar(),
        *this);


    primitives_detail::register_policy_primitives(
        prim_registrar(),
        *this);

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
