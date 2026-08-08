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

// Issue #365: MAX_HYGIENE_DEPTH — hard safety ceiling on recursive
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
//
// Issue #2101: MAX_HYGIENE_DEPTH remains the immutable hard
// ceiling. Runtime / capability / MacroSelfEvo policy may only
// *tighten* the effective limit (never raise past this).
export constexpr int MAX_HYGIENE_DEPTH = 1024;

// Issue #2101: process-wide runtime hygiene depth/pass caps.
// Scope: process-wide atomics (not per-tenant / per-fiber) — a
// concurrent set on one fiber is immediately visible to expand
// on another. Capability (MacroSelfEvo) can only tighten further.
//
// set_hygiene_depth_cap(n): n must be in [1, MAX_HYGIENE_DEPTH];
// returns false if rejected (above hard ceiling or n < 1).
// set_hygiene_pass_cap(n): n==0 clears (no runtime pass clamp);
// n>0 clamps max_passes; returns false if n < 0.
// Defaults: depth=MAX, pass=0 (no runtime pass clamp).
export int hard_hygiene_depth_limit() noexcept;    // == MAX_HYGIENE_DEPTH
export int runtime_hygiene_depth_cap() noexcept;   // process-wide setter value
export int runtime_hygiene_pass_cap() noexcept;    // 0 = no runtime pass clamp
export bool set_hygiene_depth_cap(int n) noexcept; // reject if n∉[1,MAX]
export bool set_hygiene_pass_cap(int n) noexcept;  // reject if n<0; 0 clears
export void reset_hygiene_runtime_caps_for_test() noexcept;
// Live effective limit that the next expand / clone will enforce:
// min(hard, runtime depth cap, capability max_depth when tightening).
export int effective_hygiene_depth_limit() noexcept;
// Live effective pass cap: min of runtime pass cap (if >0) and
// capability max_expansion_passes (if >0); 0 means "no extra clamp".
export int effective_hygiene_pass_cap() noexcept;

// Issue #1245 Phase 1: concurrent macro-clone hygiene counters (defined in .cpp).
export extern std::atomic<std::uint64_t> g_macro_clone_concurrent_fiber_total;
export extern std::atomic<std::uint64_t> g_macro_clone_hygiene_dirty_total;
// Issue #2021: live occupancy + peak concurrent top-level clone_macro_body.
export extern std::atomic<std::uint64_t> g_macro_clone_in_flight;
export extern std::atomic<std::uint64_t> g_macro_clone_concurrent_peak;

// Issue #1247–#1248 Phase 1: macro-origin provenance + hygiene tracer.
export extern std::atomic<std::uint64_t> g_macro_origin_provenance_errors;
export extern std::atomic<std::uint64_t> g_hygiene_tracer_expansions;
export extern std::atomic<std::uint64_t> g_hygiene_tracer_depth_max;

// Issue #1652 / #2020: expand success + hygiene-violation totals (Agent diagnostics).
export extern std::atomic<std::uint64_t> g_macro_expansion_total;
export extern std::atomic<std::uint64_t> g_macro_introduced_nodes_created_total;
export extern std::atomic<std::uint64_t> g_hygiene_violation_in_macro_expand_total;
// Issue #2242 build cascade: pre-existing latent — these atomics were defined
// inline in macro_expansion.cpp but not exported via the module interface, so
// other TUs (e.g. evaluator_primitives_obs_eval.cpp) hit 'not declared' errors
// after module BMI regeneration. Forward-declared here to fix the visibility.
export extern std::atomic<std::uint64_t> g_macro_self_evo_fiber_violation_budget;
export extern std::atomic<std::uint64_t> g_macro_self_evo_fiber_violation_deny_total;

