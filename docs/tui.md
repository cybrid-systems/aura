# TUI Domain (`tui:*`) — Issue #1967

**Status: commercial UI vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 21 `tui:*` primitives are **kept** as a commercial / UI vertical, not
deleted. They are already excluded from the SlimSurface *core* budget
via `DOMAIN_STATUS["tui:"] = "deferred"` (`scripts/check_primitive_surface.py`).

| Option | Chosen | Why |
|---|---|---|
| Remove | No | Active tests (`test_terminal_domain_batch`, production sweep, render tier), `lib/std/tui/*`, and headless CI paths depend on them. |
| Keep + gate | **Yes** | Formalize as optional commercial surface with a budget freeze. |

## Build flag: `AURA_ENABLE_TUI`

CMake option (default **ON** so existing CI / stdlib keep working):

```bash
# Default full build (TUI registered)
cmake -B build -S .

# Slim / core-only: skip tui:* registration at Evaluator construct
cmake -B build_slim -S . -DAURA_ENABLE_TUI=OFF
```

When `AURA_ENABLE_TUI=0`, `register_tui_primitives` is a no-op
(`src/compiler/evaluator_primitives_tui.cpp`). Source inventory for the
primitive freeze still lists the 21 names (source-scanned `add("tui:…")`)
so the freeze baseline stays stable across ON/OFF builds.

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["tui:"] = 21   # scripts/check_primitive_surface.py
```

`./build.py gate` runs `check_primitive_surface.py --strict`, which fails
if the source-scanned `tui:` count exceeds this budget. Raising the budget
requires an explicit PR edit to `COMMERCIAL_DOMAIN_BUDGETS` + justification
(same governance spirit as SlimSurface `--update-baseline`).

## Surface (21 primitives)

`tui:init`, `tui:shutdown`, `tui:size`, `tui:cell`, `tui:get-cell`,
`tui:present`, `tui:read-event`, `tui:raw-mode-on`, `tui:raw-mode-off`,
`tui:is-raw-mode`, `tui:terminal-size`, `tui:enable-mouse`,
`tui:inject-bytes`, `tui:hide-cursor`, `tui:show-cursor`, `tui:set-title`,
`tui:clear`, `tui:pixel`, `tui:mouse`, `tui:frame-ansi`, `tui:inject-key`.

Implementation: `src/tui/tui_runtime.hh`, `src/tui/tui_input.hh`,
`src/compiler/evaluator_primitives_tui.cpp`. Stdlib: `lib/std/tui/`.

## 3D TUI foundation (Epic #1979)

Software voxel raycasting base on the terminal path (no GPU). Child order:

1. **#1980** half-block pixel framebuffer — landed as C++ engine API  
   `src/renderer/pixel_framebuffer.{hh,cpp}`  
   Each cell = two vertical pixels via U+2580 `▀` (upper=fg, lower=bg RGB).  
   View over `FramebufferSoA` + `DirtyRegion`; headless via `pixel_present_to_string`.  
   Tests: `tests/arena/test_pixel_framebuffer.cpp`
2. **#1981** camera + primary rays — landed (header-only)  
   `src/renderer/camera.hh` — `Vec3` / `Ray` / `Camera` / `camera_ray` / `generate_primary_rays`  
   Right-handed Y-up; yaw=0,pitch=0 → forward −Z; pitch clamped ≈±89°.  
   Tests: `tests/arena/test_camera_rays.cpp`
3. **#1982** voxel volume / chunks — landed (header-only)  
   `src/renderer/voxel_volume.hh` — `BlockId` / `VoxelVolume` / `ChunkGrid` / `voxel_fill_box`  
   Y-up; X-major layout `x + sx*(y + sy*z)` for DDA X-step locality; OOB → `oob_block`.  
   Tests: `tests/arena/test_voxel_volume.cpp`
4. **#1983** DDA voxel raycaster — landed (header-only)  
   `src/renderer/voxel_raycast.hh` — `Hit` / `raycast_voxel` / `raycast_frame` / counters  
   Amanatides–Woo; solid = non-air; start-inside → `VoxelFace::Inside`; no heap.  
   Tests: `tests/arena/test_voxel_raycast.cpp`
5. **#1984** shading / fog / sky — landed (header-only)  
   `src/renderer/voxel_shade.hh` — `Material` / `ShadeParams` / `shade_hit` / `shade_sky` / `shade_ray`  
   Face factors (+Y 1.0 / sides 0.8 / −Y 0.6), exp fog, zenith→horizon sky, ambient.  
   Tests: `tests/arena/test_voxel_shade.cpp`
6. **#1985** frame loop + present_batch — landed  
   `src/renderer/voxel_frame.hh` — `render_frame` / `build_demo_scene` / `FrameStats`  
   Tests: `tests/arena/test_voxel_frame.cpp` · Demo: `demo_voxel_3d --headless --frames 3`  
   Soft target: 80×48 cells ≥15–20 FPS (Phase 1).
7. **#1986** Aura EDSL surface — landed  
   `render3d:*` primitives (gated with `AURA_ENABLE_TUI`) + `lib/std/render3d.aura`  
   Engine: `src/renderer/render3d_runtime.hh` · Prim: `evaluator_primitives_render3d.cpp`  
   Tests: `tests/arena/test_render3d_primitives.cpp`  
   Opaque 1-based volume IDs; camera args in milliradians; `(render3d:frame-ansi)` headless.

### Aura quickstart (#1986)

```scheme
(define v (render3d:create-volume 32 16 32))
(render3d:build-demo v)
(render3d:resize-fb 40 20)
(render3d:set-camera 16 8 28 0 -350 1050)  ; x y z yaw_mrad pitch_mrad fov_mrad
(render3d:frame v 1)                       ; headless
(render3d:stats)                           ; → hash schema 1986
(display (render3d:frame-ansi v))
```

## Related

- SlimSurface: #1448 / #1449
- TUI original series: #1331–#1343 / #1353
- 3D rendering epic: #1979 / children #1980–#1986
- Sibling deferred domains: #1968–#1976 + render3d: (#1986)
- Orch multi-agent remove (contrast): #1966
