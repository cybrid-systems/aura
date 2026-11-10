// evaluator_primitives_registry.cpp — P1-c: register_all_primitives orchestration
// aura.compiler.evaluator module partition; orchestrates register_all_primitives().
//
// ═══════════════════════════════════════════════════════════════════════
// #1552 — Agent / developer discoverability (canonical registration map)
// #2915 — PrimRegistrar + PrimMeta scaffolding (do not invent styles)
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
// Registration styles (do NOT invent new ones):
//   - Preferred general: register_prim(add, ev, name, fn, PrimSpec)
//     → src/compiler/prim_registrar_scaffold.hh (#2915 / #2996)
//   - register_render_hot_prim retired with tui:* (#2217 / #2626)
//   - Legacy add / prim_registrar_with_meta / DEFINE_PRIMITIVE_META still OK
//     for unmigrated TUs; core TUs migrated (#2996): list, math+regex+arith,
//     json, pair+string, vector+hash. New prims must use register_prim unless
//     a specialized helper applies. Contract: docs/stdlib/primitive-authoring-contract.md
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

namespace {

    // Issue #5097: v0.2.1 unregistered these prefixes. The sweep is a backstop:
    // a reintroduced ast:/workspace:/… name is flagged instead of becoming a
    // silent second entry point for (query …) / (mutate …).
    struct RetiredPrefix5009 {
        std::string_view prefix;
        std::string_view verb; // the verb whose op now names this primitive
    };

    constexpr RetiredPrefix5009 kRetiredPrefixes5009[] = {
        {"ast:", "query"},
        {"workspace:", "query"},
        {"dirty:", "query"},
        {"mutation-log:", "query"},
        {"syntax:", "query"},
        {"ws:", "mutate"},
        {"fiber:overlay-", "mutate"},
        {"fiber:spawn-backend", "query"},
    };

    // Issue #5006: single-use prefixes are not kernel. They stay registered for
    // one release cycle; the operation is an engine:metrics / mutate / query op or
    // a std library function.
    constexpr std::string_view kRetiredSingles5006[] = {
        "session:", "closure:",     "hot-swap:", "string-pool:", "scheduler:", "evaluator:",
        "runtime:", "ir-cache-v2:", "jit:",      "hygiene:",     "arena:",     "orchestration:",
    };

    // Issue #5006: bare names that are not kernel. Each one keeps its
    // registration for one release cycle (the #5008 alias window) and carries
    // the replacement in its doc hint, so the walk to the new spelling is
    // mechanical. A name is listed here only once the replacement exists:
    //   - a std library function (std/diagnose, std/test), or
    //   - a query / mutate op over engine state.
    // docs/stdlib/primitive-authoring-contract.md carries the keep/leave table
    // and docs/stdlib/primitive-tiers.json the same flag, so docs, runtime and
    // the tier gate agree.
    struct RetiredBare5006 {
        std::string_view name;
        std::string_view replaces;
    };

    // The bare spellings are unregistered in v0.2.1. Listing them here only
    // flags a re-registration; it must not match the query:/mutate: bodies.
    constexpr RetiredBare5006 kRetiredBare5006[] = {
        {"diagnose", "(require \"std/diagnose\" all:) then (diagnose error-string)"},
        {"check-success", "(require \"std/test\" all:) then (check-success output expected)"},
        {"api-reference", "(query :api-reference)"},
        {"arena-offset", "(query :arena-offset)"},
        {"auto-rollback-on-panic",
         "(mutate :auto-rollback-on-panic #t|#f) / (query :auto-rollback-on-panic)"},
        {"define-strategy", "(mutate :define-strategy name body …)"},
        {"edsl:define-struct", "(mutate :define-struct name doc schema)"},
    };

    // Issue #5006: the five public orchestration verbs. Everything else in orch:
    // moves to std/orch.
    bool is_orch_verb_5006(std::string_view name) noexcept {
        return name == "orch:spawn" || name == "orch:send" || name == "orch:recv" ||
               name == "orch:join" || name == "orch:cancel";
    }

    void retire_name(Primitives& prims, std::size_t slot, std::string_view hint) {
        const std::string& name = prims.name_for_slot(slot);
        PrimMeta meta = prims.meta_for_slot(slot);
        meta.deprecated = true;
        meta.category = "deprecated";
        if (meta.doc.empty() || meta.doc.find("DEPRECATED") == std::string::npos)
            meta.doc = meta.doc.empty() ? std::string(hint) : std::string(hint) + ". " + meta.doc;
        prims.set_meta_for_name(name, std::move(meta));
    }

