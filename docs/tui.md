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

## Related

- SlimSurface: #1448 / #1449
- TUI original series: #1331–#1343 / #1353
- Sibling deferred domains: #1968–#1976
- Orch multi-agent remove (contrast): #1966
