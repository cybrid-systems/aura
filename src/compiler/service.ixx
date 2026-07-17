module;
#include <algorithm>
#include <atomic>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "aura_jit.h"
#include "aura_jit_bridge.h"
#include "lock_order_audit.h"
#include "runtime_shared.h"
#include "value_tags.h" // Issue #181 Cycle 2: v2 string encoding helpers
#include "observability_metrics.h"
#include "observability_snapshot.h"
#include "per_defuse_index.h"
#include <atomic>
#include "messaging_bridge.h"
#include "core/arena_auto_policy_stats.h"
#include "core/gc_hooks.h"
#include "jit_typed_mutation_stats.h"
#include "linear_occurrence_mutate_stats.h"
#include "shape_jit_pass_closedloop_stats.h"
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>
#include "serve/fiber.h"
#include "serve/gc_coordinator.h"
#include "shape.h"
#include "shape_profiler.h"
#include "spec_jit_controller.h"
#include "hash_meta.h" // FNV constants (#901)

export module aura.compiler.service;
import std;
import aura.core;
import aura.compiler.ir_cache_pure;
import aura.compiler.ast_walkers;
import aura.core.type;
import aura.parser.parser;
import aura.core.mutation;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.ir_soa;
import aura.compiler.ir_executor;
import aura.compiler.pass_manager;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.compiler.cache;
import aura.diag;
import aura.core.error; // Issue #807/#808: AuraResult bridge

// ── JIT primitive call dispatcher ────────────────────────
// Bridges OpPrimCall/OpPrimitive from JIT code to evaluator PrimFn table.
// The PrimId enum value is used as an index into kPrimNames to find
// the primitive name, then looked up in the evaluator's primitives table.
// Uses a global primitives pointer set by CompilerService::register_jit_primitives().

// PrimId name table (mirrors ir.ixx kPrimNames — must stay in sync)
static constexpr const char* kPrimNameTable[] = {
    "hash",          "hash-length",    "hash-has-key?",  "hash-keys",     "hash-values",
    "string-append", "string-length",  "string-ref",     "substring",     "string=?",
    "string<?",      "number->string", "string->number", "display",       "write",
    "newline",       "error",          "assert",         "read",          "read-file",
    "write-file",    "file-exists?",   "gensym",         "apply",         "vector",
    "vector-ref",    "vector-set!",    "vector-length",  "vector?",       "make-vector",
    "import",        "char=?",         "char<?",         "char->integer", "integer->char",
    "quotient",      "remainder",      "length",         "list-ref",      "reverse",
    "raise",         "error?",         "pair?",          "null?",
};

static std::atomic<const aura::compiler::Primitives*> g_jit_prim_ctx{nullptr};

