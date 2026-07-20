# SEVA Domain (`seva:*`) — Issue #1972

**Status: commercial service-evaluation vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 5 `seva:*` primitives are **kept** as a commercial SEVA / OpenClaw
goal surface, not deleted. They are already excluded from SlimSurface
*core* via `DOMAIN_STATUS["seva:"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove | No | OpenClaw integration (#445), demo metrics (#446 / #1841), and related issue suites call these high-level goals. |
| Keep + gate | **Yes** | Same pattern as #1967–#1971. |

## Build flag: `AURA_ENABLE_SEVA`

CMake option (default **ON**):

```bash
cmake -B build -S .
cmake -B build_slim -S . -DAURA_ENABLE_SEVA=OFF
```

When OFF, the five `seva:*` adds in `register_compile_p59`–`p62`
(`evaluator_primitives_compile.cpp`) are not registered.

**Not gated:** `query:seva-audit-log` (query: stats facade, same p61 TU).

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["seva:"] = 5
```

## Surface (5 primitives)

| Primitive | Role |
|---|---|
| `seva:achieve-coverage` | Coverage gap vs target (#445) |
| `seva:fix-reset-bugs` | Identify reset-related verify dirty holes |
| `seva:generate-regression` | Emit Aura regression script skeleton |
| `seva:approve-mutation` | Safety gate for agent mutations |
| `seva:run-demo-with-metrics` | L4–L5 demo metrics hash (#446) |

Related: SEVA demo #442, OpenClaw #445, metrics #446 / #1841.

## Related

- Sibling commercial keep: #1967–#1971
- Sibling deferred: #1973–#1976
- SlimSurface: #1448 / #1449
