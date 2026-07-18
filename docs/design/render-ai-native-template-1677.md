# AI Native render primitive template + evolution (#1677)

**Issue:** [#1677](https://github.com/cybrid-systems/aura/issues/1677)  
**Builds on:** #1559–#1563 · #1673–#1676 (present, obs, memory, dispatch fences)  
**Status:** P2 usability — template + facade query/mutate for Agents.

## Problem

Agents lacked a documented render-prim scaffold and first-class
`query:render-{closure,buffer,evolution}-stats` / optimize hooks without
blowing SlimSurface (520).

## Deliverables

1. **`docs/render-primitive-template.md`** + **`render_prim_template.hh`**  
   (`AURA_RENDER_HOT_ENTRY`, `aura_is_render_evolution_name`, phase stamps)
2. **Production exemplar** — `terminal-present-batch` / `render-draw-batch` /
   `tui:present` use the template macros.
3. **Facade stats** (register_stats_impl only):
   - `query:render-closure-stats` schema **1677**
   - `query:render-buffer-stats` schema **1677**
   - `query:render-evolution-stats` schema **1677**
   - `mutate:render-optimize` (side-effect facade + `(mutate :render-optimize)`)
4. **Rebind hook** — successful `mutate:rebind` of render-like names bumps
   `render_evolution_rebind_total`.

## SlimSurface

No new public `add()` names. Optimize is `(mutate :render-optimize …)` or
`(stats:get "mutate:render-optimize")`.

## Tests

`tests/test_render_ai_native_template_1677.cpp`

## Non-goals

- New public `query:render-*` via `add()`.  
- Automatic multi-pass draw AST rewrites.
