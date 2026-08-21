// prim_registrar_scaffold.hh — Issue #2915: official PrimRegistrar + PrimMeta
// scaffolding for human and AI-Agent primitive authors.
//
// ═══════════════════════════════════════════════════════════════════════
// Primitive authoring contract (canonical; do not invent registration styles)
// ═══════════════════════════════════════════════════════════════════════
//
// 1. Placement
//    - Implementation: matching evaluator_primitives_*.cpp in this directory
//      (or a peel registered by an existing register_*_primitives orchestrator)
//    - Orchestration: Evaluator::register_all_primitives() in
//      evaluator_primitives_registry.cpp MUST invoke the register_* group
//
// 2. Registration API (pick ONE; do not invent a third style)
//    A. register_prim(add, ev, name, fn, PrimSpec{...})   ← preferred (#2915)
//       → add(name, fn) + PrimMeta stamp via set_meta_for_name
//       → required_effects: leave 0 for #2152 name-prefix auto-infer, or set
//         explicit bits when the name does not encode the effect
//    B. register_render_hot_prim retired with tui:* (#2217 / #2626)
//    C. Legacy: add(name, fn) alone, or add + set_meta_for_name / DEFINE_PRIMITIVE_META
//       + prim_registrar_with_meta() — still valid for existing TUs; new code
//       should prefer (A) unless a specialized helper applies
//
// 3. PrimMeta defaults (scaffold stamps; override via PrimSpec)
//    - arity: 255 = variadic (match PrimMeta)
//    - pure: true for pure/general; false for I/O / mutate / fiber
//    - safety_flags: kPrimSafetyMutates / kPrimSafetyIo / kPrimSafetyFiber
//    - perf_tier: kPrimPerfNormal default; kPrimPerfHot only with hot-path
//      discipline (and finalize_hot_table after all regs — Evaluator ctor)
//    - security_level: kPrimSecSafe default; sandboxed/privileged when gated
//    - category: "general" | domain string (rendering → use B)
//    - schema + doc: Agent-visible; fill for discovery (#1552 / #480)
//
// 4. Body discipline (see also primitives_detail.h)
//    - Errors: make_primitive_error / PRIM_ERROR / make_merr — not silent void
//      on true failures (docs/stdlib/primitive-error-convention.md)
//    - Mutate: capture [&ev] (+ error counter), MutationBoundaryGuard
//    - Metrics: prefer existing CompilerMetrics atomics; do not add one-off
//      globals without an engine:metrics / query surface
//    - Locks: shared_lock for reads; unique_lock for writes; respect lock order
//
// 5. Discovery (do not invent new surfaces without updating registry #1552 map)
//    - (require "std/primitives" all:) / primitives:help|list|discover
//    - (primitive:describe name) / (query:primitives-meta) /
//      (query:primitives-meta-catalog) / (query:primitive-list-with-meta)
//    - docs/generated/primitives.md + primitives-registry.md
//
// 6. Hot path
//    - hot_map_ / HotEntry / finalize_hot_table remain the only hot-tier path
//    - Scaffold never bypasses Primitives::add / set_meta_for_name
//
// Full prose: docs/stdlib/primitive-authoring-contract.md
//
#ifndef AURA_COMPILER_PRIM_REGISTRAR_SCAFFOLD_HH
#define AURA_COMPILER_PRIM_REGISTRAR_SCAFFOLD_HH

#include "primitives_detail.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aura::compiler {

// Issue #2915: scaffold stamp + observability (mirrors #2217 render helper).
inline constexpr int kPrimRegistrarScaffoldIssue = 2915;
inline constexpr int kPrimRegistrarScaffoldVersion = 1;

inline std::atomic<std::uint64_t>& g_register_prim_scaffold_total() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}

// Lightweight PrimMeta authoring view. Converted to PrimMeta at register_prim.
// Leave required_effects == 0 so #2152 auto-stamps from the primitive name.
struct PrimSpec {
    std::uint8_t arity = 255; // 255 = variadic
    bool pure = true;
    std::uint8_t safety_flags = 0;
    std::uint8_t perf_tier = kPrimPerfNormal;
    std::uint8_t security_level = kPrimSecSafe;
    bool deprecated = false;
    bool render_critical = false;
    bool stable_hot_path = false;
    std::uint16_t required_effects = 0; // 0 → #2152 infer
    bool effect_enforced_in_body = false;
    bool security_exempt = false;
    bool guard_exempt = false;            // #2986 metadata-only mutate:*
    bool requires_mutation_guard = false; // #3197 non-exempt mutate:*
    std::string_view doc{};
    std::string_view category = kPrimCategoryGeneral;
    std::string_view schema{};
};

