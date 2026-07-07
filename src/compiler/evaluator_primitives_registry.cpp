// evaluator_primitives_registry.cpp — P1-c: register_all_primitives orchestration
// aura.compiler.evaluator module partition; orchestrates register_all_primitives().

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.evaluator_pure;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
using namespace types;

void Evaluator::register_all_primitives() {
    primitives_detail::register_type_and_char_primitives(prim_registrar());

    auto* primitive_error_counter = primitive_error_counter_ptr();

    primitives_detail::register_pair_and_string_primitives(prim_registrar(), pairs_, string_heap_,
                                                           error_values_, primitive_error_counter);

    primitives_detail::register_json_primitives(prim_registrar(), pairs_, string_heap_);

    primitives_detail::register_list_primitives(prim_registrar(), pairs_, string_heap_,
                                                error_values_, *this);

    primitives_detail::register_vector_and_hash_primitives(prim_registrar(), pairs_, string_heap_,
                                                           error_values_, vector_heap_,
                                                           primitive_error_counter);

    primitives_detail::register_math_regex_and_arithmetic_primitives(
        prim_registrar(), pairs_, string_heap_, error_values_, primitive_error_counter, *this);

    primitives_detail::register_reflect_and_type_primitives(prim_registrar(), pairs_, string_heap_,
                                                            keyword_table_, type_registry_);

    primitives_detail::register_query_primitives(
        prim_registrar(), pairs_, string_heap_, type_registry_,
        [this](const std::string& path) { return resolve_module_path(path); }, *this);

    primitives_detail::register_runtime_primitives(prim_registrar(), *this);

    primitives_detail::register_test_primitives(prim_registrar(), *this);

    primitives_detail::register_diagnostic_primitives(prim_registrar(), *this);

    primitives_detail::register_misc_primitives(prim_registrar(), *this);

    primitives_detail::register_file_primitives(prim_registrar(), *this);

    primitives_detail::register_git_primitives(prim_registrar(), *this);

    primitives_detail::register_module_primitives(prim_registrar(), *this);

    primitives_detail::register_control_primitives(prim_registrar(), *this);

    primitives_detail::register_char_primitives(prim_registrar(), *this);

    primitives_detail::register_mutation_primitives(prim_registrar(), *this);

    primitives_detail::register_auto_evolve_primitives(prim_registrar(), *this);

    // ═══════════════════════════════════════════════════════════════
    // P6: Query/Transform EDSL 原语
    // ═══════════════════════════════════════════════════════════════

    // (set-code code-string) — Parse code and set as current workspace AST
    // Nodes in workspace AST have stable IDs across query/mutate operations
    // Multi-expression code is automatically wrapped in (begin ...) by the parser.
    std::function<EvalValue(const std::string&, const std::string&)> mev =
        [this](const std::string& kind, const std::string& msg) { return make_merr(kind, msg); };

    primitives_detail::register_workspace_query_primitives(
        prim_registrar(), workspace_mtx_, workspace_flat_, workspace_pool_, type_registry_,
        keyword_table_, pairs_, string_heap_, temp_arena_, tag_arity_index_,
        // Issue #371: pass the (tag, arity) index mutex
        // so query:pattern's fast path read can take a
        // shared_lock while build/invalidate take unique
        // locks internally (the member functions lock
        // themselves — passing the reference lets the
        // reader acquire a paired shared_lock).
        tag_arity_index_mtx_, [this]() { return canonical_pool(); },
        [this]() { build_tag_arity_index(); }, mev, *this);

    primitives_detail::register_mutate_primitives(
        prim_registrar(), *this, mev, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_workspace_primitives(
        prim_registrar(), *this, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_eval_primitives(prim_registrar(), *this, mev,
                                                [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_eval_observability_primitives(prim_registrar(), *this);

    // Issue #499: foundational EDA parse/query/mutate primitives module.
    primitives_detail::register_eda_primitives(prim_registrar(), *this);

    primitives_detail::register_security_primitives(prim_registrar(), *this);

    // Issue #443: external simulator tool-calling +
    // structured result parsing primitives.
    primitives_detail::register_verify_tool_primitives(
        prim_registrar(), *this, [this](std::int32_t idx) { return make_string(idx); },
        [this](std::int64_t v) { return make_int(v); }, mev);
}

} // namespace aura::compiler
