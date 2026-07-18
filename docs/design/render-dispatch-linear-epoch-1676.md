# Render dispatch overhead + linear/epoch TUI fences (#1676)

**Issue:** [#1676](https://github.com/cybrid-systems/aura/issues/1676)  
**Builds on:** #1559/#1560 (present/FFI hot path) · #1563 (render_critical) · #1673–#1675 (closed loop / obs / memory)  
**Status:** P1 FFI/hot-path — trusted render dispatch + entry fences.

## Problem

Primitive dispatch for present/draw paid full `invoke_prim_with_telemetry` security tier checks every call. TUI/render entry points did not participate in dual-epoch / `linear_post_mutate_enforce` (Apply prologue only).

## #1676 deltas

### 1. Render-tier fast dispatch

`invoke_prim_with_telemetry`: when `is_render_critical_meta(meta)`, skip capability + deprecation tax and bump `render_hotpath_dispatch_fast_total`. Non-critical calls while `in_render_hotpath()` bump `render_hotpath_dispatch_full_total` (cold fallback observability).

### 2. Entry fence

`Evaluator::fence_render_hot_entry()` + `RenderHotEntryGuard`:

1. enter render hotpath depth  
2. sample bridge epoch  
3. `linear_post_mutate_enforce` on newest live EnvFrame  
4. refresh stale EnvFrame via `refresh_stale_frame_in_walk(..., "render_hot_entry")`  
5. exit depth on scope end  

Wired into:

| Primitive | Fence |
|-----------|--------|
| `terminal-present-batch` | yes |
| `render-draw-batch` | yes |
| `tui:present` / `tui:cell` | yes |

### 3. TUI render-critical meta

`set_meta_for_name` with `RENDER_PRIMITIVE_META` for `tui:present`, `tui:cell`, `tui:clear`, `tui:frame-ansi` → hot_map_ + fast dispatch.

### 4. hot_path_primitives phase 2

Render-tier names (`terminal-present-batch`, `render-draw-batch`, `tui:present`, …) + process counters in `g_hot_path_prim_stats`.

### 5. `query:render-stats` schema **1676**

New keys (facade-only, no SlimSurface growth):

- `dispatch-fast-total` / `dispatch-full-total`  
- `linear-fence-total` / `epoch-fence-total`  
- `linear-block-total` / `epoch-stale-total`  
- `linear-enforcements`

## Tests

`tests/test_render_dispatch_linear_epoch_1676.cpp`

## Non-goals

- Full `linear_post_mutate_enforce_all` every present (O(frames) — reserved for post-mutate).  
- New public prim names.  
- Changing privileged/sandbox gate for non-render prims.
