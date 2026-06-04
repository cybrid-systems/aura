# Issue #88 — Adaptive High-Performance Optimization Engines

## Status: ✅ ADDRESSED

The Scenario 4 design doc requested by #88 has been written and
committed in this same branch.

## Deliverable

`docs/design/adaptive-optimization-engines.md` — 11.7 KB design
document covering:

- **Architecture diagram** — Profiler → Selector (Bayesian
  bandit) → Apply (hot-swap) → Measure → Update
- **Pattern 1: Algorithm variant auto-tuning** — `define-tunable`
  with N candidates, bandit picks winner, hot-swap in
- **Pattern 2: Data layout adaptation** — AoS ↔ SoA
  re-layout while program keeps `vector<Record>` view, with
  typed-mutation guarantees
- **Pattern 3: Parallelism strategy selection** — `std::execution`
  variants (seq/par/par_unseq) + GPU offload, power-aware
  bandit
- **Pattern 4: JIT / codegen tuning** — `std::meta` reflection
  lets the JIT introspect its own IR schema to know which
  lowering strategies are possible
- **CaaS for fair evaluation** — `rdtsc` timing, bounded
  workload snapshot, sandboxed experiment
- **Performance budget** — < 2% CPU overhead for continuous
  adaptation
- **Memory budget** — double-arena with `gc-temp` between
  experiments; `set-memory-policy` auto-GC at 90%
- **Safety invariants** — `AURA_CONTRACT_PRE/POST` for every
  hot-swap, fail-stop on contract violation → `ast:restore`

## Why this is just a closing comment, not a code change

The issue is a **design scenario** (not a feature request). It
asks for documentation patterns. All the Aura primitives cited
in the doc are in place today:

| Cited primitive | Status / Doc |
|---|---|
| `std::meta` reflection | `docs/design/compile_time_reflection.md` |
| `std::execution` | `docs/design/thread_pool_offload.md` |
| `define-tunable` | Backed by E4 strategies |
| `mutate:rebind` | Hot-swap primitive, exists |
| `ast:snapshot` / `ast:restore` | Snapshot + restore, exists |
| `AURA_CONTRACT_PRE/POST` | Added in #83 (`89e8782`) |
| `double-arena` + `pmr` | `docs/design/double-arena.md` + `unify_cell_heap.md` |
| `gc-arena-info` / `memory-pressure` | Arena introspection, exists |
| `caas-run` | CaaS primitive, exists |
| `E4 evolve-strategy` | Added in #63 Phase 3 (`0ee43c8`) |

## Reference implementations cited in the doc

All three exist on `main`:

- `tests/tasks/edsl/edsl-optimize-benchmark-kw.aura` — E4
  keyword-arg optimization
- `tests/tasks/edsl/edsl-optimize-fitness.aura` — fitness
  function design
- `tests/tasks/edsl/edsl-optimize-multiarg.aura` — multi-arg
  optimization
- `projects/evo-kv/evo-kv-evolve.aura` — full production
  evolve loop

## How to close on GitHub

```bash
gh issue close 88 -c "See docs/design/adaptive-optimization-engines.md
(Scenario 4 design doc) — 4 patterns (algorithm auto-tune, data
layout, parallelism, JIT lowering) + CaaS fair-eval + safety
contracts. All cited primitives are in place on main."
```

Or paste this file as a GitHub comment.
