# Production terminal rendering closed loop (#1673)

**Issue:** [#1673](https://github.com/cybrid-systems/aura/issues/1673)  
**Builds on:** #1349/#1350 (ANSI/RGB) · #1352 (TermBuf registry) · #1559 (present_batch) · #1562 (dirty delta)  
**Status:** P1 rendering — production path exists; #1673 wires diff→dirty + unified stats.

## Issue names → shipped EDSL surface

| Issue suggestion | Production primitive | Notes |
|------------------|----------------------|--------|
| `(term-cell-grid-create w h)` | `(make-terminal-buffer w h)` | TermBuf + dirty all |
| `(term-diff-update old new)` | `(terminal-diff-update old new)` | **#1673**: marks dirty AABB on *new* |
| `(term-present-batch buffer)` | `(terminal-present-batch id [fd])` / `(render-present-batch …)` | zero-copy + dirty short-circuit |
| cell write | `(terminal-set-cell …)` / `(render-draw-batch …)` | hot-tier RENDER_PRIMITIVE_META |
| TUI present | `(tui:present)` | full-screen path (global_tui) |

Deprecated no-ops: `terminal:create-buffer`, `terminal:diff`, `terminal:present`, … (#1351) — use hyphen names above.

## Hot path (#1559 engine)

```
dirty short-circuit → render hotpath → zero-copy frame arena → ANSI dirty emit → write(fd)
```

`RENDER_PRIMITIVE_META` sets `perf_tier=hot` + `category=rendering` for dispatch telemetry.

## #1673 delta

1. **`terminal-diff-update`** marks dirty region on the *new* buffer for every changed cell (or full frame on size mismatch) so a pure double-buffer swap still feeds `present_batch` partial ANSI.
2. **`query:render-stats`** (schema **1673**, facade-only) aggregates engine counters + terminal metrics:
   - present-calls / present-skips / present-bytes  
   - draw-calls / draw-cells  
   - zero-copy-acquires / sgr-emits / dirty-cells-emitted  
   - buffer-creates / live / diff-updates / present-batch-total / set-cell-total  
   - hot-dispatch-hits-render  

Access: `(stats:get "query:render-stats")` or `(engine:metrics "query:render-stats")`.

## SlimSurface

No new public `add()` names (ceiling 520). Stats via `register_stats_impl` only.

## Tests

`tests/test_terminal_render_production_1673.cpp` — create → set → diff marks dirty → present skip/bytes → stats schema.

## Non-goals

- New public `term-*` synonyms (would blow SlimSurface).  
- SoA TermCell redesign (Phase note in FramebufferSoA).  
- Full 60 FPS matrix-rain Agent self-evo harness (covered by existing pets/render demos + this closed-loop unit test).