    void mark_retired_surfaces(Primitives& prims) {
        const std::size_t slots = prims.slot_count();
        for (std::size_t slot = 0; slot < slots; ++slot) {
            const std::string& name = prims.name_for_slot(slot);
            const std::string_view view(name);
            bool retired = false;
            for (const auto& entry : kRetiredPrefixes5009) {
                if (!view.starts_with(entry.prefix))
                    continue;
                retire_name(prims, slot,
                            "DEPRECATED (#5009): prefer (" + std::string(entry.verb) +
                                " …) — this name retires after the v0.2.0 alias window.");
                retired = true;
                break;
            }
            if (retired)
                continue;
            if (view.starts_with("orch:") && !is_orch_verb_5006(view)) {
                // Issue #5118: std/orch never grew these verbs. The registered
                // name is the function to call. orch:parallel-intend is the
                // one alias whose body is a different live primitive.
                const std::string replacement = (view == "orch:parallel-intend")
                                                    ? std::string("parallel-intend")
                                                    : std::string(view);
                retire_name(prims, slot,
                            "DEPRECATED (#5118): call (" + replacement +
                                " ...). This registration is the function; std/orch does not "
                                "replace it.");
                continue;
            }
            for (const auto& bare : kRetiredBare5006) {
                if (view != bare.name)
                    continue;
                retire_name(prims, slot,
                            "DEPRECATED (#5006): use " + std::string(bare.replaces) +
                                " — this name retires after the v0.2.0 alias window.");
                retired = true;
                break;
            }
            if (retired)
                continue;
            for (const auto single : kRetiredSingles5006) {
                if (!view.starts_with(single))
                    continue;
                retire_name(
                    prims, slot,
                    "DEPRECATED (#5006): single-use prefix; use an engine:metrics / mutate / "
                    "query op or the std library.");
                break;
            }
        }
    }

} // namespace

void Evaluator::register_all_primitives() {
    using primitives_detail::full_primitives_enabled;
    // ── S0: language core + AI work surface + metrics facade ──
    primitives_detail::register_type_and_char_primitives(prim_registrar(), *this);

    auto* primitive_error_counter = primitive_error_counter_ptr();

    primitives_detail::register_pair_and_string_primitives(
        prim_registrar(), *this, pairs_, string_heap_, error_values_, primitive_error_counter);

    primitives_detail::register_json_primitives(prim_registrar(), pairs_, string_heap_,
                                                error_values_, primitive_error_counter, *this);

    primitives_detail::register_list_primitives(prim_registrar(), pairs_, string_heap_,
                                                error_values_, *this);

    primitives_detail::register_vector_and_hash_primitives(prim_registrar(), pairs_, string_heap_,
                                                           error_values_, vector_heap_,
                                                           primitive_error_counter, *this);

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

    // Issue #4715: per-fiber COW overlay for env bindings (fiber:overlay-*).
    // Registered in the always-on EDSL group: an overlay is language
    // semantics, not an observability extra, and the Strict-sandbox gate is
    // the fiber capability (same as fiber:spawn).
    primitives_detail::register_fiber_cow_primitives(prim_registrar(), *this);

    primitives_detail::register_eval_primitives(prim_registrar(), *this, mev,
                                                [this]() { defuse_index_destroy(&defuse_index_); });

    // Eval-side bulk observability (full only). Metrics facade is registered
    // from register_jit_arena_primitives in the ctor (always / s0-facade).
    primitives_detail::register_eval_observability_primitives(prim_registrar(), *this);

    // ── Extended / vertical (S1–S2): gated by full_primitives_enabled() ──
    if (full_primitives_enabled()) {
        primitives_detail::register_stdlib_review_primitives(prim_registrar(), *this);

        // Issue #499 / #2629: eda:* C++ vertical + lib/std/eda.aura retired.

        primitives_detail::register_security_primitives(prim_registrar(), *this);

        // Issue #443: external simulator tool-calling +
        // structured result parsing primitives.
        primitives_detail::register_verify_tool_primitives(
            prim_registrar(), *this, [this](std::int32_t idx) { return make_string(idx); },
            [this](std::int64_t v) { return make_int(v); }, mev);
    }

    // Issue #5009: after every group has registered, retire the duplicate
    // read/write prefixes from the public surface (deprecated, not deleted).
    mark_retired_surfaces(primitives_);
}

void Evaluator::retire_public_surface_names() {
    // Issue #5006: the ctor registers the agent / EDSL groups after
    // register_all_primitives(), so the sweep runs again once every name
    // exists. Idempotent: retire_name only writes the meta it already set.
    mark_retired_surfaces(primitives_);
}

} // namespace aura::compiler
