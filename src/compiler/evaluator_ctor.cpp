// evaluator_ctor.cpp — P1-p: Evaluator construction / teardown
// aura.compiler.evaluator module partition.

module;

#include "messaging_bridge.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "primitives_meta.h"
#include "compiler/aura_jit_bridge.h" // Issue #1367: aura_cleanup_aot_state

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

void Evaluator::init_pair_primitives() {
    register_all_primitives();
}

void Evaluator::build_primitive_slots() {
    // No longer needed — Primitives manages ordering internally
}

Evaluator::Evaluator() {
    // Issue #1746: monotonic instance id for TLS maps (depth slot).
    // Never recycled; independent of heap address reuse.
    static std::atomic<std::uint64_t> next_instance_id{1};
    instance_id_ = next_instance_id.fetch_add(1, std::memory_order_relaxed);

    // Issue #1352: retain process-wide terminal buffer registry for this Evaluator.
    primitives_detail::retain_terminal_buffer_registry();

    aura::messaging::g_heap_mutex = [this]() -> std::mutex& { return heap_mutex(); };

    top_.set_primitives(&primitives_);
    top_.set_owner(this);
    top_.set_parent_id(alloc_env_frame(NULL_ENV_ID /* no parent */, &primitives_));
    primitives_.set_string_heap(&string_heap_);
    arena_group_ = std::make_unique<aura::ast::ArenaGroup>();
    init_pair_primitives();

    ffi_runtime_.register_primitives(prim_registrar(), &string_heap_, &opaque_heap_,
                                     &coverage_counters_);

    adt_runtime_.register_primitives(prim_registrar(), &string_heap_, &opaque_heap_,
                                     &coverage_counters_);

    build_primitive_slots();

    primitives_detail::register_network_primitives(prim_registrar(), *this);

    // Issues #1331–#1343 Phase 1: TUI pixel/cell rendering surface
    primitives_detail::register_tui_primitives(prim_registrar(), *this);

    primitives_detail::register_type_primitives(prim_registrar(), *this);

    install_defuse_subsystem();

    primitives_detail::register_hot_swap_primitives(prim_registrar(), *this);

    primitives_detail::register_compile_primitives(prim_registrar(), *this);

    primitives_detail::register_messaging_primitives(prim_registrar(), *this);

    primitives_detail::register_synthesize_primitives(
        prim_registrar(), *this, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_strategy_primitives(prim_registrar(), *this);

    primitives_detail::register_memory_primitives(
        prim_registrar(), *this, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_jit_arena_primitives(prim_registrar(), *this);

    primitives_detail::register_policy_primitives(prim_registrar(), *this);

    // Issue #697: backfill SV/EDA PrimMeta after compile partition registers
    // eda:run-verification-feedback and eda:demo-sv-self-evolution.
    backfill_eda_sv_primitive_meta();

    // Issue #1416: tier-assign the 7 EDSL escape-hatch primitives
    // (Part 4 #1396) to kPrimSecPrivileged so the dispatch-site
    // capability gate in invoke_prim_with_telemetry can deny
    // unauthorized calls. Called after backfill_eda_sv_primitive_meta
    // so the dispatch gate sees the tier-assigned security_level.
    backfill_capability_tiers();

    // Issue #1356: rebuild HotTierTable after all registrations + meta backfill.
    primitives_.finalize_hot_table();
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_)) {
        m->prim_hot_table_size.store(static_cast<std::uint64_t>(primitives_.hot_table_size()),
                                     std::memory_order_relaxed);
        m->prim_hot_tier_active.store(1, std::memory_order_relaxed);
    }
}

