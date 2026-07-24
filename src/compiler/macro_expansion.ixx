// Issue #265: hygienic macro cloning + inner expansion helpers.
// Extracted from evaluator_eval_flat.cpp for isolated testing.
// No Evaluator state — operates only on FlatAST + StringPool.

module;

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/transparent_string_hash.hh" // C++20 heterogeneous-lookup hash for std::unordered_map<std::string, V>

export module aura.compiler.macro_expansion;

import std;
import aura.core.ast;

namespace aura::compiler::macro_exp {

// Issue #365: MAX_HYGIENE_DEPTH — upper bound on recursive
// clone_macro_body nesting. Exported so tests + other modules
// can read it (and so operators can detect when their macros
// are close to the limit via compile-time diagnostic).
//
// Issue #1392: raised from 256 → 1024. Modern Linux default
// thread stack is 8MB; 256 was conservative (only pathological
// inputs triggered the silent NULL_NODE fallback). When the
// limit IS exceeded the fallback is still observable via
// `g_macro_origin_provenance_errors` atomic counter (read
// through `(compile:macro-origin-provenance-errors)` primitive).
// Returning a NodeId-typed merr would require changing the
// function signature (invasive); observability path is the
// scope-limited fix.
export constexpr int MAX_HYGIENE_DEPTH = 1024;

// Issue #1245 Phase 1: concurrent macro-clone hygiene counters (defined in .cpp).
export extern std::atomic<std::uint64_t> g_macro_clone_concurrent_fiber_total;
export extern std::atomic<std::uint64_t> g_macro_clone_hygiene_dirty_total;

// Issue #1247–#1248 Phase 1: macro-origin provenance + hygiene tracer.
export extern std::atomic<std::uint64_t> g_macro_origin_provenance_errors;
export extern std::atomic<std::uint64_t> g_hygiene_tracer_expansions;
export extern std::atomic<std::uint64_t> g_hygiene_tracer_depth_max;

// Issue #2018: rest-param gensyms (`__rest_<name>_<n>`) applied in
// clone_macro_body pre-scan / rename path.
export extern std::atomic<std::uint64_t> g_macro_rest_param_hygiene_total;

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
    const std::unordered_map<std::string, aura::ast::NodeId, aura::core::TransparentStringHash,
                             std::equal_to<>>* subst = nullptr,
    std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                       std::equal_to<>>* name_map = nullptr,
    aura::ast::SyntaxMarker cloned_marker = aura::ast::SyntaxMarker::User);

export aura::ast::NodeId expand_inner_macros(
    aura::ast::FlatAST* flat, aura::ast::StringPool* pool, aura::ast::NodeId root, int depth,
    int max_depth,
    const std::unordered_map<std::string, MacroExpansionDef, aura::core::TransparentStringHash,
                             std::equal_to<>>& macros);

export aura::ast::NodeId macro_expand_all(aura::ast::FlatAST& flat, aura::ast::StringPool& pool,
                                          aura::ast::NodeId root, int max_passes = 32);

} // namespace aura::compiler::macro_exp
