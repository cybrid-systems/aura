// evaluator_primitives_registry.cpp — P1-c: register_all_primitives orchestration
// extracted from Evaluator::init_pair_primitives().

module;

#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.evaluator_pure;
import aura.compiler.value;

namespace aura::compiler {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using namespace types;

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

void defuse_index_destroy(void** slot);

void Evaluator::register_all_primitives() {
    primitives_detail::register_type_and_char_primitives(
        prim_registrar());

    primitives_detail::register_pair_and_string_primitives(
        prim_registrar(),
        pairs_, string_heap_, error_values_);

    primitives_detail::register_json_primitives(
        prim_registrar(),
        pairs_, string_heap_);

    primitives_detail::register_list_primitives(
        prim_registrar(),
        pairs_, string_heap_, error_values_, *this);

    primitives_detail::register_vector_and_hash_primitives(
        prim_registrar(),
        pairs_, string_heap_, error_values_, vector_heap_);

    primitives_detail::register_math_regex_and_arithmetic_primitives(
        prim_registrar(),
        pairs_, string_heap_, error_values_);

    primitives_detail::register_reflect_and_type_primitives(
        prim_registrar(),
        pairs_, string_heap_, keyword_table_, type_registry_);

    primitives_detail::register_query_primitives(
        prim_registrar(),
        pairs_, string_heap_, type_registry_,
        [this](const std::string& path) { return resolve_module_path(path); });

    primitives_detail::register_runtime_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_test_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_diagnostic_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_misc_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_file_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_git_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_module_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_control_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_char_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_mutation_primitives(
        prim_registrar(),
        *this);

    primitives_detail::register_auto_evolve_primitives(
        prim_registrar(),
        *this);

    // ═══════════════════════════════════════════════════════════════
    // P6: Query/Transform EDSL 原语
    // ═══════════════════════════════════════════════════════════════

    // (set-code code-string) — Parse code and set as current workspace AST
    // Nodes in workspace AST have stable IDs across query/mutate operations
    // Multi-expression code is automatically wrapped in (begin ...) by the parser.
    // Helper: build structured error value as a pair ("kind" "message")
    // Inline lambda to avoid capture issues — used by set-code, eval-current, etc.
    auto make_error_val = [this](const std::string& kind, const std::string& msg) -> EvalValue {
        auto msg_idx = string_heap_.size();
        string_heap_.push_back(msg);
        auto kind_idx = string_heap_.size();
        string_heap_.push_back(kind);
        auto nil = EvalValue(0);
        // (cons "kind" (cons "message" nil)) → ("kind" "message") as a proper list
        auto msg_pair = make_pair(pairs_.size());
        pairs_.push_back({make_string(msg_idx), nil});
        auto kind_pair = make_pair(pairs_.size());
        pairs_.push_back({make_string(kind_idx), msg_pair});
        return kind_pair;
    };
    std::function<EvalValue(const std::string&, const std::string&)> mev = make_error_val;

    primitives_detail::register_workspace_query_primitives(
        prim_registrar(),
        workspace_mtx_, workspace_flat_, workspace_pool_, type_registry_, keyword_table_, pairs_,
        string_heap_, temp_arena_, tag_arity_index_, [this]() { return canonical_pool(); },
        [this]() { build_tag_arity_index(); }, mev);

    primitives_detail::register_mutate_primitives(
        prim_registrar(),
        *this, mev, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_workspace_primitives(
        prim_registrar(),
        *this, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_eval_primitives(
        prim_registrar(),
        *this, mev, [this]() { defuse_index_destroy(&defuse_index_); });

    primitives_detail::register_eval_observability_primitives(
        prim_registrar(),
        *this);
}

} // namespace aura::compiler