void Evaluator::backfill_eda_sv_primitive_meta() {
    primitives_.set_meta_for_name(
        "eda:weaken-property",
        DEFINE_PRIMITIVE_META(2, false, kPrimSafetyMutates, "sva",
                              "Weaken SVA property with disable-iff clause on Property nodes.",
                              "(int string) -> bool"));
    primitives_.set_meta_for_name(
        "eda:add-coverpoint-bin",
        PrimMeta{.arity = 2,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .doc = "Append a named bin to a structured Coverpoint AST node.",
                 .category = "sva",
                 .schema = "(int string) -> bool"});
    primitives_.set_meta_for_name(
        "eda:update-constraint",
        PrimMeta{.arity = 2,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .doc = "Append a constraint expression to a native Constraint AST node.",
                 .category = "sva",
                 .schema = "(int string) -> bool"});
    primitives_.set_meta_for_name(
        "eda:run-verification-feedback",
        PrimMeta{.arity = 2,
                 .pure = false,
                 .safety_flags = static_cast<std::uint8_t>(kPrimSafetyMutates | kPrimSafetyFiber),
                 .doc = "Parse verification report and apply targeted SV mutate + re-emit.",
                 .category = "verification",
                 .schema = "(string string) -> bool"});
    primitives_.set_meta_for_name(
        "eda:demo-sv-self-evolution",
        PrimMeta{.arity = 2,
                 .pure = false,
                 .safety_flags = static_cast<std::uint8_t>(kPrimSafetyMutates | kPrimSafetyFiber),
                 .doc = "Bounded EDA-SV verification self-evolution demo harness.",
                 .category = "eda",
                 .schema = "(string int) -> int"});
    primitives_.set_meta_for_name(
        "eda:run-commercial-simulator-stub",
        PrimMeta{.arity = 2,
                 .pure = false,
                 .safety_flags = static_cast<std::uint8_t>(kPrimSafetyMutates | kPrimSafetyFiber),
                 .doc = "Run commercial simulator stub: re-emit + validate SV for node.",
                 .category = "eda",
                 .schema = "(string int) -> bool"});
    primitives_.set_meta_for_name(
        "eda:parse-netlist",
        PrimMeta{.arity = 1,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .doc = "Parse minimal netlist/RTL text into native SV FlatAST nodes.",
                 .category = "eda",
                 .schema = "(string) -> int"});
    primitives_.set_meta_for_name(
        "eda:query-nodes", PrimMeta{.arity = 1,
                                    .pure = true,
                                    .safety_flags = 0,
                                    .doc = "Count workspace nodes matching an SV NodeTag name.",
                                    .category = "eda",
                                    .schema = "(string) -> int"});
    primitives_.set_meta_for_name(
        "eda:mutate-add-instance",
        PrimMeta{.arity = 3,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .doc = "Add a modport instance to an Interface with StableRef safety.",
                 .category = "eda",
                 .schema = "(int string string) -> bool"});
    primitives_.set_meta_for_name(
        "eda:waveform-snapshot",
        PrimMeta{.arity = 1,
                 .pure = true,
                 .safety_flags = 0,
                 .doc = "Export re-emit text length as waveform snapshot observability.",
                 .category = "eda",
                 .schema = "(int) -> int"});
    primitives_.set_meta_for_name(
        "eda:run-hardware-feedback",
        PrimMeta{.arity = 1,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .doc = "Trigger hardware_backend closed-loop hook + SV re-emit for node.",
                 .category = "eda",
                 .schema = "(int) -> bool"});
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->primitive_eda_meta_backfill_total.fetch_add(10, std::memory_order_relaxed);
}

