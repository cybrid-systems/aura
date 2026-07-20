// evaluator_primitives_registry.cpp — P1-c: register_all_primitives orchestration
// aura.compiler.evaluator module partition; orchestrates register_all_primitives().
//
// ═══════════════════════════════════════════════════════════════════════
// #1552 — Agent / developer discoverability (canonical registration map)
// ═══════════════════════════════════════════════════════════════════════
// This file is the CENTRAL entry for all PrimRegistrar callbacks.
// Implementation bodies live in evaluator_primitives_*.cpp (+ verticals).
//
// Agent discovery surfaces (do not invent new ones without updating these):
//   - (require "std/primitives" all:) → primitives:help / :list / :discover
//   - (require "std/INDEX" all:) → (stdlib:help "primitives")
//   - (primitive:describe name) / (query:primitive-list-with-meta)
//   - (query:primitives-meta) / (query:primitives-meta-catalog)
//   - docs/generated/primitives.md + docs/generated/primitives-registry.md
//
// Registration groups (order below = boot order):
//   S0 core: type-char, pair-string, json, list, vector-hash, math,
//            reflect, query, runtime, test, diagnostic, misc, file, git,
//            module, control, char, mutation, auto-evolve
//   EDSL:    workspace-query, mutate, workspace, persist, eval, observability
//   S1–S2:   stdlib-review, eda, security, verify-tool (full_primitives only)
//
// Fiber / mutation integration points for Agents:
//   mutation + mutate + workspace-query + control (+ safe-yield / boundary)
// ═══════════════════════════════════════════════════════════════════════

module;

#include "runtime_shared.h"
#include "primitives_detail.h"

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

void Evaluator::register_all_primitives() {
    using primitives_detail::full_primitives_enabled;
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

    // Issue #1970: git-* gated by AURA_ENABLE_GIT inside register_git_primitives.
    primitives_detail::register_git_primitives(prim_registrar(), *this);

    primitives_detail::register_module_primitives(prim_registrar(), *this);

    primitives_detail::register_control_primitives(prim_registrar(), *this);

    primitives_detail::register_char_primitives(prim_registrar(), *this);

    primitives_detail::register_mutation_primitives(prim_registrar(), *this);

    // Issue #1969: auto-evolve-* gated by AURA_ENABLE_AUTO_EVOLVE inside
    // register_auto_evolve_primitives (agent:/strategy: co-adds stay on).
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

    // Eval-side bulk observability (full only). Metrics facade is registered
    // from register_jit_arena_primitives in the ctor (always / s0-facade).
    primitives_detail::register_eval_observability_primitives(prim_registrar(), *this);

    // ── Extended / vertical (S1–S2): gated by full_primitives_enabled() ──
    if (full_primitives_enabled()) {
        primitives_detail::register_stdlib_review_primitives(prim_registrar(), *this);

        // Issue #499: foundational EDA parse/query/mutate primitives module.
        // Issue #1968: gated by AURA_ENABLE_EDA inside register_eda_primitives
        // (no-op when commercial EDA vertical is disabled).
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
