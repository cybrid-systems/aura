# Contributing (Aura)

Entry points for humans and AI Agents working in this repository.

## Testing

- Test layout and how to run suites: [tests/README.md](../tests/README.md)
- Strategy and prioritization: [tests/STRATEGY.md](../tests/STRATEGY.md)

## Engine primitives (C++)

Adding or changing built-in primitives follows the **primitive authoring contract**:

- Contract: [stdlib/primitive-authoring-contract.md](stdlib/primitive-authoring-contract.md) (Issue #2915)
- Error-return convention: [stdlib/primitive-error-convention.md](stdlib/primitive-error-convention.md) (Issue #2914)
- Scaffold header: `src/compiler/prim_registrar_scaffold.hh` (`register_prim` + `PrimSpec`)
- Central registration map: `src/compiler/evaluator_primitives_registry.cpp` (#1552)
- Generated catalogs: [generated/primitives.md](generated/primitives.md), [generated/primitives-registry.md](generated/primitives-registry.md)

**Do not invent new registration styles.** Prefer `register_prim` for general prims and `register_render_hot_prim` only for render-critical hot paths.

## Multi-fiber heap quotas (Agent self-evo)

Soft limits on `pairs` / `strings` / `vectors` growth for concurrent Agent loops:

- Contract: [stdlib/prim-heap-quota.md](stdlib/prim-heap-quota.md) (Issue #2916)
- `(resource:quota-set "pairs"|"strings"|"vectors" N)` + `(engine:metrics "query:prim-heap-quota-stats")`

## Agent recovery closed loop

Error → diagnose → safe fix under Guard → clear hold → poll stats:

- Contract: [stdlib/agent-recovery.md](stdlib/agent-recovery.md) (Issue #2917)
- `(agent:recover-from-error err code)` + `(engine:metrics "query:agent-recovery-stats")`

## Workspace source SSOT

FlatAST is authoritative after mutate; do not trust cached set-code text:

- Contract: [stdlib/workspace-source-ssot.md](stdlib/workspace-source-ssot.md) (Issue #2920)
- Prefer `(current-source :workspace)` / live unparse for JIT & serialize

## Docs regeneration

```bash
./build.py docs
```
