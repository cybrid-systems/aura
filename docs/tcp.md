# TCP Domain (`tcp-*`) — Issue #1975

**Status: integration vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 4 `tcp-*` primitives are **kept** as a TCP networking integration
vertical, not deleted. They are already excluded from SlimSurface *core*
via `DOMAIN_STATUS["tcp-"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove | No | Low-level socket surface used by agents / fuzz corpus paths; small surface but still part of commercial_readiness integration set. |
| Keep + gate | **Yes** | Same pattern as #1967–#1974. |

## Build flag: `AURA_ENABLE_TCP`

CMake option (default **ON**):

```bash
cmake -B build -S .
cmake -B build_slim -S . -DAURA_ENABLE_TCP=OFF
```

When OFF, the four `tcp-*` adds in `register_network_primitives`
(`evaluator_primitives_io.cpp`) are not registered.

**Not gated** (same TU / function):

- `getenv`, `http-get`, `http-post`
- hyphenated terminal-buffer APIs (`make-terminal-buffer`, …)
- `sys-*` and other later IO primitives

`AURA_ENABLE_GIT` (#1970) is independent (same `io.cpp` COMPILE_DEFINITIONS list).

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["tcp-"] = 4
```

## Surface (4 primitives)

| Primitive | Role |
|---|---|
| `tcp-connect` | Host:port → fd (non-blocking connect + timeout) |
| `tcp-send` | Send string on fd |
| `tcp-recv` | Recv into string heap |
| `tcp-close` | Close fd |

## Related

- Sibling commercial keep: #1967–#1974 · deferred: #1973 (`strategy:`), #1976 (`m4-`)
- SlimSurface: #1448 / #1449
