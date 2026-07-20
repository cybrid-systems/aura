# Terminal Domain (`terminal:*`) — Issue #1971

**Status: UI/integration vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**
(as gated deprecated stubs; not the real terminal buffer APIs).

## Decision

The 7 `terminal:*` colon-prefixed primitives are **kept under a budget** as
Phase-A deprecated no-ops (#1351), not deleted in this issue. They remain
excluded from SlimSurface *core* via `DOMAIN_STATUS["terminal:"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove now | No (this issue) | `test_terminal_deprecation` + production sweeps still assert presence, `#f` return, and one-shot WARN. Phase-B delete can zero the budget later. |
| Keep + gate | **Yes** | Matches #1967–#1970 pattern; slim builds can drop the stubs with `-DAURA_ENABLE_TERMINAL=OFF`. |

**Real terminal APIs** use hyphenated names (not this domain prefix):
`make-terminal-buffer`, `terminal-set-cell*`, `terminal-present-batch`,
`terminal-diff-update`, etc.

## Build flag: `AURA_ENABLE_TERMINAL`

CMake option (default **ON**):

```bash
cmake -B build -S .
cmake -B build_slim -S . -DAURA_ENABLE_TERMINAL=OFF
```

When OFF, the 7 deprecated `terminal:*` adds in
`register_jit_p59` / `register_jit_p113`
(`evaluator_primitives_obs_jit.cpp`) are not registered. Other obs-jit
primitives in those functions stay available.

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["terminal:"] = 7
```

Raising requires PR edit + justification. Phase-B deletion should set the
budget to 0 and remove the adds + deprecation test (or retarget it).

## Surface (7 deprecated no-ops)

| Primitive | Replacement hint (#1351) |
|---|---|
| `terminal:clear` | `make-terminal-buffer` + `terminal-present-batch` |
| `terminal:draw-batch` | `terminal-set-cell` / `terminal-set-cell-rgb` |
| `terminal:present` | `terminal-present-batch` / `terminal-present` |
| `terminal:mark-dirty-region` | `terminal-diff-update` |
| `terminal:present-delta` | `terminal-diff-update` + `terminal-present-batch` |
| `terminal:create-buffer` | `make-terminal-buffer` |
| `terminal:diff` | `terminal-diff-update` |

Behavior when enabled: return `#f`, bump metrics, one-shot stderr WARN.

## Related

- Deprecation: #1351 · render/terminal buffer work: #824 / #856 / #1135
- Sibling commercial keep: #1967–#1970
- Sibling deferred: #1972–#1976
- Active TUI vertical (different prefix): #1967 `tui:*` + `docs/tui.md`
