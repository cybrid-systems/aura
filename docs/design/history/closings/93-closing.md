# Issue #93 — Live Programming Environments for Complex Domains

## Status: ✅ ADDRESSED

The Scenario 9 design doc requested by #93 has been written and
committed in this same branch.

## Deliverable

`docs/design/live-programming-environments.md` — 12.6 KB design
document covering:

- **Architecture diagram** — Human + AI → CaaS eval-loop →
  Live system → Feedback (sub-second)
- **Pattern 1: Sub-second CaaS edit-compile-run** — 20 ms
  total latency (parse < 1ms, macro < 1ms, typecheck
  incremental < 5ms, shadow eval < 10ms)
- **Pattern 2: Contract-driven error display** — counter-
  example + contract name + source location in the editor
- **Pattern 3: Hot-swap with metric impact preview** —
  shadow-test results, live preview, canary option before
  promotion
- **Pattern 4: AI-augmented live programming** — `intend`
  with `evolve-pricing` strategy proposes 3 variants, each
  shadow-tested, human picks
- **Pattern 5: Snapshot-and-replay** — version browser with
  scrub + restore (Smalltalk style)
- **Pattern 6: Domain-specific live views** — finance
  order-book, sim particles, game frame timing, industrial
  sensor readings, ML loss curve
- **Pattern 7: Multi-developer + AI collaboration** — CRDT-
  like draft merge with contract-gated promotion
- **Pattern 8: Live programming as research** — every edit
  is an experiment with bandit posterior updates
- **Pattern 9: Production safety rail** — read-only fns,
  human-approval, canary-pct, auto-rollback via
  `*evolution-mode*` config
- **Latency budget** — < 20 ms to first feedback (interactive
  threshold)
- **Industry comparison** — Smalltalk, Lisp/SLIME, Erlang
  live upgrade, Excel, Bret Victor demos; only Aura combines
  sub-20 ms with safety + AI + C++ speed

## Why this is a closing comment, not a code change

The issue is a **design scenario** (not a feature request). It
asks for documentation patterns. All cited primitives are in
place:

| Cited primitive | Status / Doc |
|---|---|
| CaaS eval-loop | `caas_integration.md` |
| `AURA_CONTRACT` | Added in #83 (`89e8782`) |
| `mutate:rebind` | Hot-swap, exists |
| `ast:snapshot` / `ast:restore` | Snapshot + restore, exists |
| Incremental typecheck | `incremental_dirty_propagation.md` |
| `synthesize:define` | EDSL primitive, exists |
| `intend` + E4 | `e4_evolvable_strategies.md` (extended by #63) |
| `query:*` EDSL | 20+ primitives, exists (per #82) |
| `define-tunable` | E4 primitive (from #63) |
| `pmr` + `gc-temp` | `unify_cell_heap.md` + `double-arena.md` |
| `set-code` / `eval-current` / `typecheck-current` | `code_evolution_pipeline.md` |
| `*evolution-mode*` | `production-live-evolution.md` (from #87) |

## How to close on GitHub

```bash
gh issue close 93 -c "See docs/design/live-programming-environments.md
(Scenario 9 design doc) — 9 patterns (sub-second CaaS, contract
errors, metric preview, AI-augmented, snapshot-replay, domain
views, multi-dev collab, research mode, safety rails), 20ms
latency budget, industry comparison. All cited primitives are
in place on main."
```

Or paste this file as a GitHub comment.