// ── Common PrimSpec factories (Agent-friendly) ──────────────────────────

[[nodiscard]] inline PrimSpec pure_general(std::uint8_t arity, std::string_view schema,
                                           std::string_view doc) noexcept {
    PrimSpec s;
    s.arity = arity;
    s.pure = true;
    s.safety_flags = 0;
    s.perf_tier = kPrimPerfNormal;
    s.security_level = kPrimSecSafe;
    s.category = kPrimCategoryGeneral;
    s.schema = schema;
    s.doc = doc;
    return s;
}

[[nodiscard]] inline PrimSpec io_general(std::uint8_t arity, std::string_view schema,
                                         std::string_view doc) noexcept {
    PrimSpec s = pure_general(arity, schema, doc);
    s.pure = false;
    s.safety_flags = kPrimSafetyIo;
    s.security_level = kPrimSecSandboxed;
    return s;
}

[[nodiscard]] inline PrimSpec mutate_general(std::uint8_t arity, std::string_view schema,
                                             std::string_view doc) noexcept {
    PrimSpec s = pure_general(arity, schema, doc);
    s.pure = false;
    s.safety_flags = kPrimSafetyMutates;
    s.security_level = kPrimSecSandboxed;
    s.requires_mutation_guard = true; // #3197
    // required_effects left 0 — #2152 stamps from mutate:/workspace: names.
    return s;
}

// Convert PrimSpec → dependent PrimMetaT (complete only inside evaluator module).
template <typename PrimMetaT>
[[nodiscard]] inline PrimMetaT prim_meta_from_spec(const PrimSpec& s) {
    PrimMetaT meta{};
    meta.arity = s.arity;
    meta.pure = s.pure;
    meta.safety_flags = s.safety_flags;
    meta.perf_tier = s.perf_tier;
    meta.security_level = s.security_level;
    meta.deprecated = s.deprecated;
    meta.render_critical = s.render_critical;
    meta.stable_hot_path = s.stable_hot_path;
    meta.required_effects = s.required_effects;
    meta.effect_enforced_in_body = s.effect_enforced_in_body;
    meta.security_exempt = s.security_exempt;
    meta.guard_exempt = s.guard_exempt;
    meta.requires_mutation_guard = s.requires_mutation_guard;
    meta.doc = std::string(s.doc);
    meta.category = std::string(s.category);
    meta.schema = std::string(s.schema);
    return meta;
}

// Issue #2915: unified registration for general (non-render-hot) primitives.
//   add(name, fn) then set_meta_for_name with PrimSpec fields.
//   #2152 auto-infer of required_effects remains the default (required_effects==0).
// EvaluatorT is a template param so this header stays module-safe (no
// ambiguous forward-decl of Evaluator / PrimMeta vs export class).
template <typename PrimRegistrarT, typename EvaluatorT, typename PrimFnT>
inline void register_prim(PrimRegistrarT&& add, EvaluatorT& ev, std::string_view name, PrimFnT&& fn,
                          PrimSpec spec) {
    std::string name_s(name);
    std::forward<PrimRegistrarT>(add)(name_s, std::forward<PrimFnT>(fn));
    using PrimMetaT = std::remove_cvref_t<decltype(ev.primitives().meta_for_slot(0))>;
    auto meta = prim_meta_from_spec<PrimMetaT>(spec);
    ev.primitives().set_meta_for_name(name_s, std::move(meta));
    g_register_prim_scaffold_total().fetch_add(1, std::memory_order_relaxed);
}

// Convenience: register with default empty PrimMeta (still gets #2152 stamp on
// set path via add's empty meta; use when schema/doc are filled later by backfill).
template <typename PrimRegistrarT, typename EvaluatorT, typename PrimFnT>
inline void register_prim(PrimRegistrarT&& add, EvaluatorT& /*ev*/, std::string_view name,
                          PrimFnT&& fn) {
    std::string name_s(name);
    std::forward<PrimRegistrarT>(add)(name_s, std::forward<PrimFnT>(fn));
    g_register_prim_scaffold_total().fetch_add(1, std::memory_order_relaxed);
}

} // namespace aura::compiler

#endif // AURA_COMPILER_PRIM_REGISTRAR_SCAFFOLD_HH
