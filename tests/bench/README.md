# tests/bench

## C++ hot-path benches

Two in-process C++ benches source from `tests/bench/`:

| Target | Source | What it measures |
|--------|--------|------------------|
| `ast_hotpath_bench` | `tests/bench/ast_hotpath_bench.cpp` | Consolidated FlatAST hot paths (#393/#398/#399): `stable-ref` (1k + 10k), `children-stable`, `mark-dirty` |
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
./build/cycle221_pcv_bench
```

## Language / pipeline benches (Python harness)

- `benchmark.py` + `benchmark_cases.py` + fixtures under `tests/fixtures/benchmark/`
- Shootout-style `.aura` ports: `binarytrees_{cons,while,array}.aura`, `pidigits.aura`, `regexredux.aura`, `revcomp.aura`
  - Three `binarytrees` variants are intentional (cons vs while vs array memory/stack ceilings), not duplicates.

## Unrelated

- `run_bench_all.py` — multi-model LLM EDSL comparison (paths still point at a personal layout; not the C++ suite).
