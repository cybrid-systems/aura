# Render memory predictability (#1675)

**Issue:** [#1675](https://github.com/cybrid-systems/aura/issues/1675)  
**Builds on:** #1559/#1561 (present zero-copy + FrameBumpArena) · #1673/#1674 (render closed loop)  
**Status:** P1 memory — hot-path GC defer + bounded cell/ANSI allocation metrics.

## Problem

General `string_heap_` / arena compact / GC safepoints are too noisy for 60 FPS
terminal present. Issue sketch (`term-buffer-alloc` public prims) would blow
SlimSurface; the production path already has dedicated machinery:

| Piece | Location |
|-------|----------|
| Frame bump ANSI arena | `FrameBumpArena` + `g_render_frame_arena()` (TLS) |
| Zero-copy views | `ZeroCopyFramebuffer::acquire_view(size, arena)` |
| Cell grids | `TermBuf` in `terminal_buffer_registry.hh` |
| GC soft-gate | `in_render_hotpath()` → auto-compact policy skip |

## #1675 deltas

1. **`request_gc_safepoint()`** returns deferred (**1**) while `in_render_hotpath()`
   so STW cannot land mid-present (pairs with arena compact soft-gate).
2. **`make-terminal-buffer`** `cells.reserve(w*h)` before `assign` — no geometric
   reallocation jitter for fixed-size grids.
3. **`compact-terminal-buffers`** bumps compact metrics; resets frame-arena *used*
   (keeps capacity warm) when not in hotpath.
4. **Present path** mirrors zero-copy process metrics into `CompilerMetrics`.
5. **`query:render-memory-stats`** (schema **1675**, facade-only):
   - `frame-arena-capacity` / `used` / `alloc-calls` / `total-bytes`
   - `zero-copy-arena-alloc-bytes` / `acquires` / `vector-fallback` / `hit-in-render`
   - `buffer-live-count` / `buffer-live-capacity-bytes`
   - `buffer-creates` / `buffer-compacts` / `hotpath-enter-total`

## Non-goals

- New public `term-buffer-alloc` / `term-buffer-move` names (ceiling 520).
- SoA cell storage rewrite.
- Disabling GC process-wide (only hotpath defer).

## Tests

`tests/test_render_memory_predictability_1675.cpp`
