# Issue #91 — Evolving Domain-Specific Languages

## Status: ✅ ADDRESSED

The Scenario 7 design doc requested by #91 has been written and
committed in this same branch.

## Deliverable

`docs/design/evolving-dsls.md` — 11.3 KB design document
covering:

- **Two flavors of evolution** — Flavor A (semantics evolves,
  syntax stable, common) and Flavor B (syntax + semantics
  evolve, rarer)
- **Evolution taxonomy** — 2×2 grid of {syntax, semantics} ×
  {stable, evolves}
- **Pattern 1: Tunable constants in DSLs** — `define-tunable`
  for hot thresholds; user code never changes
- **Pattern 2: Evolving rule composition** — business-rules DSL
  with E4-tuned rule priority per cohort
- **Pattern 3: Evolving strategy parameters** — game-AI DSL
  with bandit-tuned cowardice
- **Pattern 4: Evolving DSL interpretation (meta case)** — the
  DSL's interpreter itself evolves (Flavor B territory)
- **Pattern 5: Evolving DSL contracts** — the spec is also a
  tunable (research-grade)
- **Roles of macros / contracts / hot-swap / CaaS / E4** —
  each primitive mapped to a specific job in the evolving-DSL
  stack
- **Industry comparison** — QuantConnect, AnyLogic, Drools,
  behavior-tree game AIs; only Aura's DSLs evolve without
  restart

## Why this is a closing comment, not a code change

The issue is a **design scenario** (not a feature request). It
asks for documentation patterns. All cited primitives are in
place:

| Cited primitive | Status / Doc |
|---|---|
| `defmacro` + quasiquote | `macro_system_v2.md`, `hygienic_macros.md` |
| `define-tunable` (E4) | Added in #63 Phase 3 (`0ee43c8`) |
| `AURA_CONTRACT` | Added in #83 (`89e8782`) |
| `CaaS` (sandboxed eval) | CaaS primitive, exists |
| `mutate:rebind` | Hot-swap, exists |
| `ast:snapshot` / `ast:restore` | Snapshot + restore, exists |
| `intend` | `intent_orchestration.md` |
| `evolve-strategy` (E4) | `e4_evolvable_strategies.md` |

## How to close on GitHub

```bash
gh issue close 91 -c "See docs/design/evolving-dsls.md (Scenario 7
design doc) — 2 flavors × 2×2 evolution taxonomy, 5 patterns
(tunable constants, rule composition, strategy params, meta-
interpreter, evolvable contracts), roles of macros/contracts/
hot-swap/CaaS/E4, industry comparison. All cited primitives
are in place on main."
```

Or paste this file as a GitHub comment.
