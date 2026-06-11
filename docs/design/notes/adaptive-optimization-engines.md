# Adaptive High-Performance Optimization Engines

**Status:** Design exploration for Scenario 4 of the
[Scenario issues series] (issue #88).

## Why this is a killer scenario

Most optimization engines are static: you ship a compiler with
JIT, a query optimizer with cost model, an ML runtime with a
hand-tuned kernel library. To get better, you do a release.

Adaptive optimization flips this. The runtime itself:

1. **Profiles its own workload** — op mix, branch prediction hit
   rate, cache miss ratio, allocation churn
2. **Picks a strategy from a candidate pool** — algorithm
   variant, data layout, inlining heuristic, kernel choice
3. **Hot-swaps the new strategy** — no restart, no recompile
4. **Measures the impact** — p99 latency, throughput, memory
5. **Keeps the winner, rolls back the loser** — Bayes-style
   bandits or simple threshold

Aura provides all five primitives at the language level (not via
external config + ops ceremony), and at C++26 performance
levels (not via VM overhead). That's the gap this doc fills.

## The optimization engine architecture

```
                ┌─────────────────────────────────────┐
                │       Workload (real traffic)       │
                └────────────────┬────────────────────┘
                                 │ observed ops
                                 ▼
        ┌────────────────────────────────────────────┐
        │  Profiler  (introspect via std::meta)      │
        │  - op frequencies                           │
        │  - shape distributions (tensor dims etc.)   │
        │  - cache miss rate (gc-arena-info)          │
        └────────────────┬───────────────────────────┘
                         │ profile record
                         ▼
        ┌────────────────────────────────────────────┐
        │  Strategy Selector (E4 bandit)              │
        │  - candidates: A, B, C, D                  │
        │  - posterior over (cost, throughput)        │
        │  - chooses candidate via UCB                │
        └────────────────┬───────────────────────────┘
                         │ chosen: B
                         ▼
        ┌────────────────────────────────────────────┐
        │  Apply candidate (hot-swap)                 │
        │  - mutate:rebind to new algorithm           │
        │  - ast:snapshot for rollback                │
        │  - AURA_CONTRACT_POST validates invariants  │
        └────────────────┬───────────────────────────┘
                         │ live traffic continues
                         ▼
        ┌────────────────────────────────────────────┐
        │  Measure (after 30s window)                 │
        │  - p99 latency vs baseline                  │
        │  - throughput vs baseline                   │
        │  - memory headroom                          │
        └────────────────┬───────────────────────────┘
                         │ pass / fail
                         ▼
                keep winner, update bandit,
                schedule next experiment
```

## Pattern 1: Algorithm variant auto-tuning

A workload has hot inner loops. Each loop has N implementation
candidates (different algorithms, different constants, different
unroll factors). The engine picks the best per-region.

```aura
;; Define a tunable region with candidates
(define-tunable fast-sort
  candidates: (quicksort
               radix-sort
               pdqsort
               ;; LLM-discovered variant:
               introsort-with-fallback)
  budget: 200                    ;; max evals per candidate
  metric: throughput-on-random-1M)

;; Profile says: 80% of CPU in fast-sort
;; Selector picks radix-sort, hot-swaps in.
;; p99 drops 40%, bandit updates.
```

**Implementation hooks:**
- `compile_time_reflection.md` — each candidate's signature
  reflected via `std::meta` to verify interchangeability
- `e4_evolvable_strategies.md` — bandit selection logic
- `mutate:rebind` — atomic hot-swap of the loop body
- `AURA_CONTRACT_POST` — verifies output matches reference
  implementation (uses shadow test from production-live-evolution.md)

## Pattern 2: Data layout adaptation

A polymorphic container starts as `vector<Record>`. Profile
shows 90% sequential scan → switch to `vector<Record>` with
struct-of-arrays layout. Hot-swap.

```aura
(define-tunable column-store
  candidates: (aos-layout
               soa-layout
               hybrid-by-access-pattern)
  trigger: (and (> scan-ratio 0.7)
                (> row-count 10000))
  metric: scan-throughput)

;; On trigger:
;;   1. snapshot (ast:snapshot "pre-soa")
;;   2. mutate:rebind to soa-layout variant
;;   3. copy data (Aura-level arena transfer, no allocs)
;;   4. measure for 60s
;;   5. keep or ast:restore
```

**Implementation hooks:**
- `unify_cell_heap.md` + `double-arena.md` — `pmr` vector with
  arena resource lets us resize during layout migration without
  heap fragmentation
- `typed_mutation_design.md` (now `design/core/typed_mutation.md`) — typed re-layout must preserve
  type identity, so the rest of the program can keep its
  `vector<Record>` view
- `value_encoding.md` — encoding choice (`short-string` for
  small records) interacts with the layout choice; both must be
  tuned together

## Pattern 3: Parallelism strategy selection

For embarrassingly parallel work, choose between:

- `std::execution::seq` (single thread, no overhead)
- `std::execution::par` (thread pool, work-stealing)
- `std::execution::par_unseq` (vectorization, may break ordering)
- Aura's `thread_pool_offload` (GPU offload for eligible ops)

The selector picks based on workload size, branch density, and
current pool saturation.

```aura
(define-tunable parallel-matmul
  candidates: (sequential
               par-thread-pool
               par_unseq-simd
               gpu-offload)
  trigger: always-once-per-session
  metric: gflops-per-watt)   ;; power-aware choice

;; On a power-constrained server, par_unseq wins.
;; On a desktop with discrete GPU, gpu-offload wins.
;; The bandit learns this once and never re-runs the sweep
;; unless hardware changes (detected via /proc/cpuinfo hash).
```

**Implementation hooks:**
- `concurrency_model.md` + `concurrent_channels.md` — the
  channel primitives that `par_unseq` operations use internally
- `thread_pool_offload.md` — Aura's executor abstraction over
  `std::execution`
- `execution_adapter.md` — backpressure + cancellation
  semantics that make long-running GPU kernels safe to abort

## Pattern 4: JIT / codegen tuning

For engines that emit code at runtime (compilers, query
optimizers), the meta-decision is: which lowering strategy?

```aura
(define-tunable codegen-lowering
  candidates: (one-pass-walk
               worklist-with-cse
               dataflow-after-parse
               ;; discovered by E4 LLM:
               bidirectional-type-driven)
  metric: compile-time-p99)
```

Reflection via `std::meta` is critical here — the JIT needs to
introspect its own IR schema to know which lowering strategies
are even possible. C++26 reflection makes this 100x simpler
than the typical `template <typename T> struct traits` pattern.

**Implementation hooks:**
- `compile_time_reflection.md` — the IR schema is reflected at
  startup
- `ir_pipeline_design.md` — the lowering pipeline plug points
- `synthesize-pipeline-v2.md` + `synthesize_strategies.md` —
  the synthesis path that can propose new lowering strategies
- `llvm_jit.md` — the final emit step (once a strategy wins, it
  emits to LLVM and the rest is C++ speed)

## The role of CaaS

Each candidate evaluation is itself a small CaaS run:

```aura
(caas-run
  (lambda ()
    (let ((start (rdtsc)))
      (run-workload-snapshot 1000)
      (list 'elapsed-cycles (- (rdtsc) start)
            'p99 (collect-p99)
            'memory (cdr (assq 'overall-pct (memory-pressure)))))))
```

CaaS gives you:
- **Sandboxed evaluation** — candidate can't corrupt the
  runtime
- **Cycle-accurate timing** — `rdtsc` is sub-microsecond,
  not `current-time` (which is 1-second resolution)
- **Bounded resource** — `run-workload-snapshot 1000` runs
  exactly 1000 ops and returns, no infinite loops
- **Reproducibility** — same input snapshot, same outcome
  (modulo hardware jitter), enables Bayesian comparison

This is the missing piece in most "adaptive runtime" designs:
they have the candidate set, but no cheap, fair, sandboxed way
to evaluate. CaaS fills it.

## Performance budget

The adaptive engine itself must not be the bottleneck. A
typical budget:

| Component | Time budget | Frequency |
|---|---|---|
| Profiler (sample) | < 1% of CPU | continuous |
| Bandit update | < 100 µs | per experiment |
| Hot-swap | < 1 ms | per experiment (rare) |
| Re-measure | 30-60 s | per experiment |

Total: < 2% CPU overhead for continuous adaptation, even with
4-8 candidates per tunable region.

## Memory budget

The double-arena design is critical:

- All candidate implementations live in `arena_` (persistent)
- All experiment state lives in `temp_arena_` and is reclaimed
  on `gc-temp` after each experiment
- `set-memory-policy` with `"auto-gc" #t "critical-pct" 90`
  ensures we never blow the memory budget under adaptation

Without this, an adaptive engine that "tries things" would
leak memory proportional to candidate count × experiment
count — fatal in production.

## Safety invariants (contract checks)

Every adaptive hot-swap runs three contract checks:

```aura
;; AURA_CONTRACT_PRE: pre-conditions of the candidate
(define-contract pre:algorithm-swap
  (old-impl new-impl input-shape)
  (and (same-signature? old-impl new-impl)
       (acceptable-input-range? new-impl input-shape)))

;; AURA_CONTRACT_POST: outputs match reference
(define-contract post:algorithm-swap
  (old-impl new-impl input-shape)
  (let ((ref (old-impl input-shape))
        (new (new-impl input-shape)))
    (within-tolerance? ref new 0.001)))
```

If `post:` fails, `ast:restore` is called automatically. The
adaptive engine is **fail-stop** — it never leaves the
runtime in an inconsistent state.

## Reference implementations

- `tests/tasks/edsl/edsl-optimize-benchmark-kw.aura` — E4
  keyword-arg optimization
- `tests/tasks/edsl/edsl-optimize-fitness.aura` — fitness
  function design
- `tests/tasks/edsl/edsl-optimize-multiarg.aura` — multi-arg
  optimization
- `projects/evo-kv/evo-kv-evolve.aura` — full production-grade
  evolve loop using E4 + CaaS

## Comparison vs industry patterns

| Pattern | Used by | Aura's take |
|---|---|---|
| PGO (profile-guided opt) | GCC, Clang, HotSpot | Compile-time, not runtime |
| Auto-tuning libraries (ATLAS, FFTW) | HPC | Off-line, not adaptive |
| Adaptive query processing | Eddies, Telegraph | DB-specific, not general |
| ML-based compiler optimization | Google/AutoML, TVM | External toolchain |
| Bayesian opt for kernels | Survey paper 2024 | Python + subprocess |
| **Aura adaptive engine** | **This work** | **In-runtime, C++ perf, safe hot-swap** |

Aura combines in-runtime adaptation with C++26 performance and
safe rollback. The closest analog is Facebook's "adaptive
concurrency" approach for HHVM, but without the strategy pool
or Bayesian selection.

## Open follow-ups (not blocking this issue)

- **Strategy discovery via E4 LLM** — currently the candidate
  pool is hand-curated. The next step is letting E4 propose
  new candidates via synthesis. Tracked as `#86` follow-up.
- **Cross-region coordination** — when multiple tunables
  interact (layout choice affects which kernels win), the
  bandit needs to be joint, not independent. Open research.
- **Hardware fingerprinting** — caching the bandit posterior
  by `/proc/cpuinfo` + `/proc/meminfo` hash so the engine
  doesn't re-learn on every restart. Small follow-up.
