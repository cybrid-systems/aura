# Aura naming & comment conventions

**Issue:** #1886  
**Audience:** humans and AI agents writing or reviewing production code.

This document is the single authority for **names** and **comment templates**.
Formatting is enforced by `.clang-format` / pre-commit; this file covers
*semantics* (what to call things, what to write above them).

Related:

| Doc / source | Role |
|--------------|------|
| [`docs/contributing.md`](contributing.md) | Workflow, tests, architecture pointers |
| [`docs/architecture.md`](architecture.md) | Module layering (#1885) |
| [`src/core/module_boundary.ixx`](../src/core/module_boundary.ixx) | Dependency DAG |
| [`src/core/concepts.ixx`](../src/core/concepts.ixx) | Type-shape concepts (good comment examples) |
| [`scripts/check_primitive_surface.py`](../scripts/check_primitive_surface.py) | Public primitive freeze |
| [`.clang-format`](../.clang-format) | Indent / column limit (100) |

---

## 1. General principles

1. **Code is the source of truth** — names and short comments beat long design essays.
2. **AI-native** — every non-obvious public API should answer: *what does an agent
   need to know to call this safely under mutation / fiber / provenance?*
3. **Issue-linked** — non-trivial behavior references `#NNNN` (GitHub issue).
4. **No silent magic** — prefer explicit prefixes (`mutate:`, `query:`, `k…`) over
   overloaded bare words.
5. **Stable over cute** — rename only with a deprecation path for public primitives.

---

## 2. Modules & files

| Kind | Convention | Example |
|------|------------|---------|
| C++ module name | `aura.<layer>.<topic>` | `aura.core.module_boundary` |
| Module file | `snake_case.ixx` under `src/<layer>/` | `src/core/module_boundary.ixx` |
| Impl TU | `snake_case_impl.cpp` or topic file | `type_checker_impl.cpp` |
| Header (non-module) | `snake_case.h` / `.hh` | `typed_mutation_audit.h` |
| Test | prefer `tests/domain/…`; legacy `test_*_NNNN.cpp` | `test_module_boundary_1885.cpp` |
| Docs | `kebab-or-snake.md` under `docs/` | `naming_convention.md` |

**Namespace:** `aura::<layer>` (e.g. `aura::core`, `aura::compiler`, `aura::ast`).
Nested detail: `…::detail` or `…::boundary`.

**Export:** prefer `export namespace aura::… { … }` or `export template` at the
point of definition. Re-export umbrellas only from `aura.core` / intentional
facades (`pass_manager` re-exports concept_constraints).

---

## 3. Types, values, functions

### 3.1 Types

| Kind | Style | Example |
|------|-------|---------|
| Class / struct | `PascalCase` | `CompilerService`, `MutationRecord` |
| Enum class | `PascalCase` type, `PascalCase` enumerators | `ModuleLayer::Compiler` |
| Concept | `PascalCase` noun/adjective | `NodeHandle`, `StableNodeRefLike`, `AllowedDependency` |
| Type alias | `PascalCase` or existing domain name | `NodeId`, `AuraResult` |
| Template param | short `PascalCase` | `T`, `Id`, `C`, `From`/`To` for layers |

### 3.2 Values & members

| Kind | Style | Example |
|------|-------|---------|
| Variable / member | `snake_case` | `fixpoint_rounds`, `target_node` |
| Constant (constexpr / const) | `kPascalOrSnake` | `kModuleBoundaryIssue`, `NULL_NODE` |
| Atomic counter field | `snake_case` + `_total` when cumulative | `type_prop_evidence_lost_total` |
| Boolean | positive name | `is_dirty`, `has_error` (not `not_clean`) |
| Private member | trailing `_` when ambiguous with locals | `predicate_memo_` |

### 3.3 Functions

| Kind | Style | Example |
|------|-------|---------|
| Free / member function | `snake_case` | `layer_may_depend_on`, `record_invariant_audit_result` |
| C ABI / hooks | `aura_` prefix | `aura_typed_audit_note_predicate_memo_eviction` |
| Query-style (C++) | verb or `note_` / `capture_` / `record_` | `note_type_propagation_pass` |
| Predicates | `is_` / `has_` / `can_` / `may_` | `is_render_critical_meta` |

**`[[nodiscard]]`:** use on pure observers and result types that must not be ignored
(`AuraResult`, bool success of audit gates).

**`noexcept`:** prefer on hot-path counters, audit stamps, and pure predicates that
must not throw across fiber boundaries.

---

## 4. Primitive & metrics naming

Public Aura Lisp primitives are registered via `Primitives::add` /
`register_stats_impl`. Names are **product surface** — freeze-gated.

### 4.1 Prefixes

| Prefix | Role | Notes |
|--------|------|--------|
| `query:` | Read-only observability / introspection | Prefer `register_stats_impl` + `(engine:metrics …)` |
| `mutate:` | Workspace / AST mutation | Needs `MutationBoundaryGuard` when structural |
| `compile:` | Compile / lower / type-check entry | |
| `workspace:` | Workspace tree ops | |
| `engine:` | Engine facade (`engine:metrics`) | Preferred for new stats |
| `eda:` / `seva:` / `verify:` / `tui:` / … | Domain verticals | Counted separately in SlimSurface |

### 4.2 Forbidden / freeze patterns (new names)

Do **not** add new public names matching (see `check_primitive_surface.py`):

- `*-stats`, `*-stats-hash`, `*-stats-*` via public `add()` (use catalog + `engine:metrics`)
- `string-*` / `json-*` / `math-*` / `vector-*` / `path-*` / `time-*` (stdlib)
- `ast:ref-*` (StableRef API)

Intentional growth: `python3 scripts/check_primitive_surface.py --update-baseline`
with PR justification.

### 4.3 PrimMeta fields (self-describing registration)

`PrimMeta` (`evaluator.ixx`) — fill when registering Agent-visible primitives:

| Field | Expectation |
|-------|-------------|
| `arity` | Fixed arity or `255` = variadic |
| `pure` | `true` if no mutation / no observable side effects |
| `safety_flags` | `kPrimSafetyMutates` / `Io` / `Fiber` bits |
| `perf_tier` | `kPrimPerfHot` / `Normal` / `Cold` |
| `security_level` | `Safe` / `Sandboxed` / `Privileged` |
| `doc` | One-line purpose; Agent discovery text |
| `category` | `general` \| `eda` \| `sva` \| `verification` \| `rendering` \| … |
| `schema` | e.g. `"(string) -> hash"` |
| `deprecated` | Prefer `(engine:metrics)` migration path |

Skeleton helpers: `primitives_meta.h` / `primitive:generate-skeleton`.

### 4.4 Metrics / counters

- Process-wide audit counters: `snake_case` + semantic suffix (`_total`, `_bp`).
- Schema integers for hash stats: issue number when dedicated (`schema: 1884`).
- Query hash keys: **kebab-case** strings (`correlation-total`, `pass-with-evidence`).

---

## 5. Comment templates

Use these on **new or significantly changed** public APIs, concepts, and
cross-layer contracts. Internal helpers may stay short.

### 5.1 Function / concept / type (canonical)

```cpp
// Purpose: <one sentence — what and why>
// Pre: <caller must ensure …; empty if none>
// Post: <guarantees on success; counters / trail side effects>
// Safety Class: <P0 memory / P1 incremental / P2 observability / P3 docs>
// Issue: #<nnnn>
// AI-Native Rationale: <how this helps agents / self-mod / provenance>
```

**Safety Class guide:**

| Class | Meaning |
|-------|---------|
| **P0** | Memory safety, lifetime, contracts, panic/rollback |
| **P1** | Incremental correctness, dirty cascade, type/invariant |
| **P2** | Observability, metrics, audit trails (sampled OK) |
| **P3** | Docs, naming, non-functional layering aids |

### 5.2 File / module banner

```cpp
// <filename> — Issue #<nnnn>: <title phrase>
//
// Purpose: <module role in one sentence>
// Layer: <Core|Parser|Compiler|…>  (see module_boundary.ixx)
// See also: <related modules / docs>
```

### 5.3 Invariants & contracts

```cpp
// Invariant: <must always hold after this function returns>
// Contract: pre(…) / post(…) — C++26 trailing contracts preferred when available
// Failure: returns AuraResult error | AuditOutcome::Error | throws only OOM/init
```

Hot path error policy: prefer `AuraResult` / expected; exceptions only for OOM /
init / hard invariants (see `error.ixx` policy comments).

### 5.4 Error handling comment (catch / boundary)

```cpp
// SILENCE-PRIM: intentional empty catch — <reason>  (#1669)
// or
// On failure: roll back mutation_id=…; stamp typed_audit trail (always-on fail path)
```

### 5.5 Primitive registration comment

```cpp
// Purpose: engine:metrics surface for <topic>
// Schema: <issue#>  Category: general  Safety: pure query
// AI-Native Rationale: agents correlate X with Y without re-running pipeline
ObservabilityPrims::register_stats_impl("query:…", …);
```

---

## 6. AI-Native rationale standards

Every **P0/P1** public change should state (in comment or PR body) how it affects
the agent loop:

1. **Observability** — can an agent detect success/fail without parsing logs?
2. **Provenance** — is the mutation / ref attributable (tenant, fiber, mutation_id)?
3. **Rollback** — is failure always-on in the audit trail when safety requires it?
4. **Hot path cost** — sampled vs Full strategy; atomic relaxed counters OK for stats.
5. **Layer edge** — if a new import crosses layers, update `module_boundary.ixx`
   (see #1885 / PR template).

Bad rationale: “needed for the feature.”  
Good rationale: “agents map type_invariant_fail to last TypeProp narrow hits via
query:type-propagation-invariant-stats (#1884).”

---

## 7. Tests & commits

| Item | Convention |
|------|------------|
| Test category tags | `// @category: unit|integration|…` + `// @reason: Issue #N — …` |
| CHECK labels | human-readable; include issue when asserting schema |
| Commit | `type(scope): summary (#issue)` — e.g. `feat(arch): … (#1885)` |
| PR | use checklist in `.github/pull_request_template.md` |

---

## 8. Examples (canonical in-tree)

Apply or mirror these when editing:

| File | What to copy |
|------|----------------|
| `src/core/module_boundary.ixx` | Layer DAG comments + `layer_may_depend_on` template |
| `src/core/concepts.ixx` | Concept blocks (shape + non-goals) |
| `src/core/concept_constraints.ixx` | Pass concept contracts |
| `src/compiler/typed_mutation_audit.h` | Audit / correlate API templates |
| `src/core/mutation.ixx` | MutationRecord provenance notes |

Gate helper (doc presence + section checklist):

```bash
python3 scripts/check_naming_convention.py
```

---

## 9. Checklist for authors / agents

- [ ] Name matches section 2–4 (module, type, primitive prefix).
- [ ] New public API has section 5.1 template (or file already documents it).
- [ ] PrimMeta filled for new Agent-visible primitives; no freeze violation.
- [ ] Metrics use kebab keys in hashes; snake counters in C++.
- [ ] Cross-layer edge? Update `module_boundary.ixx` + architecture doc.
- [ ] `./build.py gate` clean.
