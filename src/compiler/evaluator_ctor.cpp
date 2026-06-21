// evaluator_ctor.cpp — P1-p: Evaluator construction / teardown
// extracted from evaluator_impl.cpp.

module;

#include <functional>
#include <mutex>
#include <string>
#include <tuple>
#include "messaging_bridge.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using namespace types;

namespace primitives_detail {
void register_network_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_type_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_hot_swap_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_compile_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_messaging_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_synthesize_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                                    std::function<void()> destroy_defuse_index);
void register_strategy_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_memory_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev,
                                std::function<void()> destroy_defuse_index);
void register_jit_arena_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
void register_policy_primitives(std::function<void(std::string, PrimFn)> add, Evaluator& ev);
}

void defuse_index_destroy(void** slot);

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