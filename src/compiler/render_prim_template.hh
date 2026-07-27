// render_prim_template.hh — Issue #1677: high-perf render primitive development template.
//
// Canonical pattern for production terminal/TUI/draw primitives:
//   1. Register with RENDER_PRIMITIVE_META(arity, doc, schema)
//      → perf_tier=hot, category=rendering, render_critical + stable_hot_path
//      → required_effects=kEffectRender (#2136 — auto require_effect at dispatch)
//   2. Body opens with AURA_RENDER_HOT_ENTRY(ev)
//      → enter render hotpath + linear/epoch fence (#1676)
//   3. Prefer frame bump arena / zero-copy / dirty short-circuit (#1559–#1675)
//   4. Bump targeted metrics; never grow SlimSurface public add() for dashboards
//   5. FFI batch hand-off: FFIBatchHotPath::dispatch_batch(..., render_effect_ok)
//      after require_effect(kEffectRender) (#2136)
//   6. Present policy via RenderStrategy (render:set-strategy); kernel paths
//      stay fixed — dirty-aabb / full / skip / auto (#2138)
//
// Agent discoverability: schema string on PrimMeta + facade query:render-* stats.
// Evolution: (mutate :rebind …) for draw logic; (mutate :render-optimize …) for
// pattern-based hot-path preference (no new public prim name).
//
// ── Issue #2051: Render self-evolution contract (Agent closed-loop) ─────────
// Safe mutate window (default 500 ms = deopt throttle window):
//   - Prefer soft dirty / set-body / rebind on evolution-named defines
//     (draw/present/render/tui/terminal/frame/cell/ansi in the name).
//   - Before mutating at 60 fps: (stats:get "query:render-stats") and check:
//       safe-to-mutate == 1
//       agent-health-score >= 60
//       agent-action in {0 hold, 1 optimize-ok, 3 prefer-dirty-delta}
//       agent-action 2 → reduce mutate frequency; 4 → stop mutate this frame
//   - After mutate: re-present, re-query; stamp outcome via
//       (mutate :closed-loop-tick)     // round
//       (mutate :closed-loop-tick 1)   // stable
//       (mutate :closed-loop-tick 2)   // improve
//   - Keep healthy:
//       render-mutate-avg-us << frame budget (~16ms @ 60fps)
//       render-critical-deopt-throttled grows under pressure (not applied storm)
//       frame-time-p99-us stays within budget; dirty-short-circuit-rate-bp rises
//   - LifetimePin / epoch / deopt throttle (#2048/#2050): pin handoffs and
//     JIT keep-native are automatic under soft dirty; Agents must not force
//     hard invalidate of present/draw on the hot path.
// Primary surface: query:render-stats schema-2051 (aggregates memory / JIT /
// dirty / pin / mutate-cost). Drill-down siblings stamped on the same hash.
//
// ── Issue #2214: Prefer tui:present-dirty after sparse mutations ────────────
// After soft dirty / set-body / cell writes on evolution-named defines:
//   - Call (tui:present-dirty) or (tui:present-dirty x0 y0 x1 y1) instead of
//     full (tui:present). Clean dirty AABB short-circuits (0 bytes).
//   - LinearCellGrid.dirty is consumed when that grid is the active buffer.
//   - Query: present-dirty-calls / present-dirty-short-circuit-rate-bp on
//     query:render-stats schema-2214.
//
// ── Issue #2215: RenderFastExit under MutationBoundary + hotpath ───────────
// When outermost MutationBoundaryGuard is entered under AURA_RENDER_HOT_ENTRY
// (in_render_hotpath), success exit uses RenderFastExit:
//   - Skip Full TypedMutationAudit + full linear/dual-path EnvFrame walk
//   - Always: lightweight commit, pin restamp, held clear + unlock
//   - Defer synchronous reemit (epoch notify only); coalesce on next
//     non-render boundary
// Agents must NOT put topology-changing workspace ops inside render-hotpath
// Guards (use GlobalExclusive + full exit). Query: render-fast-exit-* on
// query:mutation-boundary-hold-stats schema-2215.

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

// Issue #2051: default safe mutate window (ms) — matches deopt throttle.
inline constexpr int kRenderSafeMutateWindowMs = 500;
inline constexpr int kRenderAgentClosedLoopIssue = 2051;

#endif // AURA_COMPILER_RENDER_PRIM_TEMPLATE_HH
