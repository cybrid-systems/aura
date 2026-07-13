// evaluator_primitives_registry.cpp — P1-c: register_all_primitives orchestration
// aura.compiler.evaluator module partition; orchestrates register_all_primitives().

module;

#include "runtime_shared.h"

#include <cstdlib>

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.evaluator_pure;
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

// P2a surface refactor: full vs s0 registration tiers.
// Default = full (compat). Set AURA_PRIMITIVES=s0 or AURA_FULL_PRIMITIVES=0
// to skip vertical/extended clusters (eda, security, verify-tool, stdlib-review).
// Observability (including engine:metrics) stays in S0 so the facade works.
static bool full_primitives_enabled() noexcept {
    if (const char* p = std::getenv("AURA_PRIMITIVES")) {
        std::string_view v{p};
        if (v == "s0" || v == "S0" || v == "minimal")
            return false;
        if (v == "full" || v == "FULL")
            return true;
    }
    if (const char* f = std::getenv("AURA_FULL_PRIMITIVES")) {
        // 0/false/no → s0; anything else → full
        if (f[0] == '0' || f[0] == 'n' || f[0] == 'N' || f[0] == 'f' || f[0] == 'F')
            return false;
    }
    return true; // default: full surface
}

void Evaluator::register_all_primitives() {
    // ── S0: language core + AI work surface + metrics facade ──
    primitives_detail::register_type_and_char_primitives(prim_registrar());

    auto* primitive_error_counter = primitive_error_counter_ptr();

    primitives_detail::register_pair_and_string_primitives(
        prim_registrar(), *this, pairs_, string_heap_, error_values_, primitive_error_counter);

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

    // Issue #1381: workspace FlatAST + AOT meta + mutation log binary persist
    primitives_detail::register_persist_primitives(prim_registrar(), *this);

    primitives_detail::register_eval_primitives(prim_registrar(), *this, mev,
                                                [this]() { defuse_index_destroy(&defuse_index_); });

    // Observability stays in S0: includes (engine:metrics) / (stats:list|count).
    // Individual query:*-stats remain registered until Phase 5 demotion.
    primitives_detail::register_eval_observability_primitives(prim_registrar(), *this);

    // ── Extended / vertical (S1–S2): gated by full_primitives_enabled() ──
    if (full_primitives_enabled()) {
        primitives_detail::register_stdlib_review_primitives(prim_registrar(), *this);

        // Issue #499: foundational EDA parse/query/mutate primitives module.
        primitives_detail::register_eda_primitives(prim_registrar(), *this);

        primitives_detail::register_security_primitives(prim_registrar(), *this);

        // Issue #443: external simulator tool-calling +
        // structured result parsing primitives.
        primitives_detail::register_verify_tool_primitives(
            prim_registrar(), *this, [this](std::int32_t idx) { return make_string(idx); },
            [this](std::int64_t v) { return make_int(v); }, mev);
    }
}

} // namespace aura::compiler
