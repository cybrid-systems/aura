# High-performance render primitive template (#1677)

**Issue:** [#1677](https://github.com/cybrid-systems/aura/issues/1677)  
**Code:** `src/compiler/render_prim_template.hh` · `RENDER_PRIMITIVE_META` · `AURA_RENDER_HOT_ENTRY`  
**Production exemplar:** `terminal-present-batch` in `evaluator_primitives_io.cpp`

## Goals

1. Standardize hot render prim registration (tier, schema, linear/epoch safety).  
2. Give AI Agents discoverable `query:render-*` stats and a SlimSurface-safe evolve path.  
3. No new public `add()` names (ceiling **520**).

## Registration template

```cpp
#include "render_prim_template.hh"

auto add_render = [&ev](const char* name, PrimFn fn, PrimMeta meta) {
    ev.primitives().add(name, std::move(fn), std::move(meta));
};

add_render(
    "my-render-op",
    [&ev](std::span<const EvalValue> a) -> EvalValue {
        AURA_RENDER_HOT_ENTRY(ev);   // #1676 fence + hotpath depth
        // … hot body: prefer dirty delta / zero-copy / frame arena …
        return make_int(0);
    },
    RENDER_PRIMITIVE_META(
        /*arity*/ 1,
        "Agent-visible doc string.",
        "(int) -> int"));
```

`RENDER_PRIMITIVE_META` sets:

| Field | Value |
|-------|--------|
| `perf_tier` | hot (`kPrimPerfHot`) |
| `category` | `"rendering"` |
| `render_critical` / `stable_hot_path` | true (deopt throttle #1563) |
| `security_level` | sandboxed |
| `schema` | Agent contract string |

Trusted dispatch (#1676): render-critical skips capability tax in `invoke_prim_with_telemetry` (tree-walker **and** IR Call).

## Hot body checklist

1. **`AURA_RENDER_HOT_ENTRY(ev)`** — linear_post_mutate on newest EnvFrame + epoch refresh.  
2. Prefer **dirty short-circuit** + **present_batch** / **draw_batch**.  
3. Prefer **FrameBumpArena** / zero-copy for ANSI (no per-frame `string_heap` thrash).  
4. Bump existing metrics; dashboards via **`register_stats_impl` only**.

## AI Native query / mutate

| Surface | Access | Purpose |
|---------|--------|---------|
| `query:render-stats` | `(stats:get …)` | Present/diff/draw closed loop |
| `query:render-memory-stats` | facade | Frame arena + live grids |
| `query:render-hotpath-stats` | facade | Dirty/delta/JIT under mutate |
| `query:render-closure-stats` | facade **#1677** | render_critical meta + dispatch fences |
| `query:render-buffer-stats` | facade **#1677** | TermBuf create/live/diff |
| `query:render-evolution-stats` | facade **#1677** | rebind/optimize counters |
| `(mutate :render-optimize [buf])` | existing `mutate` | Prefer deopt throttle + fence |
| `(mutate :rebind name code)` | existing | Evolve draw fn; bumps rebind when name is render-like |

**Name heuristic** for evolution rebind (`aura_is_render_evolution_name`):  
`render`, `draw`, `present`, `tui`, `terminal`, `frame`, `cell`, `ansi`.

## Agent evolution sketch

```scheme
(set-code "(define (draw-frame b)
  (terminal-set-cell b 0 0 65 7 0)
  (terminal-present-batch b 1))")

;; Prefer dirty-delta / throttle
(mutate :render-optimize)

;; Hot-swap draw body (counts as render evolution rebind)
(mutate :rebind "draw-frame"
  "(lambda (b)
     (render-draw-batch b 0 0 66 7 0)
     (terminal-present-batch b 1))"
  "use draw-batch")

(hash-ref (stats:get "query:render-evolution-stats") "rebind-total")
(hash-ref (stats:get "query:render-closure-stats") "dispatch-fast-total")
```

## Non-goals

- New public `term-*` / `mutate:render-optimize` top-level names.  
- Full AST rewrite engine for draw graphs (use `mutate:rebind` / `:set-body`).  
- SoA TermCell redesign.
