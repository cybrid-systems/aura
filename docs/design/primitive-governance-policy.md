# Aura Primitives Governance Policy v1.1

> **Issue**: [#1451](https://github.com/cybrid-systems/aura/issues/1451)  
> **Status**: authoritative policy (Agent-Proof)  
> **Companions** (do not replace — read together):  
> - [primitive-vs-stdlib-decision-framework.md](primitive-vs-stdlib-decision-framework.md) — red-line / green-light  
> - [primitives-slim-surface-v2.md](primitives-slim-surface-v2.md) — freeze, budget ≤420, facade  
> - [agentic-slim-surface-rectification.md](agentic-slim-surface-rectification.md) — epic priority  
> - [agent-prompt-template.md](../agent-prompt-template.md) — agent-facing short form  

---

## 0. TL;DR

| Rule | Enforcement |
|------|-------------|
| Default = **stdlib** (`lib/std/`) | Process + review |
| New public `add("…")` only on **red-line** | PR checklist + review |
| No new blocked patterns (`*-stats`, convenience prefixes, `ast:ref-*`) | `check_primitive_surface.py` (hard) |
| Public surface budget ≤ **420** (interim ceiling 700) | `--strict` in `./build.py gate` |
| Observability via **facade only** | `stats:get` / `engine:metrics` / `register_stats_impl` |
| Agents must not invent primitives | Prompt + `(primitive:validate-new)` |

**Before any new C++ primitive**, run:

```scheme
(primitive:validate-new "candidate-name")
```

If `:ok` is `#f`, do **not** land an `add()` without an intentional baseline update + PR justification.

---

## 1. Philosophy alignment (Code-as-Memory)

Every primitive must serve **code-as-memory**:

1. **Clear** — stable name, documented schema, discoverable via `api-reference` / `primitive:describe`.  
2. **Traceable** — mutations and observations go through Guard / StableNodeRef / metrics, not ad-hoc side channels.  
3. **Formally checkable** — prefer typed mutate/query and existing invariant paths over one-off helpers.  

**Forbidden:**

- One-shot convenience functions that only wrap existing ops without a red-line.  
- Temporary / issue-local `*-stats` public names (use `CompilerMetrics` + facade).  
- “Just add a primitive for the test” without demotion plan.

---

## 2. Core capability alignment

| Capability | Governance rule |
|------------|-----------------|
| **Mutation safety** | All `mutate*` paths use outermost `MutationBoundaryGuard`, typed mutate, and StableNodeRef where nodes cross generations. |
| **EDSL core** | Prefer op-dispatch: `(query :op …)`, `(mutate :op …)`, `(workspace :op …)`. Do not fragment into new micro-names. |
| **TUI protected** | `render` / `terminal` / `ansi` / `display` stay available; product surface via `lib/std/tui` (or equivalent). Mark TUI-impacting changes **protected** in PR. |
| **Per-fiber safety** | New primitives must not bypass fiber / mutation-stack / GC safepoint contracts. |
| **1M context / Agent self-evolution** | Prefer fewer, composable names. New public names need `primitive:validate-new` + red-line note. |

---

## 3. Design-goal alignment

| Goal | Mechanism |
|------|-----------|
| **Surface ≤420** | SlimSurface budget (`TARGET_BUDGET=420`, interim hard ceiling 700) |
| **Production readiness** | `./build.py gate` (docs + lint + format + fixtures + freeze + **strict**) |
| **Agent reliability** | Agent prompt forbids inventing stats; registry freeze blocks bad names |
| **Long-term maintainability** | Deprecation → facade → hard remove; demotion batches under epic #1449 |

---

## 4. Agent red lines (non-negotiable)

1. **Do not** `primitives_.add` / invent new public primitive names from agent loops.  
2. **Must** try facade or `lib/std` first (`stats:get`, `engine:metrics`, `std/surface`, op-dispatch).  
3. **TUI** changes must be labeled protected and keep aura-pets / terminal paths working.  
4. **Every new primitive PR** must include a short alignment note: philosophy · capability · design goal · red-line id(s).  
5. **Every behavior change** ships tests (C++ issue test and/or EDSL suite as appropriate).  

Agents that need a capability should:

```
1. (primitive:validate-new "foo")
2. If blocked → implement in lib/std or use existing facade
3. If ok → still require human PR + red-line citation before C++ add()
```

---

## 5. Technical enforcement (landed)

| Layer | Tool |
|-------|------|
| Freeze + budget | `scripts/check_primitive_surface.py` / `--strict` |
| Gate | `./build.py gate` → `cmd_primitive_surface` |
| Runtime inventory | `(engine:surface)`, `(stats:count)`, `(stats:list)` |
| Runtime proposal check | **`(primitive:validate-new name)`** (#1451) |
| Metadata | `PrimMeta` (`deprecated`, `category`, `schema`, `doc`) |
| Deprecation telemetry | `deprecated_prim_dispatch_total` / metrics hits |
| Decision tree | [primitive-vs-stdlib-decision-framework.md](primitive-vs-stdlib-decision-framework.md) |

### `(primitive:validate-new name)` contract

```scheme
(primitive:validate-new "my-new-op")
;; → hash:
;;   schema              1
;;   name                string
;;   ok                  bool     ; #t only if name is free + not freeze-blocked
;;   already-registered  bool
;;   blocked             bool
;;   blocked-category    string or void   ; stats | string | json | …
;;   prefer-stdlib       bool     ; #t when blocked or name looks convenience-like
;;   requires-red-line   bool     ; always #t for new engine prims (process)
;;   advice              string
```

Does **not** register the primitive. Does **not** replace human review.

---

## 6. PR checklist (copy into PR body)

- [ ] Read [decision framework](primitive-vs-stdlib-decision-framework.md); cite red-line id(s) or explain stdlib choice  
- [ ] Ran `(primitive:validate-new "…")` — `:ok` is true **or** intentional baseline update documented  
- [ ] No new blocked-pattern public name  
- [ ] Observability → metrics / `register_stats_impl`, not public `*-stats`  
- [ ] Prefer `(query|mutate|workspace :op …)` over new aliases  
- [ ] Tests updated (issue test and/or suite)  
- [ ] TUI protected callout if terminal/render paths touched  
- [ ] Alignment note: philosophy · capability · design goal  

---

## 7. Category taxonomy (`PrimMeta.category`)

| Category | Use |
|----------|-----|
| `general` | Language / AI work surface |
| `eda` / `sva` / `verification` | Domain packs |
| `deprecated` | Compat aliases (still registered; prefer op-dispatch / facade) |
| (empty) | Legacy only — new work must set a category |

---

## 8. File map

| Path | Role |
|------|------|
| **This file** | Governance policy v1.1 (Agent-Proof) |
| `docs/design/primitive-vs-stdlib-decision-framework.md` | Red-line decision tree |
| `docs/design/primitives-slim-surface-v2.md` | Surface freeze + budget |
| `docs/agent-prompt-template.md` | Short agent paste template |
| `scripts/check_primitive_surface.py` | CI freeze + strict |
| `src/compiler/evaluator_primitives_obs_eval_00.cpp` | `primitive:describe` / **`primitive:validate-new`** |
| `tests/test_issue_1451.cpp` | Policy runtime ACs |

---

## 9. Versioning

| Version | Date | Notes |
|---------|------|--------|
| v1.1 | 2026-07-15 | #1451: philosophy / capability / goals + Agent-Proof + `validate-new` |
| v1.0 | (prior) | Scattered across decision-framework + SlimSurface docs |

_Edits: PR against this file; keep companions in sync for freeze constants and red-line lists._