// Issue #2018 / #2169: rest-param gensyms (`__rest_<name>_<serial>`) applied
// in clone_macro_body pre-scan / rename path. Process-wide serial for fiber
// concurrent uniqueness (#2169).
export extern std::atomic<std::uint64_t> g_macro_rest_param_hygiene_total;
// Issue #2169: rest rename skipped while allow_rest_hygiene is on (incomplete
// path / depth fallback) — Agent-visible hard signal vs successful total.
export extern std::atomic<std::uint64_t> g_macro_rest_param_hygiene_incomplete_total;
// Process-wide gensym serial (never resets; concurrent expand unique).
export extern std::atomic<std::uint64_t> g_macro_rest_gensym_serial;

// Issue #2019: post-expand MacroIntroduced generation restamp calls.
export extern std::atomic<std::uint64_t> g_macro_restamp_after_flat_total;
// Issue #2096: per-cloned-subtree MacroIntroduced restamp counter
// (subtree-local coherence at expand exit + critical mutate entry).
export extern std::atomic<std::uint64_t> g_macro_expand_mutate_restamp_total;
// Issue #2098: per-cloned-subtree schema-cache + dirty/provenance
// stamp counter (clone_macro_body walk visibility for rest-param
// + nested qq + schema_cache copy paths).
export extern std::atomic<std::uint64_t> g_macro_schema_cache_dirty_stamped_total;
// Issue #2169 nested qq rest-param hits + schema_cache rest stamp totals
// (defs in macro_expansion.cpp; tests/query surfaces import these).
export extern std::atomic<std::uint64_t> g_macro_rest_param_nested_qq_hits_total;
export extern std::atomic<std::uint64_t> g_macro_schema_cache_rest_stamped_total;
// Issue #2097: per-fiber hygiene query counters (Agent-throttlable
// surface under concurrent self-evo / fiber-steal; #2021's depth /
// concurrent counters are process-wide; #2097 adds per-fiber-id keyed
// map + per-fiber query counters).
export extern std::atomic<std::uint64_t> g_fiber_hygiene_query_total;
export extern std::atomic<std::uint64_t> g_fiber_hygiene_violation_per_fiber_total;

// Issue #2023: MacroSelfEvo capability gate observability.
export extern std::atomic<std::uint64_t> g_macro_self_evo_denied_total;
export extern std::atomic<std::uint64_t> g_macro_self_evo_allowed_total;
export extern std::atomic<std::uint64_t> g_macro_self_evo_pass_clamp_total;
export extern std::atomic<std::uint64_t> g_macro_self_evo_depth_clamp_total;
// Issue #2243: surfaces alongside the existing 4 macros.
export extern std::atomic<std::uint64_t> g_macro_self_evo_force_hygienic_denied_total;
export extern std::atomic<std::uint64_t> g_macro_self_evo_gensym_map_size_exceeded_total;
// Issue #2804: clone-walk rename_binding ceiling denials (vs pre-scan).
export extern std::atomic<std::uint64_t> g_clone_walk_gensym_ceiling_exceeded_total;

export struct MacroExpansionDef {
    std::vector<std::string> params;
    bool dotted = false;
    aura::ast::FlatAST* flat = nullptr;
    aura::ast::StringPool* pool = nullptr;
    aura::ast::NodeId body_id = aura::ast::NULL_NODE;
};

// Issue #2097: per-fiber hygiene metrics for Agent query under concurrent
// self-evo / fiber-steal. Each fiber's expansion state (depth snapshot,
// hygiene violations observed on that fiber, gensym map occupancy) is
// kept per-fiber-id so Agents can throttle / diagnose without log scraping.
// Zero-cost when fiber_id not requested — the per-fiber map is only
// populated on expand entry/exit events (same hot path that already bumps
// the global atomics); the hash lookup is amortized.
export struct FiberHygieneStats {
    int depth = 0;
    std::uint64_t violations = 0;
    std::size_t gensym_map_size = 0;
};

// Snapshot a fiber's accumulated hygiene state. Returns a default-constructed
// FiberHygieneStats (all zeros) if the fiber_id has no recorded expand events.
export [[nodiscard]] FiberHygieneStats get_fiber_hygiene_metrics(std::uint32_t fiber_id) noexcept;

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
