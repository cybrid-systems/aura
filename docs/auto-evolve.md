# Auto-Evolve Domain (`auto-evolve-*`) — Issue #1969

**Status: commercial self-evolution AI vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 7 `auto-evolve-*` primitives are **kept** as a commercial self-evolution
AI vertical, not deleted. They are already excluded from the SlimSurface
*core* budget via `DOMAIN_STATUS["auto-evolve-"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove | No | Agent closed-loop tests (`test_auto_evolve_*`, production sweep, openclaw integration) and `agent:tick` bridge depend on the machinery. |
| Keep + gate | **Yes** | Same pattern as #1967 (`tui:`) / #1968 (`eda:`): optional commercial surface + budget freeze. |

## Build flag: `AURA_ENABLE_AUTO_EVOLVE`

CMake option (default **ON** so existing CI / agent tests keep working):

```bash
# Default full build (auto-evolve registered)
cmake -B build -S .

# Slim / core-only: skip auto-evolve-* registration
cmake -B build_slim -S . -DAURA_ENABLE_AUTO_EVOLVE=OFF
```

When `AURA_ENABLE_AUTO_EVOLVE=0`, the seven `auto-evolve-*` names are not
registered. Co-registered surfaces in the same function stay available:

- `agent:running?` / `agent:tick` — always on; `agent:tick` degrades when
  `auto-evolve-tick` / `auto-evolve-once` are missing (lookup fails → false / 0)
- `strategy:*` adds that historically live in
  `register_auto_evolve_primitives` — always on (tracked separately as #1973)

Source inventory for the primitive freeze still lists all 7 names
(source-scanned `add("auto-evolve-…")`) so the freeze baseline stays stable
across ON/OFF builds.

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["auto-evolve-"] = 7   # scripts/check_primitive_surface.py
```

`./build.py gate` runs `check_primitive_surface.py --strict`, which fails
if the source-scanned `auto-evolve-` count exceeds this budget. Raising the
budget requires an explicit PR edit to `COMMERCIAL_DOMAIN_BUDGETS` +
justification.

## Surface (7 primitives)

All in `src/compiler/evaluator_primitives_agent.cpp` /
`register_auto_evolve_primitives`:

| Primitive | Role |
|---|---|
| `auto-evolve-once` | One detect→fix cycle |
| `auto-evolve-loop` | Start background loop (interval + closures) |
| `auto-evolve-stop` | Stop background loop |
| `auto-evolve-running?` | Loop running? |
| `auto-evolve-tick` | One tick of the background loop |
| `auto-evolve-cycle-count` | Cycles completed |
| `auto-evolve-total-fixed` | Gaps fixed total |

Capability: `kCapSelfEvo` (Issue #1232). Related: #97, #1712, #1713, #1327.

## Related

- SlimSurface: #1448 / #1449
- Sibling commercial keep: #1967 (`tui:`), #1968 (`eda:`)
- Sibling deferred domains: #1970–#1976
