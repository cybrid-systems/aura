# tests/bench

## C++ hot-path benches

Two in-process C++ benches source from `tests/bench/`:

| Target | Source | What it measures |
|--------|--------|------------------|
| `ast_hotpath_bench` | `tests/bench/ast_hotpath_bench.cpp` | Consolidated FlatAST hot paths (#393/#398/#399): `stable-ref` (1k + 10k), `children-stable`, `mark-dirty`; plus counter-shard (#4719 P6: sharded hot-path counter bump cost, 1 vs 8 parallel fibers, AC <= 5%) |
| `cycle221_pcv_bench` | `tests/bench/cycle221_pcv_bench.cpp` | PersistentChildVector ops / COW (#221 slice 5/5, AC < 2µs/op) |

Other issue-keyed C++ benches live next to their feature tests, not in `tests/bench/`:

| Target | Source | What it measures |
|--------|--------|------------------|
| `issue_212_bench` | `tests/compiler/bench_refactor_surfaces.cpp` | Escape-analysis / IR pipeline |
| `issue_307_bench` | `tests/compiler/bench_ir_relower.cpp` | Incremental typecheck at EDA scale |
| `issue_508_bench` | `tests/compiler/bench_dead_coercion_elim.cpp` | DeadCoercionElimination CastOp reduction |
| `cycle14_reflect_bench` | `tests/reflect/bench_reflect.cpp` | P2996 reflect serialize throughput (`-freflection`) |

Usage:
```bash
./build/ast_hotpath_bench                  # all scenarios
./build/ast_hotpath_bench stable-ref 10k
./build/ast_hotpath_bench children-stable
./build/ast_hotpath_bench mark-dirty
./build/ast_hotpath_bench counter-shard   # Issue #4719: 1 fiber vs 8 parallel fibers
./build/cycle221_pcv_bench
```

## Language / pipeline benches (Python harness)

- `benchmark.py` + `benchmark_cases.py` + fixtures under `tests/fixtures/benchmark/`
- Shootout-style `.aura` ports: `binarytrees_{cons,while,array}.aura`, `fasta.aura`, `fannkuchredux.aura`, `nbody.aura`, `pidigits.aura`, `regexredux.aura`, `revcomp.aura`
  - Three `binarytrees` variants are intentional (cons vs while vs array memory/stack ceilings), not duplicates.

## Unrelated

- `run_bench_all.py` — multi-model LLM EDSL comparison (paths still point at a personal layout; not the C++ suite).

## Multi-objective search perf gate (#4940)

`tests/fixtures/bench/moead_4940_perf.aura` is the strict perf item for
`std/moead`: ZDT1 at 30 dim / 20 000 evaluations must finish within 30 s on
the reference `dev:v1.1.1` x86_64 walker.

```bash
./build/aura tests/fixtures/bench/moead_4940_perf.aura                 # measure + print
AURA_PERF_GATE=1 ./build/aura tests/fixtures/bench/moead_4940_perf.aura  # enforce 30 s
```

Without `AURA_PERF_GATE` the run always exits 0 (a slow arm64 dev box or a
loaded CI walker is not an x86 regression); the HV ratio and nfe it prints
are deterministic and match the 20k ZDT1 line of
`tests/fixtures/bench/pareto_4940_calibration.aura`.