// Issue #1416: tier-assign the 7 EDSL escape-hatch primitives (Part 4
// #1396) to kPrimSecPrivileged so the dispatch-site capability gate
// in invoke_prim_with_telemetry can deny unauthorized calls. All 7
// are write-side primitives that mutate IR cache state — they
// should require kCapWildcard (the same gate that EDSL escape-hatch
// mutations already check individually in their lambda bodies; this
// centralizes the gate at dispatch for the 7 cases listed below).
//
// Note: the primitive lambdas still contain their own per-primitive
// capability checks (e.g. compile:mark-block-dirty! checks
// kCapCompileDirty / kCapCompile at the lambda body). The dispatch-
// site gate adds a STRONGER outer envelope (kCapWildcard required)
// on top of the existing inner check — defense-in-depth. If a
// future refactor removes the inner check, the outer gate still
// holds.
void Evaluator::backfill_capability_tiers() {
    // 1. compile:mark-block-dirty! (compile_03.cpp:224)
    // Issue #1416: PrimMeta designators must follow declaration order
    // (arity, pure, safety_flags, perf_tier, security_level, deprecated,
    // doc, category, schema).
    primitives_.set_meta_for_name(
        "compile:mark-block-dirty!",
        PrimMeta{.arity = 3,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .security_level = kPrimSecPrivileged,
                 .doc = "Mark a single (function, block) dirty in the named define's IR cache.",
                 .category = "compile",
                 .schema = "(string int int) -> bool"});
    // 2. compile:clear-block-dirty! (compile_03.cpp:264)
    primitives_.set_meta_for_name(
        "compile:clear-block-dirty!",
        PrimMeta{.arity = 3,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .security_level = kPrimSecPrivileged,
                 .doc =
                     "Clear a single (function, block) dirty bit in the named define's IR cache.",
                 .category = "compile",
                 .schema = "(string int int) -> bool"});
    // 3. compile:mark-dirty-upward-fast (compile_02.cpp:616)
    primitives_.set_meta_for_name(
        "compile:mark-dirty-upward-fast",
        PrimMeta{.arity = 1,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .security_level = kPrimSecPrivileged,
                 .doc = "Fast path: mark all callers of a name dirty in the dep_graph.",
                 .category = "compile",
                 .schema = "(string) -> bool"});
    // 4. compile:mark-instruction-dirty! (compile_03.cpp:323)
    primitives_.set_meta_for_name(
        "compile:mark-instruction-dirty!",
        PrimMeta{.arity = 4,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .security_level = kPrimSecPrivileged,
                 .doc = "Mark a single instruction dirty in a function's IR cache.",
                 .category = "compile",
                 .schema = "(string int int int) -> bool"});
    // 5. compile:clear-instruction-dirty! (compile_03.cpp:354)
    primitives_.set_meta_for_name(
        "compile:clear-instruction-dirty!",
        PrimMeta{.arity = 4,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .security_level = kPrimSecPrivileged,
                 .doc = "Clear a single instruction dirty bit in a function's IR cache.",
                 .category = "compile",
                 .schema = "(string int int int) -> bool"});
    // 6. compile:clear-macro-dirty! (compile_04.cpp:78)
    primitives_.set_meta_for_name(
        "compile:clear-macro-dirty!",
        PrimMeta{.arity = 1,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .security_level = kPrimSecPrivileged,
                 .doc = "Clear macro dirty flag after macro re-expansion.",
                 .category = "compile",
                 .schema = "(string) -> bool"});
    // 7. compile:mark-narrowing-dirty! (compile_04.cpp:745)
    primitives_.set_meta_for_name(
        "compile:mark-narrowing-dirty!",
        PrimMeta{.arity = 1,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .security_level = kPrimSecPrivileged,
                 .doc = "Mark narrowing-derived bindings dirty for re-analysis.",
                 .category = "compile",
                 .schema = "(int) -> bool"});
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->primitive_capability_tier_backfill_total.fetch_add(7, std::memory_order_relaxed);
}

void Evaluator::set_type_registry(void* reg) {
    // Issue #911: drop owned registry before adopting external.
    // Issue #1837: not concurrent-safe vs readers of type_registry_
    // (compile:hw-coercion-lossy? / hw-coercion-warning / typecheck).
    // Call only under eval quiescence (see evaluator.ixx contract).
    if (owns_type_registry_ && type_registry_ && type_registry_ != reg) {
        delete static_cast<aura::core::TypeRegistry*>(type_registry_);
        owns_type_registry_ = false;
    }
    type_registry_ = reg;
    owns_type_registry_ = false;
}

void* Evaluator::ensure_type_registry() {
    // Issue #911/#912: single ownership path for TypeRegistry.
    if (!type_registry_) {
        type_registry_ = new aura::core::TypeRegistry();
        owns_type_registry_ = true;
    }
    return type_registry_;
}

