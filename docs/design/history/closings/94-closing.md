# Issue #94 — AI-Driven Meta-Programming and Tool Evolution

## Status: ✅ ADDRESSED

The Scenario 10 design doc requested by #94 has been written
and committed in this same branch.

## Deliverable

`docs/design/ai-driven-meta-programming.md` — 14.1 KB design
document covering:

- **Three layers of meta-programming** — L1 application code,
  L2 primitives, L3 compiler/runtime. The scenario targets L2
  only (L3 is research-grade out-of-scope).
- **The tool-evolution loop** — Observe (usage mining) →
  Propose (synthesize:define) → Verify (contract + property-
  based tests) → Stage (sandboxed toolset) → Pilot (1 site)
  → Promote (stdlib)
- **Pattern 1: Primitive usage mining** — `mine-repeated-
  compositions` finds N-grams in `*intend-history*` that
  appear N+ times
- **Pattern 2: AI-proposed primitive synthesis** — `intend`
  with `synthesize-primitive` strategy returns impl + test
  suite
- **Pattern 3: Property-based test generation** — `gen-iso-
  date` produces 1000 random dates; check new impl matches
  reference
- **Pattern 4: AURA_CONTRACT for primitives** — every new
  primitive has `pre:` / `post:`; gates hot-swap
- **Pattern 5: Sandbox the candidate primitive** —
  `*stdlib-sandbox*` is mutable, `*stdlib-v3*` is immutable;
  compile-time check via `AURA_CONTRACT_PRE`
- **Pattern 6: Pilot and measure** — replace 1 of 23 sites,
  shadow for 1h, verify p99 / correctness / error rate
- **Pattern 7: Hot-swap integration** — same pattern as
  `production-live-evolution.md` but for primitives
- **Pattern 8: Promote to stdlib** — if used in >5 projects /
  >100 sites, raise to stdlib with full doc + tests
- **Pattern 9: Rollback and obsolescence** — `ast:restore` or
  `*stdlib-deprecated*` list
- **Pattern 10: AI for primitive composition** — `compose`
  higher-level workflows, optionally promote to macro
- **Roles of compile-time reflection (`std::meta`)** and
  **macros (`defmacro`)** in primitive synthesis
- **Safety rails** — no L3 modifications, sandboxed prims,
  mandatory contracts, pilot required, human review for
  stdlib, audit log

## Why this is a closing comment, not a code change

The issue is a **design scenario** (not a feature request). It
asks for documentation patterns. All cited primitives are in
place:

| Cited primitive | Status / Doc |
|---|---|
| `std::meta` (compile-time reflection) | `compile_time_reflection.md` |
| `defmacro` + quasiquote | `macro_system_v2.md`, `hygienic_macros.md` |
| `mutate:rebind` | Hot-swap, exists |
| `intend` + `*intend-history*` | `intent_orchestration.md` |
| `synthesize:define` | EDSL primitive, exists |
| `AURA_CONTRACT` | Added in #83 (`89e8782`) |
| CaaS (sandboxed eval) | `caas_integration.md` |
| `ast:snapshot` / `ast:restore` | Snapshot + restore, exists |
| `evolve-strategy` (E4) | Added in #63 Phase 3 (`0ee43c8`) |
| `set-code` / `eval-current` | `code_evolution_pipeline.md` |
| `pid:analyze` | Failure diagnosis, exists |
| `std/llm` | `llm_stdlib.md` |

## How to close on GitHub

```bash
gh issue close 94 -c "See docs/design/ai-driven-meta-programming.md
(Scenario 10 design doc) — 3 layers, tool-evolution loop, 10
patterns (usage mining, synthesis, property tests, contracts,
sandbox, pilot, hot-swap, stdlib promotion, rollback, compo-
sition), safety rails. All cited primitives are in place on
main."
```

Or paste this file as a GitHub comment.
