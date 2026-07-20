# Strategy Domain (`strategy:*`) — Issue #1973

**Status: commercial strategy DSL vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 4 `strategy:*` evolution-controller primitives are **kept** as a
commercial vertical, not deleted. They are already excluded from
SlimSurface *core* via `DOMAIN_STATUS["strategy:"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove | No | ~36 tests (set-errors, intend mutex, issue suites) exercise the controller. |
| Keep + gate | **Yes** | Same pattern as #1967–#1975. |

## Build flag: `AURA_ENABLE_STRATEGY`

CMake option (default **ON**):

```bash
cmake -B build -S .
cmake -B build_slim -S . -DAURA_ENABLE_STRATEGY=OFF
```

When OFF, the four `strategy:*` adds in
`register_auto_evolve_primitives` (`evaluator_primitives_agent.cpp`) are
not registered.

**Not gated:**

| Surface | Why |
|---|---|
| `query:strategy-evolution-stats` | `query:` stats facade (same function) |
| `intend`, `define-strategy`, `register-strategy!`, … | Live in `register_strategy_primitives` (no `strategy:` prefix) |

Shares agent.cpp COMPILE_DEFINITIONS with `AURA_ENABLE_AUTO_EVOLVE` and
`AURA_ENABLE_SYNTHESIZE`.

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["strategy:"] = 4
```

## Surface (4 primitives)

| Primitive | Role |
|---|---|
| `strategy:set-strategy` | Select coverage-greedy / bug-fix-priority / minimal-mutation |
| `strategy:active` | Current strategy name |
| `strategy:report-success` | Bump success pheromone |
| `strategy:escalate` | Record escalation event |

Related: #444, #1714, #1720 / #1722.

## Related

- Sibling commercial keep: #1967–#1975 · remaining deferred: #1976 (`m4-`)
- SlimSurface: #1448 / #1449