Evaluator::~Evaluator() {
    // Issue #1667: release process-wide PanicCheckpoint GC defer if this
    // Evaluator still holds the arm (save without commit/restore, or
    // exception mid-window). Without this, g_gc_defer_pending_panic_depth
    // leaks until process exit and every request_gc_safepoint defers.
    // Mirrors #63723 / #1662 teardown hygiene for process-wide / owner links.
    // release is idempotent (no-op when not armed) and noexcept.
    release_gc_defer_for_pending_panic();

    // Issue #1662 (P0): clear arena owner + compact hooks FIRST so a
    // surviving ASTArena cannot UAF into this dying Evaluator via
    // allocate_raw (quota callback) or on_compact_hook. set_arena /
    // set_temp_arena install `this` as arena_owner_; ~Evaluator must
    // clear those links before any further teardown (same hygiene as
    // g_yield_hook_evaluator / g_query_evaluator below).
    // Issue #1663: hold arena_set_mtx_ so concurrent set_arena cannot
    // interleave with teardown.
    {
        std::lock_guard<std::mutex> lock(arena_set_mtx_);
        if (arena_) {
            arena_->clear_arena_owner();
            arena_->set_on_compact_hook({});
            arena_ = nullptr;
        }
        if (temp_arena_) {
            temp_arena_->clear_arena_owner();
            temp_arena_ = nullptr;
        }
        if (arena_group_)
            arena_group_->clear_default_arena_owner();
    }

    // Issue #63723: clear all thread-local Evaluator* slots
    // that might still point at this dying instance. Without
    // this, when the fiber's stack frame is reused after the
    // closure returns, the worker thread's g_yield_hook_evaluator
    // / g_query_evaluator / g_scheduler_stats_evaluator still
    // point at the dead stack — and the next
    // aura_evaluator_bump_mutation_steal_attempt() / work-steal
    // path dereferences a use-after-return (verified by ASan:
    // stack-use-after-return in bump_mutation_steal_attempt at
    // evaluator.ixx:3130). This is what caused test_issue_226
    // to hang on t.join() — the worker's steal code called
    // bump_mutation_steal_attempt on a dead Evaluator and
    // crashed/hung inside the atomic fetch_add.
    //
    // Use the public member function unbind_yield_hook_evaluator
    // for the g_yield_hook_evaluator slot. The other two slots
    // (g_query_evaluator + g_scheduler_stats_evaluator) are
    // exposed via a similar helper in evaluator_fiber_mutation.cpp.
    unbind_yield_hook_evaluator();
    unbind_query_evaluator();

    // Issue #1367: drop per-evaluator AotState (region/module masks)
    aura_cleanup_aot_state(this);

    // Issue #1352: drop process-wide terminal buffers when the last Evaluator
    // is destroyed (refcount). Concurrent multi-CS tests share the registry.
    primitives_detail::release_terminal_buffer_registry();

    defuse_index_destroy(&defuse_index_);
    modules_.clear();
    module_cache_.clear();
    module_arena_ptrs_.clear();
    module_names_.clear();
    closures_.clear();
    cells_.clear();
    pairs_.clear();
    error_values_.clear();
    opaque_heap_.clear();
    string_heap_.clear();
    {
        // Issue #1720: strategies_ guarded for concurrent fiber access.
        std::unique_lock<std::shared_mutex> lk(strategies_mtx_);
        strategies_.clear();
    }
    // Issue #911: free Evaluator-owned TypeRegistry
    if (owns_type_registry_ && type_registry_) {
        delete static_cast<aura::core::TypeRegistry*>(type_registry_);
        type_registry_ = nullptr;
        owns_type_registry_ = false;
    }
}

// Issue #1720: concurrent-safe timeline / intend-history API.
void Evaluator::timeline_clear() {
    std::unique_lock<std::shared_mutex> lk(timeline_mtx_);
    timeline_.clear();
}
void Evaluator::timeline_push(std::string line) {
    std::unique_lock<std::shared_mutex> lk(timeline_mtx_);
    timeline_.push_back(std::move(line));
}
std::string Evaluator::timeline_snapshot() const {
    std::shared_lock<std::shared_mutex> lk(timeline_mtx_);
    std::string result;
    for (std::size_t i = 0; i < timeline_.size(); ++i)
        result += std::to_string(i) + ":" + timeline_[i] + "\n";
    return result;
}
std::string Evaluator::timeline_tail(std::size_t max_n) const {
    std::shared_lock<std::shared_mutex> lk(timeline_mtx_);
    if (timeline_.empty())
        return "  (empty)\n";
    std::string out;
    std::size_t start = timeline_.size() > max_n ? timeline_.size() - max_n : 0;
    for (std::size_t i = start; i < timeline_.size(); ++i)
        out += "  " + std::to_string(i) + ":" + timeline_[i] + "\n";
    return out;
}
void Evaluator::intend_history_push(IntendRecord rec) {
    std::unique_lock<std::shared_mutex> lk(intend_history_mtx_);
    rec.record_id = next_record_id_++;
    intend_history_.push_back(std::move(rec));
    if (intend_history_.size() > MAX_HISTORY_SIZE)
        intend_history_.erase(intend_history_.begin());
}

} // namespace aura::compiler