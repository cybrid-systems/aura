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



// ── ADT state now in adt_runtime_ (refactor Step 2.3, FFI pattern) ───────
// The old global g_adt_constructors + AdtCtorEntry struct have been
// removed. Per-Evaluator state is in adt_runtime_ (see adt_runtime.ixx/ _impl).
// Lookups and registration now go through it (see Env::lookup and ctor wiring below).
// Parser (parse_datatype) still populates via the new runtime in full extraction.






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



} // namespace aura::compiler