// Key equality callback for JIT hash scan loop (Phase 4b)
// Compares two string-encoded values, handling both JIT and evaluator string encoding.
extern "C" std::int64_t aura_hash_callback_key_eq(std::int64_t stored_key,
                                                  std::int64_t search_key) {
    using aura::compiler::types::is_string_raw_v2;
    using aura::compiler::types::STRING_BIAS_VAL_2;
    using aura::compiler::types::string_idx_raw_v2;
    auto* prims = g_jit_prim_ctx.load(std::memory_order_acquire);
    // Fast path: raw value equality
    if (stored_key == search_key)
        return 1;
    // String comparison (Issue #181 Cycle 2: v2 encoding)
    auto is_str_val = [](std::int64_t v) { return is_string_raw_v2(v) && v <= STRING_BIAS_VAL_2; };
    if (is_str_val(stored_key) && is_str_val(search_key) && prims) {
        auto& sh = const_cast<aura::compiler::Primitives*>(prims)->string_heap();
        std::uint64_t eval_idx = string_idx_raw_v2(stored_key);
        if (static_cast<std::size_t>(eval_idx) >= sh.size())
            return 0;
        const std::string* search_str = nullptr;
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

extern "C" std::int64_t aura_jit_prim_dispatch(std::int64_t prim_id, std::int64_t* args,
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

    // Convert int64_t args to EvalValue vector.
    // After JIT encoding unification: args are pointer-tagged (fixnum=val<<1,
    // bool=7/3, void=11). EvalValue uses identical encoding, so pass through directly.
    std::vector<aura::compiler::types::EvalValue> eval_args;
    eval_args.reserve(static_cast<std::size_t>(argc));
    for (std::int32_t i = 0; i < argc; ++i)
        eval_args.emplace_back(args[i]);

    // Call the primitive function
    auto result = (*pfn)(eval_args);

    // Convert result back to int64_t.
    // EvalValue uses same pointer tagging, return the raw tagged value.
    return result.val;
}

// ── Hash operation dispatch ──────────────────────────────
// Bridges OpHashRef/OpHashSet/OpHashRemove from JIT code to evaluator's hash primitives.
// These are separate from kPrimNameTable (which covers PrimCall-level primitives)
// because hash ops have dedicated IROpcodes for inline dispatch.

// ── Helpers: convert JIT strings to evaluator string heap indices ──
// JIT-compiled code allocates strings in its own g_string_pool, but the
// evaluator's hash primitives use the evaluator's string_heap_ for equality
// and hashing.  We must migrate JIT strings to the evaluator heap on the fly.

// Issue #181 Cycle 2: STRING_BIAS_VAL is now STRING_BIAS_VAL_2
// (the v2 bias, with low 2 bits = 2 = dedicated string tag). The
// old literal -9000000000000000000LL is no longer the upper
// bound of the string range — v2 strings can be at
// STRING_BIAS_VAL_2 = STRING_BIAS_VAL + 2.
static constexpr std::int64_t STRING_BIAS_VAL_LOCAL = aura::compiler::types::STRING_BIAS_VAL_2;

// Returns true when val is a JIT/evaluator string-encoded value.
// Issue #181 Cycle 2: v2 encoding. Pure tag check (v & 3) == 2
// is the string signature, plus range check via STRING_BIAS_VAL_2
// to reject fixnums in the right range with the string tag bit
// set (defense-in-depth).
static bool is_str_val(std::int64_t val) {
    return aura::compiler::types::is_string_raw_v2(val) && val <= STRING_BIAS_VAL_LOCAL;
}

// JIT string pool access (declared in aura_jit_runtime.cpp)
extern "C" std::size_t aura_jit_pool_size();
extern "C" const char* aura_jit_pool_string(std::size_t idx);
extern "C" void aura_set_hash_str_eq_callback(std::int64_t (*fn)(std::int64_t, std::int64_t));
extern "C" std::int64_t aura_hash_string_cmp_fn(std::int64_t, std::int64_t);
extern "C" void aura_set_hash_str_convert_callback(std::int64_t (*fn)(std::int64_t));
extern "C" std::int64_t aura_hash_string_convert_fn(std::int64_t);

// Convert one JIT-string argument to an evaluator-string argument by
// copying the content from the JIT pool to the evaluator string heap.
// Non-string values pass through unchanged.
static std::int64_t convert_str_for_eval(std::int64_t val,
                                         const aura::compiler::Primitives* prims) {
    if (!is_str_val(val))
        return val; // not a string, pass through
    // JIT string encoding (v2): STRING_BIAS_VAL_2 - (idx << 2)
    // idx = (STRING_BIAS_VAL_2 - val) >> 2
    std::uint64_t idx = aura::compiler::types::string_idx_raw_v2(val);
    // Get the string content from the JIT string pool
    const char* content = aura_jit_pool_string(static_cast<std::size_t>(idx));
    if (!content)
        return val;
    // Push into evaluator string heap and return v2 encoding
    auto& eval_heap = const_cast<aura::compiler::Primitives*>(prims)->string_heap();
    auto new_idx = eval_heap.size();
    eval_heap.push_back(content);
    return aura::compiler::types::make_string_raw_v2(static_cast<std::uint64_t>(new_idx));
}


namespace aura::compiler {


// Convert FlatParseResult to Diagnostic with structured location.
// Falls back to a generic parse error if no structured errors exist.
static aura::diag::Diagnostic parse_error_diag(const aura::parser::FlatParseResult& pr) {
    if (!pr.errors.empty()) {
        return {aura::diag::ErrorKind::ParseError, pr.errors[0].message, pr.errors[0].location};
    }
    return {aura::diag::ErrorKind::ParseError, pr.error.empty() ? "parse error" : pr.error};
}

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
    std::function<bool(std::uint32_t)> block_dirty_fn_;

    void set_block_dirty_fn(std::function<bool(std::uint32_t)> fn) {
        block_dirty_fn_ = std::move(fn);
    }

    // Issue #684: DirtyAwarePass hook for incremental escape analysis.
    [[nodiscard]] bool is_block_dirty(std::uint32_t block_id) const {
        if (!block_dirty_fn_)
            return true;
        return block_dirty_fn_(block_id);
    }

    // Issue #1574: IncrementalPass entry points for dirty pipeline.
    void run(aura::ir::IRFunction& func) {
        if (!g_use_arena)
            return;
        if (func.id >= maps.size())
            maps.resize(func.id + 1);
        std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs(func.blocks.size());
        for (std::size_t bi = 0; bi < func.blocks.size(); ++bi) {
            if (block_dirty_fn_ && !is_block_dirty(static_cast<std::uint32_t>(bi)))
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
                     0});
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

    bool has_error() const { return false; }
    std::string_view name() const { return "escape-analysis"; }

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

    CompilerService()
        : user_bindings_{"#t", "#f", "nil"}
        , session_id_("default") {
        // Issue #456: register this service's Evaluator as
        // the active query-evaluator for the current thread.
        // query:mutation-impact / query:epoch-stats /
        // query:dirty-subtree read counters from it; without
        // this registration, those primitives see nullptr
        // whenever no MutationBoundaryGuard is active.
        Evaluator::set_query_evaluator(&evaluator_);
        evaluator_.set_arena(&arena_);
        evaluator_.set_temp_arena(&temp_arena_);
        // Issue #685/#743/#1518/#1521: ShapeProfiler + IRSoA dirty synergy on
        // compact/defrag. Prefer on_arena_compact (soft version bump + preserve
        // stability, no deopt-storm ring feed) over invalidate_all. Hard
        // invalidate_all only when throttle allows AND no profiles yet need
        // the soft path (legacy fallback when profiler empty is a no-op).
        // 500ms throttle still gates aggressive JIT cache thrash accounting.
        arena_.set_on_compact_hook([this]() {
            // Issue #1521: always soft-coordinate ShapeProfiler (version bump
            // + ArenaCompact deopt hooks, keep is_stable / history).
            const auto touched = shape_profiler_.on_arena_compact();
            metrics_.shape_inval_on_compact_triggered_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.shape_stability_post_compact_preserved_total.store(
                shape::shape_stability_post_compact_preserved.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            metrics_.deopt_from_arena_compact_total.store(
                shape::deopt_from_arena_compact_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            (void)touched;

            if (aura::core::arena_policy::try_render_deopt_throttle(500)) {
                // Throttle pass: count as triggered deopt coordination.
                // Soft on_arena_compact already fired kArenaCompact hooks;
                // do NOT invalidate_all (would clear history + feed storm).
                metrics_.arena_compact_deopt_triggered_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                aura::core::arena_policy::record_compact_deopt_triggered();
                // Fiber / boundary post-compact sync (clears spurious storm).
                (void)shape_profiler_.on_boundary_or_fiber_sync(/*clear_compact_only_storm=*/true);
            } else {
                // Soft path under pressure: IR dirty only + throttle count.
                metrics_.arena_compact_deopt_throttled_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                aura::core::arena_policy::record_compact_deopt_throttled();
            }
            aura::core::arena_policy::record_shape_inval_on_compact();
            for (auto& [_, entry] : ir_cache_v2_) {
                entry.dirty = true;
                entry.mark_all_blocks_dirty();
            }
        });
        aura::gc_hooks::g_arena_auto_compact_trigger.store(+[]() noexcept {
            auto* raw = aura::messaging::g_current_compiler_service;
            if (!raw)
                return;
            auto& m = static_cast<CompilerService*>(raw)->metrics();
            m.arena_auto_compact_trigger_total.fetch_add(1, std::memory_order_relaxed);
        });
        aura::gc_hooks::g_arena_fiber_safe_compact.store(+[]() noexcept {
            auto* raw = aura::messaging::g_current_compiler_service;
            if (!raw)
                return;
            auto& m = static_cast<CompilerService*>(raw)->metrics();
            m.arena_live_move_yield_total.fetch_add(1, std::memory_order_relaxed);
        });
        // Issue #686: shape stability loss / invalidate → IRSoA block_dirty_.
        shape_profiler_.set_dirty_hook([this](shape::FnKey fn_key, std::uint32_t dirty_scope) {
            (void)dirty_scope;
            mark_shape_dirty_for_fn_key(fn_key);
        });
        // Issue #492: stability loss / invalidate → JIT eviction +
        // fiber refresh (closes ShapeProfiler → JIT hot-swap loop).
        shape::set_shape_deopt_hook(&CompilerService::shape_deopt_hook_trampoline);
        // Issue #744: Shape stability → Pass/JIT closed-loop probes.
        shape_jit_pass::g_mutation_epoch_fn.store(+[]() noexcept -> std::uint64_t {
            auto* raw = aura::messaging::g_current_compiler_service;
            if (!raw)
                return 0;
            return static_cast<CompilerService*>(raw)->bridge_epoch();
        });
        aura::compiler::set_fn_shape_stable_probe(+[](std::string_view name) noexcept -> bool {
            auto* raw = aura::messaging::g_current_compiler_service;
            if (!raw)
                return false;
            return static_cast<CompilerService*>(raw)->is_shape_stable(std::string(name));
        });
        // Issue #494: yield between pass-pipeline stages when running on a fiber.
        aura::compiler::set_pipeline_yield_hook(&CompilerService::pipeline_yield_trampoline);
        evaluator_.set_type_registry(&type_registry_);
        // Issue #252: wire the shared CompilerMetrics to the
        // Evaluator. apply_closure increments the closure_*
        // counters on this struct (also incremented by the
        // IR's IROpcode::Call/Apply via IRContext). This
        // gives us a single source of truth for the dispatch
        // counters, so the snapshot and closure:stats read
        // a consistent value.
        evaluator_.set_compiler_metrics(&metrics_);
        // Issue #708: wire AOT bridge counters at construction so
        // aura_reload_aot_module / checkpoint probes bump metrics_
        // before the first JIT eval (register_jit_primitives is lazy).
        aura_set_aot_metrics(&metrics_);
        // Issue #1522: register AuraJIT for bridge C-API batch_deopt_for.
        aura_set_jit_batch_deopt_target(&jit_);
        // Issue #1540: wire Evaluator::linear_post_mutate_enforce into JIT
        // linear_safety_probe / Apply prologue (returns 1 if UNSAFE).
        aura_set_linear_post_mutate_enforce_fn(
            [](void* user, std::uint32_t env_id) -> int {
                if (!user)
                    return 0;
                auto* ev = static_cast<Evaluator*>(user);
                // linear_post_mutate_enforce returns true if SAFE.
                return ev->linear_post_mutate_enforce(env_id) ? 0 : 1;
            },
            &evaluator_);
        // Issue #1545: JIT ResourceTracker pre-evict linear live-closure scan.
        aura_set_linear_live_closure_scan_fn(
            [](void* user) {
                if (!user)
                    return;
                static_cast<Evaluator*>(user)->scan_live_closures_for_linear_captures(
                    /*mark_invalid=*/true);
            },
            &evaluator_);
        evaluator_.set_compiler_service(this);
        // Issue #681: wire mutation_epoch / bridge_epoch for
        // apply_closure + IRClosure lifetime checks.
        evaluator_.install_bridge_epoch_fn([](void* svc) -> std::uint64_t {
            return static_cast<CompilerService*>(svc)->bridge_epoch();
        });
        // Issue #1510 / #1526: compact_env_frames dual-epoch bump —
        // bridge_epoch + AOT func table (lockstep with defuse_version_
        // inside compact). Survivors then restamp Closure::bridge_epoch
        // to the new value under the same interlock.
        evaluator_.install_bridge_epoch_bump_fn([](void* svc) {
            if (!svc)
                return;
            auto* s = static_cast<CompilerService*>(svc);
            s->bump_bridge_epoch();
            // Dual-domain with JIT aura_closure_call (#1508 / #1524).
            aura_aot_bump_func_table_epoch();
        });
        // Issue #1526: IR runtime_closures_ env_id remap + bridge restamp.
        evaluator_.install_compact_env_remap_fn(
            [](void* ctx, const std::int64_t* remap, std::size_t n) {
                if (!ctx || !remap)
                    return;
                static_cast<CompilerService*>(ctx)->remap_ir_closure_env_ids_on_compact_(remap, n);
            },
            this);
        evaluator_.install_compact_env_restamp_fn([](void* ctx,
                                                     std::uint64_t new_epoch) -> std::size_t {
            if (!ctx)
                return 0;
            return static_cast<CompilerService*>(ctx)->restamp_ir_closure_bridge_epochs_(new_epoch);
        });
        // Issue #682: register compiler IRClosure/EnvId GC roots at safepoint.
        evaluator_.install_compiler_gc_roots_fn([](void* svc, void* roots) {
            static_cast<CompilerService*>(svc)->flush_compiler_gc_roots(roots);
        });
        evaluator_.set_workspace_adt_sync_fn([](void* svc) {
            static_cast<CompilerService*>(svc)->refresh_adt_constructors_from_workspace();
        });
        evaluator_.set_session_id(session_id_);

        // Issue #272: route cached-define closures through persistent IR runtimes.
        install_persistent_define_closure_bridge();
        // Phase 2: EDSL IR cache V2 hooks. Let evaluator partition TUs mark
        // cached defines dirty via these std::function pointers, without
        // needing to import CompilerService (which would be circular).
        evaluator_.set_mark_define_dirty_fn(
            [this](const std::string& name) { this->mark_define_dirty(name); });
        evaluator_.set_mark_all_defines_dirty_fn([this]() { this->mark_all_defines_dirty(); });
        // Issue #680: precise IR/JIT/bridge invalidation for closure-heavy Defines.
        evaluator_.set_invalidate_function_fn(
            [this](const std::string& name) { this->invalidate_function(name); });
        evaluator_.set_define_impact_scope_fn(
            [this](aura::ast::NodeId root) { this->run_define_impact_scope(root); });
        // Issue #657: compiler-core incremental hooks (lowering bridge epoch +
        // linear metadata + JIT unhandled invalidate).
        install_lowering_compiler_core_hooks();
        aura_set_jit_unhandled_invalidate_fn(&CompilerService::jit_unhandled_invalidate_trampoline);
        // Phase 2: pre-populate v2 IR cache from workspace defines.
        // Called from (set-code ...) primitive after a successful parse.
        // Plan A + Follow-up 3: hook calls BOTH the lightweight
        // populate_dep_graph (no side effects) AND the heavy
        // populate_ir_cache_v2 (which uses bind_in_env=false to skip
        // the env pollution that broke tests in early Phase 3).
        //
        // Issue #687: dep-graph doubling is now prevented via
        // record_dependency idempotence. The multi-define binding
        // issue (a/b/c not bound in top_ after eval-current) is
        // fixed by eval-current explicitly syncing workspace
        // defines into top_env after eval_flat (see eval-current
        // implementation). The heavy populate stays as opt-in.
        evaluator_.set_pre_cache_workspace_defines_fn([this]() {
            this->populate_dep_graph_from_workspace();
            this->populate_ir_cache_v2_from_workspace();
        });
        // Issue #63723: lightweight dep_graph-only repopulate hook
        // (called from mutate:rebind / mutate:set-body after the
        // rebind success). mutate:rebind does NOT call
        // invalidate_function (it uses mark_define_dirty + the
        // dep_graph BFS cascade), so the IR cache for `name` is
        // just marked dirty and re-lower happens lazily on the
        // next (eval-current). The dep_graph however must be
        // repopulated eagerly so that subsequent
        // public_invalidate_function(name) sees the same caller
        // edges the original (set-code ...) recorded — otherwise
        // the BFS cascade finds no dependents and silently
        // evicts only the mutated function itself. This is the
        // dep_graph integrity contract test_issue_401 AC5
        // verifies. Skips populate_ir_cache_v2 (heavy lower)
        // because the lazy re-lower on next (eval-current) is
        // sufficient and avoids an O(n^2) cost on rebind
        // storms.
        evaluator_.set_repopulate_workspace_dep_graph_fn(
            [this]() { this->populate_dep_graph_from_workspace(); });
        // Issue #1495: lazy partial re-lower of dirty defines on
        // (eval-current) — consumes body-only dirty bitmask from
        // mark_define_dirty without requiring a set-code pre-cache.
        evaluator_.set_relower_dirty_defines_fn(
            [this]() { (void)this->relower_dirty_defines_from_workspace(); });
        // Phase 3 debugging: expose is_define_dirty + get_dependents.
        evaluator_.set_is_define_dirty_fn([this](const std::string& name) -> bool {
            const auto* entry = this->get_define_v2(name);
            if (!entry)
                return false;
            return entry->dirty;
        });
        evaluator_.set_get_dependents_fn(
            [this](const std::string& name) -> std::vector<std::string> {
                std::shared_lock dep_read(dep_graph_mtx_);
                auto it = dep_graph_.find(name);
                if (it == dep_graph_.end())
                    return {};
                return it->second.called_by;
            });
        // Phase 4: hook for (eval-current :jit). Re-runs the workspace
        // through eval_ir (which has the JIT pipeline) and returns the
        // result. Falls back to the IR interpreter if LLVM isn't available
        // or the JIT compile fails.
        evaluator_.set_try_jit_fn(
            [this](const std::string& source) -> std::optional<types::EvalValue> {
                auto result = this->eval_ir(source);
                if (!result)
                    return std::nullopt;
                return *result;
            });
        // Phase 4: get workspace source via unparse_node (the proper
        // way to serialize a workspace FlatAST back to a string).
        evaluator_.set_get_workspace_source_fn([this]() -> std::string {
            auto* ws_flat = evaluator_.workspace_flat();
            auto* ws_pool = evaluator_.workspace_pool();
            if (!ws_flat || !ws_pool)
                return "";
            return unparse_node(*ws_flat, *ws_pool, ws_flat->root, 0);
        });
        // Issue #194: hook to query the runtime→intrinsic migration
        // counter from the AuraJIT. Used by the (jit:intrinsic-count)
        // Aura-level primitive. Returns 0 if no JIT is attached
        // (e.g. --no-llvm build, unit-test Evaluator).
        evaluator_.set_get_intrinsic_count_fn([this]() -> std::uint64_t {
            // The Metrics struct on AuraJIT includes
            // intrinsic_count (the per-lowering migration counter
            // shipped in 9901a91). Access via Metrics() — returns
            // a fresh struct snapshot, so reads are atomic.
            // The intrinsic_count field is std::atomic<uint64_t>.
            return jit_.metrics().intrinsic_count.load(std::memory_order_relaxed);
        });
        // Issue #193: hook to query the per-function
        // unhandled-opcode count. Used by the (jit:deopt-fn?)
        // primitive (Issue #193 follow-up).
        evaluator_.set_get_jit_unhandled_count_fn([this](const char* name) -> std::uint64_t {
            return jit_.unhandled_opcode_count_for_function(name);
        });
        // Issue #427: hook for (query:jit-stats) — returns the
        // full JIT metrics line (same string AuraJIT::Metrics::format
        // produces). The closure formats into a thread-local
        // buffer to avoid lifetime issues across the std::function
        // boundary. Returns "" if no JIT is attached.
        evaluator_.set_get_jit_stats_fn([this]() -> const char* {
            thread_local char buf[1024];
            // Always format — even a no-JIT build produces a
            // valid string with all-zero counters. The primitive
            // (query:jit-stats) just returns it as-is. Saves
            // us from having to plumb a "no jit" flag through
            // the service.
            jit_.metrics().format(buf, sizeof(buf));
            return buf;
        });
        // Issue #196: hook to query the incremental-compilation
        // observability struct. Used by the (compile:cache-size),
        // (compile:dirty-count), (compile:epoch), (compile:dep-edges)
        // primitives.
        evaluator_.set_get_incremental_stats_fn([this]() -> std::uint64_t {
            // Return 4 values packed as (cache << 48) | (dirty << 32) | (epoch << 16) | edges
            std::uint64_t cache_size = static_cast<std::uint64_t>(ir_cache_v2_.size());
            std::uint64_t dirty_count = 0;
            for (auto& [_, e] : ir_cache_v2_) {
                if (e.dirty)
                    ++dirty_count;
            }
            std::uint64_t epoch = mutation_epoch_.load(std::memory_order_acquire);
            std::uint64_t edges = 0;
            {
                std::shared_lock dep_read(dep_graph_mtx_);
                for (auto& [_, dep_entry] : dep_graph_) {
                    edges += static_cast<std::uint64_t>(dep_entry.calls.size());
                    edges += static_cast<std::uint64_t>(dep_entry.called_by.size());
                }
            }
            // Pack into a single uint64 — simpler than a struct
            // crossing the module boundary.
            return (cache_size << 48) | (dirty_count << 32) | (epoch << 16) | (edges & 0xFFFF);
        });
        // Issue #196: per-block dirty hooks. Mirror the
        // get_incremental_stats hook pattern: stateless
        // lambdas that read the current state of
        // ir_cache_v2_ on demand. No caching at the
        // hook layer — the underlying reads are O(1) or
        // O(num_functions_in_entry) which is also small
        // (typically <10 functions per define).
        evaluator_.set_get_dirty_block_count_fn([this](const char* name) -> std::uint64_t {
            std::string n = name ? std::string(name) : std::string();
            auto it = ir_cache_v2_.find(n);
            if (it == ir_cache_v2_.end())
                return 0;
            return static_cast<std::uint64_t>(it->second.dirty_block_count());
        });
        evaluator_.set_get_func_dirty_block_count_fn(
            [this](const char* name, std::size_t func_idx) -> std::uint64_t {
                std::string n = name ? std::string(name) : std::string();
                auto it = ir_cache_v2_.find(n);
                if (it == ir_cache_v2_.end())
                    return 0;
                return static_cast<std::uint64_t>(it->second.func_dirty_block_count(func_idx));
            });
        evaluator_.set_is_block_dirty_fn(
            [this](const char* name, std::size_t func_idx, std::uint32_t block_idx) -> bool {
                std::string n = name ? std::string(name) : std::string();
                auto it = ir_cache_v2_.find(n);
                if (it == ir_cache_v2_.end())
                    return false;
                return it->second.is_block_dirty(func_idx, block_idx);
            });
        evaluator_.set_mark_block_dirty_fn(
            [this](const char* name, std::size_t func_idx, std::uint32_t block_idx) -> bool {
                std::string n = name ? std::string(name) : std::string();
                return mark_block_dirty_v2(n, func_idx, block_idx);
            });
        evaluator_.set_clear_block_dirty_fn(
            [this](const char* name, std::size_t func_idx, std::uint32_t block_idx) -> bool {
                std::string n = name ? std::string(name) : std::string();
                return clear_block_dirty_v2(n, func_idx, block_idx);
            });
        // Issue #429: hook for (query:soa-dirty-stats).
        // The closure reads the live SoaDirtyStats in one
        // pass over ir_cache_v2_ (cheap; ~1ns per entry).
        evaluator_.set_get_soa_dirty_stats_fn(
            [this]() -> Evaluator::SoaDirtyStats { return get_soa_dirty_stats(); });
        // Issue #460: per-instruction dirty hooks. The
        // P0 ship wires no-op implementations (always
        // return false) because the underlying
        // ir_cache_v2_ doesn't have an instruction-level
        // dirty bitmask yet. The hooks are exposed so
        // the primitive path is exercisable; the
        // follow-up adds the bitmask and the
        // mark/clear implementations.
        evaluator_.set_is_instruction_dirty_fn([this](const char* name, std::size_t func_idx,
                                                      std::uint32_t block_idx,
                                                      std::uint32_t inst_idx) -> bool {
            std::string n = name ? std::string(name) : std::string();
            auto it = ir_cache_v2_.find(n);
            if (it == ir_cache_v2_.end())
                return false;
            return it->second.is_instruction_dirty(func_idx, block_idx, inst_idx);
        });
        evaluator_.set_mark_instruction_dirty_fn([this](const char* name, std::size_t func_idx,
                                                        std::uint32_t block_idx,
                                                        std::uint32_t inst_idx) -> bool {
            std::string n = name ? std::string(name) : std::string();
            auto it = ir_cache_v2_.find(n);
            if (it == ir_cache_v2_.end())
                return false;
            it->second.mark_instruction_dirty(func_idx, block_idx, inst_idx);
            return true;
        });
        evaluator_.set_clear_instruction_dirty_fn([this](const char* name, std::size_t func_idx,
                                                         std::uint32_t block_idx,
                                                         std::uint32_t inst_idx) -> bool {
            std::string n = name ? std::string(name) : std::string();
            auto it = ir_cache_v2_.find(n);
            if (it == ir_cache_v2_.end())
                return false;
            it->second.clear_instruction_dirty(func_idx, block_idx, inst_idx);
            return true;
        });
        // Issue #240: per-node occurrence-dirty hook. The
        // hook reads / writes the kOccurrenceDirty bit on
        // the workspace FlatAST's dirty bitmask
        // (DirtyReason::kOccurrenceDirty = 0x04). The bit
        // gates find_occurrence_contexts in
        // type_checker_impl.cpp so the post-mutation
        // invariant check scopes its diagnostic to
        // contexts where narrowing is genuinely stale.
        //
        // The hook returns the PRIOR bit state (true if
        // it was already set, false otherwise). This lets
        // the read-only (compile:narrowing-dirty?) primitive
        // peek the state via a no-op set/restore pair.
        evaluator_.set_set_occurrence_dirty_fn([this](std::uint32_t node_id, bool set) -> bool {
            auto* ws = evaluator_.workspace_flat();
            if (!ws || node_id >= ws->size())
                return false;
            bool prior = ws->is_dirty_for(
                node_id,
                static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
            if (set) {
                ws->mark_dirty(node_id, static_cast<std::uint8_t>(
                                            aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
                // Issue #518: mirror kOccurrenceDirty into the
                // occurrence-stale column for observability
                // (query:occurrence-stale-count) and re-narrow.
                ws->mark_occurrence_stale(node_id);
            } else {
                ws->clear_dirty_for(
                    node_id,
                    static_cast<std::uint8_t>(aura::ast::FlatAST::DirtyReason::kOccurrenceDirty));
            }
            return prior;
        });
        // Issue #197: hook for (compile:inline-pass-stats).
        // The hook reads the static lifetime counters
        // maintained by InlinePass (process-wide totals).
        // If the InlinePass hasn't been run yet, both
        // counters are 0.
        evaluator_.set_get_inline_stats_fn([]() -> std::uint64_t {
            std::uint64_t inlined =
                static_cast<std::uint64_t>(aura::compiler::InlinePass::total_inlined());
            std::uint64_t branch_aware = static_cast<std::uint64_t>(
                aura::compiler::InlinePass::total_inlined_branch_aware());
            return (branch_aware << 32) | (inlined & 0xFFFFFFFF);
        });
        // Issue #388: macro-hygiene skipped total (separate
        // getter so the packed uint64 layout stays unchanged).
        evaluator_.set_get_macro_hygiene_skipped_fn([]() -> std::uint64_t {
            return static_cast<std::uint64_t>(
                aura::compiler::InlinePass::total_macro_hygiene_skipped());
        });
        // Issue #1420 AC3: hook for (compile:bidirectional-stats).
        // Reads the 4 CompilerMetrics counters incremented
        // by InferenceEngine::check_flat_call and packs
        // them into a single uint64. Bit layout (see
        // Evaluator::get_bidirectional_stats_fn_ comment):
        //   bits  0-23: compile_bidirectional_check_call_total
        //   bits 24-39: compile_bidirectional_annotation_pass_total
        //   bits 40-55: compile_bidirectional_annotation_fail_total
        //   bits 56-63: compile_bidirectional_coercion_deferred_total
        // Mode (full / disabled) is read separately via
        // CompilerService::bidirectional_mode() because
        // the persistent CompilerService flag is the
        // authoritative source (per-call TypeChecker
        // instances are short-lived and pick up the flag
        // via tc.set_bidirectional_mode(bidirectional_mode_)
        // at construction).
        evaluator_.set_get_bidirectional_stats_fn([this]() -> std::uint64_t {
            auto& m = this->metrics_;
            std::uint64_t check_call =
                m.compile_bidirectional_check_call_total.load(std::memory_order_relaxed);
            std::uint64_t pass =
                m.compile_bidirectional_annotation_pass_total.load(std::memory_order_relaxed);
            std::uint64_t fail =
                m.compile_bidirectional_annotation_fail_total.load(std::memory_order_relaxed);
            std::uint64_t coercion =
                m.compile_bidirectional_coercion_deferred_total.load(std::memory_order_relaxed);
            return ((coercion & 0xFF) << 56) | ((fail & 0xFFFF) << 40) | ((pass & 0xFFFF) << 24) |
                   (check_call & 0xFFFFFF);
        });
        aura::messaging::g_current_compiler_service = this;
        // Setup messaging bridge (avoids circular module dependency)
        aura::messaging::g_messaging_bridge.send = [](const std::string& target,
                                                      const std::string& msg) -> bool {
            auto* svc = CompilerService::lookup(target);
            if (!svc)
                return false;
            auto* self = static_cast<CompilerService*>(aura::messaging::g_current_compiler_service);
            auto sender = self ? self->session_id() : std::string("(unknown)");
            svc->push_message(sender, msg);
            return true;
        };
        aura::messaging::g_messaging_bridge.recv =
            [](int timeout_ms) -> std::optional<std::string> {
            // This relies on the compiler_service_ being set correctly,
            // which doesn't work across sessions. Instead, use the
            // current CompilerService from context.
            // For now, return empty — recv is session-specific.
            return std::nullopt;
        };
        aura::messaging::g_messaging_bridge.my_id = []() -> std::string { return "(unknown)"; };
        // Set per-service access functions
        // Arena reset callback for benchmark task cleanup
        aura::messaging::g_reset_arena = [](void* svc) {
            if (!svc)
                return;
            static_cast<CompilerService*>(svc)->reset();
        };

        aura::messaging::g_mailbox_read = [](void* svc,
                                             int timeout_ms) -> std::optional<std::string> {
            if (!svc)
                return std::nullopt;
            return static_cast<CompilerService*>(svc)->pop_message(timeout_ms);
        };
        aura::messaging::g_mailbox_last_sender = [](void* svc) -> std::string {
            if (!svc)
                return "";
            return static_cast<CompilerService*>(svc)->last_sender();
        };
        aura::messaging::g_mailbox_count = [](void* svc) -> std::size_t {
            if (!svc)
                return 0;
            return static_cast<CompilerService*>(svc)->mailbox_size();
        };
        aura::messaging::g_session_id = [](void* svc) -> std::string {
            if (!svc)
                return "";
            return static_cast<CompilerService*>(svc)->session_id();
        };
        aura::messaging::g_session_exists = [](const std::string& id) -> bool {
            return CompilerService::lookup(id) != nullptr;
        };
        // Cache module defines in IR after each import (incl. recursive fns)
        evaluator_.set_module_loaded_callback(
            [this](const std::string& content, const std::string& path) {
                cache_module(content, path);
            });

        // Issue #97 Action 1: hot-swap callback. Allows (hot-swap:fn "name" "new-src")
        // primitive to replace a function's body while keeping its id.
        evaluator_.set_hot_swap_fn([this](const std::string& name, const std::string& new_source) {
            return hot_swap_function_impl(name, new_source);
        });
    }

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

        // Issue #1262/#1264: acquire mutation epoch with fence before
        // hot-swap so concurrent invalidate_function / mutate cannot
        // tear bridge_epoch vs mutation_epoch observations.
        const std::uint64_t epoch_at_entry = mutation_epoch_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acq_rel);
        // Issue #1262: stamp AOT emit defuse_version for versioned mangle
        // so subsequent AOT reloads enforce current epoch in symbol names.
        aura_set_aot_defuse_version(evaluator_.defuse_version());

        // Find existing function by name
        std::uint32_t existing_id = 0xFFFFFFFF;
        for (std::uint32_t i = 0; i < last_ir_mod_->functions.size(); ++i) {
            if (last_ir_mod_->functions[i].name == name) {
                existing_id = i;
                break;
            }
        }
        if (existing_id == 0xFFFFFFFF)
            return false;

        // Parse the new source in the temp arena (avoids clobbering workspace)
        auto alloc = temp_arena_.allocator();
        auto* new_pool = temp_arena_.create<aura::ast::StringPool>(alloc);
        auto* new_flat = temp_arena_.create<aura::ast::FlatAST>(alloc);
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
        const std::uint64_t epoch_now = mutation_epoch_.load(std::memory_order_acquire);
        if (epoch_now != epoch_at_entry) {
            metrics_.hot_update_race_detected.fetch_add(1, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acq_rel);
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
        mutation_epoch_.fetch_add(1, std::memory_order_release);
        metrics_.hot_swap_versioned_mangle_enforced.fetch_add(1, std::memory_order_relaxed);

        // Hot-swap: replace in last_ir_mod_ at existing_id, preserving the id
        return last_ir_mod_->hot_swap_function(existing_id, std::move(new_mod.functions[new_id]));
    }

    // ── Inter-agent messaging (P0) ──────────────────────────
    void set_session_id(const std::string& id) {
        session_id_ = id;
        evaluator_.set_session_id(id);
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
        std::lock_guard lk(registry_mtx());
        registry()[id] = svc;
    }

    static void unregister_session(const std::string& id) {
        std::lock_guard lk(registry_mtx());
        registry().erase(id);
    }

    static CompilerService* lookup(const std::string& id) {
        std::lock_guard lk(registry_mtx());
        auto it = registry().find(id);
        return it != registry().end() ? it->second : nullptr;
    }

    void reset() {
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
        ir_disk_snapshots_.clear();
        // Issue #223 + #1258 + #1263: bump mutation_epoch_ so any stale
        // ClosureBridgeData that captured the old epoch is detected
        // by the bridge callback / apply_closure. The bridge_epoch_
        // field on ClosureBridgeData captures this at construction
        // time; a mismatch indicates the bridge's flat*/pool* are
        // dangling (the arena was reset). The bridge falls back
        // to re-parse from body_source (or invalidates the closure).
        // Force-dirty was applied above before clear — no false-clean.
        mutation_epoch_.fetch_add(1, std::memory_order_release);
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

    // ---- Unified evaluation (IR-first with fallback) -----------------

    // Check if an expression needs the tree-walker evaluator.
    // IR pipeline cannot handle: EDSL primitives, quoted pairs, special forms,
    // macro definitions, error handling, or non-primitive variable references
    // (which may come from runtime imports).
    bool needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                    const aura::ast::StringPool& pool,
                                    aura::ast::NodeId root) const {
        // Issue #402 observability: bump the call counter
        // first so callers see every invocation (incl. early
        // returns for NULL_NODE / out-of-range root).
        metrics_.needs_tree_walker_fallback_calls.fetch_add(1, std::memory_order_relaxed);
        if (root == aura::ast::NULL_NODE || root >= flat.size())
            return false;
        // Issue #657: quote-containing defines need bridge/fallback refresh.
        bool flat_has_quote = false;
        for (aura::ast::NodeId qid = 0; qid < flat.size(); ++qid) {
            if (flat.get(qid).tag == aura::ast::NodeTag::Quote) {
                flat_has_quote = true;
                break;
            }
        }

        // Issue #402: FAST PATH. When summary_flags_ == 0,
        // no MacroDef / DefineType / DefineModule / Set /
        // Lambda-dotted / TypeAnnotation-var is present ANY
        // in the flat. The fast path walks only the root
        // subtree (typically 4-10 nodes for arithmetic /
        // primitive calls) instead of the entire flat, and
        // applies the same Variable / Call resolution logic
        // as the slow path. For the common case (`(+ 1 2)`,
        // `(* x 3)` with x in ir_cache_, etc.) the fast path
        // returns false without paying the O(flat.size())
        // scan cost.
        if (flat.summary_flags() == 0) {
            metrics_.needs_tree_walker_fast_path_hits.fetch_add(1, std::memory_order_relaxed);
            // Issue #402: top-level defines need a body-aware
            // check (params + function name are NOT free vars,
            // even though walk_subtree would see them as such).
            // Delegate to define_body_needs_tree_walker_fallback
            // when root is a Define so the slow path's logic
            // is reused verbatim — same answer for the same AST.
            auto fast_root_v = flat.get(root);
            if (fast_root_v.tag == aura::ast::NodeTag::Define && !fast_root_v.children.empty()) {
                auto fast_body_id = fast_root_v.child(0);
                if (fast_body_id < flat.size()) {
                    auto fast_name = std::string(pool.resolve(fast_root_v.sym_id));
                    const bool needs =
                        define_body_needs_tree_walker_fallback(flat, pool, fast_body_id, fast_name);
                    if (flat_has_quote && needs)
                        metrics_.compiler_core_quote_fallback_refresh_total.fetch_add(
                            1, std::memory_order_relaxed);
                    return needs;
                }
            }
            const bool needs = subtree_needs_tree_walker_fallback(flat, pool, root);
            if (flat_has_quote && needs)
                metrics_.compiler_core_quote_fallback_refresh_total.fetch_add(
                    1, std::memory_order_relaxed);
            return needs;
        }

        // Issue #402: SLOW PATH. summary_flags_ != 0 means
        // at least one fallback-relevant tag is present
        // somewhere in the flat. Fall back to the original
        // O(flat.size()) scan, which has the same logic as
        // before #402.
        metrics_.needs_tree_walker_slow_path_hits.fetch_add(1, std::memory_order_relaxed);

        // Issue #272: top-level defines are IR-native when the body only
        // references params, self, primitives, cached functions, or IR value cells.
        auto root_v = flat.get(root);
        if (root_v.tag == aura::ast::NodeTag::Define && !root_v.children.empty()) {
            auto body_id = root_v.child(0);
            if (body_id < flat.size()) {
                auto name_str = std::string(pool.resolve(root_v.sym_id));
                return define_body_needs_tree_walker_fallback(flat, pool, body_id, name_str);
            }
        }

        // ── Known names that must go through tree-walker ───────────────
        // Includes: EDSL primitives, special forms, module system operations.
        static const std::unordered_set<std::string> tree_walker_only = {
            // EDSL / AI agent primitives
            "define-type",
            "set-code",
            "eval-current",
            "apply",
            "typecheck-current",
            "typed-mutate",
            "typed-mutate-atomic", // Issue #1442 / #1408 EDSL atomic multi-mutate
            "rollback",
            "mutation-log",
            "query-mutation-log",
            // Mutation primitives (issue #165/#166): tree-walker so the
            // returned bool/Int propagates correctly. The IR pipeline
            // either silently drops unknown primitives or wraps them as
            // ConstI64 0, breaking the success checks in
            // test_issue_165/166 (e.g. "mutate > 0").
            "mutate:rebind",
            "mutate:set-body",
            "mutate:remove-node",
            "mutate:move-node",
            "intend",
            "fiber:spawn",
            "fiber:join",
            "define-strategy",
            "register-strategy!",
            "intend-history",
            "intend-analytics",
            "strategy-field",
            "strategy-set-field!",
            "strategy-inspect",
            "coverage-report",
            // C FFI — tree-walker needed to dispatch closure calls
            "c-func",
            "c-load",
            // Messaging primitives (tree-walker for argument passing)
            "send",
            "recv",
            "my-id",
            "json-encode",
            "json-get-string",
            "json-parse",
            // Scheduler metrics (Issue #32)
            "orch:metrics",
            "orch:reset-metrics",
            // Special forms not in IR
            "when",
            "set!",
            "load",
            "equal?",
            "unless",
            "export",
            "and",
            "or",
            "cond",
            "case",
            // Capability special forms
            "with-capability",
            "check-capability",
            "capability-stack", // DEPRECATED — uses primitive path instead
            // Module system (env side-effects)
            // "import", "use", "require" — now in lowering_known
        };

        // Root-level bare variables (like `pi`, `sort`) may come from runtime imports.
        // The lowering doesn't know about them, so fallback to tree-walker.
        // Issue #1284: ir_cache_v2_ define cache also counts as known.
        if (flat.get(root).tag == aura::ast::NodeTag::Variable) {
            auto root_name = pool.resolve(flat.get(root).sym_id);
            if (evaluator_.primitives().slot_for_name(std::string(root_name)) >=
                    evaluator_.primitives().slot_count() &&
                !name_in_ir_define_cache(std::string(root_name)))
                return true;
        }

        // Type annotations: if storing a variable name (3-arg form),
        // tree-walker handles variable binding; IR/JIT cannot.
        if (flat.get(root).tag == aura::ast::NodeTag::TypeAnnotation) {
            auto root_v = flat.get(root);
            if (!root_v.children.empty()) {
                if (root_v.int_value != 0 ||
                    flat.get(root_v.child(0)).tag == aura::ast::NodeTag::Variable)
                    return true;
            }
        }

        // Names that lowering explicitly handles (special forms lowered to IR)
        // These should NOT trigger tree-walker fallback even though they're
        // not primitives or cached defines.
        static const std::unordered_set<std::string> lowering_known = {
            "try",
            "catch",
            "raise",
            "require",
            "import",
            "use",
            "with-arena",
            "performance-region",
            "evolution-region",
        };

        for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
            auto nv = flat.get(id);

            // MacroDef cannot be lowered to IR
            if (nv.tag == aura::ast::NodeTag::MacroDef)
                return true;
            if (nv.tag == aura::ast::NodeTag::DefineType)
                return true;
            if (nv.tag == aura::ast::NodeTag::DefineModule)
                return true;

            // Dotted rest lambda cannot be lowered to IR (rest param is
            // lowered as single Arg slot, not as pair list)
            if (nv.tag == aura::ast::NodeTag::Lambda && nv.int_value != 0)
                return true;

            // TypeAnnotation with var_annot (3-arg form (: name Type val)):
            // tree-walker handles variable binding; IR/JIT cannot.
            if (nv.tag == aura::ast::NodeTag::TypeAnnotation && nv.int_value != 0)
                return true;

            // General variable references to user-defined top-level values
            // (from (define name val) where val is not a lambda) need tree-walker
            // because IR lowering can't resolve them (they live in evaluator's env).
            // Lambda params and let-bound vars are NOT in user_bindings_, so they
            // won't trigger fallback.
            // Keyword variables (:foo) are self-evaluating — need tree-walker
            if (nv.tag == aura::ast::NodeTag::Variable) {
                auto var_name = pool.resolve(nv.sym_id);
                if (!var_name.empty() && var_name[0] == ':')
                    return true;
                if (user_bindings_.count(std::string(var_name))) {
                    // Issue #1284: also accept ir_cache_v2_ define hits.
                    if (!name_in_ir_define_cache(std::string(var_name)))
                        return true;
                }
                auto vn = std::string(var_name);
                if (ir_value_cell_bindings_.count(vn))
                    continue;
                if (!vn.empty() &&
                    evaluator_.primitives().slot_for_name(vn) >=
                        evaluator_.primitives().slot_count() &&
                    !name_in_ir_define_cache(vn) && !lowering_known.count(vn)) {
                    return true;
                }
            }

            // Set nodes (from set!) must go through tree-walker because they
            // mutate cells in the evaluator's cell heap, not the IR's local heap.
            if (nv.tag == aura::ast::NodeTag::Set)
                return true;

            if (nv.tag == aura::ast::NodeTag::Call) {
                auto callee = nv.child(0);
                if (callee != aura::ast::NULL_NODE && callee < flat.size()) {
                    auto callee_v = flat.get(callee);
                    if (callee_v.tag == aura::ast::NodeTag::Variable) {
                        auto name = std::string(pool.resolve(callee_v.sym_id));

                        // Names starting with query: / mutate:[ ] trigger AST server fallback
                        if (name.starts_with("query:") || name.starts_with("mutate:"))
                            return true;

                        // Known tree-walker-only names (EDSL, special forms, module)
                        if (tree_walker_only.count(name))
                            return true;

                        // Catch binding forms like (catch (e) handler) have the
                        // variable binding (e) as a Call node with callee="e".
                        // These should not trigger fallback — the try lowering
                        // handles them explicitly. Skip fallback check for nodes
                        // whose parent is a catch form.
                        if (name == "catch")
                            continue;

                        // Call callee that's not a known primitive or cached define
                        // may come from a runtime import — fallback to tree-walker.
                        // Issue #1284: ir_cache_v2_ counts as a define-cache hit.
                        if (evaluator_.primitives().slot_for_name(name) >=
                                evaluator_.primitives().slot_count() &&
                            !name_in_ir_define_cache(name) && !lowering_known.count(name)) {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    // Issue #402: subtree-walked fallback check. Used by
    // the fast path in needs_tree_walker_fallback when
    // summary_flags_ == 0 (no MacroDef/DefineType/etc.
    // anywhere). Walks only the root subtree (typically
    // a handful of nodes for arithmetic / primitive-call
    // expressions) instead of the entire flat. Applies the
    // SAME per-node logic as the slow-path scan — Variable
    // keyword check, Call query:/mutate: prefix, primitive
    // slot lookup, etc. — but bounded by root size.
    bool subtree_needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                            const aura::ast::StringPool& pool,
                                            aura::ast::NodeId root) const {
        if (root == aura::ast::NULL_NODE || root >= flat.size())
            return false;

        // Reuse the same known-names sets as the slow path.
        // They're already lazily-initialized static — no
        // allocation per call.
        static const std::unordered_set<std::string> tree_walker_only = {
            // EDSL / AI agent primitives
            "define-type",
            "set-code",
            "eval-current",
            "apply",
            "typecheck-current",
            "typed-mutate",
            "typed-mutate-atomic", // Issue #1442
            "rollback",
            "mutation-log",
            "query-mutation-log",
            "mutate:rebind",
            "mutate:set-body",
            "mutate:remove-node",
            "mutate:move-node",
            "intend",
            "fiber:spawn",
            "fiber:join",
            "define-strategy",
            "register-strategy!",
            "intend-history",
            "intend-analytics",
            "strategy-field",
            "strategy-set-field!",
            "strategy-inspect",
            "coverage-report",
            "c-func",
            "c-load",
            "send",
            "recv",
            "my-id",
            "json-encode",
            "json-get-string",
            "json-parse",
            "orch:metrics",
            "orch:reset-metrics",
            "when",
            "set!",
            "load",
            "equal?",
            "unless",
            "export",
            "and",
            "or",
            "cond",
            "case",
            "with-capability",
            "check-capability",
            "capability-stack",
        };
        static const std::unordered_set<std::string> lowering_known = {
            "try",
            "catch",
            "raise",
            "require",
            "import",
            "use",
            "with-arena",
            "performance-region",
            "evolution-region",
        };

        bool needs = false;
        const std::size_t visited = flat.walk_subtree(root, [&](aura::ast::NodeId id) {
            if (needs)
                return; // short-circuit
            const auto v = flat.get(id);
            // Tag-only early-returns (mirrors slow path).
            switch (v.tag) {
                case aura::ast::NodeTag::MacroDef:
                case aura::ast::NodeTag::DefineType:
                case aura::ast::NodeTag::DefineModule:
                    needs = true;
                    return;
                case aura::ast::NodeTag::Lambda:
                    if (v.int_value != 0) {
                        needs = true;
                        return;
                    }
                    break;
                case aura::ast::NodeTag::TypeAnnotation:
                    if (v.int_value != 0) {
                        needs = true;
                        return;
                    }
                    break;
                case aura::ast::NodeTag::Set:
                    needs = true;
                    return;
                default:
                    break;
            }
            if (v.tag == aura::ast::NodeTag::Variable) {
                auto var_name = pool.resolve(v.sym_id);
                if (!var_name.empty() && var_name[0] == ':') {
                    needs = true;
                    return;
                }
                if (user_bindings_.count(std::string(var_name))) {
                    // Issue #1284: ir_cache_v2_ define cache avoids fallback.
                    if (!name_in_ir_define_cache(std::string(var_name))) {
                        needs = true;
                        return;
                    }
                }
                auto vn = std::string(var_name);
                if (ir_value_cell_bindings_.count(vn))
                    return;
                if (!vn.empty() &&
                    evaluator_.primitives().slot_for_name(vn) >=
                        evaluator_.primitives().slot_count() &&
                    !name_in_ir_define_cache(vn) && !lowering_known.count(vn)) {
                    needs = true;
                    return;
                }
            }
            if (v.tag == aura::ast::NodeTag::Call && v.children.empty() == false) {
                auto callee = v.child(0);
                if (callee != aura::ast::NULL_NODE && callee < flat.size()) {
                    const auto callee_v = flat.get(callee);
                    if (callee_v.tag == aura::ast::NodeTag::Variable) {
                        auto name = std::string(pool.resolve(callee_v.sym_id));
                        if (name.starts_with("query:") || name.starts_with("mutate:")) {
                            needs = true;
                            return;
                        }
                        if (tree_walker_only.count(name)) {
                            needs = true;
                            return;
                        }
                        if (name == "catch")
                            return;
                        // Issue #1284: ir_cache_v2_ define cache hits.
                        if (evaluator_.primitives().slot_for_name(name) >=
                                evaluator_.primitives().slot_count() &&
                            !name_in_ir_define_cache(name) && !lowering_known.count(name)) {
                            needs = true;
                            return;
                        }
                    }
                }
            }
        });
        // Issue #402 observability: record visited-node count
        // for the fast path. If visited < flat.size() (i.e.
        // subtree was smaller than flat), the fast path saved
        // the iteration cost.
        (void)visited;
        return needs;
    }

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
        return aura::core::make_unexpected_from_kind_name(aura::diag::kind_name(d.kind), d.message);
    }

    // Issue #807/#808/#809: convert an existing EvalResult without re-running eval.
    [[nodiscard]] static aura::core::AuraResult<types::EvalValue>
    eval_result_to_aura(const EvalResult& r) {
        if (r)
            return *r;
        const auto& d = r.error();
        return aura::core::make_unexpected_from_kind_name(aura::diag::kind_name(d.kind), d.message);
    }

    [[nodiscard]] EvalResult eval(std::string_view input) {
        // Issue #1431 follow-up Race #3 (and Race #4 #5 ...):
        // serialize the entire eval pipeline across threads.
        // Two concurrent eval callers share arena_ (which holds
        // the upstream memory_resource) and ASTArena::create
        // races on the polymorphic_allocator, surfacing as a
        // NULL memory_resource in pmr::memory_resource::allocate
        // from StringPool::rehash's _M_fill_assign. Each earlier
        // fix (TypeRegistry mutex, FlatAST add_node mutex)
        // exposed the next race in the parser/typecheck/eval
        // pipeline — eval_mutex_ serializes the whole pipeline
        // so all three races (and likely more) are resolved at
        // once. The push_back lock inside the Evaluator
        // (alloc_storage_lock_ tested by the stress test) is
        // unaffected — it protects cells_/pairs_/string_heap_
        // writes during eval, while eval_mutex_ protects the
        // parse + typecheck setup that runs before those writes.
        //
        // recursive_mutex: Path B (workspace fn call shortcut) may
        // re-enter eval() for non-literal args (e.g. string args
        // after eval_arg_fast fails). A plain mutex deadlocks the
        // calling thread and timed out jit_late1 on (f "hi") paths.
        std::lock_guard<std::recursive_mutex> lock(eval_mutex_);
        // Issue #411: snapshot the workspace mutation_log_ size at
        // entry so the post-eval auto-incremental typecheck can
        // detect whether this eval produced a new mutation
        // (mutate:* primitive) and run infer_flat_partial on the
        // most recent record. The check is "size grew" — when it
        // didn't, the eval was a pure read (define / typecheck /
        // query) and we skip the auto-invoke.
        cs_eval_mutation_log_size_at_entry_ = 0;
        if (auto* ws = evaluator_.workspace_flat())
            cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

        // Fix #165/#166 tests: workspace-aware eval shortcut. When a
        // entry so the post-eval auto-incremental typecheck can
        // detect whether this eval produced a new mutation
        // (mutate:* primitive) and run infer_flat_partial on the
        // most recent record. The check is "size grew" — when it
        // didn't, the eval was a pure read (define / typecheck /
        // query) and we skip the auto-invoke.
        cs_eval_mutation_log_size_at_entry_ = 0;
        if (auto* ws = evaluator_.workspace_flat())
            cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

        // Issue #411: RAII guard. Fires the auto-incremental
        // typecheck on every return path (tree-walker fallback,
        // early returns from the IR pipeline, the final return).
        PostEvalAutoInvokeGuard post_eval_guard(this, cs_eval_mutation_log_size_at_entry_);

        // Fix #165/#166 tests: workspace-aware eval shortcut. When a
        // workspace is set (via (set-code ...)), short-circuit bare
        // identifiers and top-level function calls so they find their
        // definitions in workspace_flat_ instead of being parsed into a
        // fresh FlatAST where workspace bindings are invisible.
        //
        // Path A (bare identifier): eval("x") — look up x's body in
        // workspace, eval body in top_env.
        //
        // Path B (function call): eval("(f arg1 arg2 ...)") — find f's
        // define in workspace, eval each arg in top_env, build a child
        // env with param bindings, eval f's body in that env. This
        // covers the common case (test_issue_166 tests 3 and 4 — calls
        // after set-code) without forcing set-code to install workspace
        // defines into top_env_ (which has been historically avoided
        // because of env-pollution concerns across reset/cycles).
        //
        // Macros (define-hygienic-macro / defmacro) still fall through
        // to the standard pipeline below, which runs macro_expand_all
        // against the freshly parsed AST. Macro-defined bindings will
        // still be picked up via Path A and Path B at the call-site
        // level; hygiene-preserving re-expansion after mutate:rebind
        // is a separate issue (#165 macro-re-expansion).
        if (evaluator_.workspace_flat() && evaluator_.workspace_pool()) {
            std::string trimmed(input);
            while (!trimmed.empty() && std::isspace((unsigned char)trimmed.front()))
                trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && std::isspace((unsigned char)trimmed.back()))
                trimmed.pop_back();
            bool is_bare = !trimmed.empty() && (std::isalpha((unsigned char)trimmed[0]) ||
                                                trimmed[0] == '_' || trimmed[0] == '-');
            for (char c : trimmed) {
                if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') {
                    is_bare = false;
                    break;
                }
            }
            auto* ws_flat = evaluator_.workspace_flat();
            auto* ws_pool = evaluator_.workspace_pool();
            if (is_bare) {
                for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                    auto v = ws_flat->get(id);
                    if (v.tag != aura::ast::NodeTag::Define)
                        continue;
                    if (v.sym_id == aura::ast::INVALID_SYM)
                        continue;
                    if (v.children.empty())
                        continue;
                    auto name = std::string(ws_pool->resolve(v.sym_id));
                    if (name != trimmed)
                        continue;
                    auto body_id = v.child(0);
                    return evaluator_.eval_flat(*ws_flat, *ws_pool, body_id, evaluator_.top_env());
                }
                // Not in workspace — fall through to the normal
                // pipeline (which will return an "undefined variable"
                // diagnostic just as it would without the workspace).
            } else if (!trimmed.empty() && trimmed.front() == '(') {
                // Function-call shortcut: parse "(head arg1 arg2 ...)"
                // directly to find the callee name without spinning up
                // a full FlatAST. Only handle the simple case (head is
                // a bare identifier matching a workspace define).
                auto close = trimmed.find_last_of(')');
                if (close != std::string::npos && close + 1 == trimmed.size()) {
                    // Scan inside the outer parens for the head symbol.
                    std::size_t i = 1;
                    while (i < trimmed.size() && std::isspace((unsigned char)trimmed[i]))
                        ++i;
                    std::size_t head_start = i;
                    while (i < trimmed.size() && !std::isspace((unsigned char)trimmed[i]) &&
                           trimmed[i] != ')')
                        ++i;
                    std::string head_name = trimmed.substr(head_start, i - head_start);
                    bool head_is_bare =
                        !head_name.empty() && (std::isalpha((unsigned char)head_name[0]) ||
                                               head_name[0] == '_' || head_name[0] == '-');
                    for (char c : head_name) {
                        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') {
                            head_is_bare = false;
                            break;
                        }
                    }
                    if (head_is_bare) {
                        // Look up head_name in workspace defines.
                        aura::ast::NodeId def_body = aura::ast::NULL_NODE;
                        std::vector<aura::ast::SymId> fn_params;
                        bool dotted = false;
                        for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                            auto v = ws_flat->get(id);
                            if (v.tag != aura::ast::NodeTag::Define)
                                continue;
                            if (v.sym_id == aura::ast::INVALID_SYM)
                                continue;
                            if (v.children.empty())
                                continue;
                            auto wname = std::string(ws_pool->resolve(v.sym_id));
                            if (wname != head_name)
                                continue;
                            auto body_id = v.child(0);
                            if (body_id >= ws_flat->size())
                                continue;
                            auto body_v = ws_flat->get(body_id);
                            if (body_v.tag != aura::ast::NodeTag::Lambda)
                                continue;
                            def_body =
                                body_v.children.empty() ? aura::ast::NULL_NODE : body_v.child(0);
                            fn_params.assign(body_v.params.begin(), body_v.params.end());
                            // Dotted flag is bit 0 of int_value (see ast.ixx
                            // add_lambda encoding).
                            dotted = (body_v.int_value & 1) != 0;
                            break;
                        }
                        if (def_body != aura::ast::NULL_NODE) {
                            // Tokenize the args between head_end and close.
                            // Simple splitter: walks parens / strings.
                            std::size_t arg_start = i;
                            std::vector<std::string> arg_strs;
                            int depth = 0;
                            bool in_str = false;
                            std::size_t tok_start = arg_start;
                            for (std::size_t k = arg_start; k < close; ++k) {
                                char ch = trimmed[k];
                                if (in_str) {
                                    if (ch == '\\' && k + 1 < close) {
                                        ++k;
                                        continue;
                                    }
                                    if (ch == '"')
                                        in_str = false;
                                    continue;
                                }
                                if (ch == '"') {
                                    in_str = true;
                                    continue;
                                }
                                if (ch == '(') {
                                    ++depth;
                                    continue;
                                }
                                if (ch == ')') {
                                    --depth;
                                    continue;
                                }
                                if (depth > 0)
                                    continue;
                                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                                    if (tok_start < k) {
                                        arg_strs.push_back(
                                            trimmed.substr(tok_start, k - tok_start));
                                        tok_start = k + 1;
                                    } else {
                                        tok_start = k + 1;
                                    }
                                }
                            }
                            if (tok_start < close) {
                                arg_strs.push_back(trimmed.substr(tok_start, close - tok_start));
                            }
                            // Evaluate args in top_env (recursively via eval()).
                            // We pass them through eval() for non-trivial
                            // expressions (so they see macros / IR cache),
                            // but for a pure numeric literal we parse
                            // directly to avoid the standard pipeline's
                            // global-state mutation (set_flat_pool,
                            // set_current_flat, last_ir_mod_) that was
                            // causing the second recursive eval to
                            // return the first's result.
                            std::vector<aura::compiler::types::EvalValue> arg_vals;
                            arg_vals.reserve(arg_strs.size());
                            for (auto& as : arg_strs) {
                                aura::compiler::types::EvalValue val = eval_arg_fast(as);
                                if (val.val == 0 && as != "0") {
                                    // Fallback: try full eval for non-numeric args
                                    auto ar = eval(as);
                                    if (!ar)
                                        return ar;
                                    val = *ar;
                                }
                                arg_vals.push_back(val);
                            }
                            // Build a child env with param bindings.
                            Env call_env(&evaluator_.top_env());
                            call_env.set_primitives(&evaluator_.primitives());
                            if (ws_pool)
                                call_env.set_pool(ws_pool);
                            std::size_t named = dotted && !fn_params.empty() ? fn_params.size() - 1
                                                                             : fn_params.size();
                            for (std::size_t pi = 0; pi < named && pi < arg_vals.size(); ++pi) {
                                call_env.bind_symid(fn_params[pi], std::move(arg_vals[pi]));
                            }
                            // Dotted-rest: collect remaining args into a pair list.
                            if (dotted && !fn_params.empty() && arg_vals.size() > named) {
                                types::EvalValue rest = types::make_void();
                                for (std::size_t ri = arg_vals.size(); ri > named; --ri) {
                                    std::size_t pid = evaluator_.pairs().size();
                                    evaluator_.pairs().push_back(
                                        {std::move(arg_vals[ri - 1]), rest});
                                    rest = types::make_pair(pid);
                                }
                                call_env.bind_symid(fn_params.back(), rest);
                            }
                            auto dbg_ret =
                                evaluator_.eval_flat(*ws_flat, *ws_pool, def_body, call_env);
                            return dbg_ret;
                        }
                        // head is a Variable but no workspace define —
                        // fall through to the normal pipeline (which
                        // handles primitives, IR-cached calls, etc.).
                    }
                }
            }
        }
        // Phase 4: parse directly into FlatAST, evaluator reads FlatAST directly.
        // Arena-allocate FlatAST/Pool so closures can reference them across calls.
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
        if (pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        // If there were parse errors but we recovered, log them and continue
        if (!pr.success && !pr.errors.empty()) {
            for (auto& e : pr.errors)
                std::println(std::cerr, "parse warning: {}", e.format());
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting (CompilerService's tracking, used by last_flat/last_pool)
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;
        // Dual-workspace (Phase 1): make this flat/pool visible to
        // source-reading primitives (current-source default, etc.) and
        // also to mutate:* primitives. Set BEFORE any user code runs.
        // See dual-workspace design (archived: docs-archive-pre-2026-06)
        evaluator_.set_flat_pool(flat_ptr, pool_ptr);
        evaluator_.set_current_flat(flat_ptr);
        evaluator_.set_current_pool(pool_ptr);

        // Pre-expand all macros in this expression
        auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Pre-execute top-level require/import/use calls to fill ir_cache_
        // so the remaining expression can go through the IR path without fallback.
        // Issue #123: capture the (possibly stripped) root and use it for
        // the rest of the eval pipeline.
        expanded_root = pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);
        // After pre_exec, root may be NULL_NODE (if the whole program
        // was a single require). Treat as a no-op.
        if (expanded_root == aura::ast::NULL_NODE) {
            return EvalResult(types::make_void());
        }

        // Compile-time AST validation (structural correctness)
        validate_ast(*flat_ptr, *pool_ptr, expanded_root);

        // Register ADT constructors from define-type forms (for match exhaustiveness)
        register_adt_from_define_types(*flat_ptr, *pool_ptr, expanded_root);

        // Collect match clause metadata on expanded flat (post-macro-expansion,
        // where node IDs are stable for the type checker and tree-walker)
        collect_match_info(*flat_ptr, *pool_ptr, expanded_root);

        // If root is a define form, skip fallback check — the IR pipeline
        // handles defines via try_extract_define + cache_define.
        // The Variable checks in needs_tree_walker_fallback would otherwise
        // always trigger fallback (variables in function body aren't in cache yet).
        auto expanded_v =
            expanded_root < flat_ptr->size() ? flat_ptr->get(expanded_root) : aura::ast::NodeView{};
        bool is_fn_define = false;
        if (expanded_v.tag == aura::ast::NodeTag::Define && !expanded_v.children.empty()) {
            auto body_id = expanded_v.child(0);
            if (body_id < flat_ptr->size()) {
                auto body_node = flat_ptr->get(body_id);
                if (body_node.tag == aura::ast::NodeTag::Lambda)
                    is_fn_define = true;
            }
        }

        if (!is_fn_define && needs_tree_walker_fallback(*flat_ptr, *pool_ptr, expanded_root)) {
            auto result =
                evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
            // Track all bound names so subsequent eval calls don't fall
            // through to the IR pipeline (which silently returns 0 for
            // unknown vars). Issue #132: extracted to
            // aura::compiler.collect_user_bindings.
            for (auto& name :
                 aura::compiler::collect_user_bindings(*flat_ptr, *pool_ptr, expanded_root)) {
                user_bindings_.insert(std::move(name));
            }
            return result;
        }

        // === Level 2: Type check via TypeCheckWrap pass ===
        // Issue #280: tc_pass is declared at this scope (not in
        // an inner block) so the IR pipeline below can read the
        // narrowing evidence captured by the type check.
        // Issue #283 follow-up #5: propagate bidirectional_mode_
        // so the user-controlled opt-out flag flows into the
        // typechecker.
        aura::compiler::TypeCheckWrap tc_pass;
        aura::diag::DiagnosticCollector diags;
        tc_pass.set_bidirectional_mode(bidirectional_mode_);
        tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, expanded_root, type_registry_, diags,
                                      mutation_epoch_.load(std::memory_order_relaxed), &metrics_);
        {
            bool has_type_error = false;
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError ||
                    d.kind == aura::diag::ErrorKind::Note) {
                    std::println(std::cerr, "type: {}", d.format());
                    if (d.kind == aura::diag::ErrorKind::TypeError)
                        has_type_error = true;
                }
            }
            if (strict_mode_ && has_type_error) {
                return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                              "type error (strict mode)"});
            }
        }

        // Issue #1506: before define / call IR path, partially re-lower
        // any dirty ir_cache_v2_ entries (body-only bitmasks from
        // mark_define_dirty after set-body / rebind). Mirrors
        // eval-current's relower_dirty_defines_fn_ hook so eval("(f 5)")
        // after mutate consumes partial re-lower, not only full lower.
        (void)relower_dirty_defines_from_workspace();

        // Check for top-level (define ...) — cache IR + eval tree-walker for env persistence
        auto def = try_extract_define(*flat_ptr, *pool_ptr, expanded_root);
        if (def) {
            auto& [name, body_id] = *def;
            // Only cache function defines (Lambda body) are cached as IR
            // Value defines must go through tree-walker for env persistence
            auto body_node =
                body_id < flat_ptr->size() ? flat_ptr->get(body_id) : aura::ast::NodeView{};
            if (body_node.tag == aura::ast::NodeTag::Lambda) {
                // Issue #1506: prefer partial re-lower when already cached+dirty.
                auto result = cache_define_prefer_partial(input, *flat_ptr, *pool_ptr,
                                                          expanded_root, std::string(name));
                if (!result)
                    return result;
                user_bindings_.insert(std::string(name));
                return EvalResult(types::make_void());
            }
            // Issue #272 Cycle 3: value define via IR when the body is lowerable.
            if (!define_body_needs_tree_walker_fallback(*flat_ptr, *pool_ptr, body_id,
                                                        std::string(name))) {
                if (bind_value_define_via_ir(*flat_ptr, *pool_ptr, expanded_root,
                                             std::string(name))) {
                    auto bound = evaluator_.top_env().lookup_binding(std::string(name));
                    if (bound && types::is_cell(*bound))
                        return evaluator_.cells()[types::as_cell_id(*bound)];
                    return EvalResult(types::make_void());
                }
            }
            // Value define: try compile-time constant evaluation
            auto const_val = try_const_eval(*flat_ptr, *pool_ptr, body_id);
            if (const_val) {
                auto ci = bind_define_value_in_env(std::string(name), *const_val);
                ir_value_cell_bindings_[std::string(name)] = ci;
                return *const_val;
            }
            // Fallback: tree-walker for env persistence + subsequent fallback tracking.
            auto result =
                evaluator_.eval_flat(*flat_ptr, *pool_ptr, expanded_root, evaluator_.top_env());
            user_bindings_.insert(std::string(name));
            return result;
        }

        // ========== IR pipeline (default path for non-define expressions) ==========
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            *flat_ptr, *pool_ptr, arena_, cache_ptr, nullptr, &evaluator_.primitives(), nullptr,
            cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering(),
            tc_pass.last_narrowing_evidence()); // Issue #280

        // Run passes (silent in default path — use eval_ir for debug)
        // Issue #1457: TypePropagationPass before DCE so CastOp
        // type_id / narrow_evidence reach zero-overhead elision.
        TypeSpecializationWrap ts(&type_registry_);
        TypePropagationPass tprop(&type_registry_);
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        DeadCoercionEliminationPass dce(&type_registry_);
        ts.run(ir_mod);
        tprop.run(ir_mod);
        ck.run(ir_mod);
        ar.run(ir_mod);
        cf.run(ir_mod);
        dce.run(ir_mod);
        accumulate_type_propagation_metrics(tprop);
        // Issue #253: accumulate linear-move elision count
        // into the shared metrics so snapshot() + the Aura
        // primitive (compile:linear-elide-count) read a single
        // source of truth.
        if (ts.linear_elide_count() > 0) {
            metrics_.linear_elide_count.fetch_add(ts.linear_elide_count(),
                                                  std::memory_order_relaxed);
        }
        // Issue #433: dead coercion elimination
        // observability. Accumulate the dce pass's
        // eliminated_count() into the lifetime
        // CompilerMetrics counter. The pass existed
        // pre-#433 and was wired into the pipeline
        // (here, in the dce.run() call above), but
        // the eliminated_count was never surfaced
        // to the user — the metric lived only on
        // the per-call pass instance. Post-#433 the
        // lifetime total is readable via snapshot()
        // and the new (compile:dead-coercion-stats)
        // Aura primitive.
        // Issue #508 / #629 / #538: coercion elimination observability.
        accumulate_coercion_pass_metrics(ts, dce);

        if (ar.has_error()) {
            for (auto& d : ar.result().diagnostics) {
                std::println(std::cerr, "arity warning: {}", d.message);
            }
        }

        // ── Run escape analysis pass ───────────────────────
        // Issue #1531: EscapeAnalysisWrap (JIT maps) + EscapeAnalysisPass
        // (IRFunction::escape_map + linear ownership propagators) +
        // LinearOwnershipPass for post-escape ownership validation.
        EscapeAnalysisWrap escape_pass;
        escape_pass.run(ir_mod);
        EscapeAnalysisPass ir_escape;
        ir_escape.run(ir_mod);
        LinearOwnershipPass linear_own;
        linear_own.run(ir_mod);
        metrics_.ir_escape_analysis_runs_total.fetch_add(1, std::memory_order_relaxed);
        if (ir_escape.escaped_slots_total() > 0) {
            metrics_.ir_escape_slots_marked_total.fetch_add(ir_escape.escaped_slots_total(),
                                                            std::memory_order_relaxed);
        }
        metrics_.linear_ownership_escape_check_total.fetch_add(1, std::memory_order_relaxed);

        // Issue #462: ShapeAwareFoldingPass — runs AFTER
        // EscapeAnalysis so it can use the per-function
        // escape_maps to drive linear elision (MoveOp on
        // a non-escaping Owned slot is a no-op). Also runs
        // AFTER ConstantFolding so any fold-attempted ops
        // are already reduced to a stable form. Accumulates
        // fold_count / linear_elide_count / narrow_check_count
        // into the lifetime metrics for (query:shape-folding-stats).
        ShapeAwareFoldingPass saf;
        saf.run(ir_mod);
        if (saf.fold_count() > 0) {
            metrics_.shape_fold_count.fetch_add(saf.fold_count(), std::memory_order_relaxed);
        }
        if (saf.linear_elide_count() > 0) {
            metrics_.shape_linear_elide_count.fetch_add(saf.linear_elide_count(),
                                                        std::memory_order_relaxed);
        }
        if (saf.narrow_check_count() > 0) {
            metrics_.shape_narrow_check_count.fetch_add(saf.narrow_check_count(),
                                                        std::memory_order_relaxed);
        }
        if (saf.guard_shape_hits() > 0) {
            metrics_.guard_shape_hits.fetch_add(saf.guard_shape_hits(), std::memory_order_relaxed);
        }

        last_ir_mod_ = ir_mod;
        last_ir_stats_ = aura::ir::compute_ir_stats(*last_ir_mod_);
        last_escape_maps_ = escape_pass.take_maps();

// ── Try JIT execution when LLVM available ──────────────
#ifdef AURA_HAVE_LLVM
        {
            if (!jit_initialized_) {
                register_jit_primitives();
                jit_initialized_ = true;
            }

            // Only try JIT for expressions without complex structural nodes
            // (Lambda, strings, floats, Let/LetRec, macros, quotes, coercions).
            // Named function calls like (add 42) ARE allowed (unlike the old
            // jit_safe_primitives guard which blocked all non-arithmetic calls).
            bool skip_jit = false;
            for (aura::ast::NodeId nid = 0; nid < flat_ptr->size(); ++nid) {
                auto nv = flat_ptr->get(nid);
                auto tag = nv.tag;
                if (tag == aura::ast::NodeTag::Lambda || tag == aura::ast::NodeTag::LiteralString ||
                    tag == aura::ast::NodeTag::LiteralFloat || tag == aura::ast::NodeTag::Let ||
                    tag == aura::ast::NodeTag::LetRec || tag == aura::ast::NodeTag::Quote ||
                    tag == aura::ast::NodeTag::Coercion || tag == aura::ast::NodeTag::MacroDef) {
                    skip_jit = true;
                    break;
                }
                // Check Call nodes for non-user-defined callees.
                // Built-in primitives (number?, cons, display, etc.) have
                // different result encoding than user-defined functions.
                // Allow calls to cached user-defined functions to pass
                // through for JIT optimization.
                if (tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                    auto callee_id = nv.child(0);
                    if (callee_id < flat_ptr->size()) {
                        auto callee_v = flat_ptr->get(callee_id);
                        if (callee_v.tag == aura::ast::NodeTag::Variable) {
                            auto callee_name = pool_ptr->resolve(callee_v.sym_id);
                            auto cname = std::string(callee_name);
                            // Only block if NOT a cached user-defined function
                            if (ir_cache_.count(cname) == 0) {
                                skip_jit = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (!skip_jit) {
                auto jit_result = try_jit_execute(ir_mod, &last_escape_maps_);
                if (jit_result) {
                    record_eval_result_shape(session_id_, last_ir_mod_, nullptr, *jit_result);
                    return *jit_result;
                }
            }
        }
#endif

        aura::compiler::IRContext ctx(evaluator_.primitives(), &type_registry_, &metrics_,
                                      &evaluator_);
        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, ctx);
        ir_interp.set_strategy(strategy_);
        if (!last_escape_maps_.empty())
            ir_interp.set_escape_maps(last_escape_maps_);
        if (strict_mode_)
            ir_interp.set_strict_mode(true);

        try {

            // Set IR closure bridge: enables tree-walker primitives (map/filter/foldl)
            // to call IR-produced closures.
            evaluator_.set_closure_bridge([this, &ir_interp,
                                           &ir_mod](aura::compiler::ClosureId cid,
                                                    std::span<const types::EvalValue> args)
                                              -> std::optional<types::EvalValue> {
                auto snap = ir_interp.inspect_closure(cid);
                if (!snap) {
                    if (auto ir_define = dispatch_ir_define_closure(cid, args))
                        return ir_define;
                    return std::nullopt;
                }

                // Issue #708 / thread-pool-enqueue-stdin: construct the bridge
                // Env with top_env as parent so owner_ is inherited (mirrors the
                // sibling bridge below). A default Env leaves owner_=nullptr →
                // eval_env.owner()->bump_primitive_call_count() null-derefs
                // (SIGSEGV si_addr=0x528) on the first primitive call in an IR
                // bridge, e.g. the body of an enqueued (lambda () (+ 1 2)).
                aura::compiler::Env ne(&evaluator_.top_env());
                ne.set_primitives(&evaluator_.primitives());
                for (std::size_t i = 0; i < snap->env.size() && i < snap->func_free_vars.size();
                     ++i)
                    ne.bind(snap->func_free_vars[i], snap->env[i]);
                for (std::size_t i = 0; i < snap->func_params.size() && i < args.size(); ++i)
                    ne.bind(snap->func_params[i], args[i]);

                // Try fast path: bridge data from current module.
                // Issue #224 Cycle 2: shared_ptr is a non-owning view;
                // we dereference to get the raw reference. The const
                // is discarded via const_cast because eval_flat takes
                // non-const refs (the FlatAST itself isn't mutated by
                // eval_flat's implementation, but the signature is
                // non-const for historical reasons).
                if (snap->func_id < last_ir_mod_->closure_bridge.size()) {
                    auto& bd = last_ir_mod_->closure_bridge[snap->func_id];
                    if (bd.flat && bd.pool) {
                        auto r = evaluator_.eval_flat(*const_cast<ast::FlatAST*>(bd.flat.get()),
                                                      *const_cast<ast::StringPool*>(bd.pool.get()),
                                                      bd.body_id, ne);
                        if (r) {
                            return *r;
                        }
                    }
                    // Fallback: re-parse lambda body from body_source
                    // (needed when cached function's inner lambda has stale FlatAST ptr)
                    if (!bd.body_source.empty()) {
                        auto fallback_alloc = arena_.allocator();
                        auto* f_pool = arena_.create<aura::ast::StringPool>(fallback_alloc);
                        auto* f_flat = arena_.create<aura::ast::FlatAST>(fallback_alloc);
                        auto f_pr = aura::parser::parse_to_flat(bd.body_source, *f_flat, *f_pool);
                        if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                            f_flat->root = f_pr.root;
                            auto r = evaluator_.eval_flat(*f_flat, *f_pool, f_pr.root, ne);
                            if (r)
                                return *r;
                        }
                    }
                }

                // Fallback: re-parse from function_sources_ (survives arena resets)
                auto func_name = snap->func_name;
                auto src_it = function_sources_.find(func_name);
                if (src_it != function_sources_.end()) {
                    auto fallback_alloc = arena_.allocator();
                    auto* f_pool = arena_.create<aura::ast::StringPool>(fallback_alloc);
                    auto* f_flat = arena_.create<aura::ast::FlatAST>(fallback_alloc);
                    auto f_pr = aura::parser::parse_to_flat(src_it->second, *f_flat, *f_pool);
                    if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                        f_flat->root = f_pr.root;
                        // The source is a (define name body) — body is child 0
                        auto define_v = f_flat->get(f_pr.root);
                        if (define_v.tag == aura::ast::NodeTag::Define &&
                            !define_v.children.empty()) {
                            auto r = evaluator_.eval_flat(*f_flat, *f_pool, define_v.child(0), ne);
                            if (r)
                                return *r;
                        }
                    }
                }
                return std::nullopt;
            });

            auto result = ir_interp.execute();

            // Restore persistent define bridge (session bridge references ir_interp).
            install_persistent_define_closure_bridge();

            last_closures_ = ir_interp.list_closures();
            last_cells_ = ir_interp.list_cells();
            return result;
        } catch (const std::exception& e) {
            return std::unexpected(aura::diag::Diagnostic{
                aura::diag::ErrorKind::TypeError, std::string("runtime type error: ") + e.what()});
        }
    }

    // ---- IR pipeline ------------------------------------------------

    [[nodiscard]] EvalResult eval_ir(std::string_view input) {
        // Issue #411: snapshot mutation_log_ size at entry so the
        // post-eval auto-incremental typecheck can detect new
        // mutations. See eval() above for the full pattern.
        cs_eval_mutation_log_size_at_entry_ = 0;
        if (auto* ws = evaluator_.workspace_flat())
            cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

        // Issue #411: RAII guard. See eval() above.
        PostEvalAutoInvokeGuard post_eval_guard(this, cs_eval_mutation_log_size_at_entry_);

        // Phase 4: parse directly into FlatAST (bypasses Expr* entirely)
        // Arena-allocate FlatAST/Pool so closures can reference them across calls.
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;
        // Store for mutation targeting
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;
        // Dual-workspace (Phase 1): propagate to Evaluator.
        evaluator_.set_flat_pool(flat_ptr, pool_ptr);
        evaluator_.set_current_flat(flat_ptr);
        evaluator_.set_current_pool(pool_ptr);

        // IR pipeline doesn't support macros — fall back to tree-walker evaluator
        for (aura::ast::NodeId id = 0; id < flat_ptr->size(); ++id) {
            if (flat_ptr->get(id).tag == aura::ast::NodeTag::MacroDef) {
                return evaluator_.eval_flat(*flat_ptr, *pool_ptr, flat_ptr->root,
                                            evaluator_.top_env());
            }
        }

        // Pre-expand all macros in this expression
        auto expanded_root = aura::compiler::macro_expand_all(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Pre-execute top-level require/import/use calls to fill ir_cache_
        // so cached define functions are available during lowering.
        // Issue #123: capture the (possibly stripped) root.
        expanded_root = pre_exec_requires(*flat_ptr, *pool_ptr, expanded_root);
        if (expanded_root == aura::ast::NULL_NODE) {
            return EvalResult(types::make_void());
        }

        // Update root to expanded version
        flat_ptr->root = expanded_root;

        // Compile-time AST validation
        validate_ast(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Pre-evaluate define-type forms to register constructor primitives
        // before IR lowering, so constructor calls in the code are resolvable.
        for (aura::ast::NodeId nid = 0; nid < flat_ptr->size(); ++nid) {
            if (flat_ptr->get(nid).tag == aura::ast::NodeTag::DefineType) {
                (void)evaluator_.eval_flat(*flat_ptr, *pool_ptr, nid, evaluator_.top_env());
            }
        }

        // Re-register ADT constructors from define-types for match exhaustiveness
        register_adt_from_define_types(*flat_ptr, *pool_ptr, flat_ptr->root);

        // Issue #73: run typecheck before lowering so the FlatAST's
        // type_id column is populated. Without this, the IR-direct
        // path (eval_ir) reads 0 from flat.type_id() and the IR's
        // type_id field is never set — which means type-driven
        // optimizations (TypeSpecializationWrap), the runtime type
        // check (IRInterpreter), and --inspect ir all see no types.
        // Compare to eval() (line ~661) which does this already.
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.set_bidirectional_mode(bidirectional_mode_);
            tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, flat_ptr->root, type_registry_,
                                          diags, mutation_epoch_.load(std::memory_order_relaxed),
                                          &metrics_);
            bool has_type_error = false;
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError) {
                    std::println(std::cerr, "type warning (eval_ir): {}", d.format());
                    has_type_error = true;
                }
            }
            if (strict_mode_ && has_type_error) {
                return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                              "type error (strict mode, eval_ir)"});
            }
        }

        // Issue #1506: partial re-lower dirty defines before define/call
        // paths (parity with eval() + eval-current).
        (void)relower_dirty_defines_from_workspace();

        // === Phase 1: Define separation (IR caching) ===
        auto def = try_extract_define(*flat_ptr, *pool_ptr, flat_ptr->root);
        if (def) {
            auto& [name, _body_id] = *def;
            // Issue #1506: prefer partial re-lower when already cached+dirty.
            auto result = cache_define_prefer_partial(input, *flat_ptr, *pool_ptr, flat_ptr->root,
                                                      std::string(name));
            if (!result)
                return result;
            return EvalResult(types::make_void());
        }

        // === Normal IR path (with cache awareness) ===
        auto cache_ptr_local = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            *flat_ptr, *pool_ptr, arena_, cache_ptr_local, nullptr, &evaluator_.primitives(),
            nullptr, cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());

        TypeSpecializationWrap ts(&type_registry_);
        TypePropagationPass tprop(&type_registry_);
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        // Issue #1418: DeadCoercionEliminationPass was wired on the
        // default eval() path but missing from eval_ir's run_pipeline
        // pack — dead CastOps accumulated on --inspect / IR-direct.
        // Issue #1457: TypePropagation before DCE for CastOp elision.
        DeadCoercionEliminationPass dce(&type_registry_);
        const std::uint64_t pipeline_epoch = mutation_epoch_.load(std::memory_order_relaxed);
        ts.set_pipeline_epoch(pipeline_epoch);
        tprop.set_pipeline_epoch(pipeline_epoch);
        ar.set_pipeline_epoch(pipeline_epoch);
        cf.set_pipeline_epoch(pipeline_epoch);
        dce.set_pipeline_epoch(pipeline_epoch);

        std::println(std::cerr, "PM: running {}->{}->{}->{}->{}->{}", ts.name(), tprop.name(),
                     ck.name(), ar.name(), cf.name(), dce.name());

        // Issue #163: use run_pipeline (Pass concept fold) instead of
        // individual *.run() calls. Short-circuits on has_error().
        // Issue #1418: include DCE after ConstantFolding.
        // Issue #1457: type-propagation between TypeSpec and DCE.
        aura::compiler::run_pipeline(ir_mod, ts, tprop, ck, ar, cf, dce);
        accumulate_type_propagation_metrics(tprop);
        accumulate_coercion_pass_metrics(ts, dce);

        if (ar.has_error()) {
            for (auto& d : ar.result().diagnostics) {
                std::println(std::cerr, "arith: {}", d.message);
            }
            return std::unexpected(
                aura::diag::Diagnostic{aura::diag::ErrorKind::ArityMismatch, "arity check failed"});
        }

        // Issue #253: accumulate linear-move elision count.
        if (ts.linear_elide_count() > 0) {
            metrics_.linear_elide_count.fetch_add(ts.linear_elide_count(),
                                                  std::memory_order_relaxed);
        }

        if (cf.folded_count() > 0) {
            // Debug print removed (#63723): was polluting
            // test framework stream redirect for tests like
            // edsl-ir-cache:cascade-after-mutate. The folded
            // count is already in metrics_ via cf_pass metrics.
        }

        // ── Run escape analysis pass ───────────────────────
        EscapeAnalysisWrap escape_pass;
        escape_pass.run(ir_mod);

        last_ir_mod_ = ir_mod;
        last_ir_stats_ = aura::ir::compute_ir_stats(*last_ir_mod_);
        last_escape_maps_ = escape_pass.take_maps();

        aura::compiler::IRContext ctx(evaluator_.primitives(), &type_registry_, &metrics_,
                                      &evaluator_);
        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, ctx);
        ir_interp.set_strategy(strategy_);
        if (!last_escape_maps_.empty())
            ir_interp.set_escape_maps(last_escape_maps_);
        if (strict_mode_)
            ir_interp.set_strict_mode(true);

        // Set IR closure bridge — enables tree-walker primitives (map/filter/foldl)
        // to call IR-produced closures.
        evaluator_.set_closure_bridge([this, &ir_interp](aura::compiler::ClosureId cid,
                                                         std::span<const types::EvalValue> args)
                                          -> std::optional<types::EvalValue> {
            auto snap = ir_interp.inspect_closure(cid);
            if (!snap)
                return std::nullopt;
            // Bridge Env must inherit `owner_` from top_env so primitive
            // dispatch (which calls eval_env.owner()->bump_primitive_call_count)
            // can find the CompilerMetrics via the SoA walk. Default-constructing
            // `Env ne` leaves owner_=nullptr → SIGSEGV (si_addr=0x528, vtable
            // slot offset) on the first primitive call inside an IR bridge,
            // e.g. `(map (lambda (x) (+ x 1)) (list 1 2 3))`. Mirrors
            // dispatch_ir_define_closure's pattern at the cached-define site.
            aura::compiler::Env ne(&evaluator_.top_env());
            ne.set_primitives(&evaluator_.primitives());
            for (std::size_t i = 0; i < snap->env.size() && i < snap->func_free_vars.size(); ++i)
                ne.bind(snap->func_free_vars[i], snap->env[i]);
            for (std::size_t i = 0; i < snap->func_params.size() && i < args.size(); ++i)
                ne.bind(snap->func_params[i], args[i]);
            // Try bridge data from IR module. Issue #224 Cycle 2:
            // shared_ptr is a non-owning view; dereference via .get()
            // and const_cast (eval_flat's signature takes non-const
            // refs but doesn't actually mutate the FlatAST).
            if (snap->func_id < last_ir_mod_->closure_bridge.size()) {
                auto& bd = last_ir_mod_->closure_bridge[snap->func_id];
                if (bd.flat && bd.pool) {
                    auto r = evaluator_.eval_flat(*const_cast<ast::FlatAST*>(bd.flat.get()),
                                                  *const_cast<ast::StringPool*>(bd.pool.get()),
                                                  bd.body_id, ne);
                    if (r)
                        return *r;
                }
                if (!bd.body_source.empty()) {
                    auto fallback_alloc = arena_.allocator();
                    auto* f_pool = arena_.create<aura::ast::StringPool>(fallback_alloc);
                    auto* f_flat = arena_.create<aura::ast::FlatAST>(fallback_alloc);
                    auto f_pr = aura::parser::parse_to_flat(bd.body_source, *f_flat, *f_pool);
                    if (f_pr.success && f_pr.root != aura::ast::NULL_NODE) {
                        f_flat->root = f_pr.root;
                        auto r = evaluator_.eval_flat(*f_flat, *f_pool, f_pr.root, ne);
                        if (r)
                            return *r;
                    }
                }
            }
            return std::nullopt;
        });

        auto result = ir_interp.execute();

        // Clear bridge after execution
        install_persistent_define_closure_bridge();

        // Capture runtime state for --inspect
        last_closures_ = ir_interp.list_closures();
        last_cells_ = ir_interp.list_cells();

        // ── Shape profile recording (Phase 1, #53) ─────────────
        if (result) {
            record_eval_result_shape(session_id_, last_ir_mod_, &ir_interp, *result);
        }

        // Issue #411: post-eval auto-incremental typecheck.
        // The guard (constructed in eval_ir) fires the auto-
        // invoke on scope exit, covering ALL return paths.
        // See PostEvalAutoInvokeGuard for the full pattern.
        return result;
    }

    // ── --jit: compile via LLVM ORC JIT and execute ──────────────
    // --jit: compile via LLVM ORC JIT and execute
    [[nodiscard]] EvalResult exec_jit(std::string_view input) {
        // Issue #411: snapshot mutation_log_ size at entry. See
        // eval() above for the full pattern.
        cs_eval_mutation_log_size_at_entry_ = 0;
        if (auto* ws = evaluator_.workspace_flat())
            cs_eval_mutation_log_size_at_entry_ = ws->all_mutations().size();

        // Issue #411: RAII guard. See eval() above.
        PostEvalAutoInvokeGuard post_eval_guard(this, cs_eval_mutation_log_size_at_entry_);

#ifdef AURA_HAVE_LLVM
        // Issue #59 Iter 3: shared-lock the Mutation Lock for the
        // duration of the JIT compile path. Compiles can run
        // concurrently with each other, but a mutate:* must wait
        // for in-flight compiles to drain.
        std::shared_lock mutate_read(mutate_mtx_);
        // Parse expression
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(input, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;
        current_ast_ = flat_ptr;
        current_pool_ = pool_ptr;
        // Dual-workspace (Phase 1): propagate to Evaluator.
        evaluator_.set_flat_pool(flat_ptr, pool_ptr);
        evaluator_.set_current_flat(flat_ptr);
        evaluator_.set_current_pool(pool_ptr);

        // Issue #73 Phase 5: run typecheck before lowering so the
        // FlatAST's type_id column is populated. Without this, the
        // JIT path compiles type-blind IR (every instruction's
        // type_id = 0) and any type-driven optimization (CastOp
        // insertion via TypeSpecializationWrap, type-aware
        // specialization) sees no types. Compare to eval_ir
        // (line ~957) and eval (line ~663) which both do this already.
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.set_bidirectional_mode(bidirectional_mode_);
            tc_pass.check_before_lowering(*flat_ptr, *pool_ptr, flat_ptr->root, type_registry_,
                                          diags, mutation_epoch_.load(std::memory_order_relaxed),
                                          &metrics_);
            bool has_type_error = false;
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError) {
                    std::println(std::cerr, "type warning (exec_jit): {}", d.format());
                    has_type_error = true;
                }
            }
            if (strict_mode_ && has_type_error) {
                return std::unexpected(aura::diag::Diagnostic{
                    aura::diag::ErrorKind::TypeError, "type error (strict mode, exec_jit)"});
            }
        }

        // Lower to IR
        auto ir_mod = aura::compiler::lower_to_ir(*flat_ptr, *pool_ptr, arena_,
                                                  &evaluator_.primitives(), &type_registry_);

        // Run passes
        {
            aura::compiler::TypeSpecializationWrap ts(&type_registry_);
            aura::compiler::TypePropagationPass tprop(&type_registry_);
            aura::compiler::ComputeKindWrap ck;
            aura::compiler::ArityWrap ar;
            aura::compiler::ConstantFoldingWrap cf;
            // Issue #1418: DCE was missing from the exec_jit
            // run_pipeline pack (only default eval + post-mutate
            // re-lower ran it). Wire after ConstantFolding so JIT
            // never compiles dead CastOps.
            // Issue #1457: TypePropagation before DCE.
            aura::compiler::DeadCoercionEliminationPass dce(&type_registry_);
            const std::uint64_t pipeline_epoch = mutation_epoch_.load(std::memory_order_relaxed);
            ts.set_pipeline_epoch(pipeline_epoch);
            tprop.set_pipeline_epoch(pipeline_epoch);
            ar.set_pipeline_epoch(pipeline_epoch);
            cf.set_pipeline_epoch(pipeline_epoch);
            dce.set_pipeline_epoch(pipeline_epoch);
            // Issue #163: run_pipeline (Pass concept fold) replaces
            // the individual *.run() calls. Issue #1418: include DCE.
            aura::compiler::run_pipeline(ir_mod, ts, tprop, ck, ar, cf, dce);
            accumulate_type_propagation_metrics(tprop);
            accumulate_coercion_pass_metrics(ts, dce);
            // Issue #253: accumulate linear-move elision count.
            if (ts.linear_elide_count() > 0) {
                metrics_.linear_elide_count.fetch_add(ts.linear_elide_count(),
                                                      std::memory_order_relaxed);
            }
        }

        // ── Run escape analysis pass ───────────────────────
        EscapeAnalysisWrap escape_pass;
        escape_pass.run(ir_mod);

        if (ir_mod.functions.empty()) {
            return EvalResult(types::make_void());
        }


        // Register primitives with JIT runtime (first call only)
        if (!jit_initialized_) {
            register_jit_primitives();
            jit_initialized_ = true;
        }

        // Pass string pool to JIT compiler for OpConstString support
        jit_.set_string_pool(&ir_mod.string_pool);

        // Shape map storage for specialized compilation
        std::vector<std::uint8_t> current_shape_map;

        // Helper: convert IR function to FlatFunction
        // Optionally includes shape_map for L1 specialization.
        struct FlatFnBuilder {
            std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs;
            std::vector<aura::jit::FlatBlock> flat_blocks;
            aura::jit::FlatFunction flat_fn;
            std::string name_storage;
            // Shape map storage (must outlive FlatFunction usage)
            std::vector<std::uint8_t> shape_map_storage;
            std::vector<std::uint8_t> escape_map_storage;

            // Build from IR function, optionally accepting pre-computed escape map.
            FlatFnBuilder(const aura::ir::IRFunction& ir_fn,
                          const std::uint8_t* precomputed_escape = nullptr,
                          std::size_t precomputed_size = 0,
                          const std::uint8_t* precomputed_shape = nullptr,
                          const std::vector<std::uint8_t>* inst_dirty = nullptr) {
                flat_instrs.resize(ir_fn.blocks.size());
                flat_blocks.resize(ir_fn.blocks.size());

                for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                    auto& block = ir_fn.blocks[bi];
                    std::size_t inst_base = 0;
                    for (std::size_t pbi = 0; pbi < bi; ++pbi)
                        inst_base += ir_fn.blocks[pbi].instructions.size();
                    std::size_t inst_off = 0;
                    for (auto& instr : block.instructions) {
                        // Issue #60 Iter 2: stamp shape_id on the FlatInstruction
                        // from the per-function shape_map. ops[0] is the result
                        // slot for ops with a result (most arith/load ops).
                        std::uint32_t shape = 0;
                        if (precomputed_shape && instr.operands[0] < ir_fn.local_count) {
                            shape = precomputed_shape[instr.operands[0]];
                        }
                        std::uint8_t dirty = 0;
                        if (inst_dirty) {
                            const auto gi = inst_base + inst_off;
                            if (gi < inst_dirty->size())
                                dirty = (*inst_dirty)[gi];
                        }
                        flat_instrs[bi].push_back({static_cast<std::uint32_t>(instr.opcode),
                                                   {instr.operands[0], instr.operands[1],
                                                    instr.operands[2], instr.operands[3]},
                                                   shape,
                                                   instr.narrow_evidence,
                                                   instr.type_id,
                                                   instr.linear_ownership_state,
                                                   dirty});
                        ++inst_off;
                    }
                    flat_blocks[bi] = {block.id, flat_instrs[bi].data(),
                                       static_cast<std::uint32_t>(flat_instrs[bi].size())};
                }

                name_storage = ir_fn.name;
                flat_fn.name = name_storage.c_str();
                flat_fn.entry_block = ir_fn.entry_block;
                flat_fn.local_count = ir_fn.local_count;
                flat_fn.arg_count = ir_fn.arg_count;
                flat_fn.blocks = flat_blocks.data();
                flat_fn.num_blocks = static_cast<std::uint32_t>(flat_blocks.size());
                flat_fn.func_id_map = nullptr;
                flat_fn.num_callees = 0;
                flat_fn.const_tags = nullptr;
                flat_fn.shape_map = nullptr;
                flat_fn.escape_map = nullptr;
                flat_fn.region = static_cast<std::uint8_t>(ir_fn.region);

                // Apply pre-computed escape map, or run analysis inline
                if (g_use_arena) {
                    if (precomputed_escape && precomputed_size >= ir_fn.local_count) {
                        // Use pre-computed from EscapeAnalysisWrap pass
                        escape_map_storage.assign(precomputed_escape,
                                                  precomputed_escape + ir_fn.local_count);
                        flat_fn.escape_map = escape_map_storage.data();
                    } else {
                        // Run escape analysis inline
                        escape_map_storage.resize(flat_fn.local_count, 0);
                        aura::jit::run_escape_analysis(flat_instrs, flat_fn.local_count,
                                                       escape_map_storage);
                        flat_fn.escape_map = escape_map_storage.data();
                    }
                }
            }

            // Populate shape_map from profiler data.
            // Fills shape_map_storage and sets flat_fn.shape_map.
            bool set_shape_map(const shape::ShapeProfiler& profiler, const std::string& session,
                               const std::string& fn_name) {
                auto fn_key = shape::make_fn_key(session, fn_name);
                if (!profiler.is_stable(fn_key))
                    return false;

                auto dom = profiler.dominant_shape(fn_key);
                // Map ShapeID to shape_map byte code
                std::uint8_t code = 0; // Dynamic
                if (dom == shape::SHAPE_INT)
                    code = 1;
                else if (dom == shape::SHAPE_FLOAT)
                    code = 2;
                else if (dom == shape::SHAPE_BOOL)
                    code = 3;
                else if (dom == shape::SHAPE_STRING)
                    code = 4;
                else if (dom == shape::SHAPE_VOID)
                    code = 5;
                else if (dom == shape::SHAPE_PAIR)
                    code = 10;
                else if (dom == shape::SHAPE_VECTOR)
                    code = 11;
                else if (dom == shape::SHAPE_HASH)
                    code = 12;
                else if (dom == shape::SHAPE_CLOSURE)
                    code = 13;
                else if (dom == shape::SHAPE_REF)
                    code = 14;
                else
                    return false; // Not a simple leaf shape

                shape_map_storage.resize(flat_fn.local_count, 0);
                // Only annotate argument slots (slots 0..arg_count-1 are args)
                for (std::uint32_t i = 0; i < flat_fn.arg_count && i < flat_fn.local_count; ++i)
                    shape_map_storage[i] = code;

                flat_fn.shape_map = shape_map_storage.data();

                std::fprintf(stderr, "spec: L1 for '%s' — arg shape=%d\n", fn_name.c_str(),
                             (int)code);
                return true;
            }
        };

        // Compile ALL functions (with JIT cache) and register with runtime
        int64_t entry_func_id = -1;
        for (auto& ir_fn : ir_mod.functions) {
            if (ir_fn.id == ir_mod.entry_function_id) {
                entry_func_id = static_cast<int64_t>(ir_fn.id);
            }

            // env_count = number of captured free variables
            std::uint32_t env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());

            // Check JIT cache (shared lock for read, unique for write).
            aura::jit::ScalarFn fn_ptr = nullptr;
            bool is_top_level = (ir_fn.name == "__top__");
            bool need_compile = true;
            if (!is_top_level) {
                {
                    std::shared_lock cache_read(jit_cache_mtx_);
                    auto cache_it = jit_cache_.find(ir_fn.name);
                    if (cache_it != jit_cache_.end()) {
                        // Issue #166 Phase 1: epoch check. If the
                        // entry's last_seen_epoch_ doesn't match the
                        // current mutation_epoch_, the entry is stale
                        // (some mutation happened since this entry was
                        // cached, even if invalidate_function didn't
                        // explicitly target this function name — could
                        // be a transitive invalidation the dep_graph
                        // missed, or a mutation in a different function
                        // that the JIT code implicitly depends on).
                        // Treat as a cache miss and force re-compile.
                        auto current_epoch = mutation_epoch_.load(std::memory_order_relaxed);
                        if (cache_it->second.last_seen_epoch_ != current_epoch) {
                            // Skip the cached entry — fall through to
                            // the compile path below. Don't erase here
                            // (we hold only the shared lock); the
                            // insert path will overwrite with a fresh
                            // entry that has the new epoch.
                        } else {
                            // Hot recompilation: if profiler now has stable shape for this
                            // function but cache entry was compiled without shape_map,
                            // evict and recompile with shape specialization.
                            auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                            if (!cache_it->second.has_shape_map &&
                                shape_profiler_.is_stable(fn_key)) {
                                // Drop shared lock; take unique for erase.
                            } else if (jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                                // Issue #605: shape version bumped by mutate — re-compile.
                                shape::jit_shape_miss_count.fetch_add(1, std::memory_order_relaxed);
                            } else {
                                fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                                need_compile = false;
                            }
                        }
                    }
                }
                if (need_compile && fn_ptr == nullptr && !is_top_level) {
                    // Re-probe under unique lock for the hot-recompile path.
                    std::unique_lock cache_write(jit_cache_mtx_);
                    auto cache_it = jit_cache_.find(ir_fn.name);
                    auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                    if (cache_it != jit_cache_.end() && !cache_it->second.has_shape_map &&
                        shape_profiler_.is_stable(fn_key)) {
                        std::fprintf(stderr, "spec: hot-recompile '%s' (shape now stable)\n",
                                     ir_fn.name.c_str());
                        jit_cache_.erase(cache_it);
                    } else if (cache_it != jit_cache_.end() &&
                               jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                        // Issue #605: drop stale shape-specialized entry.
                        jit_cache_.erase(cache_it);
                    } else if (cache_it != jit_cache_.end()) {
                        fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                        need_compile = false;
                    }
                }
            }
            if (need_compile) {
                auto* precomp_escape = escape_pass.get_map(ir_fn.id);
                auto precomp_size = escape_pass.get_map_size(ir_fn.id);
                const std::vector<std::uint8_t>* inst_dirty = nullptr;
                if (auto cit = ir_cache_v2_.find(ir_fn.name); cit != ir_cache_v2_.end()) {
                    if (ir_fn.id < cit->second.instruction_dirty_per_func_.size())
                        inst_dirty = &cit->second.instruction_dirty_per_func_[ir_fn.id];
                }
                FlatFnBuilder builder(ir_fn, precomp_escape, precomp_size, nullptr, inst_dirty);
                // Set shape_map from profiler for L1 specialization
                bool had_shape = builder.set_shape_map(shape_profiler_, session_id_, ir_fn.name);
                // Issue #60 Iter 2: rebuild flat_instrs with shape_id stamped
                // onto each instruction from the shape_map. We rebuild
                // instead of patching because the builder has already
                // populated flat_instrs in its ctor; we need to redo
                // the stamping after shape_map is known.
                if (had_shape) {
                    auto& shape_vec = builder.shape_map_storage;
                    for (std::size_t bi = 0; bi < builder.flat_instrs.size(); ++bi) {
                        for (auto& fi : builder.flat_instrs[bi]) {
                            if (fi.ops[0] < shape_vec.size()) {
                                fi.shape_id = shape_vec[fi.ops[0]];
                            }
                        }
                    }
                }
                fn_ptr = jit_.compile(builder.flat_fn);
                // Issue #1512: mirror AuraJIT coverage / unhandled masks into
                // CompilerMetrics for Agent query surfaces.
                metrics_.jit_opcode_covered_mask.store(
                    jit_.metrics().opcode_covered_mask.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics_.jit_opcode_unhandled_mask.store(
                    jit_.metrics().opcode_unhandled_mask.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                // Issue #1516: mirror EH + per-function AOT counters.
                metrics_.jit_exception_opcode_lowered.store(
                    jit_.metrics().exception_opcode_lowered.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics_.jit_exception_opcode_mask.store(
                    jit_.metrics().exception_opcode_mask.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics_.jit_exception_opcodes_covered.store(jit_.exception_opcode_coverage_count(),
                                                             std::memory_order_relaxed);
                metrics_.aot_per_function_ir_total.store(
                    jit_.metrics().aot_per_function_ir_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics_.aot_per_function_object_total.store(
                    jit_.metrics().aot_per_function_object_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics_.aot_per_function_miss_total.store(
                    jit_.metrics().aot_per_function_miss_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                metrics_.aot_last_module_object_total.store(
                    jit_.metrics().aot_last_module_object_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                if (!fn_ptr) {
                    // Issue #62 Iter 1: count compile misses
                    metrics_.jit_compile_misses.fetch_add(1, std::memory_order_relaxed);
                    metrics_.opcode_cov_unhandled_hot_total.fetch_add(1, std::memory_order_relaxed);
                    return std::unexpected(aura::diag::Diagnostic{
                        aura::diag::ErrorKind::InternalError,
                        std::string("JIT compilation failed for function '") + ir_fn.name + "'"});
                }
                metrics_.opcode_cov_hits_total.fetch_add(1, std::memory_order_relaxed);
                // Success counter
                metrics_.jit_compilations.fetch_add(1, std::memory_order_relaxed);

                // Issue #744: seed ShapeProfiler observations at JIT compile
                // so mutate/invalidate closed-loop has per-fn profiles.
                if (ir_fn.name != "__top__") {
                    auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                    shape::ShapeID seed = shape::SHAPE_INT;
                    if (had_shape) {
                        const auto dom = shape_profiler_.dominant_shape(fn_key);
                        if (dom != shape::SHAPE_UNKNOWN)
                            seed = dom;
                    }
                    for (std::uint32_t si = 0; si < shape::ShapeProfiler::kStableThreshold; ++si)
                        shape_profiler_.record_shape(fn_key, seed);
                }

                // Cache compiled function (skip __top__ — IR varies per eval)
                if (ir_fn.name != "__top__") {
                    std::unique_lock cache_write(jit_cache_mtx_);
                    auto [it, _ins] = jit_cache_.try_emplace(ir_fn.name);
                    it->second.fn_ptr.store(fn_ptr, std::memory_order_release);
                    // Issue #166: stamp the entry with the current
                    // epoch. On the next access, if the epoch has
                    // changed, the entry is treated as stale.
                    it->second.last_seen_epoch_ = mutation_epoch_.load(std::memory_order_relaxed);
                    it->second.local_count = ir_fn.local_count;
                    it->second.arg_count = ir_fn.arg_count;
                    it->second.env_count = env_count;
                    it->second.has_shape_map = had_shape;
                    if (had_shape) {
                        auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                        it->second.compiled_shape_version_ =
                            shape_profiler_.current_snapshot(fn_key).version;
                    }
                }
            }

            // Register with runtime for closure calls.
            // Issue #660 Option 1: pass ir_fn.name so the runtime can
            // register the function by name (for cross-module closure
            // identity). The name is assigned by cache_define as
            // <user_define_name>#<bundle_position>.
            jit_.register_function(static_cast<int64_t>(ir_fn.id), fn_ptr, ir_fn.local_count,
                                   ir_fn.arg_count, env_count,
                                   ir_fn.name.empty() ? nullptr : ir_fn.name.c_str());
        }

        // Find entry function and execute it
        auto entry_it = std::find_if(
            ir_mod.functions.begin(), ir_mod.functions.end(),
            [&](const aura::ir::IRFunction& f) { return f.id == ir_mod.entry_function_id; });
        if (entry_it == ir_mod.functions.end()) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                          "Entry function not found"});
        }

        auto& entry = *entry_it;
        std::vector<std::int64_t> locals(entry.local_count, 0);
        auto fn_ptr = jit_.get_function_ptr(entry.name.c_str());
        if (!fn_ptr) {
            return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                          "JIT entry function lookup failed"});
        }

        auto raw_result =
            reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)(locals.data(), entry.arg_count);

        // ── Convert JIT result to proper EvalValue type ──
        types::EvalValue ev_result;
        std::uint32_t ret_slot = std::numeric_limits<std::uint32_t>::max();
        for (auto& block : entry.blocks)
            for (auto& instr : block.instructions)
                if (instr.opcode == aura::ir::IROpcode::Return)
                    ret_slot = instr.operands[0];
        // Follow data flow through OpLocal to find the actual producer instruction
        if (ret_slot != std::numeric_limits<std::uint32_t>::max()) {
            uint32_t search_slot = ret_slot;
            bool chasing = true;
            while (chasing) {
                chasing = false;
                for (auto& block : ir_mod.functions) {
                    for (auto& iblock : block.blocks) {
                        for (auto& instr : iblock.instructions) {
                            if (instr.operands[0] == search_slot &&
                                instr.opcode != aura::ir::IROpcode::Return) {
                                // Follow Local passthrough
                                if (instr.opcode == aura::ir::IROpcode::Local) {
                                    search_slot = instr.operands[1];
                                    chasing = true;
                                    goto found_chain;
                                }
                                switch (instr.opcode) {
                                    case aura::ir::IROpcode::ConstBool:
                                    case aura::ir::IROpcode::Eq:
                                    case aura::ir::IROpcode::Lt:
                                    case aura::ir::IROpcode::Gt:
                                    case aura::ir::IROpcode::Le:
                                    case aura::ir::IROpcode::Ge:
                                    case aura::ir::IROpcode::And:
                                    case aura::ir::IROpcode::Or:
                                    case aura::ir::IROpcode::Not:
                                        ev_result = types::make_bool(raw_result == 7);
                                        goto done;
                                    case aura::ir::IROpcode::ConstI64:
                                        ev_result = types::EvalValue(raw_result);
                                        goto done;
                                    case aura::ir::IROpcode::ConstVoid:
                                        ev_result = types::make_void();
                                        goto done;
                                    case aura::ir::IROpcode::MakePair:
                                        if (raw_result < 0)
                                            ev_result = types::make_pair(
                                                static_cast<std::uint64_t>(-raw_result - 1));
                                        else
                                            ev_result = types::make_pair(
                                                static_cast<std::uint64_t>(raw_result >> 2));
                                        goto done;
                                    case aura::ir::IROpcode::NewCell:
                                        ev_result = types::make_cell(
                                            static_cast<std::uint64_t>(raw_result));
                                        goto done;
                                    case aura::ir::IROpcode::MakeClosure:
                                        ev_result = types::make_closure(
                                            static_cast<std::uint64_t>(raw_result));
                                        goto done;
                                    case aura::ir::IROpcode::ConstF64:
                                        ev_result = types::EvalValue(raw_result);
                                        goto done;
                                    case aura::ir::IROpcode::ConstString: {
                                        auto* str_content = aura_jit_string_content(raw_result);
                                        if (str_content) {
                                            auto& sh = evaluator_.primitives().string_heap();
                                            auto new_idx = sh.size();
                                            sh.push_back(str_content);
                                            ev_result = types::make_string(new_idx);
                                        } else {
                                            ev_result = types::EvalValue(raw_result);
                                        }
                                        goto done;
                                    }
                                    case aura::ir::IROpcode::PrimCall:
                                        ev_result = types::EvalValue(raw_result);
                                        goto done;
                                    default:
                                        break;
                                }
                                goto done; // unknown producer, fallback to value decode
                            }
                        }
                    }
                }
                break; // no more matches
            found_chain:;
            }
        }
    done:
        // Fallback: try to decode by value pattern if IR scan didn't determine type
        if (ev_result.val == 0 && raw_result != 0) {
            if (raw_result == 11)
                ev_result = types::make_void();
            else if (raw_result == 3 || raw_result == 7)
                ev_result = types::make_bool(raw_result == 7);
            else if ((raw_result & 1) == 0 && raw_result > -10000000000000000LL)
                ev_result = types::EvalValue(raw_result); // tagged fixnum
            else
                ev_result = types::EvalValue(raw_result);
        }
        // PrimCall void-returning prims (Display/Write/Newline): void result
        if (types::is_int(ev_result) && types::as_int(ev_result) == 0 &&
            ret_slot != std::numeric_limits<std::uint32_t>::max()) {
            for (auto& block : ir_mod.functions) {
                for (auto& iblock : block.blocks) {
                    for (auto& instr : iblock.instructions) {
                        if (instr.opcode == aura::ir::IROpcode::PrimCall &&
                            (instr.operands[0] == ret_slot || instr.operands[3] == ret_slot)) {
                            auto prim_id = static_cast<aura::ir::PrimId>(instr.operands[0]);
                            if (prim_id == aura::ir::PrimId::Display ||
                                prim_id == aura::ir::PrimId::Write ||
                                prim_id == aura::ir::PrimId::Newline) {
                                ev_result = types::make_void();
                                break;
                            }
                        }
                    }
                    if (types::is_void(ev_result))
                        break;
                }
                if (types::is_void(ev_result))
                    break;
            }
        }
        // Record JIT result shape for profiling (triggers hot-recompilation)
        record_eval_result_shape(session_id_, last_ir_mod_, nullptr, ev_result);
        // Issue #411: post-eval auto-incremental typecheck.
        // The guard (constructed in exec_jit) fires the auto-
        // invoke on scope exit, covering ALL return paths.
        // See PostEvalAutoInvokeGuard for the full pattern.
        return EvalResult(ev_result);
#else
        (void)input;
        return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::InternalError,
                                                      "JIT not available — rebuild with LLVM"});
