// evaluator_ctor.cpp — P1-p: Evaluator construction / teardown
// aura.compiler.evaluator module partition.

module;

#include "messaging_bridge.h"
#include "observability_metrics.h"
#include "primitives_meta.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
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

    ffi_runtime_.register_primitives(
        prim_registrar(),
        &string_heap_, &opaque_heap_, &coverage_counters_);

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

    // Issue #697: backfill SV/EDA PrimMeta after compile partition registers
    // eda:run-verification-feedback and eda:demo-sv-self-evolution.
    backfill_eda_sv_primitive_meta();
}

void Evaluator::backfill_eda_sv_primitive_meta() {
    primitives_.set_meta_for_name(
        "eda:weaken-property",
        PrimMeta{.arity = 2,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .doc = "Weaken SVA property with disable-iff clause on Property nodes.",
                 .category = "sva",
                 .schema = "(int string) -> bool"});
    primitives_.set_meta_for_name(
        "eda:add-coverpoint-bin",
        PrimMeta{.arity = 2,
                 .pure = false,
                 .safety_flags = kPrimSafetyMutates,
                 .doc = "Append a named bin to a structured Coverpoint AST node.",
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
    if (auto* m = static_cast<CompilerMetrics*>(compiler_metrics_))
        m->primitive_eda_meta_backfill_total.fetch_add(
            4, std::memory_order_relaxed);
}

Evaluator::~Evaluator() {
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
}

} // namespace aura::compiler