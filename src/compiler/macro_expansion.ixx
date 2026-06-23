// Issue #265: hygienic macro cloning + inner expansion helpers.
// Extracted from evaluator_eval_flat.cpp for isolated testing.
// No Evaluator state — operates only on FlatAST + StringPool.

module;

#include <string>
#include <unordered_map>
#include <vector>

export module aura.compiler.macro_expansion;

import std;
import aura.core.ast;

namespace aura::compiler::macro_exp {

export struct MacroExpansionDef {
    std::vector<std::string> params;
    bool dotted = false;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
    aura::ast::NodeId body_id = aura::ast::NULL_NODE;
};

// Clone a FlatAST subtree with optional param substitution and
// hygienic renaming (name_map). hyg_ctr is per-call (instance-local).
export aura::ast::NodeId clone_macro_body(
    aura::ast::FlatAST& target, aura::ast::StringPool& target_pool, aura::ast::FlatAST& source,
    aura::ast::StringPool& source_pool, aura::ast::NodeId body_id,
    const std::unordered_map<std::string, aura::ast::NodeId>* subst = nullptr,
    std::unordered_map<std::string, std::string>* name_map = nullptr,
    aura::ast::SyntaxMarker cloned_marker = aura::ast::SyntaxMarker::User);

export aura::ast::NodeId expand_inner_macros(
    aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root, int depth,
    int max_depth, const std::unordered_map<std::string, MacroExpansionDef>& macros);

export aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeId root, int max_passes = 32);

} // namespace aura::compiler::macro_exp