#endif
    }

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

    TypecheckResult typecheck_full(std::string_view input) {
        TypecheckResult r;
        auto alloc = arena_.allocator();
        aura::ast::StringPool pool(alloc);
        aura::ast::FlatAST flat(alloc);
        auto pr = aura::parser::parse_to_flat(input, flat, pool);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            auto diag = parse_error_diag(pr);
            r.output = std::string("parse error: ") + diag.format() + "\n";
            r.has_errors = true;
            r.diagnostics.push_back(diag);
            return r;
        }
        flat.root = pr.root;

        // Use the CompilerService's persistent type_registry_ so that ADT
        // constructors registered during eval (via define-type) are visible
        // to the match exhaustiveness check.
        aura::compiler::TypeChecker tc(type_registry_);
        aura::diag::DiagnosticCollector diag;

        // Issue #79: opt into strict mode. The `typecheck` command's whole
        // purpose is to surface type errors; without strict mode, the
        // gradual-coercion path reports cross-type mismatches as `Note`
        // diagnostics that pass through `has_errors() == false` (Bug A).
        tc.set_strict(true);

        // Issue #168: gate the type cache by the global mutation
        // epoch. Any mutation since the last infer_flat bumps the
        // epoch; the type checker's cache_invalidated_ flag is
        // set on the next call, forcing re-inference of every
        // node. Coarse but provably correct for the stale-cache
        // bug class.
        tc.set_cache_epoch(mutation_epoch_.load(std::memory_order_relaxed));
        // Issue #258: plumb metrics so solve_delta timing
        // accumulates into CompilerMetrics::delta_solve_time_us.
        tc.set_metrics(&metrics_);
        // Issue #385: plumb poly metrics so register_forall
        // + instantiate_forall accumulate into
        // CompilerMetrics::poly_*_total. The setter
        // takes 3 atomic addresses (register +
        // dedup_hits + instantiate) — avoids the
        // cross-module dependency on the full
        // CompilerMetrics struct.
        type_registry_.set_poly_metrics(&metrics_.poly_register_total,
                                        &metrics_.poly_dedup_hits_total,
                                        &metrics_.poly_instantiate_total);

        auto result = tc.infer_flat(flat, pool, pr.root, diag);

        // Issue #258: accumulate incremental typecheck stats
        // into CompilerMetrics (lifetime totals). The per-call
        // TypeChecker is short-lived, so without this
        // accumulation multi-mutation workloads would have
        // nowhere to surface the cache hit/miss ratio.
        // Wire the metrics pointer so ConstraintSystem::solve_delta
        // can also accumulate time (see type_checker_impl.cpp).
        metrics_.typecheck_cache_hits_total.fetch_add(tc.stats().cache_hits,
                                                      std::memory_order_relaxed);
        metrics_.typecheck_cache_misses_total.fetch_add(tc.stats().cache_misses,
                                                        std::memory_order_relaxed);
        metrics_.typecheck_stale_cache_total.fetch_add(tc.stats().stale_cache,
                                                       std::memory_order_relaxed);
        metrics_.typecheck_gen_saved_total.fetch_add(tc.stats().gen_saved,
                                                     std::memory_order_relaxed);
        // Issue #386: narrowing observability. Mirror
        // the per-call stats into the lifetime
        // CompilerMetrics counters.
        metrics_.narrowing_applied_total.fetch_add(tc.stats().narrowing_applied,
                                                   std::memory_order_relaxed);
        metrics_.narrowing_skipped_total.fetch_add(tc.stats().narrowing_skipped,
                                                   std::memory_order_relaxed);
        metrics_.narrowing_reanalyzed_total.fetch_add(tc.stats().narrowing_reanalyzed,
                                                      std::memory_order_relaxed);
        // Issue #338: and/or precision.
        metrics_.and_or_meet_uses_total.fetch_add(tc.stats().and_or_meet_uses,
                                                  std::memory_order_relaxed);
        metrics_.and_or_join_uses_total.fetch_add(tc.stats().and_or_join_uses,
                                                  std::memory_order_relaxed);
        // Issue #434: per-node occurrence dirty recovery.
        metrics_.narrowing_dirty_recovery_total.fetch_add(tc.stats().narrowing_dirty_recovery,
                                                          std::memory_order_relaxed);
        // Issue #390: schema cache observability.
        metrics_.schema_cache_lookups_total.fetch_add(tc.stats().schema_cache_lookups,
                                                      std::memory_order_relaxed);
        metrics_.schema_cache_hits_total.fetch_add(tc.stats().schema_cache_hits,
                                                   std::memory_order_relaxed);
        // Issue #387: Type Dependency Graph observability.
        // Mirror the per-call TypeChecker counters into the
        // lifetime CompilerMetrics atomics.
        metrics_.type_dep_graph_lookups.fetch_add(tc.type_dep_graph_lookups(),
                                                  std::memory_order_relaxed);
        metrics_.type_dep_graph_hits.fetch_add(tc.type_dep_graph_hits(), std::memory_order_relaxed);
        metrics_.type_dep_graph_size.store(
            std::max(metrics_.type_dep_graph_size.load(std::memory_order_relaxed),
                     static_cast<std::uint64_t>(tc.type_dep_graph_size())),
            std::memory_order_relaxed);

        // Issue #116: the typecheck command doesn't proceed to
        // IR lowering (it just reports types + diagnostics), so
        // we don't need to apply the deferred CoercionMap to the
        // FlatAST. We do consume the map (via take_coercions) so
        // it doesn't leak across calls.
        (void)tc.take_coercions();

        std::string& out = r.output;
        out += "type: " + type_registry_.format_type(result) + "\n";

        for (auto& d : diag.diagnostics()) {
            r.diagnostics.push_back(d);
            if (d.kind != aura::diag::ErrorKind::Note)
                r.has_errors = true;
            out += d.format() + "\n";
        }
        if (r.diagnostics.empty())
            out += "no errors\n";

        return r;
    }

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
    std::size_t incremental_infer(const aura::ast::MutationRecord& rec) {
        // Issue #518: (set-code ...) via cs.eval routes through
        // workspace_flat_; cs.set_code() uses current_ast_. Prefer
        // the workspace when it carries mutations so infer_flat_partial
        // re-narrows the same flat the mutate primitive edited.
        aura::ast::FlatAST* flat = current_ast_;
        aura::ast::StringPool* pool = current_pool_;
        if (auto* ws_flat = evaluator_.workspace_flat();
            ws_flat != nullptr && !ws_flat->all_mutations().empty()) {
            flat = ws_flat;
            pool = evaluator_.workspace_pool();
        }
        if (!flat || !pool)
            return 0;
        aura::compiler::TypeChecker tc(type_registry_);
        aura::diag::DiagnosticCollector diag;
        tc.set_strict(true); // match the typecheck() default
        // Issue #168: gate by global mutation epoch (same as
        // the typecheck() path).
        tc.set_cache_epoch(mutation_epoch_.load(std::memory_order_relaxed));
        // Issue #258: plumb metrics for solve_delta timing.
        tc.set_metrics(&metrics_);
        tc.set_bidirectional_mode(bidirectional_mode_);
        // Issue #518: wire Evaluator narrowing counters to the
        // actual re-narrow path in infer_flat_partial.
        tc.set_on_narrowing_refresh([this]() { evaluator_.bump_narrowing_refresh_count(); });
        tc.set_on_selective_recheck([this]() { evaluator_.bump_selective_recheck_count(); });
        tc.set_on_touched_roots_snapshot(
            [this](std::size_t n) { evaluator_.set_touched_roots_size(n); });
        tc.set_on_cross_delta_conflict(
            [this]() { evaluator_.bump_cross_delta_conflicts_caught(); });
        // Issue #411 fu1 follow-up #3: plumb the
        // per-DefUseIndex tracker so infer_flat_partial can
        // route through the O(uses) path when the sym is
        // registered. When the tracker is non-null AND has
        // at least one index registered, the
        // per-DefUseIndex path fires (bumps
        // per_defuse_index_used_total). When null or
        // empty, falls back to the O(n) walk (bumps
        // per_symbol_used_total). The split is
        // observable via (compile:per-symbol-reinfer-stats).
        auto* tracker_ptr = per_defuse_index_tracker_.index_count() > 0
                                ? static_cast<void*>(&per_defuse_index_tracker_)
                                : nullptr;
        auto n = tc.infer_flat_partial(*flat, *pool, rec, diag, tracker_ptr);
        metrics_.typecheck_gen_saved_total.fetch_add(tc.stats().gen_saved,
                                                     std::memory_order_relaxed);
        // Issue #387: Type Dependency Graph observability.
        // Mirror the per-call TypeChecker counters into the
        // lifetime CompilerMetrics atomics. The graph is
        // built by infer_flat_partial's set_type_with_gen
        // path; lookups happen during affected-set
        // computation (the actual O(affected) wiring is the
        // follow-up).
        metrics_.type_dep_graph_lookups.fetch_add(tc.type_dep_graph_lookups(),
                                                  std::memory_order_relaxed);
        metrics_.type_dep_graph_hits.fetch_add(tc.type_dep_graph_hits(), std::memory_order_relaxed);
        metrics_.type_dep_graph_size.store(
            std::max(metrics_.type_dep_graph_size.load(std::memory_order_relaxed),
                     static_cast<std::uint64_t>(tc.type_dep_graph_size())),
            std::memory_order_relaxed);
        // Issue #411 follow-up #1: per-symbol / ancestor
        // path tracking. Mirror the per-call TypeChecker
        // stats into the lifetime CompilerMetrics
        // counters so the
        // (compile:per-symbol-reinfer-stats) Aura primitive
        // sees a single source of truth.
        metrics_.per_symbol_reinfer_used_total.fetch_add(tc.stats().per_symbol_used_total,
                                                         std::memory_order_relaxed);
        metrics_.per_symbol_reinfer_visited_total.fetch_add(tc.stats().per_symbol_visited_total,
                                                            std::memory_order_relaxed);
        metrics_.ancestor_reinfer_used_total.fetch_add(tc.stats().ancestor_used_total,
                                                       std::memory_order_relaxed);
        metrics_.ancestor_reinfer_visited_total.fetch_add(tc.stats().ancestor_visited_total,
                                                          std::memory_order_relaxed);
        // Issue #411 fu1 follow-up #3: per-DefUseIndex
        // path tracking. Mirror the per-call stats into
        // the lifetime CompilerMetrics counters.
        metrics_.per_defuse_index_used_total.fetch_add(tc.stats().per_defuse_index_used_total,
                                                       std::memory_order_relaxed);
        metrics_.per_defuse_index_walk_fallback_total.fetch_add(
            tc.stats().per_defuse_index_walk_fallback_total, std::memory_order_relaxed);
        // Issue #411 fu1 fu4: per-DefUseIndex visited
        // count (the O(uses) signal). Pre-fu4 this was
        // always 0 (the per-DefUseIndex path still did
        // the O(n) walk). Post-fu4 it's the real
        // wall-clock signal.
        metrics_.per_defuse_index_visited_total.fetch_add(tc.stats().per_defuse_index_visited_total,
                                                          std::memory_order_relaxed);
        // Issue #386: narrowing observability. Mirror
        // the per-call stats into the lifetime
        // CompilerMetrics counters.
        metrics_.narrowing_applied_total.fetch_add(tc.stats().narrowing_applied,
                                                   std::memory_order_relaxed);
        metrics_.narrowing_skipped_total.fetch_add(tc.stats().narrowing_skipped,
                                                   std::memory_order_relaxed);
        metrics_.narrowing_reanalyzed_total.fetch_add(tc.stats().narrowing_reanalyzed,
                                                      std::memory_order_relaxed);
        // Issue #338: and/or precision.
        metrics_.and_or_meet_uses_total.fetch_add(tc.stats().and_or_meet_uses,
                                                  std::memory_order_relaxed);
        metrics_.and_or_join_uses_total.fetch_add(tc.stats().and_or_join_uses,
                                                  std::memory_order_relaxed);
        // Issue #434: per-node occurrence dirty recovery.
        metrics_.narrowing_dirty_recovery_total.fetch_add(tc.stats().narrowing_dirty_recovery,
                                                          std::memory_order_relaxed);
        // Issue #390: schema cache observability.
        metrics_.schema_cache_lookups_total.fetch_add(tc.stats().schema_cache_lookups,
                                                      std::memory_order_relaxed);
        metrics_.schema_cache_hits_total.fetch_add(tc.stats().schema_cache_hits,
                                                   std::memory_order_relaxed);
        return n;
    }

    // ---- Multi-module arena support ----------------------------------

    // ---- Multi-module compilation (ArenaGroup) ----------------------

    // Get or create a per-module arena.
    ast::ASTArena& module_arena(const std::string& name,
                                std::size_t initial_size = 8 * 1024 * 1024) {
        return arena_group_.module_arena(name, initial_size);
    }

    // Reset a specific module's arena.
    void reset_module(const std::string& name) { arena_group_.reset_module(name); }

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

    // Cache directory for compiled modules (~/.cache/aura/modules/)
    static std::string module_cache_dir() {
        auto home = std::getenv("HOME");
        if (!home)
            return "/tmp/aura-cache/modules/";
        return std::string(home) + "/.cache/aura/modules/";
    }

    // Cache file path for a module name + source content hash.
    // The hash prevents loading stale cache when source changes.
    static std::string module_cache_path(const std::string& name, const std::string& source = "") {
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
        return module_cache_dir() + sanitized + ".abfc";
    }

    // Ensure cache directory exists
    static void ensure_cache_dir() {
        std::error_code ec;
        std::filesystem::create_directories(module_cache_dir(), ec);
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
    EvalResult compile_module(const std::string& name, const std::string& source) {
        // Save source for future dirty checks / reloads
        module_states_[name].source = source;

        auto& mod_arena = arena_group_.module_arena(name);
        mod_arena.reset();

        auto alloc = mod_arena.allocator();
        auto* pool_ptr = mod_arena.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = mod_arena.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(source, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            return std::unexpected(parse_error_diag(pr));
        }
        flat_ptr->root = pr.root;

        auto& flat = *flat_ptr;
        auto& pool = *pool_ptr;

        // Macro expand
        auto expanded = aura::compiler::macro_expand_all(flat, pool, flat.root);

        // Walk top-level defines. Issue #132: extracted to
        // aura::compiler.find_top_level_defines (was the
        // inline DefFinder struct in compile_module).
        auto finder = aura::compiler::find_top_level_defines(flat, pool, expanded);

        // Try disk cache: load cached IR bundles to skip lowering
        auto cache_path = module_cache_path(name, source);
        auto cached = aura::compiler::cache::open_cache(cache_path);
        bool cache_hit = cached.valid() && cached.has_ir();
        if (cache_hit) {
            auto& all_funcs = cached.ir_functions();
            for (auto& [fname, node_id] : finder) {
                if (ir_cache_.count(fname))
                    continue;
                auto dv = flat.get(node_id);
                if (dv.children.empty())
                    continue;
                if (flat.get(dv.child(0)).tag != aura::ast::NodeTag::Lambda)
                    continue;
                for (auto& func : all_funcs) {
                    if (func.name == fname && func.id != cached.ir_entry()) {
                        ir_cache_[fname] = std::vector<aura::ir::IRFunction>{func};
                        ir_cache_strings_[fname] = cached.ir_strings();
                        function_sources_[fname] = source;
                        module_functions_[name].push_back(fname);
                        break;
                    }
                }
                auto bind_result = cache_define(source, flat, pool, node_id, fname,
                                                /*bind_in_env=*/true, name);
                if (!bind_result)
                    return bind_result;
                user_bindings_.insert(fname);
            }
        }

        // Cache each define (only if not loaded from disk cache)
        if (!cache_hit) {
            for (auto& [fname, node_id] : finder) {
                auto def_node = flat.get(node_id);
                if (def_node.children.empty())
                    continue;
                auto body_id = def_node.child(0);
                if (body_id >= flat.size())
                    continue;

                if (flat.get(body_id).tag == aura::ast::NodeTag::Lambda) {
                    if (ir_cache_.count(fname))
                        continue;
                    auto result = cache_define(source, flat, pool, node_id, fname,
                                               /*bind_in_env=*/true, name);
                    if (!result)
                        return result;
                    user_bindings_.insert(fname);
                    continue;
                }

                // Issue #272 Cycle 3: value defines via IR when lowerable.
                if (!define_body_needs_tree_walker_fallback(flat, pool, body_id, fname) &&
                    bind_value_define_via_ir(flat, pool, node_id, fname)) {
                    continue;
                }
                auto result = evaluator_.eval_flat(flat, pool, node_id, evaluator_.top_env());
                if (!result)
                    return result;
                user_bindings_.insert(fname);
            }
        } // if (!cache_hit) — skip lowering when loaded from disk

        // Mark module as loaded
        loaded_modules_.insert(name);

        // Mark module clean and record dependencies from dep_graph_
        auto& state = module_states_[name];
        state.dirty = false;
        state.deps.clear();
        {
            std::shared_lock dep_read(dep_graph_mtx_);
            for (auto& [fname, _] : finder) {
                auto dit = dep_graph_.find(fname);
                if (dit != dep_graph_.end()) {
                    for (auto& callee : dit->second.calls)
                        state.deps.insert(callee);
                }
            }
        }

        // Write disk cache (only when not loaded from cache).
        // Issue #272 Cycle 4: use pre-bind snapshots (captured in cache_define).
        if (!cache_hit) {
            ensure_cache_dir();
            auto cache_path = module_cache_path(name, source);
            aura::ir::IRModule disk_mod;
            for (auto& [fname, _] : finder) {
                auto it = ir_disk_snapshots_.find(fname);
                if (it == ir_disk_snapshots_.end())
                    continue;
                for (const auto& func : it->second)
                    disk_mod.functions.push_back(func);
            }
            // 生成类型签名数据嵌入 ABF
            std::string sig_embed;
            // 从 export 声明收集已注册的类型签名
            // 直接从模块的 FlatAST 推断类型
            auto sig_embed_path = source;
            if (sig_embed_path.ends_with(".aura"))
                sig_embed_path = sig_embed_path.substr(0, sig_embed_path.size() - 5) + ".aura-type";
            struct stat sig_st;
            if (::stat(sig_embed_path.c_str(), &sig_st) == 0 && S_ISREG(sig_st.st_mode)) {
                std::ifstream sf(sig_embed_path);
                if (sf) {
                    sig_embed.assign((std::istreambuf_iterator<char>(sf)), {});
                }
            }
            aura::compiler::cache::write_cache(cache_path, flat, pool, flat.root, 0,
                                               disk_mod.functions.empty() ? nullptr : &disk_mod,
                                               sig_embed.empty() ? nullptr : &sig_embed);
        }

        return EvalResult(types::make_void());
    }

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
        std::size_t mutation_count = 0;                   // snapshot at lower time
        // Issue #166: epoch snapshot at lower time. On every
        // mutation, mutation_epoch_ is incremented atomically.
        // On next access, if the entry's epoch doesn't match
        // the current epoch, it's stale and needs re-lower.
        // The cost is one uint64 compare per cache lookup
        // (~1ns); the benefit is a single global "is anything
        // stale?" check that complements the per-function
        // invalidate_function BFS in dep_graph_.
        std::uint64_t last_seen_epoch_ = 0;
        // Issue #1042: access stamp for LRU eviction under long-running serve.
        std::uint64_t last_used = 0;
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

        // Issue #196: per-block dirty bitmask helpers.

        // Mark every block in every function dirty. Used by
        // mark_define_dirty / mark_all_defines_dirty to
        // signal a full re-lower is needed.
        void mark_all_blocks_dirty() {
            for (auto& func_blocks : block_dirty_per_func_) {
                for (auto& b : func_blocks)
                    b = 1;
            }
            for (auto& soa_fn : soa_mod.functions)
                soa_fn.mark_all_blocks_dirty();
        }

        // Mark a single block dirty. Resizes the bitmask
        // for that function if needed (e.g. caller doesn't
        // know the block count up front).
        void mark_block_dirty(std::size_t func_idx, std::uint32_t block_idx) {
            if (func_idx >= block_dirty_per_func_.size()) {
                block_dirty_per_func_.resize(func_idx + 1);
            }
            auto& fb = block_dirty_per_func_[func_idx];
            if (block_idx >= fb.size()) {
                fb.resize(block_idx + 1, 1);
                cascade_block_to_instructions(func_idx, block_idx);
                if (func_idx < soa_mod.functions.size())
                    soa_mod.functions[func_idx].mark_block_dirty(block_idx);
                return;
            }
            fb[block_idx] = 1;
            cascade_block_to_instructions(func_idx, block_idx);
            if (func_idx < soa_mod.functions.size())
                soa_mod.functions[func_idx].mark_block_dirty(block_idx);
        }

        // Issue #684: mark every instruction in a block dirty (SoA cascade).
        void cascade_block_to_instructions(std::size_t func_idx, std::uint32_t block_idx) {
            if (func_idx >= irs.size())
                return;
            if (func_idx >= instruction_dirty_per_func_.size())
                instruction_dirty_per_func_.resize(func_idx + 1);
            const auto& func = irs[func_idx];
            std::size_t total = 0;
            for (const auto& b : func.blocks)
                total += b.instructions.size();
            auto& idf = instruction_dirty_per_func_[func_idx];
            if (idf.size() < total)
                idf.resize(total, 0);
            if (block_idx >= func.blocks.size())
                return;
            std::size_t base = 0;
            for (std::uint32_t bi = 0; bi < block_idx; ++bi)
                base += func.blocks[bi].instructions.size();
            const auto count = func.blocks[block_idx].instructions.size();
            for (std::size_t i = base; i < base + count; ++i)
                idf[i] = 1;
            for (const auto& instr : func.blocks[block_idx].instructions) {
                if (instr.narrow_evidence != 0 || instr.type_id != 0 ||
                    instr.linear_ownership_state != 0)
                    jit_typed_mutation::record_type_propagation_stamp();
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
            std::size_t base = 0;
            for (std::uint32_t bi = 0; bi < block_idx; ++bi)
                base += func.blocks[bi].instructions.size();
            const auto idx = base + inst_in_block;
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
            std::size_t total = 0;
            for (const auto& b : func.blocks)
                total += b.instructions.size();
            auto& idf = instruction_dirty_per_func_[func_idx];
            if (idf.size() < total)
                idf.resize(total, 0);
            if (block_idx >= func.blocks.size())
                return;
            std::size_t base = 0;
            for (std::uint32_t bi = 0; bi < block_idx; ++bi)
                base += func.blocks[bi].instructions.size();
            const auto idx = base + inst_in_block;
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
            std::size_t base = 0;
            for (std::uint32_t bi = 0; bi < block_idx; ++bi)
                base += func.blocks[bi].instructions.size();
            const auto idx = base + inst_in_block;
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

        // Issue #293: count of functions whose dirty block
        // count falls in the "incremental re-lower" range
        // (1..7 dirty blocks, per estimate_relower_blocks).
        // Functions with 8+ dirty blocks are considered
        // "full re-lower" candidates and excluded from this
        // count.
        std::size_t incremental_candidates_count() const {
            std::size_t n = 0;
            for (const auto& fb : block_dirty_per_func_) {
                std::size_t dirty = 0;
                for (auto b : fb)
                    if (b)
                        ++dirty;
                // estimate_relower_blocks(dirty): 0 if 0 dirty,
                // (-1) if >= 8, else dirty (1..7).
                if (dirty > 0 && dirty < 8)
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
    std::unordered_map<std::string, IRCacheEntry> ir_cache_v2_;
    // Issue #959 / #1042: hard cap for long-running --serve.
    // Evicts dirty-first, then oldest last_used (LRU).
    static constexpr std::size_t kIRCacheV2MaxEntries = 2048;
    std::uint64_t ir_cache_v2_access_clock_ = 0;

    void maybe_evict_ir_cache_v2() {
        if (ir_cache_v2_.size() <= kIRCacheV2MaxEntries)
            return;
        // Prefer dropping dirty entries (will re-lower on demand).
        for (auto it = ir_cache_v2_.begin();
             it != ir_cache_v2_.end() && ir_cache_v2_.size() > kIRCacheV2MaxEntries;) {
            if (it->second.dirty) {
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
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return 2;
        // Issue #1042: touch LRU stamp on every lookup.
        it->second.last_used = ++ir_cache_v2_access_clock_;
        // Issue #126: delegate the re-lower decision to the pure
        // helper should_relower(). The function takes the relevant
        // fields as values (no this->) so the same logic can be
        // unit-tested in isolation.
        // Issue #1555: also treat body-only bitmasks (dirty_block_count>0)
        // as needs-relower even if entry.dirty was cleared inconsistently.
        if (should_relower(source_hash, it->second.source_hash, it->second.dirty,
                           it->second.mutation_count, it->second.mutation_count) ||
            it->second.dirty_block_count() > 0) {
            // Issue #487: bump the should_relower counter
            // for observability (the re-lower path fired
            // on dirty). The ratio should_relower /
            // affected_subtree measures the dirty-
            // trigger rate.
            metrics_.should_relower_total.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
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
                         std::vector<std::string> strings) {
        auto hash = fnv1a_64(source);
        auto& entry = ir_cache_v2_[name];
        entry.source = std::move(source);
        entry.source_hash = hash;
        entry.irs = std::move(irs);
        entry.bridges = std::move(bridges);
        entry.strings = std::move(strings);
        entry.dirty = false;
        entry.last_used = ++ir_cache_v2_access_clock_; // #1042
        entry.mutation_count = 0;
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
        }
    }

    // Issue #680: compute ir_cache_pure impact_scope for a Define
    // mutation root and bump Evaluator observability counters.
    void run_define_impact_scope(aura::ast::NodeId root) {
        auto* flat = evaluator_.workspace_flat();
        if (!flat || root == aura::ast::NULL_NODE || root >= flat->size())
            return;
        std::unordered_map<aura::ast::NodeId, std::pair<std::size_t, std::uint32_t>> source_to_ir;
        std::unordered_map<std::string, std::size_t> ir_cache_index;
        auto scope = compute_impact_scope(*flat, root, source_to_ir, ir_cache_index);
        const std::uint64_t blocks =
            scope.affected_blocks.empty() ? 1u : scope.affected_blocks.size();
        evaluator_.bump_impact_scope_calls(blocks);
        evaluator_.bump_edsl_mutate_invalidate_precision();
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
            std::string define_name;
            if (flat->get(root).tag == aura::ast::NodeTag::Define) {
                if (auto* pool = evaluator_.workspace_pool())
                    define_name = std::string(pool->resolve(flat->get(root).sym_id));
            }
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

    // atomic_bump_epochs_and_stamp_bridge: defined later (Issue #1522/#1476/#1524
    // authoritative helper with AOT table epoch + JIT batch_deopt).

    void mark_define_dirty(const std::string& name) {
        // Issue #1476 + #1523: unify dirty mark + dual-epoch; acquire
        // mutate FIRST when safe (skip if would invert lock order).
        using aura::compiler::lock_order::Level;
        using aura::compiler::lock_order::OrderedUniqueLock;
        OrderedUniqueLock<std::shared_mutex> mutate_guard;
        if (!lock_order::is_held(Level::Mutate)) {
            if (lock_order::is_held(Level::Workspace) || lock_order::is_held(Level::EnvFrames) ||
                lock_order::is_held(Level::DepGraph)) {
                metrics_.lock_inversion_detected_total.fetch_add(1, std::memory_order_relaxed);
                lock_order::g_lock_inversion_detected_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                mutate_guard = OrderedUniqueLock<std::shared_mutex>(mutate_mtx_, Level::Mutate);
                sync_lock_order_metrics_();
            }
        }

        // Issue #1261 / #1476: bump both epochs via unified helper.
        atomic_bump_epochs_and_stamp_bridge(name);

        auto it = ir_cache_v2_.find(name);
        if (it != ir_cache_v2_.end()) {
            auto& primary = it->second;
            primary.dirty = true;
            // Issue #1495 / #1505 / #1506: prefer body-only dirty so partial
            // re-lower wins on set-body.
            // Shapes:
            //   - synthetic / dual: irs[0]=__top__, irs[1]=body
            //   - real lower bundle: only non-entry funcs → body at irs[0]
            // Nested (irs[2..N] or >1 with free-ref self): free-var scan.
            const bool nested_primary = primary.irs.size() > 2;
            // body_idx: dual-shape uses 1; single-function bundle uses 0.
            const std::size_t body_idx = primary.irs.size() >= 2 ? 1 : 0;
            if (!primary.irs.empty() && primary.block_dirty_per_func_.size() > body_idx &&
                !primary.block_dirty_per_func_[body_idx].empty()) {
                if (primary.block_dirty_per_func_.size() < primary.irs.size())
                    primary.block_dirty_per_func_.resize(primary.irs.size());
                // Keep __top__ clean when dual-shape.
                if (body_idx == 1) {
                    for (auto& b : primary.block_dirty_per_func_[0])
                        b = 0;
                }
                for (std::uint32_t bi = 0; bi < static_cast<std::uint32_t>(
                                                    primary.block_dirty_per_func_[body_idx].size());
                     ++bi) {
                    primary.mark_block_dirty(/*func_idx=*/body_idx, bi);
                }
                // Issue #1505: free-var scan of nested lambdas for self.
                if (nested_primary) {
                    for (std::size_t fi = 2; fi < primary.irs.size(); ++fi) {
                        if (fi >= primary.block_dirty_per_func_.size())
                            break;
                        const auto& nfn = primary.irs[fi];
                        bool free_refs_self = false;
                        for (const auto& fv : nfn.free_vars) {
                            if (fv == name) {
                                free_refs_self = true;
                                break;
                            }
                        }
                        if (!free_refs_self)
                            continue;
                        auto& fb = primary.block_dirty_per_func_[fi];
                        if (fb.empty() && !nfn.blocks.empty())
                            fb.resize(nfn.blocks.size(), std::uint8_t{0});
                        for (auto& b : fb)
                            b = 1;
                    }
                    metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                        1, std::memory_order_relaxed);
                }
                metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
                metrics_.selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
            } else {
                primary.mark_all_blocks_dirty();
                // Issue #946/#950 Phase 1: instruction dirty bitmask.
                primary.mark_all_instruction_dirty();
                metrics_.selfevo_instr_dirty_total.fetch_add(1, std::memory_order_relaxed);
                if (nested_primary) {
                    metrics_.dep_graph_nested_lambda_full_dirty.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
            // Issue #598 / #1494: post-mutate linear runtime enforcement
            // on mutate:rebind / set-body paths (ir_cache_v2 dirty).
            // Scan Moved captures so long-lived closures cannot apply
            // through stale linear EnvFrame state after dirty mark.
            metrics_.linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
            metrics_.selfevo_linear_enforce_total.fetch_add(1, std::memory_order_relaxed);
            (void)evaluator_.scan_live_closures_for_linear_captures(
                /*mark_invalid=*/true, /*only_if_moved=*/true);
        }
        // Cascade: BFS over called_by. Use std::queue (FIFO) for proper BFS
        // ordering — vector-as-stack is technically DFS, which is fine for
        // correctness but std::queue is more idiomatic and self-documenting.
        //
        // Issue #224 cycle 4: dep_graph_-aware cascade. For each
        // dependent that we reach via the BFS, we know via the
        // dep_graph_ that the dependent *calls* the mutated
        // function (the edge `dependent → name` exists in
        // dep_graph_[dependent].calls). The CALL is in the
        // dependent's body Lambda (irs[1] in the entry, by
        // convention — irs[0] is the __top__ entry function).
        // Nested lambdas in the dependent (irs[2..N]) are
        // self-contained; they don't reference the mutated
        // function, so their blocks don't need re-lowering.
        //
        // Cycle-4 win: for a dependent with K nested lambdas,
        // we mark only the body function's blocks dirty (not
        // all functions in the entry). When the bitmask
        // consumer (relower_define_blocks) sees this, the
        // re-lower-define-function path can re-lower just
        // irs[1] and leave the nested lambdas alone.
        //
        // Fallback: if the convention doesn't hold (e.g., the
        // dependent has 0 or 1 IRFunction, or the body is at
        // a different index), we conservatively mark all
        // blocks dirty. This preserves correctness; the cycle-4
        // win is "typical define bodies" (single body Lambda,
        // no nested lambdas → no fallback needed).
        //
        // Issue #1261: when dependent has nested lambdas (irs.size()>2)
        // OR macro-hygiene markers on the workspace define, force full
        // dirty so defuse_version_ + hygiene edges do not under-invalidate.
        std::queue<std::string> bfs;
        std::unordered_set<std::string> visited;
        bfs.push(name);
        visited.insert(name);
        std::size_t depth = 0;
        while (!bfs.empty()) {
            ++depth;
            auto cur = bfs.front();
            bfs.pop();
            std::vector<std::string> called_by_snap;
            {
                // Issue #1523: dep_graph is LAST in canonical order
                // (mutate already held or intentionally skipped).
                lock_order::OrderedSharedLock<std::shared_mutex> dep_read(dep_graph_mtx_,
                                                                          Level::DepGraph);
                auto dit = dep_graph_.find(cur);
                if (dit == dep_graph_.end())
                    continue;
                called_by_snap = dit->second.called_by;
            }
            for (auto& dependent : called_by_snap) {
                if (!visited.insert(dependent).second)
                    continue;
                bfs.push(dependent);
                // Issue #1476: per-dependent atomic bump (closure
                // captures for the dependent need new epoch too —
                // paired with the helper that pairs with #1475's
                // is_bridge_stale / is_env_frame_stale dual check).
                atomic_bump_epochs_and_stamp_bridge(dependent);
                auto cit = ir_cache_v2_.find(dependent);
                if (cit == ir_cache_v2_.end())
                    continue;
                auto& centry = cit->second;
                const bool nested_lambdas = centry.irs.size() > 2;
                // Issue #1514 / #1505: dep_graph_-aware cascade for
                // dependents. Convention: irs[0]=__top__, irs[1]=body.
                // The CALL to `cur` lives in the body → mark body blocks.
                // Nested lambdas (irs[2..N]) are only marked when their
                // free_vars free-reference `cur` (the mutated/cascaded
                // name) — not a full-entry dirty. Falls back to full
                // dirty only when body bitmasks are missing.
                if (centry.irs.size() >= 2 && 1 < centry.block_dirty_per_func_.size()) {
                    centry.dirty = true;
                    if (centry.block_dirty_per_func_.size() < centry.irs.size())
                        centry.block_dirty_per_func_.resize(centry.irs.size());
                    // Body (call site of `cur`).
                    for (auto& b : centry.block_dirty_per_func_[1]) {
                        b = 1;
                    }
                    // Issue #1505: free-var scan of nested lambdas.
                    // Match free_vars against `cur` (immediate cascade
                    // predecessor), not only the BFS root — multi-hop
                    // cascades invalidate the intermediate name.
                    if (nested_lambdas) {
                        for (std::size_t fi = 2; fi < centry.irs.size(); ++fi) {
                            if (fi >= centry.block_dirty_per_func_.size())
                                break;
                            const auto& nfn = centry.irs[fi];
                            bool free_refs_cur = false;
                            for (const auto& fv : nfn.free_vars) {
                                if (fv == cur) {
                                    free_refs_cur = true;
                                    break;
                                }
                            }
                            if (!free_refs_cur)
                                continue;
                            // Free-ref hit: mark all blocks of this nested
                            // lambda dirty (function-level free_vars; no
                            // per-block free-var map yet).
                            auto& fb = centry.block_dirty_per_func_[fi];
                            if (fb.empty() && !nfn.blocks.empty())
                                fb.resize(nfn.blocks.size(), std::uint8_t{0});
                            for (auto& b : fb)
                                b = 1;
                        }
                        metrics_.dep_graph_nested_lambda_targeted_dirty_total.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    metrics_.cascade_body_only_count.fetch_add(1, std::memory_order_relaxed);
                } else if (nested_lambdas) {
                    // Fallback: no body bitmask → full dirty (pre-#1505).
                    centry.dirty = true;
                    centry.mark_all_blocks_dirty();
                    metrics_.cascade_full_count.fetch_add(1, std::memory_order_relaxed);
                    metrics_.dep_graph_nested_lambda_full_dirty.fetch_add(
                        1, std::memory_order_relaxed);
                } else {
                    // Fallback: convention doesn't hold —
                    // conservatively mark all blocks dirty.
                    centry.dirty = true;
                    centry.mark_all_blocks_dirty();
                    metrics_.cascade_full_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        // Issue #1476 / #1496 AC5: track invalidate_cascade_depth_max
        // via CAS + sum depth for avg (depth_total / protocol calls).
        const auto final_depth = static_cast<std::uint64_t>(depth);
        metrics_.invalidate_cascade_depth_total.fetch_add(final_depth, std::memory_order_relaxed);
        auto expected = metrics_.invalidate_cascade_depth_max.load(std::memory_order_relaxed);
        while (
            final_depth > expected &&
            !metrics_.invalidate_cascade_depth_max.compare_exchange_weak(expected, final_depth)) {
            // retry
        }
        metrics_.dep_graph_hygiene_propagate.fetch_add(1, std::memory_order_relaxed);
    }

    // Mark all defines dirty. Called when (set-code ...) re-parses the whole
    // workspace (which can change any define's body).
    // Issue #196: also flips every block in every entry to dirty.
    void mark_all_defines_dirty() {
        // Issue #2026-07-17 (EDSL SIGSEGV audit, surgical fix):
        // Clear the cid→name map only. Reasoning:
        // - ir_define_closure_owner_ (cid→name) holds ClosureIds from
        //   the PREVIOUS workspace. After (set-code ...) replaces
        //   workspace, those cids point at closures backed by the OLD
        //   flat/pool (alive in arena but logically detached).
        //   dispatch_ir_define_closure(cid) finding a stale cid →
        //   use-after-free → SIGSEGV at +24.
        // - ir_define_env_bindings_ (name→binding) KEEPS bindings
        //   (with their interpreter, module, context) tied to the OLD
        //   workspace. When eval-current later re-caches defines,
        //   it re-uses or replaces these bindings via
        //   install_ir_define_env_binding, and adds fresh cid→name
        //   entries for the NEW workspace closures.
        // Earlier attempts (reverted): clearing BOTH maps removed
        // SIGSEGV but broke normal dispatch (new closures not found);
        // adding stale-check via binding->interpreter->flat/pool failed
        // to compile because IRInterpreter doesn't expose flat/pool.
        // Minimal surgical change: only ir_define_closure_owner_.clear().
        ir_define_closure_owner_.clear();
        for (auto& [_, entry] : ir_cache_v2_) {
            entry.dirty = true;
            entry.mark_all_blocks_dirty();
        }
    }

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
            return true;
        }
        entry.mark_block_dirty(func_idx, block_idx);
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
                               aura::ast::NodeId expanded_root) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end()) {
            // No entry → caller needs to do a full first-time lower.
            return false;
        }
        const std::size_t dirty_blocks = it->second.dirty_block_count();
        // Issue #603: bump the block_dirty_hits counter whenever the
        // bitmask reports ≥1 dirty block in this entry. Pair with
        // relower_blocks_saved (the clean-block counter above) so
        // an AI agent can compute dirty_block_ratio = hits / (hits +
        // saved). Atomic relaxed; advisory telemetry only.
        if (dirty_blocks > 0)
            metrics_.ir_soa_block_dirty_hits_total.fetch_add(dirty_blocks,
                                                             std::memory_order_relaxed);
        if (dirty_blocks == 0) {
            // Bitmask says nothing changed → reuse cached IR.
            // Bump the skip counter; do NOT call lowering.
            // This is the cycle-2 win: avoid the full
            // lowering pass when the bitmask is clean.
            metrics_.relower_skipped_entirely_count.fetch_add(1, std::memory_order_relaxed);
            metrics_.irsoa_cache_miss_reduction.fetch_add(1, std::memory_order_relaxed);
            // Issue #603: every block in every function is clean —
            // that's the maximal relower_blocks_saved win per call.
            // Sum all block counts so the observability primitive
            // exposes the per-call win (rather than only the call
            // count).
            std::size_t total_blocks_saved = 0;
            for (const auto& fb : it->second.block_dirty_per_func_)
                total_blocks_saved += fb.size();
            if (total_blocks_saved > 0) {
                metrics_.ir_soa_relower_blocks_saved_total.fetch_add(total_blocks_saved,
                                                                     std::memory_order_relaxed);
                evaluator_.bump_incremental_closure_min_scope_win(total_blocks_saved);
            }
            return true;
        }
        // Issue #224 cycle 3: detect single-function-dirty
        // and dispatch to per-function re-lower. The
        // contract: when the bitmask reports dirty blocks
        // in EXACTLY ONE function in the entry, we can
        // re-lower just that one function (the body
        // Lambda, typically function idx 1) and replace
        // it in the cache without re-iterating the rest
        // of the bundle. The cycle-3 win: avoid the full
        // bundle re-iteration when only one function is
        // dirty.
        //
        // Caveats (cycle-3 scope):
        //   - We require that expanded_root points to a
        //     Lambda (the function body). For nested
        //     lambdas (multi-function bundles), the
        //     dispatch falls back to full re-lower
        //     because the cached entry doesn't store
        //     per-function source node ids.
        //   - The MakeClosure operands in the new
        //     function reference the original func_id
        //     (we preserve it on replace), so callers
        //     that reference the old function keep
        //     working.
        // Issue #1506: allow single-function bundles (irs.size()==1 —
        // the common post-store_define_v2 shape that drops __top__).
        // Previously required irs.size()>=2 which forced full re-lower
        // for every set-body on a real-lowered define.
        if (!it->second.irs.empty()) {
            std::size_t dirty_func_idx = static_cast<std::size_t>(-1);
            std::size_t dirty_func_count = 0;
            for (std::size_t fi = 0; fi < it->second.block_dirty_per_func_.size(); ++fi) {
                bool any = false;
                for (auto b : it->second.block_dirty_per_func_[fi]) {
                    if (b) {
                        any = true;
                        break;
                    }
                }
                if (any) {
                    ++dirty_func_count;
                    dirty_func_idx = fi;
                }
            }
            // Issue #1514 / #1506: any single dirty function can take
            // per-function re-lower when we have a source node.
            // Body may be at idx 0 (real lower: non-entry-only bundle)
            // or idx >= 1 (synthetic __top__ + body / nested).
            if (dirty_func_count == 1 && dirty_func_idx != static_cast<std::size_t>(-1) &&
                expanded_root != aura::ast::NULL_NODE) {
                // Snapshot dirty block ids BEFORE re-lower clears them.
                std::vector<std::uint32_t> dirty_ids;
                if (dirty_func_idx < it->second.block_dirty_per_func_.size()) {
                    const auto& fb = it->second.block_dirty_per_func_[dirty_func_idx];
                    dirty_ids.reserve(fb.size());
                    for (std::size_t bi = 0; bi < fb.size(); ++bi) {
                        if (fb[bi])
                            dirty_ids.push_back(static_cast<std::uint32_t>(bi));
                    }
                }
                // Count clean functions we skip (nested lambda win).
                const std::size_t clean_funcs = it->second.irs.size() > dirty_func_count
                                                    ? it->second.irs.size() - dirty_func_count
                                                    : 0;
                if (relower_define_function(name, dirty_func_idx, flat, pool, expanded_root)) {
                    if (clean_funcs > 0) {
                        metrics_.relower_partial_funcs_saved_total.fetch_add(
                            clean_funcs, std::memory_order_relaxed);
                    }
                    // Issue #1495: stamp source after partial so
                    // lookup_define_v2 hits (hash match + clean dirty).
                    if (!source.empty()) {
                        it->second.source = std::string(source);
                        it->second.source_hash = fnv1a_64(it->second.source);
                    }
                    it->second.dirty = false;
                    // Issue #1514: sync JIT — evict native code for this
                    // define so next exec recompiles only the dirty fn.
                    (void)jit_.partial_recompile(name.c_str(), dirty_ids.data(), dirty_ids.size());
                    metrics_.jit_partial_recompile_requests_total.fetch_add(
                        1, std::memory_order_relaxed);
                    return true;
                }
                // If per-function failed, fall through to
                // full re-lower below.
            }
        }
        // Bitmask says at least one block is dirty (or
        // per-function dispatch didn't apply) → do a
        // full re-lower. The future per-block re-lower
        // (cycle 4+) will route only the dirty blocks
        // through lowering; today we still re-lower the
        // whole function bundle.
        metrics_.relower_full_called_count.fetch_add(1, std::memory_order_relaxed);
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        std::vector<std::string> cache_hits;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), cache_bridge_ptr,
            cache_strings_ptr, &name, &type_registry_, value_cells_for_lowering());
        // Run per-function passes on the new bundle.
        // Issue #1574: feed IRCacheEntry dirty bitmasks into
        // run_incremental_dirty_pipeline so ConstantFolding /
        // ComputeKind / TypePropagation / Shape / EscapeAnalysis only
        // touch dirty blocks (or skip entirely when mask is clean).
        {
            aura::compiler::ComputeKindWrap ck_pass;
            aura::compiler::ConstantFoldingWrap cf_pass;
            aura::compiler::TypePropagationPass tp_pass;
            aura::compiler::ShapeWrap shape_pass;
            EscapeAnalysisWrap escape_pass;
            const auto& entry = it->second;
            aura::compiler::DefineDirtyMaskView define_mask;
            if (!entry.block_dirty_per_func_.empty()) {
                define_mask.block_dirty_per_func = &entry.block_dirty_per_func_;
                define_mask.instruction_dirty_per_func = &entry.instruction_dirty_per_func_;
            }
            const aura::compiler::DefineDirtyMaskView* mask_ptr =
                define_mask.block_dirty_per_func ? &define_mask : nullptr;

            (void)aura::compiler::run_incremental_dirty_pipeline(ir_mod, ck_pass, mask_ptr);
            (void)aura::compiler::run_incremental_dirty_pipeline(ir_mod, cf_pass, mask_ptr);
            (void)aura::compiler::run_incremental_dirty_pipeline(ir_mod, tp_pass, mask_ptr);
            (void)aura::compiler::run_incremental_dirty_pipeline(ir_mod, shape_pass, mask_ptr);
            (void)aura::compiler::run_incremental_dirty_pipeline(ir_mod, escape_pass, mask_ptr);

            std::size_t clean_blocks_skipped = 0;
            for (auto& func : ir_mod.functions) {
                if (func.id == ir_mod.entry_function_id)
                    continue;
                // Issue #538: DCE after post-mutate re-lower.
                run_coercion_elim_on_function(func);
            }
            if (mask_ptr) {
                clean_blocks_skipped = static_cast<std::size_t>(mask_ptr->total_block_count() -
                                                                mask_ptr->dirty_block_count());
            }
            if (!entry.block_dirty_per_func_.empty()) {
                metrics_.linear_post_mutate_enforcements_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
            }
            if (clean_blocks_skipped > 0) {
                metrics_.irsoa_cache_miss_reduction.fetch_add(clean_blocks_skipped,
                                                              std::memory_order_relaxed);
            }
        }
        // Extract the non-entry functions as the bundle.
        std::vector<aura::ir::IRFunction> bundle;
        std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
        for (auto& func : ir_mod.functions) {
            if (func.id != ir_mod.entry_function_id) {
                bundle.push_back(std::move(func));
                if (func.id < ir_mod.closure_bridge.size())
                    bridge_bundle.push_back(ir_mod.closure_bridge[func.id]);
                else
                    bridge_bundle.emplace_back();
            }
        }
        // Store in v2 cache first (rebuilds the bitmask +
        // clears all dirty bits via store_define_v2's
        // bookkeeping, and takes ownership of the bundle).
        store_define_v2(name, std::string(source), std::move(bundle), std::move(bridge_bundle),
                        ir_mod.string_pool);
        // Mirror to v1 caches (legacy path). Read from v2
        // so we don't keep two separate copies of the IR
        // — v1 is a thin view onto the same data.
        auto vit = ir_cache_v2_.find(name);
        if (vit != ir_cache_v2_.end()) {
            ir_cache_[name] = vit->second.irs;
            ir_cache_bridge_[name] = vit->second.bridges;
            ir_cache_strings_[name] = vit->second.strings;
        }
        // Mirror source bookkeeping.
        function_sources_[name] = std::string(source);
        // Record dependencies for dep_graph_.
        for (auto& called_name : cache_hits) {
            record_dependency(name, called_name);
        }
        return true;
    }

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
    //   - Mirrors the new function into ir_cache_[name] (v1)
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
                                 aura::ast::NodeId lambda_node_id) {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return false;
        auto& entry = it->second;
        if (func_idx >= entry.irs.size())
            return false;
        // Re-lower just this one function.
        auto new_func = aura::compiler::lower_function_at(flat, pool, arena_, lambda_node_id,
                                                          &evaluator_.primitives());
        if (new_func.blocks.empty()) {
            // Lowering returned empty — fall back to full re-lower.
            return false;
        }
        // Capture dirty-block mask before clearing (Issue #611).
        std::span<const std::uint8_t> dirty_blocks;
        std::vector<std::uint8_t> dirty_copy;
        if (func_idx < entry.block_dirty_per_func_.size()) {
            dirty_copy = entry.block_dirty_per_func_[func_idx];
            dirty_blocks = dirty_copy;
        }
        // Run per-function passes (mirrors cache_define).
        // Issue #1574: define-level dirty mask drives incremental opt.
        {
            aura::compiler::ComputeKindWrap ck_pass;
            aura::compiler::ConstantFoldingWrap cf_pass;
            aura::compiler::TypePropagationPass tp_pass;
            aura::compiler::ShapeWrap shape_pass;
            aura::ir::IRModule one;
            one.functions.push_back(std::move(new_func));
            std::vector<std::vector<std::uint8_t>> single_mask;
            std::vector<std::vector<std::uint8_t>> inst_mask;
            aura::compiler::DefineDirtyMaskView define_mask;
            if (!dirty_copy.empty()) {
                single_mask.push_back(dirty_copy);
                define_mask.block_dirty_per_func = &single_mask;
                if (func_idx < entry.instruction_dirty_per_func_.size()) {
                    inst_mask.push_back(entry.instruction_dirty_per_func_[func_idx]);
                    define_mask.instruction_dirty_per_func = &inst_mask;
                }
            }
            const aura::compiler::DefineDirtyMaskView* mask_ptr =
                define_mask.block_dirty_per_func ? &define_mask : nullptr;
            (void)aura::compiler::run_incremental_dirty_pipeline(one, ck_pass, mask_ptr);
            (void)aura::compiler::run_incremental_dirty_pipeline(one, cf_pass, mask_ptr);
            (void)aura::compiler::run_incremental_dirty_pipeline(one, tp_pass, mask_ptr);
            (void)aura::compiler::run_incremental_dirty_pipeline(one, shape_pass, mask_ptr);
            new_func = std::move(one.functions[0]);
            // Issue #538 / #611: DCE after per-function post-mutate
            // re-lower; scoped to dirty blocks when mask matches.
            run_coercion_elim_on_function(new_func, dirty_blocks);
        }
        // Bump the per-function re-lower counter.
        metrics_.relower_per_function_called_count.fetch_add(1, std::memory_order_relaxed);
        evaluator_.bump_partial_relower_count();
        // Replace the function in the v2 cache. Preserve the
        // original func_id so MakeClosure operands in callers
        // (which reference the old func_id) keep working.
        const auto old_func_id = entry.irs[func_idx].id;
        new_func.id = old_func_id;
        // Issue #1474: per-block selective copy. When the
        // dirty bitmask has the same block count as the new
        // (and old) function, only copy the dirty blocks from
        // new_func into entry.irs[func_idx] — clean blocks
        // keep their old IR. This is the per-block win: a
        // typical mutate:set-body of a single line only marks
        // the body block dirty, so we only re-place that one
        // block in the cache (and only the body block's
        // instructions get touched). When the block counts
        // don't match (control-flow re-shape), fall back to
        // the previous whole-function replace.
        std::size_t blocks_replaced = 0;
        if (func_idx < entry.block_dirty_per_func_.size()) {
            auto& dirty_mask = entry.block_dirty_per_func_[func_idx];
            if (dirty_mask.size() == new_func.blocks.size() &&
                dirty_mask.size() == entry.irs[func_idx].blocks.size()) {
                for (std::size_t bi = 0; bi < new_func.blocks.size(); ++bi) {
                    if (dirty_mask[bi]) {
                        entry.irs[func_idx].blocks[bi] = std::move(new_func.blocks[bi]);
                        dirty_mask[bi] = 0;
                        ++blocks_replaced;
                    }
                }
            } else {
                // Shape mismatch — fall back to full replace.
                blocks_replaced = new_func.blocks.size();
                entry.irs[func_idx] = std::move(new_func);
                for (auto& b : dirty_mask)
                    b = 0;
            }
        } else {
            // No bitmask — fall back to full replace.
            blocks_replaced = new_func.blocks.size();
            entry.irs[func_idx] = std::move(new_func);
        }
        // Issue #1474: bump per-block replaced counter.
        metrics_.incremental_relower_blocks_total.fetch_add(blocks_replaced,
                                                            std::memory_order_relaxed);
        // Issue #1514: also clear instruction-level dirty bits for
        // this function so partial re-lower state stays coherent.
        if (func_idx < entry.instruction_dirty_per_func_.size()) {
            for (auto& b : entry.instruction_dirty_per_func_[func_idx])
                b = 0;
        }
        // Also clear the entry.dirty flag if this was the only
        // dirty function.
        if (entry.dirty_block_count() == 0) {
            entry.dirty = false;
        }
        // Mirror to v1 cache. The v1 cache stores the function
        // in ir_cache_[name]; we need to find and replace.
        auto v1_it = ir_cache_.find(name);
        if (v1_it != ir_cache_.end() && func_idx > 0 && (func_idx - 1) < v1_it->second.size()) {
            v1_it->second[func_idx - 1] = entry.irs[func_idx];
        }
        return true;
    }

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
    void populate_dep_graph_from_workspace() {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return;
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
            // Build canonical source + hash for the v2 entry (lightweight —
            // just unparse, no cache_define, no eval_flat side effects).
            auto child_id = v.child(0);
            std::string body_src = unparse_node(*ws_flat, *ws_pool, child_id, 0);
            std::string canonical = "(define " + name + " " + body_src + ")";
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
                        bool sibling = false;
                        for (aura::ast::NodeId sid = 0; sid < ws_flat->size(); ++sid) {
                            auto sv = ws_flat->get(sid);
                            if (sv.tag == aura::ast::NodeTag::Define &&
                                sv.sym_id != aura::ast::INVALID_SYM) {
                                auto sname = std::string(ws_pool->resolve(sv.sym_id));
                                if (sname == vname) {
                                    sibling = true;
                                    break;
                                }
                            }
                        }
                        if (sibling) {
                            record_dependency(name, vname);
                        }
                    }
                }
                for (auto c : nv.children)
                    stack.push_back(c);
            }
            // Create/update the v2 entry (lightweight — no cache_define).
            // Only create if not already present OR if the hash changed.
            // (We don't overwrite an existing entry's depends_on, which was
            // computed last time and is still valid if the source hash matches.)
            auto& entry = ir_cache_v2_[name];
            if (entry.source_hash != hash) {
                entry.source = canonical;
                entry.source_hash = hash;
                entry.irs.clear();
                entry.bridges.clear();
                entry.strings.clear();
                entry.dirty = false; // freshly parsed, not dirty
            }
        }
    }

    // Heavy (opt-in) populate of the v2 IR cache. Calls cache_define for
    // each top-level define, which has side effects (eval_flat on top_env).
    // NOT called by default. Future work: re-enable when the side-effect
    // issue is resolved.
    void populate_ir_cache_v2_from_workspace() {
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
            std::string body_src = unparse_node(*ws_flat, *ws_pool, child_id, 0);
            std::string canonical = "(define " + name + " " + body_src + ")";
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
            auto* tmp_pool = arena_.create<aura::ast::StringPool>(alloc);
            auto* tmp_flat = arena_.create<aura::ast::FlatAST>(alloc);
            auto pr = aura::parser::parse_to_flat(canonical, *tmp_flat, *tmp_pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE)
                continue;
            tmp_flat->root = pr.root;
            // Pass bind_in_env=false: don't pollute the workspace's env
            // by calling eval_flat. The define is bound later by
            // eval-current which uses its own env.
            (void)cache_define(canonical, *tmp_flat, *tmp_pool, pr.root, name,
                               /*bind_in_env=*/false);
            // Issue #63723: ALSO run the IR-defined value through
            // bind_value_define_via_ir so the cell-id of the cached
            // value is recorded in ir_value_cell_bindings_. Without
            // this, the lowering's Variable case (state.value_cells)
            // sees nothing for the define, falls through to the
            // function-cache path, and emits MakeClosure — which
            // gives the user a closure (cell 0 when used in
            // arithmetic) instead of the actual value 1. This
            // manifested as `(set-code "(define a 1)") (eval-current)
            // (+ a 2)` returning 2 (a silently 0) instead of 3.
            //
            // The cost is one extra lower + IRInterpreter run per
            // define per set-code. For the long-running-agent
            // fleet, this is amortized over many subsequent
            // eval-ir calls that would otherwise fail. For the
            // benchmark suite, this is negligible.
            (void)bind_value_define_via_ir(*tmp_flat, *tmp_pool, pr.root, name);
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
                                std::move(strings_bundle));
                auto vit = ir_cache_v2_.find(name);
                if (vit != ir_cache_v2_.end()) {
                    ir_cache_[name] = vit->second.irs;
                    ir_cache_bridge_[name] = vit->second.bridges;
                    ir_cache_strings_[name] = vit->second.strings;
                }
            } else {
                auto& entry = ir_cache_v2_[name];
                entry.source = canonical;
                entry.source_hash = hash;
                entry.dirty = false;
            }
        }
    }

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
    std::size_t relower_dirty_defines_from_workspace() {
        auto* ws_flat = evaluator_.workspace_flat();
        auto* ws_pool = evaluator_.workspace_pool();
        if (!ws_flat || !ws_pool)
            return 0;
        std::size_t ok = 0;
        // Snapshot names first — relower may erase/replace entries.
        std::vector<std::string> dirty_names;
        dirty_names.reserve(ir_cache_v2_.size());
        for (const auto& [n, e] : ir_cache_v2_) {
            if (e.dirty || e.dirty_block_count() > 0)
                dirty_names.push_back(n);
        }
        for (const auto& name : dirty_names) {
            auto it = ir_cache_v2_.find(name);
            if (it == ir_cache_v2_.end())
                continue;
            if (!it->second.dirty && it->second.dirty_block_count() == 0)
                continue;
            // Locate the Define node + Lambda body in the workspace.
            aura::ast::NodeId def_id = aura::ast::NULL_NODE;
            aura::ast::NodeId lambda_id = aura::ast::NULL_NODE;
            for (aura::ast::NodeId id = 0; id < ws_flat->size(); ++id) {
                auto v = ws_flat->get(id);
                if (v.tag != aura::ast::NodeTag::Define)
                    continue;
                if (v.sym_id == aura::ast::INVALID_SYM)
                    continue;
                if (std::string(ws_pool->resolve(v.sym_id)) != name)
                    continue;
                def_id = id;
                if (!v.children.empty())
                    lambda_id = v.child(0);
                break;
            }
            if (def_id == aura::ast::NULL_NODE || lambda_id == aura::ast::NULL_NODE)
                continue;
            // Prefer Lambda node for per-function re-lower path.
            const auto expanded = (lambda_id < ws_flat->size() &&
                                   ws_flat->get(lambda_id).tag == aura::ast::NodeTag::Lambda)
                                      ? lambda_id
                                      : def_id;
            std::string body_src = unparse_node(*ws_flat, *ws_pool, lambda_id, 0);
            std::string canonical = "(define " + name + " " + body_src + ")";
            // Gate: should_partial_relower OR single-function dirty
            // (body-only from #1495 mark). Large dirty surfaces still
            // go through relower_define_blocks which may full-fallback.
            const std::size_t dirty_n = it->second.dirty_block_count();
            if (dirty_n == 0) {
                it->second.dirty = false;
                ++ok;
                continue;
            }
            if (relower_define_blocks(name, canonical, *ws_flat, *ws_pool, expanded)) {
                ++ok;
            }
        }
        return ok;
    }

    // Clear the whole EDSL IR cache. Called by --reset-arena / gc.
    void clear_define_cache_v2() { ir_cache_v2_.clear(); }

    // Get cached IR for a define (returns nullptr if not cached).
    // Used by (eval-current) to assemble the IRModule.
    const IRCacheEntry* get_define_v2(const std::string& name) const {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return nullptr;
        return &it->second;
    }

    // Get all cached defines' names (for the (eval-current) full-lower fallback).
    std::vector<std::string> list_cached_defines_v2() const {
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
    void unload_module(const std::string& name) {
        arena_group_.reset_module(name);

        // Collect all cached defines belonging to this module and remove them.
        // Since function_sources_ stores per-define source, we rebuild:
        // find all cached functions whose source matches the module source.
        std::vector<std::string> to_remove;
        for (auto& [fname, src] : function_sources_) {
            // Simple heuristic: check if this function was cached from this module.
            // We track module_name → function names via module_functions_ map.
            (void)src;
        }

        // Track module function membership via a reverse map
        if (auto it = module_functions_.find(name); it != module_functions_.end()) {
            for (auto& fname : it->second)
                to_remove.push_back(fname);
            module_functions_.erase(it);
        }

        for (auto& fname : to_remove) {
            ir_cache_.erase(fname);
            ir_cache_bridge_.erase(fname);
            ir_cache_strings_.erase(fname);
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                jit_cache_.erase(fname);
            }
            function_sources_.erase(fname);
            // Clean dep_graph (Issue #1376: exclusive lock)
            {
                std::unique_lock dep_write(dep_graph_mtx_);
                auto dit = dep_graph_.find(fname);
                if (dit != dep_graph_.end()) {
                    for (auto& callee : dit->second.calls) {
                        dep_graph_[callee].called_by.erase(
                            std::remove(dep_graph_[callee].called_by.begin(),
                                        dep_graph_[callee].called_by.end(), fname),
                            dep_graph_[callee].called_by.end());
                    }
                    dep_graph_.erase(dit);
                }
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
        try {
            for (auto& entry : std::filesystem::directory_iterator(dir)) {
                auto fn = entry.path().filename().string();
                if (fn.starts_with(sanitized) && fn.ends_with(".abfc")) {
                    aura::compiler::cache::remove_cache(entry.path().string());
                }
            }
        } catch (...) {
        }
        // Also try without hash (legacy format)
        aura::compiler::cache::remove_cache(module_cache_dir() + sanitized + ".abfc");
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
            if (func_id < last_ir_mod_->functions.size()) {
                new_func.id = func_id;
                (*last_ir_mod_).functions[func_id] = std::move(new_func);
            } else {
                last_ir_mod_->functions.push_back(std::move(new_func));
            }
        }
        last_ir_mod_->entry_function_id = new_mod.entry_function_id;

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
        const std::uint64_t pipeline_epoch = mutation_epoch_.load(std::memory_order_relaxed);
        ts.set_pipeline_epoch(pipeline_epoch);
        tprop.set_pipeline_epoch(pipeline_epoch);
        ar.set_pipeline_epoch(pipeline_epoch);
        cf.set_pipeline_epoch(pipeline_epoch);
        dce.set_pipeline_epoch(pipeline_epoch);
        // Issue #163: run_pipeline (Pass concept fold) replaces
        // the individual *.run() calls. Issue #1418: include DCE.
        aura::compiler::run_pipeline(*last_ir_mod_, ts, tprop, ck, ar, cf, dce);
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
        aura::compiler::IRInterpreter ir_interp(*last_ir_mod_, ctx);
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
    void cache_module(const std::string& content, const std::string& path) {
        // don't survive re-evaluation via cache_define.


        // Arena-allocate flat/pool so pointers survive (bridge data references them)
        auto alloc = arena_.allocator();
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
        auto pr = aura::parser::parse_to_flat(content, *flat_ptr, *pool_ptr);
        if (!pr.success || pr.root == aura::ast::NULL_NODE)
            return;
        flat_ptr->root = pr.root;

        auto& flat = *flat_ptr;
        auto& pool = *pool_ptr;

        // Macro expand
        auto expanded = aura::compiler::macro_expand_all(flat, pool, flat.root);

        // Walk top-level expressions to find (define ...) forms.
        // Issue #132: extracted to aura::compiler.find_top_level_defines
        // (was the inline DefineFinder struct in cache_define).
        auto finder = aura::compiler::find_top_level_defines(flat, pool, expanded);

        // Cache each define (IR only — tree-walker already evaluated the module)
        for (auto& [name, node_id] : finder) {
            if (ir_cache_.count(name))
                continue; // already cached

            // Skip value defines (e.g., (define pi 3.14)) — only cache function defines
            // A function define's body is a Lambda node
            auto define_node = flat.get(node_id);
            if (define_node.children.empty())
                continue;
            auto body_node = flat.get(define_node.child(0));
            if (body_node.tag != aura::ast::NodeTag::Lambda)
                continue;

            // Check: can this function be lowered to IR?
            // We skip functions where any variable reference in the body
            // isn't resolvable: not a parameter, not a primitive, not in ir_cache_.
            // This covers:
            //   1. Self-recursive calls (function not in ir_cache_ yet)
            //   2. Calls to other non-cached, non-primitive functions
            //   3. Any variable reference that would fall through to ConstI64 0
            bool skip_ir_cache_fn = false;
            {
                // Collect parameter names
                auto params_span = body_node.params;
                std::unordered_set<std::string> param_names;
                for (auto pid : params_span)
                    param_names.insert(std::string(pool.resolve(pid)));

                struct FnCheck {
                    const aura::ast::FlatAST& f;
                    const aura::ast::StringPool& p;
                    const std::unordered_set<std::string>& params;
                    const Evaluator& eval;
                    const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>&
                        ir_cache;
                    bool skip = false;
                    void walk(aura::ast::NodeId id) {
                        if (skip || id >= f.size())
                            return;
                        auto nv = f.get(id);
                        if (nv.tag == aura::ast::NodeTag::Variable) {
                            auto var_name = std::string(p.resolve(nv.sym_id));
                            if (params.count(var_name))
                                return;
                            if (eval.primitives().slot_for_name(var_name) <
                                eval.primitives().slot_count())
                                return;
                            if (ir_cache.count(var_name))
                                return;
                            skip = true;
                        }
                        for (auto c : nv.children)
                            walk(c);
                    }
                };
                FnCheck fc{flat, pool, param_names, evaluator_, ir_cache_};
                if (!body_node.children.empty())
                    fc.walk(body_node.child(0));
                skip_ir_cache_fn = fc.skip;
            }
            if (skip_ir_cache_fn) {
                continue;
            }

            // Skip functions with internal (define ...) — their cell setup is
            // in __top__ which isn't cached; the cached lambda can't create cells.
            bool has_nested_defines = false;
            {
                struct NestCheck {
                    aura::ast::FlatAST& flat;
                    bool found = false;
                    void walk(aura::ast::NodeId id) {
                        if (found || id >= flat.size())
                            return;
                        auto v = flat.get(id);
                        if (v.tag == aura::ast::NodeTag::Define)
                            found = true;
                        for (auto c : v.children)
                            walk(c);
                    }
                };
                NestCheck nc{flat, false};
                if (!body_node.children.empty())
                    nc.walk(body_node.child(0));
                has_nested_defines = nc.found;
            }
            if (has_nested_defines)
                continue;

            // Create a temporary flat with just this define as root
            auto def_alloc = arena_.allocator();
            aura::ast::FlatAST def_flat(def_alloc);
            aura::ast::StringPool def_pool(def_alloc);

            // Re-parse just the define expression for a clean flat
            // We use define source extraction: walk the content s-exprs
            // Actually, easier: use the existing define by setting flat.root to the define node
            // lower_to_ir_with_cache starts from flat.root
            aura::ast::NodeId saved_root = flat.root;
            flat.root = node_id;

            bool is_redefine = ir_cache_.count(name) > 0;
            auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
            auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
            std::vector<std::string> cache_hits;
            auto ir_mod = lower_to_ir_with_cache_tracked(
                flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), nullptr,
                cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());
            flat.root = saved_root; // restore

            // Run passes
            {
                aura::compiler::ComputeKindWrap ck_pass;
                aura::compiler::ConstantFoldingWrap cf_pass;
                for (auto& func : ir_mod.functions) {
                    if (func.id == ir_mod.entry_function_id)
                        continue;
                    ck_pass.compute_function(func);
                    cf_pass.fold_function(func);
                }
            }

            // Cache bundle
            std::vector<aura::ir::IRFunction> bundle;
            std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
            std::size_t own_pos = 0;
            for (auto& func : ir_mod.functions) {
                if (func.id == ir_mod.entry_function_id)
                    continue;
                // Issue #660: assign a unique stable name (same pattern as cache_define)
                // so cross-module closure identity is preserved when the cache bundle
                // is loaded into a fresh module.
                if (func.name.empty() || func.name == "__lambda__") {
                    func.name = name + std::string("#") + std::to_string(own_pos++);
                }
                bundle.push_back(std::move(func));
                // Save bridge data
                if (func.id < ir_mod.closure_bridge.size())
                    bridge_bundle.push_back(ir_mod.closure_bridge[func.id]);
                else
                    bridge_bundle.emplace_back();
            }
            ir_cache_[name] = std::move(bundle);
            ir_cache_bridge_[name] = std::move(bridge_bundle);
            // Self-referencing cached functions need tree-walker fallback
            user_bindings_.insert(name);
            function_sources_[name] = content;
            module_functions_[path].push_back(name);

            for (auto& cn : cache_hits)
                record_dependency(name, cn);
            if (is_redefine)
                invalidate_function(name);
        }
    }

    // ---- Issue #272: IR-native define env binding --------------------

    struct IRDefineEnvBinding {
        aura::ir::IRModule module;
        aura::compiler::IRContext context;
        std::unique_ptr<aura::compiler::IRInterpreter> interpreter;
        types::EvalValue bound_closure{};
        aura::compiler::ClosureId closure_id = 0;

        IRDefineEnvBinding(aura::ir::IRModule mod, aura::compiler::Primitives& prim,
                           const aura::core::TypeRegistry* reg,
                           aura::compiler::CompilerMetrics* metrics, Evaluator* eval)
            : module(std::move(mod))
            , context(prim, reg, metrics, eval) {}
    };

    void install_persistent_define_closure_bridge() {
        evaluator_.set_closure_bridge(
            [this](aura::compiler::ClosureId cid,
                   std::span<const types::EvalValue> args) -> std::optional<types::EvalValue> {
                return dispatch_ir_define_closure(cid, args);
            });
    }

    std::size_t bind_define_value_in_env(const std::string& name, const types::EvalValue& value) {
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

    [[nodiscard]] const std::unordered_map<std::string, std::size_t>* value_cells_for_lowering() {
        return ir_value_cell_bindings_.empty() ? nullptr : &ir_value_cell_bindings_;
    }

    [[nodiscard]] bool define_body_needs_tree_walker_fallback(
        const aura::ast::FlatAST& flat, const aura::ast::StringPool& pool,
        aura::ast::NodeId body_id, const std::string& self_name = {}) const {
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
            const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>& ir_cache;
            const std::unordered_map<std::string, std::size_t>& value_cells;
            bool needs_fallback = false;
            void walk(aura::ast::NodeId id) {
                if (needs_fallback || id == aura::ast::NULL_NODE || id >= f.size())
                    return;
                auto nv = f.get(id);
                if (nv.tag == aura::ast::NodeTag::Call && !nv.children.empty()) {
                    auto callee_id = nv.child(0);
                    if (callee_id < f.size()) {
                        auto callee_v = f.get(callee_id);
                        if (callee_v.tag == aura::ast::NodeTag::Variable) {
                            auto callee_name = std::string(p.resolve(callee_v.sym_id));
                            if (callee_name == "fiber:spawn" || callee_name == "fiber:join")
                                needs_fallback = true;
                        }
                    }
                }
                if (nv.tag == aura::ast::NodeTag::Variable) {
                    auto var_name = std::string(p.resolve(nv.sym_id));
                    if (param_names.count(var_name) || var_name == self_name)
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
        BodyWalker bw{
            flat, pool, self_name, param_names, evaluator_, ir_cache_, ir_value_cell_bindings_};
        bw.walk(body_id);
        return bw.needs_fallback;
    }

    void snapshot_ir_for_disk(const std::string& name) {
        auto it = ir_cache_.find(name);
        if (it == ir_cache_.end() || it->second.empty())
            return;
        ir_disk_snapshots_[name] = it->second;
    }

    bool bind_value_define_via_ir(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                  aura::ast::NodeId expanded_root, const std::string& name_str) {
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, nullptr, &evaluator_.primitives(), cache_bridge_ptr,
            cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());

        aura::compiler::IRContext ctx(evaluator_.primitives(), &type_registry_, &metrics_,
                                      &evaluator_);
        aura::compiler::IRInterpreter interp(ir_mod, ctx);
        auto result = interp.execute();
        if (!result)
            return false;

        auto ci = bind_define_value_in_env(name_str, *result);
        ir_value_cell_bindings_[name_str] = ci;
        metrics_.value_define_ir_env_bind_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Issue #272 Cycle 2: bind env from an already-cached define (no cache update).
    // Used by compile_module disk-cache hits where ir_cache_ is pre-populated.
    bool bind_define_env_only(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                              aura::ast::NodeId expanded_root, const std::string& name_str) {
        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, nullptr, &evaluator_.primitives(), cache_bridge_ptr,
            cache_strings_ptr, &name_str, &type_registry_, value_cells_for_lowering());
        return bind_function_define_via_ir(ir_mod, name_str);
    }

    bool bind_function_define_via_ir(const aura::ir::IRModule& ir_mod, const std::string& name) {
        if (ir_mod.functions.empty())
            return false;

        auto binding = std::make_unique<IRDefineEnvBinding>(
            ir_mod, evaluator_.primitives(), &type_registry_, &metrics_, &evaluator_);
        binding->interpreter =
            std::make_unique<aura::compiler::IRInterpreter>(binding->module, binding->context);

        auto result = binding->interpreter->execute();
        if (!result || !types::is_closure(*result))
            return false;

        binding->bound_closure = *result;
        binding->closure_id = types::as_closure_id(*result);

        auto old_it = ir_define_env_bindings_.find(name);
        if (old_it != ir_define_env_bindings_.end()) {
            ir_define_closure_owner_.erase(old_it->second->closure_id);
        }

        ir_define_closure_owner_[binding->closure_id] = name;
        bind_define_value_in_env(name, binding->bound_closure);
        ir_define_env_bindings_[name] = std::move(binding);

        metrics_.define_ir_env_bind_count.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::optional<types::EvalValue>
    dispatch_ir_define_closure(aura::compiler::ClosureId cid,
                               std::span<const types::EvalValue> args) {
        auto owner_it = ir_define_closure_owner_.find(cid);
        if (owner_it == ir_define_closure_owner_.end())
            return std::nullopt;
        const auto& name = owner_it->second;
        auto bind_it = ir_define_env_bindings_.find(name);
        if (bind_it == ir_define_env_bindings_.end() || !bind_it->second->interpreter)
            return std::nullopt;
        // Note: the stale-entry detection that lived here previously
        // (binding->interpreter->flat.get() compare against current
        // workspace) was REMOVED because IRInterpreter doesn't expose
        // flat/pool directly — the targeted fix failed to compile.
        // The SIGSEGV is fixed by the surgical fix in
        // mark_all_defines_dirty (clears ir_define_closure_owner_
        // on set-code, the cid→name map that holds stale ClosureIds).

        // Higher-order cached defines (bridge:cached-fn): TW lambda args
        // are not in the IR interpreter's runtime_closures_. Run the cached
        // FlatAST body via eval_flat so (f ...) dispatches correctly.
        bool has_tw_closure_arg = false;
        for (auto& a : args) {
            if (!types::is_closure(a))
                continue;
            auto ac = types::as_closure_id(a);
            if (ir_define_closure_owner_.find(ac) == ir_define_closure_owner_.end())
                has_tw_closure_arg = true;
        }
        if (has_tw_closure_arg) {
            auto bridge_it = ir_cache_bridge_.find(name);
            auto func_it = ir_cache_.find(name);
            if (bridge_it != ir_cache_bridge_.end() && !bridge_it->second.empty() &&
                func_it != ir_cache_.end() && !func_it->second.empty()) {
                auto& bd = bridge_it->second[0];
                if (bd.flat && bd.pool && bd.body_id != aura::ast::NULL_NODE) {
                    aura::compiler::Env ne(&evaluator_.top_env());
                    ne.set_primitives(&evaluator_.primitives());
                    ne.set_pool(bd.pool.get());
                    auto& params = func_it->second[0].params;
                    for (std::size_t i = 0; i < params.size() && i < args.size(); ++i)
                        ne.bind(params[i], args[i]);
                    auto r = evaluator_.eval_flat(
                        *const_cast<aura::ast::FlatAST*>(bd.flat.get()),
                        *const_cast<aura::ast::StringPool*>(bd.pool.get()), bd.body_id, ne);
                    if (r)
                        return *r;
                }
            }
        }

        auto r = bind_it->second->interpreter->call_closure(static_cast<std::uint64_t>(cid), args);
        if (!r)
            return std::nullopt;
        return *r;
    }

    void clear_ir_define_env_binding(const std::string& name) {
        auto it = ir_define_env_bindings_.find(name);
        if (it == ir_define_env_bindings_.end())
            return;
        ir_define_closure_owner_.erase(it->second->closure_id);
        ir_define_env_bindings_.erase(it);
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
    EvalResult cache_define_prefer_partial(std::string_view source, aura::ast::FlatAST& flat,
                                           aura::ast::StringPool& pool,
                                           aura::ast::NodeId expanded_root,
                                           const std::string& name_str, bool bind_in_env = true,
                                           const std::string& module_name = "__repl__") {
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
            if (it != ir_cache_v2_.end() && !it->second.irs.empty() &&
                it->second.source_hash == hash &&
                (it->second.dirty || it->second.dirty_block_count() > 0)) {
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
                // #1555/#1601: dirty_func_count==1 → relower_define_function inside
                // relower_only_dirty_blocks; multi-func dirty → partial or full fallback.
                if (relower_only_dirty_blocks(name_str, source, flat, pool, expanded)) {
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
        }
        // st == 2 (miss) or st == 1 fallback
        return cache_define(source, flat, pool, expanded_root, name_str, bind_in_env, module_name);
    }

    // Lower a define expression to IR, cache it, and bind env via IR when possible.
    // Falls back to eval_flat on skip_ir_cache or IR bind failure.
    EvalResult cache_define(std::string_view source, aura::ast::FlatAST& flat,
                            aura::ast::StringPool& pool, aura::ast::NodeId expanded_root,
                            const std::string& name_str, bool bind_in_env = true,
                            const std::string& module_name = "__repl__") {
        bool is_redefine = ir_cache_.count(name_str) > 0;

        // === Level 2: Type check via TypeCheckWrap pass ===
        {
            aura::compiler::TypeCheckWrap tc_pass;
            aura::diag::DiagnosticCollector diags;
            tc_pass.set_bidirectional_mode(bidirectional_mode_);
            tc_pass.check_before_lowering(flat, pool, expanded_root, type_registry_, diags,
                                          mutation_epoch_.load(std::memory_order_relaxed),
                                          &metrics_);
            bool has_type_error = false;
            for (auto& d : diags.diagnostics()) {
                if (d.kind == aura::diag::ErrorKind::TypeError) {
                    std::println(std::cerr, "type warning ({}): {}", name_str, d.format());
                    has_type_error = true;
                }
            }
            if (strict_mode_ && has_type_error) {
                return std::unexpected(aura::diag::Diagnostic{aura::diag::ErrorKind::TypeError,
                                                              "type error (strict mode)"});
            }
        }

        // Check for reasons to skip IR caching:
        // 1. `require` inside function body — IR lowering treats it as ConstI64 0
        // 2. Self-recursive calls — function not in ir_cache_ yet → ConstI64 0
        // (mitigated: self_name passed to lowering for MakeClosure pre-allocation)
        // 3. Calls to non-cached, non-primitive variables — not resolvable in IR
        bool skip_ir_cache = false;
        if (expanded_root < flat.size()) {
            auto def_v = flat.get(expanded_root);
            if (def_v.tag == aura::ast::NodeTag::Define) {
                auto body_id = def_v.children.empty() ? aura::ast::NULL_NODE : def_v.child(0);
                if (body_id < flat.size()) {
                    auto body_v = flat.get(body_id);
                    if (body_v.tag == aura::ast::NodeTag::Lambda) {
                        auto lambda_body =
                            body_v.children.empty() ? aura::ast::NULL_NODE : body_v.child(0);
                        // Collect parameter names to distinguish them from external references
                        auto params_list = body_v.params;
                        std::unordered_set<std::string> param_names;
                        for (auto pid : params_list)
                            param_names.insert(std::string(pool.resolve(pid)));

                        // Walk the lambda body for variables that can't be lowered
                        struct LambdaBodyWalker {
                            const aura::ast::FlatAST& f;
                            const aura::ast::StringPool& p;
                            const std::string& self_name;
                            const std::unordered_set<std::string>& param_names;
                            const Evaluator& eval;
                            const std::unordered_map<std::string,
                                                     std::vector<aura::ir::IRFunction>>& ir_cache;
                            bool skip = false;
                            void walk(aura::ast::NodeId id) {
                                if (skip || id == aura::ast::NULL_NODE || id >= f.size())
                                    return;
                                auto nv = f.get(id);
                                if (nv.tag == aura::ast::NodeTag::Variable) {
                                    auto var_name = std::string(p.resolve(nv.sym_id));
                                    // Skip params, the function's own name (self-reference handled
                                    // by lowering), primitives, cached functions
                                    if (param_names.count(var_name))
                                        return;
                                    if (var_name == self_name)
                                        return;
                                    if (eval.primitives().slot_for_name(var_name) <
                                        eval.primitives().slot_count())
                                        return;
                                    if (ir_cache.count(var_name))
                                        return;
                                    // Unknown variable — IR will emit ConstI64 0
                                    skip = true;
                                }
                                for (auto c : nv.children)
                                    walk(c);
                            }
                        };
                        LambdaBodyWalker lbw{flat,        pool,       name_str,
                                             param_names, evaluator_, ir_cache_};
                        lbw.walk(lambda_body);
                        skip_ir_cache = lbw.skip;
                    }
                }
            }
        }

        if (skip_ir_cache) {
            // Skip IR caching — use tree-walker only. The define has already been
            // evaluated via tree-walker below, so the env bindings are correct.
            function_sources_[name_str] = std::string(source);
            module_functions_["__repl__"].push_back(name_str);
            return evaluator_.eval_flat(flat, pool, expanded_root, evaluator_.top_env());
        }

        auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
        auto cache_bridge_ptr = ir_cache_bridge_.empty() ? nullptr : &ir_cache_bridge_;
        auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
        std::vector<std::string> cache_hits;
        // Pass self_name so lowering can emit correct MakeClosure for self-references
        auto ir_mod = lower_to_ir_with_cache_tracked(
            flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), cache_bridge_ptr,
            cache_strings_ptr, &name_str, &type_registry_, value_cells_for_lowering());
        // Issue #660 debug: dump IR after lowering
        if (name_str.empty()) {
            for (auto& func : ir_mod.functions) {
                std::println(std::cerr, "[IR-DUMP] func id={} name='{}' blocks={}", func.id,
                             func.name, func.blocks.size());
                for (auto& blk : func.blocks) {
                    for (auto& inst : blk.instructions) {
                        std::println(std::cerr, "  inst op={} ops=[{},{},{},{}]",
                                     static_cast<int>(inst.opcode), inst.operands[0],
                                     inst.operands[1], inst.operands[2], inst.operands[3]);
                    }
                }
            }
        }

        // Run passes per-function on the new function bundle
        {
            aura::compiler::ComputeKindWrap ck_pass;
            aura::compiler::ConstantFoldingWrap cf_pass;
            for (auto& func : ir_mod.functions) {
                if (func.id == ir_mod.entry_function_id)
                    continue;
                ck_pass.compute_function(func);
                cf_pass.fold_function(func);
                // Issue #538: DCE after define cache lower.
                run_coercion_elim_on_function(func);
            }
        }

        // Issue #660 Option 1: assign each function a unique stable name
        // (user'''s define name + "#" + position). This name is used by
        // the runtime for cross-module closure identity when the closure'''s
        // func_id is out of bounds in the current module.
        std::vector<aura::ir::IRFunction> bundle;
        std::vector<aura::ir::ClosureBridgeData> bridge_bundle;
        std::size_t own_pos = 0;
        for (auto& func : ir_mod.functions) {
            if (func.id == ir_mod.entry_function_id)
                continue;
            // Override the default "__lambda__" name (or empty name) with
            // a unique stable name for cross-module closure identity.
            if (func.name.empty() || func.name == "__lambda__") {
                func.name = name_str + std::string("#") + std::to_string(own_pos++);
            }
            // Issue #660 follow-up: COPY instead of move (preserves ir_mod for binding)
            bundle.push_back(func);
            // Also save bridge data
            if (func.id < ir_mod.closure_bridge.size())
                bridge_bundle.push_back(ir_mod.closure_bridge[func.id]);
            else
                bridge_bundle.emplace_back();
        }
        // Issue #272 Cycle 4: snapshot before move + IR env bind for disk serialize.
        ir_disk_snapshots_[name_str] = bundle;
        ir_cache_[name_str] = std::move(bundle);
        ir_cache_bridge_[name_str] = std::move(bridge_bundle);
        ir_cache_strings_[name_str] = ir_mod.string_pool;
        function_sources_[name_str] = std::string(source);
        module_functions_[module_name].push_back(name_str);

        for (auto& called_name : cache_hits) {
            record_dependency(name_str, called_name);
        }

        if (is_redefine) {
            // Issue #660 follow-up: JIT cache keys are
            // `name + "#" + position` (e.g. "mul#0"), not the
            // bare user name. The previous fix erased
            // "__lambda__" + name_str, both of which miss the
            // actual entry. Walk the cache and erase any
            // entries whose key starts with name_str + "#"
            // (covers mul#0, mul#1, ... for the user's mul).
            // Also call jit_.invalidate_prefix(name_str) to
            // clear the AuraJIT::compile_fns_ + fn_trackers_
            // caches (which are keyed by ir_fn.name, also
            // name + "#" + pos).
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                std::string prefix = name_str + "#";
                for (auto it = jit_cache_.begin(); it != jit_cache_.end();) {
                    if (it->first == "__lambda__" || it->first.rfind(prefix, 0) == 0) {
                        it = jit_cache_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            jit_.invalidate_prefix(name_str.c_str());
            // Also drop the per-function compile cache inside AuraJIT
            // (compile_fns_) so the next compile() actually re-runs the
            // LLVM pipeline. Without this, compile() short-circuits
            // on a cache hit and returns the old fn_ptr even though we
            // erased jit_cache_ — and the runtime keeps calling the
            // stale implementation. The name is "__lambda__" because
            // the body of a top-level define is lowered as an
            // anonymous lambda, so every define shares that name in
            // the JIT's per-function cache.
            jit_.invalidate("__lambda__");
            invalidate_function(name_str);
            mark_module_dirty(name_str);
        }

        // Issue #272: bind env via IRInterpreter when possible (function defines).
        // Skip when bind_in_env=false (populate_ir_cache_v2_from_workspace
        // pre-populates the v2 cache without polluting the env).
        if (bind_in_env) {
            if (bind_function_define_via_ir(ir_mod, name_str)) {
                return EvalResult(types::make_void());
            }
            return evaluator_.eval_flat(flat, pool, expanded_root, evaluator_.top_env());
        }
        // bind_in_env=false: skip the env binding. Caller (e.g.
        // populate_ir_cache_v2_from_workspace) just wants the IR cached.
        return EvalResult(types::make_void());
    }

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
    CompilerSnapshot snapshot() const {
        CompilerSnapshot s;
        s.deopt_count = metrics_.deopt_count.load(std::memory_order_relaxed);
        s.specialization_hits = metrics_.specialization_hits.load(std::memory_order_relaxed);
        s.specialization_misses = metrics_.specialization_misses.load(std::memory_order_relaxed);
        s.shape_changes_observed = metrics_.shape_changes_observed.load(std::memory_order_relaxed);
        s.jit_compilations = metrics_.jit_compilations.load(std::memory_order_relaxed);
        s.jit_compile_misses = metrics_.jit_compile_misses.load(std::memory_order_relaxed);
        s.jit_cache_evictions = metrics_.jit_cache_evictions.load(std::memory_order_relaxed);
        s.aot_emits = metrics_.aot_emits.load(std::memory_order_relaxed);
        s.aot_fallbacks = metrics_.aot_fallbacks.load(std::memory_order_relaxed);
        s.arena_bytes_used = metrics_.arena_bytes_used.load(std::memory_order_relaxed);
        s.arena_bytes_peak = metrics_.arena_bytes_peak.load(std::memory_order_relaxed);
        // Issue #250: atomic-batch observability.
        s.atomic_batch_count = evaluator_.atomic_batch_count();
        s.atomic_batch_ops_total = evaluator_.atomic_batch_ops_total();
        s.atomic_batch_rollbacks = evaluator_.atomic_batch_rollbacks();
        s.atomic_batch_bumps_saved_total = evaluator_.atomic_batch_bumps_saved_total();
        // Issue #252: closure dual-path observability. Read
        // from the shared CompilerMetrics struct (also bumped
        // by the IR's IROpcode::Call/Apply). The Evaluator's
        // apply_closure writes to the same struct via the
        // compiler_metrics_ pointer set in the constructor.
        s.closure_calls_total = metrics_.closure_calls_total.load(std::memory_order_relaxed);
        s.closure_ffi_calls = metrics_.closure_ffi_calls.load(std::memory_order_relaxed);
        s.closure_tw_calls = metrics_.closure_tw_calls.load(std::memory_order_relaxed);
        s.closure_ir_calls = metrics_.closure_ir_calls.load(std::memory_order_relaxed);
        s.closure_bridge_calls = metrics_.closure_bridge_calls.load(std::memory_order_relaxed);
        s.closure_stale_returns = metrics_.closure_stale_returns.load(std::memory_order_relaxed);
        // Issue #253: linear-move elision count (lifetime total).
        s.linear_elide_count = metrics_.linear_elide_count.load(std::memory_order_relaxed);
        // Issue #433: dead coercion elimination
        // (lifetime total of CastOps eliminated by the
        // DeadCoercionEliminationPass).
        s.dead_coercion_eliminated_total =
            metrics_.dead_coercion_eliminated_total.load(std::memory_order_relaxed);
        // Issue #508: elapsed_us + kept_for_debug exposure.
        s.dead_coercion_elapsed_us_total =
            metrics_.dead_coercion_elapsed_us_total.load(std::memory_order_relaxed);
        s.dead_coercion_kept_for_debug_total =
            metrics_.dead_coercion_kept_for_debug_total.load(std::memory_order_relaxed);
        // Issue #629: zero-overhead coercion path observability.
        s.coercion_castop_emitted_total =
            metrics_.coercion_castop_emitted_total.load(std::memory_order_relaxed);
        s.coercion_type_prop_hits_total =
            metrics_.coercion_type_prop_hits_total.load(std::memory_order_relaxed);
        s.coercion_narrow_evidence_hits_total =
            metrics_.coercion_narrow_evidence_hits_total.load(std::memory_order_relaxed);
        s.coercion_zerooverhead_win_total =
            metrics_.coercion_zerooverhead_win_total.load(std::memory_order_relaxed);
        s.coercion_post_narrow_elim_opportunities_total =
            metrics_.coercion_post_narrow_elim_opportunities_total.load(std::memory_order_relaxed);
        s.coercion_narrow_blame_chain_hits_total =
            metrics_.coercion_narrow_blame_chain_hits_total.load(std::memory_order_relaxed);
        s.coercion_cast_elim_from_narrow_total =
            metrics_.coercion_cast_elim_from_narrow_total.load(std::memory_order_relaxed);
        // Issue #487: dirty propagation + IR re-lower
        // observability. Mirror the 2 lifetime
        // counters and compute the derived trigger
        // rate (basis points: should_relower /
        // affected_subtree * 10000).
        s.should_relower_total = metrics_.should_relower_total.load(std::memory_order_relaxed);
        s.affected_subtree_total = metrics_.affected_subtree_total.load(std::memory_order_relaxed);
        if (s.affected_subtree_total > 0) {
            s.dirty_trigger_rate_bp = (s.should_relower_total * 10000u) / s.affected_subtree_total;
        } else {
            s.dirty_trigger_rate_bp = 0;
        }
        // Issue #1474 / #1495: per-block re-lower observability.
        // Mirror incremental_relower_blocks_total + full_relower_count
        // and derive dirty_block_ratio_bp from the existing
        // ir_soa_block_dirty_hits_total /
        // ir_soa_relower_blocks_saved_total pair.
        s.incremental_relower_blocks_total =
            metrics_.incremental_relower_blocks_total.load(std::memory_order_relaxed);
        s.full_relower_count = metrics_.relower_full_called_count.load(std::memory_order_relaxed);
        {
            const std::uint64_t hits =
                metrics_.ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed);
            const std::uint64_t saved =
                metrics_.ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed);
            const std::uint64_t sum = hits + saved;
            s.dirty_block_ratio_bp =
                sum > 0 ? static_cast<std::uint64_t>((hits * 10000u) / sum) : 0ull;
        }
        // Issue #387: Type Dependency Graph observability.
        // Mirrors the 3 atomics; derives type_dep_graph_hit_rate_bp.
        s.type_dep_graph_lookups = metrics_.type_dep_graph_lookups.load(std::memory_order_relaxed);
        s.type_dep_graph_hits = metrics_.type_dep_graph_hits.load(std::memory_order_relaxed);
        s.type_dep_graph_size = metrics_.type_dep_graph_size.load(std::memory_order_relaxed);
        if (s.type_dep_graph_lookups > 0) {
            s.type_dep_graph_hit_rate_bp =
                (s.type_dep_graph_hits * 10000u) / s.type_dep_graph_lookups;
        } else {
            s.type_dep_graph_hit_rate_bp = 0;
        }
        // Issue #254: IR SoA dual-emit counters (lifetime total).
        s.ir_soa_instructions_emitted =
            metrics_.ir_soa_instructions_emitted.load(std::memory_order_relaxed);
        s.ir_soa_functions_emitted =
            metrics_.ir_soa_functions_emitted.load(std::memory_order_relaxed);
        // Issue #255: reference stability observability. Read
        // the live counters from the workspace FlatAST (they
        // live on FlatAST, not on CompilerMetrics — the bump
        // sites are in ast.ixx where they have direct access
        // to the generation_ + node_gen_ state). If no
        // workspace is set (e.g. pre-init), all counts stay 0.
        if (auto* ws_flat = evaluator_.workspace_flat()) {
            s.bump_generation_count = ws_flat->bump_generation_count();
            s.is_valid_check_count = ws_flat->is_valid_check_count();
            s.stable_ref_invalidations = ws_flat->stable_ref_invalidations();
            s.atomic_batch_commits = ws_flat->atomic_batch_commits_v();
            // Issue #343: long-term stability observability.
            // current_generation is the live value of
            // FlatAST::generation_ (uint16_t, 1..65535);
            // generation_wrap_count is the lifetime total
            // of wrap-arounds; node_gen_stale_access_count
            // is the lifetime total of stale NodeId
            // accesses. These are all read from the
            // workspace FlatAST (they live there, not on
            // CompilerMetrics).
            s.current_generation = ws_flat->current_generation();
            s.generation_wrap_count = ws_flat->generation_wrap_count();
            s.node_gen_stale_access_count = ws_flat->node_gen_stale_access_count();
            // Issue #368: current wrap_epoch_ so AI agents can
            // checkpoint / compact before the next wrap.
            s.current_wrap_epoch = ws_flat->wrap_epoch();
            // Issue #369/370: structural rollback + safe-view
            // counters.
            s.structural_rollback_success = ws_flat->structural_rollback_success();
            s.structural_rollback_besteffort = ws_flat->structural_rollback_besteffort();
            s.children_safe_view_count = ws_flat->children_safe_view_count();
            s.parent_safe_view_count = ws_flat->parent_safe_view_count();
            // Issue #1281/#1282: children topology restore + wrap restamp.
            s.children_topology_restore_count = ws_flat->children_topology_restore_count();
            // Issue #1502: parent_ topology rebuild after failed boundary.
            s.parent_topology_restore_count = ws_flat->parent_topology_restore_count();
            s.auto_restamp_on_wrap_count = ws_flat->auto_restamp_on_wrap_count();
            // Issue #256: AST operation observability counters
            // (children/parent_of/mark_dirty_upward call counts
            // + total nodes marked dirty).
            s.children_call_count = ws_flat->children_call_count();
            s.parent_of_call_count = ws_flat->parent_of_call_count();
            s.mark_dirty_upward_call_count = ws_flat->mark_dirty_upward_call_count();
            s.mark_dirty_total_nodes = ws_flat->mark_dirty_total_nodes();
        }
        // Issue #258: multi-mutation incremental typecheck
        // observability. Read lifetime totals from
        // CompilerMetrics, compute the derived
        // multi_mutation_recompute_ratio (basis points:
        // cache_misses / (hits + misses + stale) * 10000).
        s.typecheck_cache_hits_total =
            metrics_.typecheck_cache_hits_total.load(std::memory_order_relaxed);
        s.typecheck_cache_misses_total =
            metrics_.typecheck_cache_misses_total.load(std::memory_order_relaxed);
        s.typecheck_stale_cache_total =
            metrics_.typecheck_stale_cache_total.load(std::memory_order_relaxed);
        s.delta_solve_time_us = metrics_.delta_solve_time_us.load(std::memory_order_relaxed);
        const std::uint64_t total = s.typecheck_cache_hits_total + s.typecheck_cache_misses_total +
                                    s.typecheck_stale_cache_total;
        if (total > 0) {
            s.multi_mutation_recompute_ratio_bp = (s.typecheck_cache_misses_total * 10000u) / total;
        } else {
            s.multi_mutation_recompute_ratio_bp = 0;
        }
        // Issue #412: mirror the gen_saved lifetime counter
        // and compute the derived gen_saved_ratio_bp (basis
        // points: gen_saved / (stale + gen_saved) * 10000).
        // Higher = more false-positive stale rejections
        // eliminated by the gen check.
        s.typecheck_gen_saved_total =
            metrics_.typecheck_gen_saved_total.load(std::memory_order_relaxed);
        const std::uint64_t gen_total = s.typecheck_stale_cache_total + s.typecheck_gen_saved_total;
        if (gen_total > 0) {
            s.typecheck_gen_saved_ratio_bp = (s.typecheck_gen_saved_total * 10000u) / gen_total;
        } else {
            s.typecheck_gen_saved_ratio_bp = 0;
        }
        // Issue #412 follow-up #1: mirror the 2
        // per-binding gen counters and compute the
        // derived hit ratio (basis points:
        // per_binding_gen_hits / (per_binding_gen_hits +
        // stale_cache) * 10000). 0 when no per-binding
        // hits have happened yet.
        s.per_binding_gen_hits_total =
            metrics_.per_binding_gen_hits_total.load(std::memory_order_relaxed);
        // The per_binding_gen_bumps_total counter comes
        // from the workspace FlatAST (it's bumped on
        // every mark_dirty_upward on a binding node).
        // Read from the workspace if available. The
        // accumulator is a separate field (NOT
        // per_binding_gen_bumps_total in CompilerMetrics)
        // because snapshot() is const and we can't
        // fetch_add on a const atomic. The accumulator
        // is mutable so the lifetime total persists
        // across snapshots.
        std::uint64_t binding_gen_bumps_from_flat = 0;
        if (auto* ws = evaluator_.workspace_flat()) {
            binding_gen_bumps_from_flat = ws->binding_gen_bumps_total();
        }
        if (binding_gen_bumps_from_flat > last_per_binding_gen_bumps_) {
            per_binding_gen_bumps_acc_ += binding_gen_bumps_from_flat - last_per_binding_gen_bumps_;
            last_per_binding_gen_bumps_ = binding_gen_bumps_from_flat;
        }
        s.per_binding_gen_bumps_total = per_binding_gen_bumps_acc_;
        const std::uint64_t pb_total = s.per_binding_gen_hits_total + s.typecheck_stale_cache_total;
        if (pb_total > 0) {
            s.per_binding_gen_hit_ratio_bp = (s.per_binding_gen_hits_total * 10000u) / pb_total;
        } else {
            s.per_binding_gen_hit_ratio_bp = 0;
        }
        // Issue #413: invalidation trace records count.
        // Read from the workspace FlatAST (the trace
        // vector is per-FlatAST, lifetime = FlatAST
        // lifetime). Accumulate via the same pattern as
        // per_binding_gen_bumps_total.
        std::uint64_t trace_records_from_flat = 0;
        if (auto* ws = evaluator_.workspace_flat()) {
            trace_records_from_flat = ws->invalidation_trace_records_total();
        }
        if (trace_records_from_flat > last_invalidation_trace_records_) {
            invalidation_trace_records_acc_ +=
                trace_records_from_flat - last_invalidation_trace_records_;
            last_invalidation_trace_records_ = trace_records_from_flat;
        }
        s.invalidation_trace_records_total = invalidation_trace_records_acc_;
        // Issue #386: mirror the 3 narrowing
        // observability counters and compute the
        // derived applied ratio (basis points:
        // applied / (applied + skipped) * 10000).
        // 0 when no narrowing has happened yet.
        s.narrowing_applied_total =
            metrics_.narrowing_applied_total.load(std::memory_order_relaxed);
        s.narrowing_skipped_total =
            metrics_.narrowing_skipped_total.load(std::memory_order_relaxed);
        s.narrowing_reanalyzed_total =
            metrics_.narrowing_reanalyzed_total.load(std::memory_order_relaxed);
        // Issue #338: mirror the 2 and/or precision
        // counters.
        s.and_or_meet_uses_total = metrics_.and_or_meet_uses_total.load(std::memory_order_relaxed);
        s.and_or_join_uses_total = metrics_.and_or_join_uses_total.load(std::memory_order_relaxed);
        // Issue #434: per-node occurrence dirty recovery.
        s.narrowing_dirty_recovery_total =
            metrics_.narrowing_dirty_recovery_total.load(std::memory_order_relaxed);
        // Issue #390: schema cache observability.
        s.schema_cache_lookups_total =
            metrics_.schema_cache_lookups_total.load(std::memory_order_relaxed);
        s.schema_cache_hits_total =
            metrics_.schema_cache_hits_total.load(std::memory_order_relaxed);
        if (s.schema_cache_lookups_total > 0) {
            s.schema_cache_hit_rate_bp =
                (s.schema_cache_hits_total * 10000u) / s.schema_cache_lookups_total;
        } else {
            s.schema_cache_hit_rate_bp = 0;
        }
        // Issue #409: fine-grained constraint
        // dependency tracking observability. Mirror
        // the 2 lifetime counters and compute the
        // derived ratio (basis points: processed /
        // total * 10000).
        s.delta_constraints_processed_total =
            metrics_.delta_constraints_processed_total.load(std::memory_order_relaxed);
        s.delta_constraints_total =
            metrics_.delta_constraints_total.load(std::memory_order_relaxed);
        s.delta_conflict_reverify_total =
            metrics_.delta_conflict_reverify_total.load(std::memory_order_relaxed);
        s.delta_conflict_detected_total =
            metrics_.delta_conflict_detected_total.load(std::memory_order_relaxed);
        s.reverify_truncated_total =
            metrics_.reverify_truncated_total.load(std::memory_order_relaxed);
        s.constraint_blame_chain_complete_total =
            metrics_.constraint_blame_chain_complete_total.load(std::memory_order_relaxed);
        s.constraint_reverify_narrow_hits_total =
            metrics_.constraint_reverify_narrow_hits_total.load(std::memory_order_relaxed);
        s.constraint_reverify_timeout_prevented_total =
            metrics_.constraint_reverify_timeout_prevented_total.load(std::memory_order_relaxed);
        s.constraint_stale_blame_invalidation_total =
            metrics_.constraint_stale_blame_invalidation_total.load(std::memory_order_relaxed);
        s.solve_delta_full_solve_fallback_total =
            metrics_.solve_delta_full_solve_fallback_total.load(std::memory_order_relaxed);
        if (s.delta_constraints_total > 0) {
            s.delta_solve_constraints_ratio_bp =
                (s.delta_constraints_processed_total * 10000u) / s.delta_constraints_total;
        } else {
            s.delta_solve_constraints_ratio_bp = 0;
        }
        // Issue #341: match + Occurrence Typing
        // observability. Mirror the 2 lifetime
        // counters and compute the derived ratio
        // (basis points: narrowed / total * 10000).
        s.match_subject_narrowed_total =
            metrics_.match_subject_narrowed_total.load(std::memory_order_relaxed);
        s.match_subject_total = metrics_.match_subject_total.load(std::memory_order_relaxed);
        if (s.match_subject_total > 0) {
            s.match_narrowed_ratio_bp =
                (s.match_subject_narrowed_total * 10000u) / s.match_subject_total;
        } else {
            s.match_narrowed_ratio_bp = 0;
        }
        // Issue #612: ADT/match exhaustiveness post-mutation
        // reliability observability.
        s.adt_exhaust_rechecks_total =
            metrics_.adt_exhaust_rechecks_total.load(std::memory_order_relaxed);
        s.adt_variant_mutate_impacts_total =
            metrics_.adt_variant_mutate_impacts_total.load(std::memory_order_relaxed);
        s.adt_stale_exhaust_prevented_total =
            metrics_.adt_stale_exhaust_prevented_total.load(std::memory_order_relaxed);
        s.adt_occurrence_narrow_in_match_total =
            metrics_.adt_occurrence_narrow_in_match_total.load(std::memory_order_relaxed);
        s.adt_pattern_narrow_refreshes_total =
            metrics_.adt_pattern_narrow_refreshes_total.load(std::memory_order_relaxed);
        s.adt_non_exhaustive_caught_total =
            metrics_.adt_non_exhaustive_caught_total.load(std::memory_order_relaxed);
        s.adt_pattern_provenance_complete_total =
            metrics_.adt_pattern_provenance_complete_total.load(std::memory_order_relaxed);
        s.hardware_backend_hook_calls_total =
            metrics_.hardware_backend_hook_calls_total.load(std::memory_order_relaxed);
        s.commercial_reemits_total =
            metrics_.commercial_reemits_total.load(std::memory_order_relaxed);
        s.feedback_mutate_hits_total =
            metrics_.feedback_mutate_hits_total.load(std::memory_order_relaxed);
        s.ppa_savings_total = metrics_.ppa_savings_total.load(std::memory_order_relaxed);
        s.verification_loop_success_total =
            metrics_.verification_loop_success_total.load(std::memory_order_relaxed);
        s.sv_emit_parse_success_total =
            metrics_.sv_emit_parse_success_total.load(std::memory_order_relaxed);
        s.sv_emit_parse_fail_total =
            metrics_.sv_emit_parse_fail_total.load(std::memory_order_relaxed);
        s.commercial_simulator_runs_total =
            metrics_.commercial_simulator_runs_total.load(std::memory_order_relaxed);
        s.sv_diff_emits_total = metrics_.sv_diff_emits_total.load(std::memory_order_relaxed);
        s.sva_structured_mutate_hits_total =
            metrics_.sva_structured_mutate_hits_total.load(std::memory_order_relaxed);
        s.eda_sv_evolution_cycles_total =
            metrics_.eda_sv_evolution_cycles_total.load(std::memory_order_relaxed);
        s.eda_sv_verification_convergence_total =
            metrics_.eda_sv_verification_convergence_total.load(std::memory_order_relaxed);
        s.eda_sv_feedback_mutate_success_total =
            metrics_.eda_sv_feedback_mutate_success_total.load(std::memory_order_relaxed);
        s.eda_sv_stable_ref_invalidation_total =
            metrics_.eda_sv_stable_ref_invalidation_total.load(std::memory_order_relaxed);
        s.eda_sv_commercial_stub_latency_us_total =
            metrics_.eda_sv_commercial_stub_latency_us_total.load(std::memory_order_relaxed);
        s.eda_sv_corruption_detected_total =
            metrics_.eda_sv_corruption_detected_total.load(std::memory_order_relaxed);
        s.primitive_skeleton_generations_total =
            metrics_.primitive_skeleton_generations_total.load(std::memory_order_relaxed);
        s.primitive_eda_meta_backfill_total =
            metrics_.primitive_eda_meta_backfill_total.load(std::memory_order_relaxed);
        // Issue #342: narrowing provenance
        // observability. Mirrors the lifetime
        // counter in CompilerMetrics.
        s.narrowing_provenance_total =
            metrics_.narrowing_provenance_total.load(std::memory_order_relaxed);
        s.occurrence_stale_refreshes_total =
            metrics_.occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
        s.occurrence_blame_chain_complete_total =
            metrics_.occurrence_blame_chain_complete_total.load(std::memory_order_relaxed);
        // Issue #639: narrow blame + stale invalidation.
        s.narrow_stale_caught_total =
            metrics_.narrow_stale_caught_total.load(std::memory_order_relaxed);
        s.narrow_blame_attached_total =
            metrics_.narrow_blame_attached_total.load(std::memory_order_relaxed);
        s.narrow_invalidation_post_mutate_total =
            metrics_.narrow_invalidation_post_mutate_total.load(std::memory_order_relaxed);
        if (auto* ws = evaluator_.workspace_flat()) {
            s.narrow_invalidation_post_mutate_total += ws->narrow_invalidation_post_mutate_count();
        }
        s.narrow_safe_fallback_total =
            metrics_.narrow_safe_fallback_total.load(std::memory_order_relaxed);
        // Issue #627: bidirectional check-mode narrow observability.
        s.check_mode_narrow_hits_total =
            metrics_.check_mode_narrow_hits_total.load(std::memory_order_relaxed);
        s.synthesize_check_switch_count_total =
            metrics_.synthesize_check_switch_count_total.load(std::memory_order_relaxed);
        s.post_mutate_narrow_consistency_total =
            metrics_.post_mutate_narrow_consistency_total.load(std::memory_order_relaxed);
        s.stale_check_narrow_prevented_total =
            metrics_.stale_check_narrow_prevented_total.load(std::memory_order_relaxed);
        // Issue #383: ConstraintSystem worklist +
        // consistent_unify observability. Mirror
        // the 3 lifetime counters in
        // CompilerMetrics.
        s.consistent_unify_total = metrics_.consistent_unify_total.load(std::memory_order_relaxed);
        s.consistent_subtype_total =
            metrics_.consistent_subtype_total.load(std::memory_order_relaxed);
        s.worklist_restart_total = metrics_.worklist_restart_total.load(std::memory_order_relaxed);
        // Issue #385: Let-Poly caching observability.
        // Mirror the 3 lifetime counters and compute
        // the derived dedup ratio (basis points:
        // dedup_hits / register * 10000).
        s.poly_register_total = metrics_.poly_register_total.load(std::memory_order_relaxed);
        s.poly_dedup_hits_total = metrics_.poly_dedup_hits_total.load(std::memory_order_relaxed);
        s.poly_instantiate_total = metrics_.poly_instantiate_total.load(std::memory_order_relaxed);
        if (s.poly_register_total > 0) {
            s.poly_dedup_ratio_bp = (s.poly_dedup_hits_total * 10000u) / s.poly_register_total;
        } else {
            s.poly_dedup_ratio_bp = 0;
        }
        const std::uint64_t narrow_total = s.narrowing_applied_total + s.narrowing_skipped_total;
        if (narrow_total > 0) {
            s.narrowing_applied_ratio_bp = (s.narrowing_applied_total * 10000u) / narrow_total;
        } else {
            s.narrowing_applied_ratio_bp = 0;
        }
        // Issue #259: type metadata propagation observability.
        // Read lifetime totals from CompilerMetrics, compute
        // the derived coverage (basis points: 0-10000).
        s.ir_instructions_total = metrics_.ir_instructions_total.load(std::memory_order_relaxed);
        s.ir_instructions_with_type_total =
            metrics_.ir_instructions_with_type_total.load(std::memory_order_relaxed);
        if (s.ir_instructions_total > 0) {
            s.type_propagation_coverage_bp =
                (s.ir_instructions_with_type_total * 10000u) / s.ir_instructions_total;
        } else {
            s.type_propagation_coverage_bp = 0;
        }
        // Issue #410: per-symbol dirty observability. Mirror
        // the 2 lifetime counters and compute the derived
        // reduction ratio (basis points). The ancestor-avg is
        // computed from the mark_dirty_total_nodes /
        // mark_dirty_upward_call_count ratio that the
        // observable already exposes (Issue #256). When that
        // avg is 0 (no calls yet) the derived ratio stays 0.
        s.per_symbol_dirty_lookups_total =
            metrics_.per_symbol_dirty_lookups_total.load(std::memory_order_relaxed);
        s.per_symbol_dirty_uses_total =
            metrics_.per_symbol_dirty_uses_total.load(std::memory_order_relaxed);
        if (s.per_symbol_dirty_lookups_total > 0 && s.mark_dirty_upward_call_count > 0) {
            const std::uint64_t avg_ancestor_depth =
                s.mark_dirty_total_nodes / s.mark_dirty_upward_call_count;
            const std::uint64_t est_ancestor_uses =
                avg_ancestor_depth * s.per_symbol_dirty_lookups_total;
            s.per_symbol_dirty_reduction_bp = (s.per_symbol_dirty_uses_total * 10000u) /
                                              (est_ancestor_uses > 0 ? est_ancestor_uses : 1);
        } else {
            s.per_symbol_dirty_reduction_bp = 0;
        }
        // Issue #411: post-mutation auto-incremental typecheck
        // observability. Mirror the 2 lifetime counters and
        // compute the derived average (basis points:
        // re_inferred * 10000 / max(auto_invocations, 1)).
        // The average is 0 when no auto-invocations have
        // happened yet (i.e. before any typed_mutate or when
        // the mode is set to Lazy/Disabled).
        s.incremental_typecheck_auto_invocations_total =
            metrics_.incremental_typecheck_auto_invocations_total.load(std::memory_order_relaxed);
        s.incremental_typecheck_re_inferred_total =
            metrics_.incremental_typecheck_re_inferred_total.load(std::memory_order_relaxed);
        if (s.incremental_typecheck_auto_invocations_total > 0) {
            s.incremental_typecheck_avg_re_inferred_bp =
                (s.incremental_typecheck_re_inferred_total * 10000u) /
                s.incremental_typecheck_auto_invocations_total;
        } else {
            s.incremental_typecheck_avg_re_inferred_bp = 0;
        }
        // Issue #411 follow-up #1: mirror the 4 per-symbol
        // counters and compute the derived path-share
        // metric. 0 when no re-inference has happened yet.
        s.per_symbol_reinfer_used_total =
            metrics_.per_symbol_reinfer_used_total.load(std::memory_order_relaxed);
        s.per_symbol_reinfer_visited_total =
            metrics_.per_symbol_reinfer_visited_total.load(std::memory_order_relaxed);
        s.ancestor_reinfer_used_total =
            metrics_.ancestor_reinfer_used_total.load(std::memory_order_relaxed);
        s.ancestor_reinfer_visited_total =
            metrics_.ancestor_reinfer_visited_total.load(std::memory_order_relaxed);
        const std::uint64_t total_visited =
            s.per_symbol_reinfer_visited_total + s.ancestor_reinfer_visited_total;
        if (total_visited > 0) {
            s.per_symbol_path_share_bp =
                (s.per_symbol_reinfer_visited_total * 10000u) / total_visited;
        } else {
            s.per_symbol_path_share_bp = 0;
        }
        // Issue #411 fu1 follow-up #2: mirror the 3
        // per-DefUseIndex counters and compute the
        // derived average (basis points:
        // per_defuse_index_visited / max(per_defuse_index_used, 1) * 10000).
        // 0 when no per-DefUseIndex invocations have
        // happened yet.
        s.per_defuse_index_used_total =
            metrics_.per_defuse_index_used_total.load(std::memory_order_relaxed);
        s.per_defuse_index_visited_total =
            metrics_.per_defuse_index_visited_total.load(std::memory_order_relaxed);
        s.per_defuse_index_walk_fallback_total =
            metrics_.per_defuse_index_walk_fallback_total.load(std::memory_order_relaxed);
        if (s.per_defuse_index_used_total > 0) {
            s.per_defuse_index_visited_avg_bp =
                (s.per_defuse_index_visited_total * 10000u) / s.per_defuse_index_used_total;
        } else {
            s.per_defuse_index_visited_avg_bp = 0;
        }
        // Issue #247: populate marker distribution by walking
        // workspace_flat_->marker_column(). We grab a
        // shared_lock on workspace_mtx_ to keep the flat
        // pointer stable during the read. If no workspace is
        // set (workspace_flat_ is null), all counts stay 0.
        //
        // Note: snapshot() is const but lock_workspace_shared is
        // non-const (it modifies the mutex's internal state). We
        // const_cast the Evaluator reference. This is safe
        // because the shared_mutex's internal state is mutable
        // (its lock/unlock operations don't change the logical
        // contents of the Evaluator).
        //
        // Re-entrancy: snapshot() can be called from the IR
        // interpreter via Aura primitives (e.g. (query:*) that
        // compute hash fields from runtime stats). If the
        // interpreter runs while set-code / restore-ast /
        // mutate holds workspace_mtx_ unique_lock, calling
        // lock_workspace_shared() here would deadlock
        // (EDEADLK / "Resource deadlock avoided"). Use
        // try_lock_shared() and skip the marker read on
        // contention — the marker counts are observability
        // hints, not correctness state, so a missed read is
        // acceptable.
        {
            auto& eval_mut = const_cast<Evaluator&>(evaluator_);
            if (eval_mut.try_lock_workspace_shared()) {
                struct UnlockGuard {
                    Evaluator& e;
                    ~UnlockGuard() { e.unlock_workspace_shared(); }
                } guard{eval_mut};
                const auto* wf = eval_mut.workspace_flat();
                if (wf) {
                    const auto& markers = wf->marker_column();
                    s.marker_total_count = markers.size();
                    for (auto m : markers) {
                        auto val = static_cast<int>(m);
                        if (val == 0)
                            ++s.marker_user_count;
                        else if (val == 1)
                            ++s.marker_macro_introduced_count;
                        else if (val == 2)
                            ++s.marker_bool_literal_count;
                    }
                }
            }
            // else: contended — the caller already holds unique_lock
            // (e.g. set-code, restore-ast, mutate:*). Skip marker read.
        }
        // Populate per-function metrics from the JIT cache.
        // Issue #968: fill total_calls / deopt / hit / miss / hit_rate /
        // specialized_for from ShapeProfiler + global specialization counters
        // (previously left at zero → --metrics / evo-explain always zeros).
        {
            std::shared_lock cache_read(jit_cache_mtx_);
            const auto hits = metrics_.specialization_hits.load(std::memory_order_relaxed);
            const auto misses = metrics_.specialization_misses.load(std::memory_order_relaxed);
            for (auto& [name, entry] : jit_cache_) {
                FnMetrics fm;
                fm.name = name;
                fm.has_shape_map = entry.has_shape_map;
                auto fn_key = shape::make_fn_key(session_id_, name);
                auto sm = shape_profiler_.metrics(fn_key);
                fm.total_calls = sm.total_calls;
                fm.deopt_count = sm.deopt_count;
                // Distribute global specialization hit/miss proportionally is
                // hard without per-fn counters; surface profile stability as
                // hit when shape-stable, miss otherwise when profiled.
                if (sm.total_calls > 0) {
                    if (shape_profiler_.is_stable(fn_key)) {
                        fm.hit_count =
                            sm.total_calls > sm.deopt_count ? sm.total_calls - sm.deopt_count : 0;
                        fm.miss_count = sm.deopt_count;
                    } else {
                        fm.hit_count = 0;
                        fm.miss_count = sm.total_calls;
                    }
                    const auto denom = fm.hit_count + fm.miss_count;
                    fm.hit_rate =
                        denom > 0 ? static_cast<double>(fm.hit_count) / static_cast<double>(denom)
                                  : 0.0;
                    fm.specialized_for =
                        static_cast<std::uint32_t>(shape_profiler_.dominant_shape(fn_key));
                } else if (entry.has_shape_map) {
                    // Compiled with shape map but not yet profiled: still mark
                    // specialized path available using global hit/miss ratio.
                    fm.hit_count = hits;
                    fm.miss_count = misses;
                    const auto denom = hits + misses;
                    fm.hit_rate =
                        denom > 0 ? static_cast<double>(hits) / static_cast<double>(denom) : 0.0;
                }
                s.functions.push_back(std::move(fm));
            }
        }
        return s;
    }
    void set_workspace_tree(void* wt) { evaluator_.set_workspace_tree(wt); }

    // Return current number of cached define functions
    std::size_t cached_function_count() const { return ir_cache_.size(); }

    // Inspect support: expose last parsed AST (for --inspect typecheck etc.)
    const aura::ast::FlatAST& last_flat() const { return *current_ast_; }
    const aura::ast::StringPool& last_pool() const { return *current_pool_; }

    // Expose evaluator env for --inspect evaluator
    std::string inspect_env() const {
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
        auto* pool_ptr = arena_.create<aura::ast::StringPool>(alloc);
        auto* flat_ptr = arena_.create<aura::ast::FlatAST>(alloc);
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
        current_ast_ = arena_.create<aura::ast::FlatAST>(alloc);
        current_pool_ = arena_.create<aura::ast::StringPool>(alloc);
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
                                                      const char* source) {
        (void)source;              // was: tag for the debug log (removed in #487 fix)
        (void)mutation_id_for_log; // ditto
        if (incremental_typecheck_mode_ != IncrementalTypecheckMode::Eager)
            return 0;
        aura::compiler::TypeChecker tc(type_registry_);
        aura::diag::DiagnosticCollector diag;
        tc.set_strict(strict_mode_);
        // Match the existing incremental_infer epoch gate
        // (Issue #168): use the current mutation epoch so
        // cache entries captured under a prior epoch are
        // correctly rejected as stale.
        tc.set_cache_epoch(mutation_epoch_.load(std::memory_order_relaxed));
        // Plumb the shared metrics so the per-call engine
        // stats (cache_hits / cache_misses / stale_cache)
        // accumulate into the lifetime totals (Issue #258 /
        // #411 wiring).
        tc.set_metrics(&metrics_);
        // Issue #518: wire Evaluator narrowing counters to the
        // actual re-narrow path in infer_flat_partial.
        tc.set_on_narrowing_refresh([this]() { evaluator_.bump_narrowing_refresh_count(); });
        tc.set_on_selective_recheck([this]() { evaluator_.bump_selective_recheck_count(); });
        tc.set_on_touched_roots_snapshot(
            [this](std::size_t n) { evaluator_.set_touched_roots_size(n); });
        tc.set_on_cross_delta_conflict(
            [this]() { evaluator_.bump_cross_delta_conflicts_caught(); });
        // Issue #411 fu1 follow-up #3: plumb the
        // per-DefUseIndex tracker (same as incremental_infer
        // above). When the tracker is non-null AND has
        // at least one index, the O(uses) per-DefUseIndex
        // path fires.
        auto* tracker_ptr2 = per_defuse_index_tracker_.index_count() > 0
                                 ? static_cast<void*>(&per_defuse_index_tracker_)
                                 : nullptr;
        auto n = tc.infer_flat_partial(flat, pool, rec, diag, tracker_ptr2);
        metrics_.typecheck_cache_hits_total.fetch_add(tc.stats().cache_hits,
                                                      std::memory_order_relaxed);
        metrics_.typecheck_cache_misses_total.fetch_add(tc.stats().cache_misses,
                                                        std::memory_order_relaxed);
        metrics_.typecheck_stale_cache_total.fetch_add(tc.stats().stale_cache,
                                                       std::memory_order_relaxed);
        metrics_.typecheck_gen_saved_total.fetch_add(tc.stats().gen_saved,
                                                     std::memory_order_relaxed);
        // Issue #411 follow-up #1: per-symbol / ancestor
        // path tracking. The auto-invoke path also routes
        // through infer_flat_partial, so it benefits from
        // the per-symbol optimization. Mirror the per-call
        // stats into the lifetime counters (same as
        // incremental_infer above).
        metrics_.per_symbol_reinfer_used_total.fetch_add(tc.stats().per_symbol_used_total,
                                                         std::memory_order_relaxed);
        metrics_.per_symbol_reinfer_visited_total.fetch_add(tc.stats().per_symbol_visited_total,
                                                            std::memory_order_relaxed);
        metrics_.ancestor_reinfer_used_total.fetch_add(tc.stats().ancestor_used_total,
                                                       std::memory_order_relaxed);
        metrics_.ancestor_reinfer_visited_total.fetch_add(tc.stats().ancestor_visited_total,
                                                          std::memory_order_relaxed);
        // Issue #411 fu1 follow-up #3: per-DefUseIndex
        // path tracking. Mirror the per-call stats into
        // the lifetime CompilerMetrics counters.
        metrics_.per_defuse_index_used_total.fetch_add(tc.stats().per_defuse_index_used_total,
                                                       std::memory_order_relaxed);
        metrics_.per_defuse_index_walk_fallback_total.fetch_add(
            tc.stats().per_defuse_index_walk_fallback_total, std::memory_order_relaxed);
        // Issue #411 fu1 fu4: per-DefUseIndex visited
        // count (the O(uses) signal).
        metrics_.per_defuse_index_visited_total.fetch_add(tc.stats().per_defuse_index_visited_total,
                                                          std::memory_order_relaxed);
        // Issue #386: narrowing observability. Mirror
        // the per-call stats into the lifetime
        // CompilerMetrics counters.
        metrics_.narrowing_applied_total.fetch_add(tc.stats().narrowing_applied,
                                                   std::memory_order_relaxed);
        metrics_.narrowing_skipped_total.fetch_add(tc.stats().narrowing_skipped,
                                                   std::memory_order_relaxed);
        metrics_.narrowing_reanalyzed_total.fetch_add(tc.stats().narrowing_reanalyzed,
                                                      std::memory_order_relaxed);
        // Issue #338: and/or precision.
        metrics_.and_or_meet_uses_total.fetch_add(tc.stats().and_or_meet_uses,
                                                  std::memory_order_relaxed);
        metrics_.and_or_join_uses_total.fetch_add(tc.stats().and_or_join_uses,
                                                  std::memory_order_relaxed);
        // Issue #434: per-node occurrence dirty recovery.
        metrics_.narrowing_dirty_recovery_total.fetch_add(tc.stats().narrowing_dirty_recovery,
                                                          std::memory_order_relaxed);
        // Issue #390: schema cache observability.
        metrics_.schema_cache_lookups_total.fetch_add(tc.stats().schema_cache_lookups,
                                                      std::memory_order_relaxed);
        metrics_.schema_cache_hits_total.fetch_add(tc.stats().schema_cache_hits,
                                                   std::memory_order_relaxed);
        // Issue #387: Type Dependency Graph observability.
        // Mirror the per-call TypeChecker counters into the
        // lifetime CompilerMetrics atomics.
        metrics_.type_dep_graph_lookups.fetch_add(tc.type_dep_graph_lookups(),
                                                  std::memory_order_relaxed);
        metrics_.type_dep_graph_hits.fetch_add(tc.type_dep_graph_hits(), std::memory_order_relaxed);
        metrics_.type_dep_graph_size.store(
            std::max(metrics_.type_dep_graph_size.load(std::memory_order_relaxed),
                     static_cast<std::uint64_t>(tc.type_dep_graph_size())),
            std::memory_order_relaxed);
        metrics_.incremental_typecheck_auto_invocations_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
        metrics_.incremental_typecheck_re_inferred_total.fetch_add(n, std::memory_order_relaxed);
        // Note: a previous version of this method printed a
        // debug line to stderr ("IncrementalTypecheck: ...")
        // after the re-inference. That output was being
        // captured by the EDSL test framework's stream
        // redirect, polluting the result strings of tests
        // like edsl-ir-cache:cascade-after-mutate (expected
        // "#t" got the debug line + "#t"). The line was
        // left over from the #411 ship and serves no
        // production purpose — the counters above
        // (incremental_typecheck_auto_invocations_total +
        // incremental_typecheck_re_inferred_total) are the
        // observability surface. Removed.
        return n;
    }

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
            }
            restore_provenance();
        }

        // Disallow copy
        TypedTransactionGuard(const TypedTransactionGuard&) = delete;
        TypedTransactionGuard& operator=(const TypedTransactionGuard&) = delete;
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
                aura::compiler::PostMutationInvariantVisitor invariant_visitor(
                    *current_pool_, type_registry_, &metrics_);
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
    [[nodiscard]] MutationResult typed_mutate(std::string_view sexpr) {
        if (!current_ast_ || !current_pool_) {
            return {0, false, "no AST loaded"};
        }
        // Issue #1523: acquire mutate_mtx_ FIRST before MutationBoundaryGuard
        // (workspace) so mark_define_dirty can take dep_graph without inversion:
        //   mutate → workspace → (optional env) → dep_graph
        using aura::compiler::lock_order::Level;
        using aura::compiler::lock_order::OrderedUniqueLock;
        OrderedUniqueLock<std::shared_mutex> mutate_guard =
            OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mutate_mtx_, Level::Mutate);
        sync_lock_order_metrics_();

        // Wrap in a transaction: if evaluation fails (parse or runtime),
        // all mutations performed by the sexpr are automatically rolled back.
        MutationTransaction tx(*current_ast_);
        auto pr = aura::parser::parse_to_flat(sexpr, *current_ast_, *current_pool_);
        if (!pr.success || pr.root == aura::ast::NULL_NODE) {
            auto diag = parse_error_diag(pr);
            return {0, false, diag.format()};
        }

        // Issue #184 Cycle 1: wrap the mutation effect in a
        // MutationBoundaryGuard (RAII). The guard acquires the
        // exclusive workspace_mtx_ write lock + bumps
        // defuse_version_ (acquire load + release store) on
        // construction, pops the checkpoint + releases the lock
        // on destruction. This composes with the existing
        // MutationTransaction (the guard handles lock/version;
        // the transaction handles AST-level rollback on failure).
        //
        // success_flag is set by this scope and read by the
        // guard's destructor. We default to true; the only
        // path that sets it false is below (when the type error
        // check fails — even though the tx auto-rolls back,
        // marking the guard as failure makes the intent
        // explicit and prepares for the future rollback path).
        bool boundary_success = true;
        // Issue #1547: typed try_acquire (check_mutation_quota → ResourceQuotaExceeded).
        auto guard_r = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
            evaluator_, /*pending_count=*/1, &boundary_success);
        if (!guard_r) {
            return {0, false, guard_r.error().message};
        }
        auto guard = std::move(*guard_r);

        auto result =
            evaluator_.eval_flat(*current_ast_, *current_pool_, pr.root, evaluator_.top_env());
        if (!result) {
            // Issue #213 follow-up: the eval failed (e.g. parse
            // error in the mutate code itself). Mark the
            // boundary as failed so the guard's destructor
            // triggers the rollback path (Cycle 1: rolls back
            // the MutationRecord log; Cycle 2: bumps version +
            // invalidates defuse_index_). The sub-primitives
            // that ran during eval_flat may have appended
            // records that we want to undo.
            boundary_success = false;
            return {0, false, result.error().message};
        }
        auto& val = *result;

        // Check if the mutation's auto-typecheck reported errors (P2 #34)
        if (evaluator_.has_type_error()) {
            auto err = evaluator_.last_mutate_error();
            evaluator_.clear_last_mutate_error();
            // Transaction auto-rollbacks on destructor
            // Issue #184: signal the boundary guard that this is a
            // rollback intent (today both paths commit; explicit
            // flag sets up the future rollback implementation).
            boundary_success = false;
            return {0, false, err};
        }

        // Mutation primitives return make_bool(true) on success or an int mutation ID.
        // Accept both: bool true = success, positive int = success with mutation ID.
        bool is_success = false;
        std::uint64_t mid = 0;
        if (aura::compiler::types::is_int(val)) {
            mid = static_cast<std::uint64_t>(aura::compiler::types::as_int(val));
            is_success = mid > 0;
        } else if (aura::compiler::types::is_bool(val)) {
            is_success = aura::compiler::types::as_bool(val);
            mid = 0; // no explicit mutation ID for bool returns
        }

        if (is_success) {
            // Phase 4 (#53): invalidate shape profiles on mutation
            // to trigger deoptimization on next eval.
            // Conservative: reset all profiles since we don't know which
            // functions were affected.
            // Issue #570: per-profile version++ + deopt hook
            // (preserves history keys vs full reset).
            shape_profiler_.invalidate_all();

            tx.commit();

            // Issue #1407 R1 / #1524: dual-epoch + bridge stamp + JIT
            // batch_deopt via the unified helper. NEVER bare
            // bump_bridge_epoch() here — that breaks the #1475 dual-epoch
            // contract (bridge bumped but AOT table / stamps lag).
            // Empty name stamps all known bridges (affected set may be
            // unknown for generic mutate:* forms; mark_define_dirty also
            // routes through the same helper for named defines).
            //
            // INVARIANT (#1524): all typed_mutate / typed_mutate_atomic
            // invalidation goes through atomic_bump_epochs_and_stamp_bridge.
            atomic_bump_epochs_and_stamp_bridge(/*name=*/{});
            metrics_.typed_mutate_epoch_bumps.fetch_add(1, std::memory_order_relaxed);
            metrics_.typed_mutate_atomic_invalidations_total.fetch_add(1,
                                                                       std::memory_order_relaxed);

            // Issue #147: post-mutation invariant check. Runs only
            // if invariant_check_mode_ is not Disabled. Iterates
            // every MutationRecord added during this transaction
            // and runs post_mutation_invariant_check on each.
            //
            // Important: mutation primitives add their records to
            // workspace_flat_ (not current_ast_), and the workspace
            // pointer can be replaced by lazy COW during eval. We
            // therefore capture a fresh pointer to the workspace
            // AFTER the eval completes (and AFTER tx.commit, so
            // the post-commit state is final). The snapshot id is
            // the workspace's mutation_count() snapshot we took
            // before the eval started.
            //
            // Mode behavior:
            //   Disabled     — skip entirely, status stays NotChecked.
            //   WarningsOnly — surface diagnostics, do NOT block
            //                  (success stays true).
            //   Strict       — any per-record Warnings status
            //                  promotes to MutationResult success=
            //                  false with the first diagnostic's
            //                  message promoted to .error.
            MutationResult res;
            res.mutation_id = mid;
            res.success = true;

            if (invariant_check_mode_ == InvariantCheckMode::Disabled) {
                res.invariant_status = aura::ast::InvariantStatus::NotChecked;
                // Issue #1383: throttled warn when Disabled is
                // set on a workspace with prior typed mutations.
                // Throttled to ONCE per mode flip (operator can
                // see the silent-bypass risk without per-mutation
                // spam). WarningsOnly/Strict modes never warn
                // because they actually run the check.
                if (auto* ws = evaluator_.workspace_flat();
                    ws && ws->mutation_count() > 0 &&
                    mode_flip_count_ != last_disabled_warn_flip_) {
                    mode_warnings_.push_back(
                        "invariant checks disabled on workspace with " +
                        std::to_string(ws->mutation_count()) +
                        " typed mutations \u2014 linear ownership narrowing may be stale");
                    last_disabled_warn_flip_ = mode_flip_count_;
                }
                return res;
            }

            // Capture the post-eval workspace. Note: lazy COW may
            // have replaced workspace_flat_ with a new pointer; we
            // look it up via the evaluator.
            auto* ws_flat = evaluator_.workspace_flat();
            if (!ws_flat) {
                // No workspace to check against — treat as Ok.
                res.invariant_status = aura::ast::InvariantStatus::Ok;
                return res;
            }

            // Issue #152 Phase 2: Mutation Impact Analysis. The
            // typed_mutate call may have changed a function's
            // value; any Define whose subtree references that
            // function (directly) is potentially affected. The
            // affected set is computed via the
            // defines_referencing_sym helper from #150 Phase 3
            // (commit 02a1c75). The affected set is logged
            // (observability hook for the agent orchestration
            // layer — "what would this mutation affect?") and
            // could be used by the AOT cache invalidation
            // (cross-link to #151 Phase 3) to invalidate only
            // the affected entries, not the global cache.
            //
            // Today the affected set is logged but not
            // consumed for invalidation (the AOT cache
            // invalidation path is a follow-up). This is the
            // read-side: the affected Define node IDs are
            // computed and emitted to stderr for observability.
            if (mid > 0) {
                const auto& mut_log = ws_flat->all_mutations();
                for (auto it = mut_log.rbegin(); it != mut_log.rend(); ++it) {
                    if (it->mutation_id != mid)
                        continue;
                    aura::ast::NodeId target = it->target_node;
                    if (target == aura::ast::NULL_NODE)
                        target = it->parent_id;
                    if (target != aura::ast::NULL_NODE && target < ws_flat->size()) {
                        auto v = ws_flat->get(target);
                        if (v.tag == aura::ast::NodeTag::Define &&
                            v.sym_id != aura::ast::INVALID_SYM) {
                            auto affected = ws_flat->defines_referencing_sym(v.sym_id);
                            std::println(
                                std::cerr,
                                "MutationImpactAnalysis: mutation {} affects {} function(s)", mid,
                                affected.size());
                        }
                    }
                    break;
                }
            }
            // The snapshot id is the count of mutations on the
            // workspace BEFORE the typed_mutate's eval started.
            // We don't have a direct way to capture that here
            // (the transaction is on current_ast_, not the
            // workspace), so we use a heuristic: the typed_mutate
            // either adds zero or one mutation per call (most
            // primitives append one record). We treat the latest
            // entry whose status is still NotChecked as a
            // candidate for this transaction. This is a soft
            // contract — it works because earlier entries already
            // got their status set by prior typed_mutate calls.
            // Issue #274: fold invariant checks via MutationVisitor pipeline.
            // Issue #1415: when suppress_invariant_check_ is true (set by
            // typed_mutate_atomic during a batch), skip the per-mutation
            // check here. A single combined check on the dirty union
            // runs at typed_mutate_atomic's commit, giving the same
            // final status with 1/N the work for an N-mutation batch.
            if (!suppress_invariant_check_) {
                // Issue #147 / #1458: type-checker half (OwnershipEnv).
                aura::compiler::PostMutationInvariantVisitor invariant_visitor(
                    *current_pool_, type_registry_, &metrics_);
                aura::ast::run_mutation_pipeline(*ws_flat, invariant_visitor);
                invariant_visitor.apply_status_updates(*ws_flat);
                res.invariant_status = invariant_visitor.worst_status();
                res.invariant_diagnostics =
                    std::vector<aura::compiler::OwnershipNote>(invariant_visitor.notes());
                // Issue #1538 / #1478: runtime half (EnvFrame linear enforce).
                // Always paired with the invariant visitor so both halves
                // of the post-mutation linear pipeline run together.
                apply_linear_post_mutate_pipeline_(res, mid);
            }
            auto& log = ws_flat->all_mutations();

            // Issue #165 Phase 1B: post-mutation macro re-expansion.
            // For every MutationRecord we just processed, walk
            // its affected subtree and re-expand any macro call
            // sites. This fixes the bug where EDSL mutations
            // (mutate:rebind, mutate:set-body) leave stale macro
            // expansions — the macro's gensym'd bindings may not
            // be re-generated, and the call site may pick up
            // caller's bindings that should have been hygiene-
            // isolated. Mirrors the post_mutation_invariant_check
            // pattern above (Issue #147).
            //
            // The function is safe on any mutation record — it
            // bails on malformed input (NULL_NODE, out-of-range,
            // empty macros_ registry). The re-expanded count is
            // logged for observability.
            if (mid > 0) {
                for (auto& rec : log) {
                    if (rec.mutation_id != mid)
                        continue;
                    auto re_expanded =
                        evaluator_.post_mutation_macro_reexpand(*ws_flat, *current_pool_, rec);
                    if (re_expanded > 0) {
                        std::println(std::cerr,
                                     "MacroReexpand: mutation {} re-expanded {} call site(s)", mid,
                                     re_expanded);
                    }
                    break; // one record per typed_mutate
                }
            }

            // Issue #411: post-mutation auto-incremental typecheck.
            // After a successful mutation, automatically invoke
            // TypeChecker::infer_flat_partial on the affected
            // subtree (the same path the manual
            // (typecheck-incremental) Aura primitive uses). The
            // result populates flat.type_id_ for the affected
            // nodes, so the next (query:type <name>) /
            // (get-inferred-type <node-id>) returns up-to-date
            // results with no manual (typecheck-incremental) call.
            //
            // Mode handling:
            //   Eager     (default) — invoke on every successful
            //                          typed_mutate. Bumps
            //                          auto_invocations_total +
            //                          re_inferred_total.
            //   Lazy                 — skip the auto-invoke. The
            //                          caller can call
            //                          (typecheck-incremental)
            //                          later in the batch.
            //   Disabled             — skip entirely. Equivalent
            //                          to pre-#411 behavior. The
            //                          full (typecheck-current)
            //                          primitive still works.
            //
            // We use *ws_flat / *ws_pool (not *current_ast_ /
            // *current_pool_) because lazy COW may have replaced
            // the workspace flat during the mutation eval — the
            // mutation record's target_node is in the workspace's
            // NodeId space, not the typed_mutate AST's. The
            // (typecheck-incremental) primitive already follows
            // this same pattern (see evaluator_primitives_eval.cpp
            // ~L916).
            if (incremental_typecheck_mode_ == IncrementalTypecheckMode::Eager && mid > 0) {
                // Find the most recent record for this mutation
                // (mirrors the MacroReexpand pattern above). If
                // the workspace has no records (e.g. mid > 0
                // was set but no log entry was appended — a
                // degenerate case where the mutation returned a
                // non-zero success code without touching the
                // log), skip the auto-invoke (no rec to re-
                // infer against).
                bool invoked = false;
                for (auto& rec : log) {
                    if (rec.mutation_id != mid)
                        continue;
                    // Issue #612: re-sync ADT ctor registry from
                    // DefineType nodes before partial re-infer so
                    // match exhaustiveness sees the post-mutate ctor
                    // list (not a stale pre-mutation snapshot).
                    if (ws_flat->root != aura::ast::NULL_NODE)
                        register_adt_from_define_types(*ws_flat, *current_pool_, ws_flat->root);
                    auto_invoke_incremental_typecheck_for(*ws_flat, *current_pool_, rec, mid,
                                                          "typed_mutate");
                    invoked = true;
                    break; // one record per typed_mutate
                }
                (void)invoked; // invoked is informational; no
                               // failure path needed when the
                               // log is empty (the macro re-
                               // expand above would have caught
                               // the same condition).
            }

            if (invariant_check_mode_ == InvariantCheckMode::Strict &&
                res.invariant_status == aura::ast::InvariantStatus::Warnings) {
                res.success = false;
                if (!res.invariant_diagnostics.empty()) {
                    res.error = res.invariant_diagnostics.front().message;
                } else {
                    res.error = "post-mutation invariant check reported violations";
                }
            }
            return res;
        }
        // If mutation returned 0/false, it indicates failure — transaction auto-rollbacks.
        // Issue #213 follow-up: mark the boundary as failed so
        // the guard's destructor triggers the rollback path.
        // The sub-primitives that ran during eval_flat may have
        // appended records to the log; we want to undo them.
        boundary_success = false;
        return {0, false, "mutation returned zero (failed)"};
    }

    // Query mutation log for a specific node (or all nodes if NULL_NODE).
    //
    // Issue #147 follow-up: mutation primitives (mutate:rebind,
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
        struct UnlockGuard {
            Evaluator& e;
            ~UnlockGuard() { e.unlock_workspace_shared(); }
        } guard{eval_mut};
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
    // The workspace is where mutation primitives (mutate:rebind,
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

private:
    // Issue #1457: surface TypePropagationPass work into metrics.
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
                const auto bp = (eliminated * 10000ull) / denom;
                metrics_.cast_elision_win_rate_bp.store(bp, std::memory_order_relaxed);
            }
            if (eliminated > 0) {
                metrics_.dce_cast_elision_total.fetch_add(eliminated, std::memory_order_relaxed);
            }
        }
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
        const std::uint64_t pipeline_epoch = mutation_epoch_.load(std::memory_order_relaxed);
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
                std::string ts(t);
                std::size_t consumed = 0;
                long long v = std::stoll(ts, &consumed);
                if (consumed == ts.size())
                    return make_int(v);
            } catch (...) {
                // Fall through to slow path
            }
        }
        // Float?
        if (t.find('.') != std::string_view::npos) {
            try {
                std::string ts(t);
                std::size_t consumed = 0;
                double v = std::stod(ts, &consumed);
                if (consumed == ts.size())
                    return make_float(v);
            } catch (...) {
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
                return types::make_int(v.int_value);
            case aura::ast::NodeTag::LiteralFloat:
                return types::make_float(v.float_value);
            case aura::ast::NodeTag::LiteralString: {
                // Push string to heap at compile time
                auto str = std::string(pool.resolve(v.sym_id));
                auto sid = evaluator_.primitives().string_heap().size();
                evaluator_.primitives().string_heap().push_back(str);
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
    aura::ast::NodeId pre_exec_requires(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                        aura::ast::NodeId root) {
        if (root >= flat.size())
            return root;
        auto v = flat.get(root);

        // Top-level standalone require: execute, no body left.
        // The caller should detect NULL_NODE and skip the rest
        // of the evaluation (or treat it as a no-op).
        if (is_require_call(flat, pool, root)) {
            (void)evaluator_.eval_flat(flat, pool, root, evaluator_.top_env());
            return aura::ast::NULL_NODE;
        }

        // (begin ...) — scan children, execute requires, rebuild begin
        // with only non-require children. If all children were requires,
        // the stripped begin is empty; we return NULL_NODE.
        if (v.tag == aura::ast::NodeTag::Begin) {
            std::vector<aura::ast::NodeId> non_require_children;
            for (auto c : v.children) {
                if (is_require_call(flat, pool, c)) {
                    (void)evaluator_.eval_flat(flat, pool, c, evaluator_.top_env());
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
    std::unordered_map<std::string, std::vector<aura::ir::IRFunction>> ir_cache_;

    // Issue #272 Cycle 4: pre-bind IR snapshots for disk cache serialization.
    // Captured in cache_define immediately after ir_cache_ is populated,
    // before IRInterpreter env binding can alias/mutate live IR state.
    std::unordered_map<std::string, std::vector<aura::ir::IRFunction>> ir_disk_snapshots_;

    // Bridge data cached alongside ir_cache_ (same keys, parallel indices).
    std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>> ir_cache_bridge_;
    // Issue #741: func indices from the latest impact_scope computation,
    // wired into lowering hooks to limit bridge shared_ptr copy scope.
    std::unordered_set<std::size_t> pending_impact_func_indices_;
    // String pool cached alongside ir_cache_ (same keys).
    std::unordered_map<std::string, std::vector<std::string>> ir_cache_strings_;

    // Issue #272: persistent IR runtimes for function-define env bindings.
    std::unordered_map<std::string, std::unique_ptr<IRDefineEnvBinding>> ir_define_env_bindings_;
    // Issue #272 Cycle 3: top-level value defines bound via IR (name → cell index).
    std::unordered_map<std::string, std::size_t> ir_value_cell_bindings_;
    std::unordered_map<aura::compiler::ClosureId, std::string> ir_define_closure_owner_;

    // Source code for each cached function, used for re-lowering on dependency changes.
    std::unordered_map<std::string, std::string> function_sources_;


    // Dependency tracking for incremental compilation.
    // DepEntry.calls = functions this one calls; DepEntry.called_by = functions that call this one.
    // When a function is redefined, all transitively dependent functions are invalidated.
    struct DepEntry {
        std::vector<std::string> calls;
        std::vector<std::string> called_by;
    };
    std::unordered_map<std::string, DepEntry> dep_graph_;

    void record_dependency(const std::string& caller, const std::string& callee) {
        // Issue #687: idempotent — skip if (caller, callee) is
        // already recorded. Without this, dep edges double
        // (32 vs 16 expected) when both
        // populate_dep_graph_from_workspace (lightweight AST
        // walker) AND populate_ir_cache_v2_from_workspace
        // (heavy via cache_define) fire on the same set-code
        // — both find the same Variable refs and call this
        // function. The walker uses a per-name `seen` set,
        // but cache_define's cache_hits vector does not,
        // leading to 2× edge counts.
        //
        // Issue #1376: the #687 find+push TOCTOU is only safe under
        // an exclusive lock. Concurrent populate / cache_define /
        // invalidate_function writers race on dep_graph_ without it.
        metrics_.dep_graph_record_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1523: dep_graph is LAST — safe alone or under mutate.
        lock_order::OrderedUniqueLock<std::shared_mutex> write(dep_graph_mtx_,
                                                               lock_order::Level::DepGraph);
        sync_lock_order_metrics_();
        auto& caller_entry = dep_graph_[caller];
        if (std::find(caller_entry.calls.begin(), caller_entry.calls.end(), callee) !=
            caller_entry.calls.end()) {
            metrics_.dep_graph_record_dedup_total.fetch_add(1, std::memory_order_relaxed);
            return; // already recorded — skip duplicate
        }
        caller_entry.calls.push_back(callee);
        dep_graph_[callee].called_by.push_back(caller);
        metrics_.dep_graph_record_inserted.fetch_add(1, std::memory_order_relaxed);
    }

    // Scan FlatAST from the given node for Variable nodes that reference cached functions.
    // Records these as dependencies of `def_name`.
    void track_define_dependencies(const std::string& def_name, aura::ast::FlatAST& flat,
                                   aura::ast::StringPool& pool) {
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
    // Issue #1523 lock order for invalidate_function:
    //   mutate_mtx_ (unique, FIRST)
    //     → dep_graph_mtx_ (unique, for BFS/erase window)
    //     → jit_cache_mtx_ (not in #1388 quartet; after mutate)
    //   Does NOT take workspace_mtx_ / env_frames_mtx_ directly.
    //   invalidate_bridge_for / batch_deopt run under mutate only.
    void invalidate_function(const std::string& name) {
        // Issue #59 Iter 3 + #1378: acquire the Mutation Lock FIRST so
        // epoch bump, block-dirty, BFS, and cache/JIT teardown are
        // atomic w.r.t. concurrent invalidate_function / mutate.
        // A mutate:* that triggers this must drain any in-flight compile
        // before erasing the cache entry, otherwise another fiber could
        // observe a half-erased state.
        //
        // Issue #166 historically bumped mutation_epoch_ BEFORE the lock
        // for "early visibility". That opened a multi-fiber re-entrancy
        // window (Issue #1378): another invalidate could interleave after
        // epoch publish but before dep_graph_ cleanup, producing
        // non-deterministic cascade topology. Epoch still uses
        // memory_order_release; readers load acquire (L739/L966/L1013).
        using aura::compiler::lock_order::Level;
        using aura::compiler::lock_order::OrderedUniqueLock;
        OrderedUniqueLock<std::shared_mutex> mutate_lock(mutate_mtx_, Level::Mutate);
        sync_lock_order_metrics_();

        // Issue #1545 / #1494 / #1606: pre-cascade walk_active_closures
        // via scan_live_closures_for_linear_captures — mark invalid
        // (bridge_epoch=0) so apply takes safe_fallback before IR/JIT
        // teardown races with linear state. Then EnvFrame enforce so
        // Moved frames bump linear_ownership_violation_prevented.
        (void)evaluator_.scan_live_closures_for_linear_captures(/*mark_invalid=*/true);
        (void)evaluator_.linear_post_mutate_enforce_all();

        // Issue #1496 / #1476: SINGLE dual-epoch + bridge stamp + JIT
        // soft-deopt protocol — same helper as mark_define_dirty.
        // Readers (apply_closure / aura_closure_call) that acquire-load
        // either domain see both advanced before hard JIT erase /
        // dep_graph teardown below. Replaces the historical hand-rolled
        // bump_bridge_epoch + defuse + aot sequence that could desync
        // with the soft path.
        atomic_bump_epochs_and_stamp_bridge(name);
        // Issue #531: bump closure_stale_refresh_count_ on
        // every invalidate_function — measures the closure
        // refresh frequency post-mutate. Stats-only
        // (relaxed-ordering); the follow-up wires the actual
        // IRClosure::invalidate_if_stale walk + the
        // bridge_epoch_hit_count_ bump in apply_closure.
        metrics_.closure_stale_refresh_count_.fetch_add(1, std::memory_order_relaxed);
        // Issue #401: lifetime counter for invalidate_function entry.
        // Bumped here (before the dep_graph_ walk) so the count is
        // observable even if the walk short-circuits on an empty graph.
        metrics_.invalidate_function_calls.fetch_add(1, std::memory_order_relaxed);
        // Issue #610: linear ownership JIT/closure refresh after
        // invalidate — pairs with closure_stale_refresh for the
        // post-mutate linear runtime contract path.
        metrics_.linear_deopt_on_invalidate_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #598: post-mutate runtime enforcement hook on
        // invalidate_function — pairs with linear_deopt_on_invalidate
        // so GuardShape/linear state re-validates after re-lower.
        metrics_.linear_post_mutate_enforcements_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #638: invalidate ShapeProfiler profiles so
        // GuardShape + linear_ownership_state re-specialize
        // after post-mutate shape/ownership change.
        invalidate_shape(name);

        // Issue #1286: per-block dirty on ir_cache_v2_ for the mutated
        // function (and cascade dependents below). Enables partial re-lower
        // consumers via is_block_dirty without full-module wipe.
        if (auto vit = ir_cache_v2_.find(name); vit != ir_cache_v2_.end()) {
            vit->second.mark_all_blocks_dirty();
            metrics_.invalidate_per_block_dirty_total.fetch_add(1, std::memory_order_relaxed);
        }

        // Issue #401: real BFS over called_by chain.
        //
        // The previous implementation used std::vector + push_back/pop_back,
        // which is stack/DFS behaviour (LIFO). The misleading comment claimed
        // "natural BFS order" but the iteration order was depth-first, which
        // made the re-lower order depend on the hash-map iteration order of
        // std::unordered_map<string, DepEntry>::called_by. For AI multi-round
        // mutate:rebind flows, that meant dep_graph_ calls/called_by edges
        // recorded by record_dependency during re-lower could land in
        // different orders across runs, producing non-deterministic dep-graph
        // shape.
        //
        // Fix: use std::deque + push_back/pop_front for FIFO BFS, then sort
        // the dependents vector lexicographically before re-lower. Sorting
        // gives a stable iteration order regardless of the underlying
        // unordered_map bucket layout.
        //
        // Issue #1376: exclusive dep_graph_mtx_ for the BFS + erase window
        // (lock order: mutate_mtx_ already held, then dep_graph_mtx_).
        // Snapshot dependents under the lock so re-lower below can proceed
        // without holding the graph mutex across IR work.
        std::vector<std::string> dependents;
        {
            // Issue #1523: mutate already held → dep_graph LAST is legal.
            OrderedUniqueLock<std::shared_mutex> dep_write(dep_graph_mtx_, Level::DepGraph);
            sync_lock_order_metrics_();
            std::deque<std::string> bfs;
            std::unordered_set<std::string> visited;

            bfs.push_back(name);
            visited.insert(name);

            while (!bfs.empty()) {
                auto current = bfs.front();
                bfs.pop_front();

                auto it = dep_graph_.find(current);
                if (it == dep_graph_.end())
                    continue;

                for (auto& dependent : it->second.called_by) {
                    if (!visited.insert(dependent).second)
                        continue;
                    dependents.push_back(dependent);
                    bfs.push_back(dependent);
                }
            }

            // Issue #401: stable re-lower order. Sort dependents lexicographically
            // so the iteration below doesn't depend on the unordered_map hash
            // layout. This is the determinism contract for the follow-up
            // record_dependency edge-creation order.
            std::sort(dependents.begin(), dependents.end());

            // Issue #1496: cascade depth for hard invalidate (root + dependents).
            // Pairs with mark_define_dirty cascade metrics so soft/hard share
            // the same observability surface.
            const auto inv_depth =
                static_cast<std::uint64_t>(1 + dependents.size()); // root + fan-out
            metrics_.invalidate_cascade_depth_total.fetch_add(inv_depth, std::memory_order_relaxed);
            auto inv_expected =
                metrics_.invalidate_cascade_depth_max.load(std::memory_order_relaxed);
            while (inv_depth > inv_expected &&
                   !metrics_.invalidate_cascade_depth_max.compare_exchange_weak(inv_expected,
                                                                                inv_depth)) {
                // retry
            }

            // Clean up old dependency info for all affected functions
            // (the redefined function and all its transitives)
            for (auto& f : dependents) {
                auto fit = dep_graph_.find(f);
                if (fit != dep_graph_.end()) {
                    for (auto& callee : fit->second.calls) {
                        auto& cb = dep_graph_[callee].called_by;
                        cb.erase(std::remove(cb.begin(), cb.end(), f), cb.end());
                    }
                    dep_graph_.erase(f);
                }
            }
        }
        // Invalidate JIT cache for affected functions.
        // Issue #491 + #1378: erase jit_cache_ AND jit_.invalidate in
        // the SAME jit_cache_mtx_ scope so a concurrent shared reader
        // never observes "cache miss but AuraJIT still has native code".
        // Lock order: mutate_mtx_ (already held) → jit_cache_mtx_.
        {
            std::unique_lock cache_write(jit_cache_mtx_);
            jit_cache_.erase(name);
            metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
            for (auto& dep_name : dependents) {
                jit_cache_.erase(dep_name);
                metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
                // Issue #1286: cascade per-block dirty to dependents.
                if (auto dit = ir_cache_v2_.find(dep_name); dit != ir_cache_v2_.end()) {
                    dit->second.mark_all_blocks_dirty();
                    metrics_.invalidate_per_block_dirty_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
                }
            }
            // Drop stale AuraJIT modules inside the same lock as erase.
            jit_.invalidate(name.c_str());
            jit_.invalidate_prefix(name.c_str());
            metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
            evaluator_.bump_incremental_closure_jit_sync();
            for (auto& dep_name : dependents) {
                jit_.invalidate(dep_name.c_str());
                jit_.invalidate_prefix(dep_name.c_str());
                metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
                evaluator_.bump_incremental_closure_jit_sync();
            }
        }

        // Issue #225 cycle 3: invalidate bridge data for
        // the mutated function and all its dependents.
        // Bumps the bridge_epoch_ field so any closure
        // holding a reference will detect staleness and
        // re-parse from body_source on next use.
        // Issue #741: quote/lambda defines use impact_scope-
        // selective shared_ptr refresh instead of full bridge wipe.
        // Issue #682: GC root coordination before bindings cleared.
        const auto invalidate_bridge_with_impact = [&](const std::string& affected_name) {
            on_compiler_invalidate_gc_coordination(affected_name);
            auto src_it = function_sources_.find(affected_name);
            if (src_it == function_sources_.end()) {
                invalidate_bridge_for(affected_name);
                return;
            }
            auto alloc = arena_.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE || !flat_has_quote_or_lambda(flat)) {
                invalidate_bridge_for(affected_name);
                return;
            }
            flat.root = pr.root;
            std::unordered_map<aura::ast::NodeId, std::pair<std::size_t, std::uint32_t>>
                source_to_ir;
            std::unordered_map<std::string, std::size_t> ir_cache_index;
            auto scope = compute_impact_scope(flat, pr.root, source_to_ir, ir_cache_index);
            selective_invalidate_bridge_for_impact(affected_name, scope);
            metrics_.incremental_closure_quote_lambda_stale_prevented_total.fetch_add(
                1, std::memory_order_relaxed);
            evaluator_.bump_incremental_closure_quote_lambda_stale_prevented();
        };
        invalidate_bridge_with_impact(name);
        for (auto& dep_name : dependents)
            invalidate_bridge_with_impact(dep_name);

        // Issue #1536: bulk walk_active_closures after invalidate so any
        // remaining captured fns (not hard-erased) are deopt-on-next-apply.
        notify_walk_active_closures_(bridge_epoch());

        // Issue #601 / #1513: live IRClosure walk after invalidate.
        //
        // Pre-#1513 restamped bridge_epoch to current so apply passed
        // the stale check while flat*/pool* could still be dangling
        // (shared_ptr copies outlive bridge table reset). That was a
        // use-after-mutation hazard.
        //
        // #1513 closed-loop: expire views on live IRClosures, leave
        // bridge_epoch at the pre-bump value so is_bridge_stale fires,
        // and zero env_version so dual-check also trips. Next apply
        // takes safe fallback (tree-walker / re-parse) instead of
        // evaluating through expired flat*/pool*.
        //
        // Runs AFTER invalidate_bridge_for and BEFORE
        // clear_ir_define_env_binding (interpreter still reachable).
        {
            const std::uint64_t cur_epoch = bridge_epoch();
            const auto live_walk_one = [&]([[maybe_unused]] const std::string& affected_name) {
                for (auto& [bname, binding] : ir_define_env_bindings_) {
                    (void)bname;
                    if (!binding || !binding->interpreter)
                        continue;
                    binding->interpreter->walk_runtime_closures(
                        [&]([[maybe_unused]] std::uint64_t cid, IRClosure& cl) {
                            // Only touch closures that predate this invalidate.
                            if (cl.bridge_epoch == 0 || cl.bridge_epoch == cur_epoch)
                                return;
                            // Expire captured views (do NOT restamp epoch).
                            cl.flat.reset();
                            cl.pool.reset();
                            cl.body_id = aura::ast::NULL_NODE;
                            cl.env_version = 0; // dual-check: force re-stamp on rebuild
                            metrics_.jit_hotswap_forced_deopt_total.fetch_add(
                                1, std::memory_order_relaxed);
                            metrics_.ir_closure_invalidate_expired_total.fetch_add(
                                1, std::memory_order_relaxed);
                            metrics_.jit_hotswap_epoch_mismatch_prevented_total.fetch_add(
                                1, std::memory_order_relaxed);
                        });
                }
            };
            live_walk_one(name);
            for (auto& dep_name : dependents)
                live_walk_one(dep_name);
        }

        // Issue #741: re-stamp EnvFrame version_ for live tree-walker
        // closures captured from quote/lambda paths in impacted blocks.
        (void)evaluator_.resync_live_closure_env_versions_on_invalidate();

        // Issue #272 Cycle 2: drop stale IR define env bindings before re-bind.
        clear_ir_define_env_binding(name);
        for (auto& dep_name : dependents)
            clear_ir_define_env_binding(dep_name);

        // Clean up the original function's dep info
        {
            std::unique_lock dep_write(dep_graph_mtx_);
            auto it = dep_graph_.find(name);
            if (it != dep_graph_.end()) {
                for (auto& callee : it->second.calls) {
                    auto& cb = dep_graph_[callee].called_by;
                    cb.erase(std::remove(cb.begin(), cb.end(), name), cb.end());
                }
                dep_graph_.erase(name);
            }
        }

        // Re-lower each dependent with current cache. The dependents vector
        // is in BFS-discovery order (FIFO from the source) and additionally
        // sorted lexicographically above (Issue #401) so the iteration order
        // here is deterministic across runs.
        for (auto& dep_name : dependents) {
            auto src_it = function_sources_.find(dep_name);
            if (src_it == function_sources_.end())
                continue;

            // Re-parse the function source
            auto alloc = arena_.allocator();
            aura::ast::StringPool pool(alloc);
            aura::ast::FlatAST flat(alloc);
            auto pr = aura::parser::parse_to_flat(src_it->second, flat, pool);
            if (!pr.success || pr.root == aura::ast::NULL_NODE)
                continue;
            flat.root = pr.root;

            // Re-lower with current cache to detect new dependencies
            auto cache_ptr = ir_cache_.empty() ? nullptr : &ir_cache_;
            auto cache_strings_ptr = ir_cache_strings_.empty() ? nullptr : &ir_cache_strings_;
            std::vector<std::string> cache_hits;
            auto ir_mod = lower_to_ir_with_cache_tracked(
                flat, pool, arena_, cache_ptr, &cache_hits, &evaluator_.primitives(), nullptr,
                cache_strings_ptr, nullptr, &type_registry_, value_cells_for_lowering());

            // Phase 4: Run passes per-function on the re-lowered function bundle.
            {
                ComputeKindWrap ck_pass;
                ConstantFoldingWrap cf_pass;
                for (auto& func : ir_mod.functions) {
                    if (func.id == ir_mod.entry_function_id)
                        continue;
                    ck_pass.compute_function(func);
                    auto nf = cf_pass.fold_function(func);
                    if (nf > 0) {
                        // Debug print removed (#63723): was polluting
                        // test framework stream redirect for tests like
                        // edsl-ir-cache:cascade-after-mutate. The folded
                        // count is already in metrics_ via cf_pass metrics.
                    }
                }
            }

            // Update cache with new IR (store full bundle)
            std::vector<aura::ir::IRFunction> bundle;
            for (auto& func : ir_mod.functions) {
                if (func.id != ir_mod.entry_function_id) {
                    bundle.push_back(std::move(func));
                }
            }
            ir_cache_[dep_name] = std::move(bundle);
            snapshot_ir_for_disk(dep_name);

            // Re-record dependencies
            for (auto& called_name : cache_hits) {
                record_dependency(dep_name, called_name);
            }

            // Issue #272 Cycle 2: re-bind dependent env via IR after re-lower.
            (void)bind_function_define_via_ir(ir_mod, dep_name);
        }

        // Issue #638: propagate shape invalidation to dependents.
        for (auto& dep_name : dependents)
            invalidate_shape(dep_name);

        // Mark dependent modules dirty
        mark_module_dirty(name);
        for (auto& d : dependents)
            mark_module_dirty(d);

        // Issue #683: post re-lower linear ownership revalidate probe.
        run_linear_ownership_revalidate_after_invalidate(name);
    }

    // Issue #684: lower + absorb SoA dual-emit snapshot.
    aura::ir::IRModule lower_to_ir_with_cache_tracked(
        ast::FlatAST& flat, ast::StringPool& pool, ast::ASTArena& /*arena*/,
        const std::unordered_map<std::string, std::vector<aura::ir::IRFunction>>* cache,
        std::vector<std::string>* cache_hits, const Primitives* primitives,
        const std::unordered_map<std::string, std::vector<aura::ir::ClosureBridgeData>>*
            cache_bridge,
        const std::unordered_map<std::string, std::vector<std::string>>* cache_strings,
        const std::string* self_name, const aura::core::TypeRegistry* /*type_reg*/ = nullptr,
        const std::unordered_map<std::string, std::size_t>* value_cells = nullptr,
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

    // Issue #683 / #1515: LinearOwnershipRevalidate after invalidate/re-lower.
    // Unified sync_linear_roots_and_bridge_epoch registers GC roots
    // under live bridge_epoch then probes EnvFrame × linear state.
    void run_linear_ownership_revalidate_after_invalidate(const std::string& name) {
        (void)name;
        metrics_.linear_relower_revalidate_hits.fetch_add(1, std::memory_order_relaxed);
        metrics_.linear_post_mutate_revalidations_total.fetch_add(1, std::memory_order_relaxed);
        metrics_.linear_jit_post_invalidate_total.fetch_add(1, std::memory_order_relaxed);
        evaluator_.sync_linear_roots_and_bridge_epoch();
        // Issue #1543: invalidate_function path audit.
        (void)evaluator_.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditInvalidate);
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
    ast::ASTArena arena_{8 * 1024 * 1024, &arena_upstream_mr_};
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
    std::unordered_map<std::string, std::vector<std::string>> module_functions_;

    // Per-module state for incremental compilation (dirty tracking).
    std::unordered_map<std::string, ModuleState> module_states_;

    // Persistent AST for mutation workflows (set_code / typed_mutate).
    aura::ast::FlatAST* current_ast_ = nullptr;
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

public:
    // Issue #300 follow-up #1: drop per-fiber / main-thread
    // mutation checkpoints before arena teardown so PCV
    // children_snapshot copies do not race ~workspace_flat_.
    ~CompilerService() {
        // Issue #984: clear thread_local lowering hooks on teardown.
        clear_lowering_compiler_core_hooks();
        Evaluator::clear_main_thread_mutation_stack();
        if (auto* wf = evaluator_.workspace_flat())
            wf->release_children_for_teardown();
        if (current_ast_ && current_ast_ != evaluator_.workspace_flat())
            current_ast_->release_children_for_teardown();
        // Issue #765 (ASan-verify fix): reset the global
        // g_current_compiler_service hook so the arena
        // auto-compact-trigger + fiber-safe-compact lambdas
        // stored in aura::gc_hooks don't dereference a
        // dangling this-pointer after the last CompilerService
        // in a test scope goes out of scope. Without this
        // reset, a subsequent arena auto-compact (e.g. a
        // small allocator cap hit) reads from the destroyed
        // CompilerService's metrics() — ASan flags it as
        // stack-use-after-scope on the lambda's local 'raw'
        // stack frame. Always reset if we still own it;
        // never blindly zero (other services may have set
        // it concurrently in a multi-service scenario).
        if (aura::messaging::g_current_compiler_service == this)
            aura::messaging::g_current_compiler_service = nullptr;
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
    [[nodiscard]] const IRCacheEntry* ir_cache_v2_find(const std::string& name) const noexcept {
        auto it = ir_cache_v2_.find(name);
        if (it == ir_cache_v2_.end())
            return nullptr;
        return &it->second;
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

    // Issue #401: public test hook for invalidate_function. Lets tests
    // drive the BFS traversal directly without going through the Aura
    // (mutate:rebind) EDSL surface, which would also rebuild the function
    // body (mixing the traversal test with the rebind path).
    void public_invalidate_function(const std::string& name) { invalidate_function(name); }
    // Issue #1514: test/Agent seam for dirty cascade without full invalidate.
    void public_mark_define_dirty(const std::string& name) { mark_define_dirty(name); }
    // Issue #1495: test/agent entry for partial re-lower of dirty defines.
    std::size_t public_relower_dirty_defines_from_workspace() {
        return relower_dirty_defines_from_workspace();
    }

    // Issue #1378: test accessors for cascade ordering / epoch accounting.
    [[nodiscard]] std::uint64_t public_mutation_epoch() const noexcept {
        return mutation_epoch_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t public_invalidate_function_calls() const noexcept {
        return metrics_.invalidate_function_calls.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool public_jit_cache_contains(const std::string& name) const {
        std::shared_lock cache_read(jit_cache_mtx_);
        return jit_cache_.find(name) != jit_cache_.end();
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

    // Issue #1376: test hook — route through locked record_dependency.
    void public_record_dependency(const std::string& caller, const std::string& callee) {
        record_dependency(caller, callee);
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

    // Issue #272: test/observability accessor.
    [[nodiscard]] std::uint64_t define_ir_env_bind_count() const noexcept {
        return metrics_.define_ir_env_bind_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t value_define_ir_env_bind_count() const noexcept {
        return metrics_.value_define_ir_env_bind_count.load(std::memory_order_relaxed);
    }

    // Issue #272 Cycle 2: test hook for needs_tree_walker_fallback on defines.
    [[nodiscard]] bool public_needs_tree_walker_fallback(const aura::ast::FlatAST& flat,
                                                         const aura::ast::StringPool& pool,
                                                         aura::ast::NodeId root) const {
        return needs_tree_walker_fallback(flat, pool, root);
    }

    // Issue #282: public accessors for the Occurrence Typing
    // provenance log. The Aura primitive (query:provenance-of)
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
        // Issue #605: ShapeProfiler version at compile time.
        // Compared on cache hit; mismatch → re-compile + jit_shape_miss.
        std::uint64_t compiled_shape_version_ = 0;
    };
    std::unordered_map<std::string, JitCachedFn> jit_cache_;
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
    // Pattern:
    //   - mutation: mutation_epoch_.fetch_add(1, release);
    //   - ir_cache lookup: if entry.last_seen_epoch_ != current,
    //                       treat as stale (re-lower)
    //   - jit_cache lookup: if entry.last_seen_epoch_ != current,
    //                        treat as stale (re-compile)
    //   - apply_closure: load current via bridge_epoch() acquire
    std::atomic<std::uint64_t> mutation_epoch_{0};

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
    // For Cycle 1 we reuse mutation_epoch_ — both are bumped
    // together on reset() and on structural mutations, so a
    // single counter suffices. Cycle 2 may split if bridge
    // and cache invalidation need different policies.
    //
    // INVARIANT (Epoch Invariant): Every closure created
    // through the bridge must capture the current epoch
    // at construction time (via bridge_epoch()). Every
    // subsequent apply_closure call must check staleness
    // via Evaluator::is_bridge_stale(). A bypass of either
    // invariant is a contract violation.
    [[nodiscard]] std::uint64_t bridge_epoch() const noexcept {
        return mutation_epoch_.load(std::memory_order_acquire);
    }
    // Issue #223: explicitly bump the bridge epoch. Called when
    // the bridge_epoch_ field on existing ClosureBridgeData should
    // be considered stale (e.g. major mutation that doesn't reset
    // the arena). For Cycle 1 we just forward to mutation_epoch_.
    //
    // INVARIANT: bump_bridge_epoch() and any cache invalidation
    // must happen as a paired operation — bumping the epoch
    // without invalidating the cache leaves stale entries
    // visible. The current implementation reuses
    // mutation_epoch_ which is bumped together with cache
    // invalidation in mark_define_dirty / mark_all_defines_dirty.
    void bump_bridge_epoch() noexcept {
        mutation_epoch_.fetch_add(1, std::memory_order_release);
        // Issue #1476: bump the bridge_epoch_bumps_total counter
        // alongside the atomic so observability tracks every epoch
        // advance. Pairs with mutation_epoch_ acquire-load counters
        // (compiler_closure_epoch_mismatch_hits / is_bridge_stale
        // helper from #1475) to verify epoch progress vs catch-up.
        metrics_.bridge_epoch_bumps_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1485 C2-wire: keep AOT/C-runtime current_bridge_epoch
        // in lockstep with mutation_epoch_ so lib/runtime.c's
        // aura_closure_call 2-check (bridge_epoch mismatch + defuse_version_
        // mismatch → return 0) sees the fresh value rather than the
        // default 0. Without this wire, g_current_bridge_epoch stays at
        // the static init value forever and the 2-check always passes
        // vacuously. Acquire/release pairing with the fetch_add above
        // mirrors the #1476 dual-epoch protocol.
        aura_set_current_bridge_epoch(mutation_epoch_.load(std::memory_order_relaxed));
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
            if (bit->second->interpreter)
                bit->second->interpreter->collect_active_gc_roots(out.compiler_closure_roots,
                                                                  current_epoch);
        }

        for (auto& [name, binding] : ir_define_env_bindings_) {
            (void)name;
            if (!binding || !binding->interpreter)
                continue;
            add_closure(static_cast<std::int64_t>(binding->closure_id));
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
        const bool has_bridge = ir_cache_bridge_.count(name) > 0;
        const bool has_binding = ir_define_env_bindings_.count(name) > 0;
        if (!has_bridge && !has_binding)
            metrics_.hotswap_root_miss.fetch_add(1, std::memory_order_relaxed);

        for (auto& [bind_name, binding] : ir_define_env_bindings_) {
            if (bind_name != name || !binding || !binding->interpreter)
                continue;
            if (binding->interpreter->has_active_frames()) {
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
    // Issue #1523 lock order: mutate_mtx_ unique FIRST (if not held),
    // then epoch + bridge stamp (no dep_graph / workspace).
    void atomic_bump_epochs_and_stamp_bridge(const std::string& name) {
        using aura::compiler::lock_order::Level;
        using aura::compiler::lock_order::OrderedUniqueLock;
        OrderedUniqueLock<std::shared_mutex> mutate_guard =
            OrderedUniqueLock<std::shared_mutex>::acquire_if_needed(mutate_mtx_, Level::Mutate);
        sync_lock_order_metrics_();
        metrics_.unified_invalidation_protocol_total.fetch_add(1, std::memory_order_relaxed);
        // Issue #1496: release fence before publishing new epochs so
        // concurrent apply_closure readers that acquire-load either
        // domain cannot observe half-updated bridge tables.
        std::atomic_thread_fence(std::memory_order_release);
        // bridge_epoch FIRST (release ordering on mutation_epoch_).
        bump_bridge_epoch();
        // Issue #1476: defuse_version_ in lockstep (acq_rel) for #1475 readers.
        evaluator_.bump_defuse_version_for_test();
        metrics_.dep_graph_defuse_version_bumps.fetch_add(1, std::memory_order_relaxed);
        // Keep AOT table epoch in lockstep for dual-check aura_closure_call.
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
        if (!remap || n == 0)
            return;
        constexpr auto kNull = std::numeric_limits<std::uint32_t>::max();
        for (auto& [name, binding] : ir_define_env_bindings_) {
            (void)name;
            if (!binding || !binding->interpreter)
                continue;
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
        std::size_t n = 0;
        for (auto& [name, binding] : ir_define_env_bindings_) {
            (void)name;
            if (!binding || !binding->interpreter)
                continue;
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
        // Issue #1543: JIT hot-swap / batch-deopt path audit.
        (void)evaluator_.run_linear_gc_root_audit(Evaluator::kLinearGcRootAuditJitHotSwap);
    }

    // Issue #1536: bulk walk_active_closures after epoch bump / invalidate.
    // Marks every captured fn that is_fn_epoch_stale for deopt-on-next-apply
    // and pairs jit_epoch_stale_check_total with live_closure_stale_prevented.
    void notify_walk_active_closures_(std::uint64_t current_epoch) {
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

    // Try to execute an IRModule via LLVM JIT
    // Returns EvalResult on success, nullopt on failure (falls back to IR interpreter)
    // escape_maps: optional pre-computed escape maps from EscapeAnalysisWrap pass.
    //              If null, escape analysis is run inline.
    std::optional<types::EvalValue>
    try_jit_execute(const aura::ir::IRModule& ir_mod,
                    const std::vector<std::vector<std::uint8_t>>* escape_maps = nullptr) {
        // Issue #59 Iter 3: shared-lock the Mutation Lock so a
        // concurrent mutate:* waits for this compile to drain before
        // invalidating. Compiles can run concurrently with each other.
        // Issue #1523: mutate is FIRST in canonical order (shared is fine).
        lock_order::OrderedSharedLock<std::shared_mutex> mutate_read(mutate_mtx_,
                                                                     lock_order::Level::Mutate);
        sync_lock_order_metrics_();
        if (ir_mod.functions.empty())
            return std::nullopt;

        // Set string pool before compiling (for OpConstString)
        jit_.set_string_pool(&ir_mod.string_pool);

        // Compile ALL functions (with JIT cache) and register with runtime
        for (auto& ir_fn : ir_mod.functions) {
            std::uint32_t env_count = static_cast<std::uint32_t>(ir_fn.free_vars.size());

            // Check JIT cache (shared lock for read, unique for write).
            aura::jit::ScalarFn fn_ptr = nullptr;
            bool need_compile = true;
            {
                std::shared_lock cache_read(jit_cache_mtx_);
                auto cache_it = jit_cache_.find(ir_fn.name);
                if (cache_it != jit_cache_.end()) {
                    auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                    if (!cache_it->second.has_shape_map && shape_profiler_.is_stable(fn_key)) {
                        // hot-recompile path: needs unique lock
                    } else if (jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                        shape::jit_shape_miss_count.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                        need_compile = false;
                    }
                }
            }
            if (need_compile && fn_ptr == nullptr) {
                std::unique_lock cache_write(jit_cache_mtx_);
                auto cache_it = jit_cache_.find(ir_fn.name);
                auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                if (cache_it != jit_cache_.end() && !cache_it->second.has_shape_map &&
                    shape_profiler_.is_stable(fn_key)) {
                    std::fprintf(stderr, "spec: hot-recompile '%s' (try_jit)\n",
                                 ir_fn.name.c_str());
                    jit_cache_.erase(cache_it);
                } else if (cache_it != jit_cache_.end() &&
                           jit_cache_shape_version_stale(cache_it->second, fn_key)) {
                    // Issue #605: drop stale shape-specialized entry.
                    jit_cache_.erase(cache_it);
                } else if (cache_it != jit_cache_.end()) {
                    fn_ptr = cache_it->second.fn_ptr.load(std::memory_order_acquire);
                    need_compile = false;
                }
            }
            if (!fn_ptr) {
                // Set up shape_storage for shape_map (keep alive until compile).
                // Issue #60 Iter 2: must be done BEFORE building flat_instrs
                // because the shape_id stamping reads final_shape_map.
                std::vector<std::uint8_t> shape_storage;
                const uint8_t* final_shape_map = nullptr;
                auto fn_key = shape::make_fn_key(session_id_, ir_fn.name);
                if (shape_profiler_.is_stable(fn_key)) {
                    auto dom = shape_profiler_.dominant_shape(fn_key);
                    uint8_t code = 0;
                    if (dom == shape::SHAPE_INT)
                        code = 1;
                    else if (dom == shape::SHAPE_FLOAT)
                        code = 2;
                    else if (dom == shape::SHAPE_BOOL)
                        code = 3;
                    else if (dom == shape::SHAPE_STRING)
                        code = 4;
                    else if (dom == shape::SHAPE_VOID)
                        code = 5;
                    else if (dom == shape::SHAPE_PAIR)
                        code = 10;
                    if (code) {
                        shape_storage.resize(ir_fn.local_count, 0);
                        for (uint32_t i = 0; i < ir_fn.arg_count && i < ir_fn.local_count; ++i)
                            shape_storage[i] = code;
                        final_shape_map = shape_storage.data();
                        std::fprintf(stderr, "spec: try_jit L1 for '%s'\n", ir_fn.name.c_str());
                    }
                }

                // Build FlatFunction from IR function (with shape_id stamping)
                std::vector<std::vector<aura::jit::FlatInstruction>> flat_instrs(
                    ir_fn.blocks.size());
                std::vector<aura::jit::FlatBlock> flat_blocks(ir_fn.blocks.size());
                for (std::size_t bi = 0; bi < ir_fn.blocks.size(); ++bi) {
                    auto& block = ir_fn.blocks[bi];
                    for (auto& instr : block.instructions) {
                        std::uint32_t shape = 0;
                        if (final_shape_map && instr.operands[0] < ir_fn.local_count) {
                            shape = final_shape_map[instr.operands[0]];
                        }
                        flat_instrs[bi].push_back({static_cast<std::uint32_t>(instr.opcode),
                                                   {instr.operands[0], instr.operands[1],
                                                    instr.operands[2], instr.operands[3]},
                                                   shape,
                                                   instr.narrow_evidence,
                                                   instr.type_id,
                                                   instr.linear_ownership_state,
                                                   0});
                    }
                    flat_blocks[bi] = {block.id, flat_instrs[bi].data(),
                                       static_cast<std::uint32_t>(flat_instrs[bi].size())};
                }
                // Use pre-computed escape maps when available (from EscapeAnalysisWrap pass)
                std::vector<std::uint8_t> escape_storage;
                if (g_use_arena) {
                    if (escape_maps && ir_fn.id < escape_maps->size() &&
                        !(*escape_maps)[ir_fn.id].empty()) {
                        // Use pre-computed maps from the pass
                        escape_storage = (*escape_maps)[ir_fn.id];
                    } else {
                        // Fallback: run escape analysis inline
                        escape_storage.resize(ir_fn.local_count, 0);
                        aura::jit::run_escape_analysis(flat_instrs, ir_fn.local_count,
                                                       escape_storage);
                    }
                }
                // (Issue #60 Iter 2: shape_storage and final_shape_map
                // are now set above, before the flat_instrs build.)

                aura::jit::FlatFunction flat_fn{ir_fn.name.c_str(),
                                                ir_fn.entry_block,
                                                ir_fn.local_count,
                                                ir_fn.arg_count,
                                                flat_blocks.data(),
                                                static_cast<std::uint32_t>(flat_blocks.size()),
                                                nullptr,
                                                0,
                                                final_shape_map};
                // Set escape map from escape analysis
                if (!escape_storage.empty())
                    flat_fn.escape_map = escape_storage.data();

                // Skip if already cached (prevents duplicate JIT symbols)
                {
                    std::shared_lock cache_read(jit_cache_mtx_);
                    if (ir_fn.name != "__top__" && jit_cache_.count(ir_fn.name)) {
                        fn_ptr = jit_cache_[ir_fn.name].fn_ptr;
                    }
                }
                if (!fn_ptr) {
                    fn_ptr = jit_.compile(flat_fn);
                    // Issue #1512: mirror AuraJIT coverage masks.
                    metrics_.jit_opcode_covered_mask.store(
                        jit_.metrics().opcode_covered_mask.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                    metrics_.jit_opcode_unhandled_mask.store(
                        jit_.metrics().opcode_unhandled_mask.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                    if (!fn_ptr) {
                        metrics_.jit_compile_misses.fetch_add(1, std::memory_order_relaxed);
                        metrics_.opcode_cov_unhandled_hot_total.fetch_add(
                            1, std::memory_order_relaxed);
                        return std::nullopt;
                    }
                    metrics_.opcode_cov_hits_total.fetch_add(1, std::memory_order_relaxed);
                    metrics_.jit_compilations.fetch_add(1, std::memory_order_relaxed);
                    if (ir_fn.name != "__top__") {
                        std::unique_lock cache_write(jit_cache_mtx_);
                        auto [it, _ins] = jit_cache_.try_emplace(ir_fn.name);
                        it->second.fn_ptr.store(fn_ptr, std::memory_order_release);
                        it->second.local_count = ir_fn.local_count;
                        it->second.arg_count = ir_fn.arg_count;
                        it->second.env_count = env_count;
                        it->second.has_shape_map = final_shape_map != nullptr;
                        if (final_shape_map != nullptr) {
                            it->second.compiled_shape_version_ =
                                shape_profiler_.current_snapshot(fn_key).version;
                        }
                    }
                }
            }

            // Register with runtime for closure calls
            jit_.register_function(static_cast<int64_t>(ir_fn.id), fn_ptr, ir_fn.local_count,
                                   ir_fn.arg_count, env_count);
        }

        // Find and execute entry function
        auto entry_it = std::find_if(
            ir_mod.functions.begin(), ir_mod.functions.end(),
            [&](const aura::ir::IRFunction& f) { return f.id == ir_mod.entry_function_id; });
        if (entry_it == ir_mod.functions.end())
            return std::nullopt;

        auto& entry = *entry_it;
        std::vector<std::int64_t> locals(entry.local_count, 0);
        auto fn_ptr = jit_.get_function_ptr(entry.name.c_str());
        if (!fn_ptr)
            return std::nullopt;

        auto raw_result =
            reinterpret_cast<aura::jit::ScalarFn>(fn_ptr)(locals.data(), entry.arg_count);

        // ── Convert JIT result to proper EvalValue type ──
        types::EvalValue result_type;
        uint32_t ret_slot = std::numeric_limits<uint32_t>::max();

        for (auto& block : entry.blocks)
            for (auto& instr : block.instructions)
                if (instr.opcode == aura::ir::IROpcode::Return)
                    ret_slot = instr.operands[0];

        // Follow data flow through OpLocal to find the actual producer
        if (ret_slot != std::numeric_limits<uint32_t>::max()) {
            uint32_t search_slot = ret_slot;
            bool chasing = true;
            while (chasing) {
                chasing = false;
                for (auto& block : entry.blocks) {
                    for (auto& instr : block.instructions) {
                        if (instr.operands[0] == search_slot &&
                            instr.opcode != aura::ir::IROpcode::Return) {
                            if (instr.opcode == aura::ir::IROpcode::Local) {
                                search_slot = instr.operands[1];
                                chasing = true;
                                goto try_chain;
                            }
                            switch (instr.opcode) {
                                case aura::ir::IROpcode::ConstBool:
                                case aura::ir::IROpcode::Eq:
                                case aura::ir::IROpcode::Lt:
                                case aura::ir::IROpcode::Gt:
                                case aura::ir::IROpcode::Le:
                                case aura::ir::IROpcode::Ge:
                                case aura::ir::IROpcode::And:
                                case aura::ir::IROpcode::Or:
                                case aura::ir::IROpcode::Not:
                                    result_type = types::make_bool(raw_result == 7);
                                    goto result_done;
                                case aura::ir::IROpcode::ConstI64:
                                case aura::ir::IROpcode::HashRef:
                                case aura::ir::IROpcode::HashRemove:
                                    result_type = types::EvalValue(raw_result);
                                    goto result_done;
                                case aura::ir::IROpcode::ConstVoid:
                                    result_type = types::make_void();
                                    goto result_done;
                                case aura::ir::IROpcode::MakePair:
                                    if (raw_result < 0)
                                        result_type = types::make_pair(
                                            static_cast<uint64_t>(-raw_result - 1));
                                    else
                                        result_type = types::make_pair(
                                            static_cast<uint64_t>(raw_result >> 2));
                                    goto result_done;
                                case aura::ir::IROpcode::NewCell:
                                    result_type =
                                        types::make_cell(static_cast<uint64_t>(raw_result));
                                    goto result_done;
                                case aura::ir::IROpcode::MakeClosure:
                                    result_type =
                                        types::make_closure(static_cast<uint64_t>(raw_result));
                                    goto result_done;
                                case aura::ir::IROpcode::ConstF64:
                                    result_type = types::EvalValue(raw_result);
                                    goto result_done;
                                case aura::ir::IROpcode::ConstString: {
                                    auto* str_content = aura_jit_string_content(raw_result);
                                    if (str_content) {
                                        auto& sh = evaluator_.primitives().string_heap();
                                        auto new_idx = sh.size();
                                        sh.push_back(str_content);
                                        result_type = types::make_string(new_idx);
                                    } else {
                                        result_type = types::EvalValue(raw_result);
                                    }
                                    goto result_done;
                                }
                                case aura::ir::IROpcode::PrimCall:
                                    result_type = types::EvalValue(raw_result);
                                    goto result_done;
                                default:
                                    break;
                            }
                            goto result_done;
                        }
                    }
                }
                break;
            try_chain:;
            }
        }

    result_done:
        if (result_type.val == 0 && raw_result != 0) {
            if (raw_result == 11)
                result_type = types::make_void();
            else if (raw_result == 3 || raw_result == 7)
                result_type = types::make_bool(raw_result == 7);
            else if ((raw_result & 1) == 0 && raw_result > -10000000000000000LL)
                result_type = types::EvalValue(raw_result);
            else
                result_type = types::EvalValue(raw_result);
        }
        return result_type;
    }

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
                sync_shape_ids_for_fn_key(fn_key, shape_id);
            return;
        }

        // Record per-function: use IR function name as key
        for (auto& fn : mod->functions) {
            if (fn.id == mod->entry_function_id) {
                auto fn_key = shape::make_fn_key(session, fn.name);
                auto shape_id = shape::inline_shape_of(result.val);
                if (profiler.record_shape(fn_key, shape_id) && profiler.is_stable(fn_key))
                    sync_shape_ids_for_fn_key(fn_key, shape_id);

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
    void mark_shape_dirty_for_fn_key(shape::FnKey fn_key) {
        for (auto& [name, entry] : ir_cache_v2_) {
            if (shape::make_fn_key(session_id_, name) != fn_key)
                continue;
            entry.mark_all_blocks_dirty();
            metrics_.irsoa_dirty_cascade_savings.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Issue #686: propagate stable dominant shape into IRSoA shape_ids_.
    void sync_shape_ids_for_fn_key(shape::FnKey fn_key, shape::ShapeID shape_id) {
        if (shape_id == shape::SHAPE_UNKNOWN)
            return;
        for (auto& [name, entry] : ir_cache_v2_) {
            if (shape::make_fn_key(session_id_, name) != fn_key)
                continue;
            const auto sid = static_cast<std::uint32_t>(shape_id);
            for (auto& soa_fn : entry.soa_mod.functions) {
                for (auto& col : soa_fn.shape_ids_) {
                    if (col == 0)
                        col = sid;
                }
            }
            metrics_.shape_ids_sync_hits.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Invalidate shape profile for a function (called after mutate:*).
    void invalidate_shape(const std::string& name) {
        auto fn_key = shape::make_fn_key(session_id_, name);
        if (shape_profiler_.metrics(fn_key).total_calls == 0) {
            for (std::uint32_t si = 0; si < shape::ShapeProfiler::kStableThreshold; ++si)
                shape_profiler_.record_shape(fn_key, shape::SHAPE_INT);
        }
        const bool needs_deopt = shape_profiler_.invalidate(fn_key);
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

    // Issue #494: yield between pass-pipeline stages when on a fiber.
    static bool pipeline_yield_trampoline() noexcept {
        if (!aura::serve::g_current_fiber)
            return false;
        aura::serve::Fiber::yield(aura::serve::YieldReason::PassPipeline);
        return true;
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
                             std::uint32_t dirty_scope) noexcept {
        (void)version;
        if (dirty_scope == 0)
            return;

        std::string name;
        for (const auto& [n, _] : ir_cache_v2_) {
            if (shape::make_fn_key(session_id_, n) == fn_key) {
                name = n;
                break;
            }
        }
        if (name.empty()) {
            std::shared_lock cache_read(jit_cache_mtx_);
            for (const auto& [n, _] : jit_cache_) {
                if (shape::make_fn_key(session_id_, n) == fn_key) {
                    name = n;
                    break;
                }
            }
        }
        if (name.empty()) {
            std::shared_lock dep_read(dep_graph_mtx_);
            for (const auto& [n, _] : dep_graph_) {
                if (shape::make_fn_key(session_id_, n) == fn_key) {
                    name = n;
                    break;
                }
            }
        }

        if (dirty_scope == shape::kShapeDirtyScopeStabilityLoss) {
            metrics_.shape_changes_observed.fetch_add(1, std::memory_order_relaxed);
            shape::shape_deopt_storm_count.fetch_add(1, std::memory_order_relaxed);
            bump_shape_fiber_refresh();
        }

        // Issue #1521: ArenaCompact — coordinate IR dirty + fiber refresh
        // without counting as deopt storm / mutation churn.
        if (dirty_scope == shape::kShapeDirtyScopeArenaCompact) {
            metrics_.deopt_from_arena_compact_total.fetch_add(1, std::memory_order_relaxed);
            bump_shape_fiber_refresh();
            if (!name.empty()) {
                mark_shape_dirty_for_fn_key(fn_key);
                // Soft: dual-epoch bump so JIT shape_map guards recheck
                // (#1496: never advance bridge without defuse). Prefer
                // mark-dirty over full jit_cache_ erase when possible.
                bump_bridge_epoch();
                evaluator_.bump_defuse_version_for_test();
                metrics_.dep_graph_defuse_version_bumps.fetch_add(1, std::memory_order_relaxed);
                metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
                shape_jit_pass::record_incremental_recompile_hit();
            }
            return;
        }

        if (!name.empty()) {
            mark_shape_dirty_for_fn_key(fn_key);
            // Issue #1496: dual-epoch write — bare bump_bridge_epoch here
            // desynced defuse_version_ when invalidate_function already ran
            // atomic_bump_epochs_and_stamp_bridge (hard path lockstep fail).
            bump_bridge_epoch();
            evaluator_.bump_defuse_version_for_test();
            metrics_.dep_graph_defuse_version_bumps.fetch_add(1, std::memory_order_relaxed);
            evaluator_.resync_live_closure_env_versions_on_invalidate();
            jit_.invalidate(name.c_str());
            jit_.invalidate_prefix(name.c_str());
            metrics_.jit_hotswap_invalidate_total.fetch_add(1, std::memory_order_relaxed);
            shape_jit_pass::record_incremental_recompile_hit();
            evaluator_.bump_incremental_closure_jit_sync();
            {
                std::unique_lock cache_write(jit_cache_mtx_);
                if (jit_cache_.erase(name) > 0)
                    metrics_.jit_cache_evictions.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Register evaluator primitives with JIT runtime
    void register_jit_primitives() {
        // Set the global primitives pointer for the JIT dispatcher
        g_jit_prim_ctx.store(&evaluator_.primitives(), std::memory_order_release);

        // Issue #452: wire AOT bridge metrics pointer so
        // aura_reload_aot_module can bump the
        // aot_stale_reject_count_, aot_region_mismatch_,
        // aot_hot_update_success_ counters exposed via
        // (query:aot-stats). Lifetime is tied to the
        // service (well-defined teardown order).
        aura_set_aot_metrics(&metrics_);
        // Issue #1522: re-bind batch_deopt target (lazy JIT prims path).
        aura_set_jit_batch_deopt_target(&jit_);
        // Issue #1540: re-bind linear_post_mutate_enforce for JIT probe.
        aura_set_linear_post_mutate_enforce_fn(
            [](void* user, std::uint32_t env_id) -> int {
                if (!user)
                    return 0;
                auto* ev = static_cast<Evaluator*>(user);
                return ev->linear_post_mutate_enforce(env_id) ? 0 : 1;
            },
            &evaluator_);
        // Issue #1545: re-bind live-closure linear scan for JIT ResourceTracker.
        aura_set_linear_live_closure_scan_fn(
            [](void* user) {
                if (!user)
                    return;
                static_cast<Evaluator*>(user)->scan_live_closures_for_linear_captures(true);
            },
            &evaluator_);

        // Issue #157 Phase 1: bind the lock hooks. Pattern matches
        // the g_prim_dispatcher pattern above — the runtime bridges
        // (aura_alloc_pair, aura_pair_car, aura_prim_call, etc.) call
        // into the evaluator's lock + version methods via this hook
        // table. Without these hooks, the bridges are no-ops
        // (single-threaded default) and the bypass is unsafe.
        //
        // The hooks are bound ONCE per CompilerService. Multi-eval
        // setups would need to rebind (deferred — not currently
        // supported, but the hook table is per-process global so
        // switching evaluators mid-flight needs care).
        aura_set_lock_hooks(
            [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->lock_workspace_shared(); },
            [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->unlock_workspace_shared(); },
            [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->lock_workspace_unique(); },
            [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->unlock_workspace_unique(); },
            [](void* e) -> std::uint64_t {
                return static_cast<aura::compiler::Evaluator*>(e)->get_defuse_version();
            },
            [](void* e) { static_cast<aura::compiler::Evaluator*>(e)->yield_mutation_boundary(); },
            static_cast<void*>(&evaluator_));

        // Issue #272 Cycle 5: TopCellLoad reads evaluator_.cells() live.
        aura_set_top_cell_getter(
            [](void* e, int64_t idx) -> int64_t {
                auto* ev = static_cast<aura::compiler::Evaluator*>(e);
                if (idx < 0 || static_cast<std::size_t>(idx) >= ev->cells().size())
                    return 0;
                return ev->cells()[static_cast<std::size_t>(idx)].val;
            },
            static_cast<void*>(&evaluator_));

// Register the dispatcher with JIT runtime
#ifdef AURA_HAVE_LLVM
        // aura_jit_prim_dispatch is defined at file scope (after imports)
        // and aura_set_prim_dispatcher is declared at file scope.
        aura_set_prim_dispatcher(aura_jit_prim_dispatch);

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

    // ── Static registry ──────────────────────────────────────
    // Using Scott Meyer's singletons to avoid ODR issues with module static members
    static std::unordered_map<std::string, CompilerService*>& registry() {
        static std::unordered_map<std::string, CompilerService*> reg;
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
    auto new_idx = sh.size();
    sh.push_back(content ? content : "");
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
