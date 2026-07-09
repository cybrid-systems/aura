// evaluator_ctor.cpp — P1-p: Evaluator construction / teardown
// aura.compiler.evaluator module partition.

module;

#include "messaging_bridge.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "primitives_meta.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.type;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
using namespace types;

void Evaluator::init_pair_primitives() {
    register_all_primitives();
}

void Evaluator::build_primitive_slots() {
    // No longer needed — Primitives manages ordering internally
}

Evaluator::Evaluator() {
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

void Evaluator::set_type_registry(void* reg) {
    // Issue #911: drop owned registry before adopting external.
    if (owns_type_registry_ && type_registry_ && type_registry_ != reg) {
        delete static_cast<aura::core::TypeRegistry*>(type_registry_);
        owns_type_registry_ = false;
    }
    type_registry_ = reg;
    owns_type_registry_ = false;
}

Evaluator::~Evaluator() {
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
    strategies_.clear();
    // Issue #911: free Evaluator-owned TypeRegistry
    if (owns_type_registry_ && type_registry_) {
        delete static_cast<aura::core::TypeRegistry*>(type_registry_);
        type_registry_ = nullptr;
        owns_type_registry_ = false;
    }
}

} // namespace aura::compiler