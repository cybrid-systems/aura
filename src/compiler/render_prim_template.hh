// render_prim_template.hh — Issue #1677: high-perf render primitive development template.
//
// Canonical pattern for production terminal/TUI/draw primitives:
//   1. Register with RENDER_PRIMITIVE_META(arity, doc, schema)
//      → perf_tier=hot, category=rendering, render_critical + stable_hot_path
//   2. Body opens with AURA_RENDER_HOT_ENTRY(ev)
//      → enter render hotpath + linear/epoch fence (#1676)
//   3. Prefer frame bump arena / zero-copy / dirty short-circuit (#1559–#1675)
//   4. Bump targeted metrics; never grow SlimSurface public add() for dashboards
//
// Agent discoverability: schema string on PrimMeta + facade query:render-* stats.
// Evolution: (mutate :rebind …) for draw logic; (mutate :render-optimize …) for
// pattern-based hot-path preference (no new public prim name).

#ifndef AURA_COMPILER_RENDER_PRIM_TEMPLATE_HH
#define AURA_COMPILER_RENDER_PRIM_TEMPLATE_HH

#include "primitives_detail.h"

#include <string_view>

// Issue #1677: RAII hot entry (linear/epoch fence + hotpath depth).
// Expands to a unique guard name per call site.
#define AURA_RENDER_HOT_ENTRY(ev)                                                                  \
    ::aura::compiler::Evaluator::RenderHotEntryGuard AURA_RENDER_HOT_ENTRY_JOIN(                   \
        _aura_render_hot_entry_, __LINE__)(ev)

#define AURA_RENDER_HOT_ENTRY_JOIN(a, b) AURA_RENDER_HOT_ENTRY_JOIN2(a, b)
#define AURA_RENDER_HOT_ENTRY_JOIN2(a, b) a##b

// Issue #1677: detect Agent-facing render evolution names (rebind / optimize).
[[nodiscard]] inline bool aura_is_render_evolution_name(std::string_view name) noexcept {
    if (name.empty())
        return false;
    auto has = [&](std::string_view needle) { return name.find(needle) != std::string_view::npos; };
    return has("render") || has("draw") || has("present") || has("tui") || has("terminal") ||
           has("frame") || has("cell") || has("ansi");
}

// Phase stamp for query:render-evolution-stats / template docs.
inline constexpr int kRenderPrimTemplateIssue = 1677;
inline constexpr int kRenderPrimTemplatePhase = 1;

#endif // AURA_COMPILER_RENDER_PRIM_TEMPLATE_HH
