# Aura architecture — module boundaries

**Authority:** [`src/core/module_boundary.ixx`](../src/core/module_boundary.ixx)
(`import aura.core.module_boundary`, namespace `aura::core::boundary`).

This is the single place that defines:

1. **Layers** (`ModuleLayer`: Core, Parser, Compiler, Serve, …)
2. **Dependency direction** (`layer_may_depend_on` / `AllowedDependency`)
3. **Cross-layer contracts** (`CrossLayerStableRef`, `CrossLayerDirtyPropagator`,
   `ProvenanceScoped`)

Type *shapes* (NodeHandle, StableNodeRefLike, DirtyPropagator, …) live in
[`src/core/concepts.ixx`](../src/core/concepts.ixx). Pass pipeline concepts live
in [`src/core/concept_constraints.ixx`](../src/core/concept_constraints.ixx).

## Dependency direction (summary)

```
Core  ←  Parser  ←  Compiler  ←  {Serve, Exec, Repl, Reflect, Renderer, Orch, Tui}
```

| From \\ To | Core | Parser | Compiler | Other upper |
|------------|------|--------|----------|-------------|
| **Core**   | ✓    | ✗      | ✗        | ✗           |
| **Parser** | ✓    | ✓      | ✗        | ✗           |
| **Compiler** | ✓  | ✓      | ✓        | ✗           |
| **Serve / Exec / …** | ✓ | ✗ | bridge OK | lateral discouraged |

- **Core never imports Compiler / Serve / …**
- **Parser is Core-only**
- **Compiler may use Core + Parser**
- Upper layers always use Core; Compiler edges should prefer thin headers / C ABI
  when adding new glue (see bridge notes in `module_boundary.ixx`)

## Key entry modules

| Layer | Entry points |
|-------|----------------|
| Core | `aura.core`, `aura.core.concepts`, `aura.core.module_boundary`, `aura.core.ast` |
| Parser | `aura.parser.lexer`, `aura.parser.parser` |
| Compiler | `aura.compiler.service`, `aura.compiler.pass_manager`, `aura.compiler.evaluator` |

CMake module lists: [`cmake/AuraModules.cmake`](../cmake/AuraModules.cmake)
(`AURA_CXX_MODULE_CORE` / `AURA_CXX_MODULE_COMPILER`).

## Cross-layer contracts

- **Stable refs across mutation / fiber / tenant:** use `StableNodeRefLike` /
  `CrossLayerStableRef` — not raw `NodeId` alone. Attach provenance via
  `provenance_tracker` / `workspace_isolation` / `capability_model`.
- **Dirty cascade:** implement `DirtyPropagator` / `CrossLayerDirtyPropagator`
  (mark_dirty, mark_dirty_upward, is_dirty, clear_dirty).
- **Multi-tenant:** prefer types satisfying `ProvenanceScoped` (`tenant_id()`).

## When you change layering

1. Update `src/core/module_boundary.ixx` (rules + comments).
2. Update this doc if the DAG summary changes.
3. Tick the PR template checkbox (“module_boundary updated if applicable”).
4. Keep entry `static_assert`s in `pass_manager.ixx` / `service.ixx` green.

## Related

- Issue **#1885** (this file + `module_boundary.ixx`)
- Naming & comment templates: **#1886** — [`docs/naming_convention.md`](naming_convention.md)
- Concepts foundation: **#501**
- Pass concepts centralization: **#1577**
- Contributing: [`docs/contributing.md`](contributing.md)
