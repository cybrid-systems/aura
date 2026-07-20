# Synthesize Domain (`synthesize:*`) — Issue #1974

**Status: commercial synthesis vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 4 `synthesize:*` primitives are **kept** as a commercial synthesis
vertical (templates + LLM define + genetic optimize), not deleted. They
are already excluded from SlimSurface *core* via
`DOMAIN_STATUS["synthesize:"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove | No | ~28 tests (optimize PRNG, JSON parse, namespace demotion, production sweep, suite edsl) depend on them. |
| Keep + gate | **Yes** | Same pattern as #1967–#1972. |

## Build flag: `AURA_ENABLE_SYNTHESIZE`

CMake option (default **ON**):

```bash
cmake -B build -S .
cmake -B build_slim -S . -DAURA_ENABLE_SYNTHESIZE=OFF
```

When OFF, the four `synthesize:*` adds in
`register_synthesize_primitives` (`evaluator_primitives_agent.cpp`) are
not registered.

**Not gated:** `query:templates` (engine accessor for template names, same
function; not `synthesize:` prefix).

Capability: `kCapSynthesize` (Issue #1232). Shares agent.cpp TU with
`AURA_ENABLE_AUTO_EVOLVE` (#1969).

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["synthesize:"] = 4
```

## Surface (4 primitives)

| Primitive | Role |
|---|---|
| `synthesize:register-template` | Register named template pattern |
| `synthesize:fill` | Fill template params → set-code |
| `synthesize:define` | LLM-backed function definition |
| `synthesize:optimize` | Genetic-algorithm optimize pass |

## Related

- Sibling commercial keep: #1967–#1972 · deferred strategy: #1973
- Sibling deferred: #1975–#1976
- SlimSurface: #1448 / #1449
