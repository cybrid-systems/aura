# tests/bench

## C++ hot-path benches (in-process, `cmake --build build --target …`)

| Target | Source | What it measures |
|--------|--------|------------------|
| **ast_hotpath_bench** | `ast_hotpath_bench.cpp` | Consolidated FlatAST hot paths (#393/#398/#399): `stable-ref` (1k+10k), `children-stable`, `mark-dirty` |
| issue_212_bench | `issue_212_bench.cpp` | Escape-analysis / IR pipeline |
| issue_307_bench | `issue_307_bench.cpp` | Incremental typecheck at EDA scale |
| issue_508_bench | `issue_508_bench.cpp` | DeadCoercionElimination CastOp reduction |
| cycle14_reflect_bench | `cycle14_reflect_bench.cpp` | P2996 reflect serialize throughput (`-freflection`) |
| cycle221_pcv_bench | `cycle221_pcv_bench.cpp` | PersistentChildVector ops / COW |

```bash
./build/ast_hotpath_bench                  # all scenarios
./build/ast_hotpath_bench stable-ref 10k
./build/ast_hotpath_bench children-stable
./build/ast_hotpath_bench mark-dirty
```

## Language / pipeline benches (Python harness)

- `benchmark.py` + `benchmark_cases.py` + fixtures under `tests/fixtures/benchmark/`
- Shootout-style `.aura` ports: `binarytrees_{cons,while,array}.aura`, `pidigits`, `regexredux`, `revcomp`
  - Three binarytrees variants are intentional (cons vs while vs array memory/stack ceilings), not duplicates.

## Unrelated

- `run_bench_all.py` — multi-model LLM EDSL comparison (paths still point at a personal layout; not the C++ suite).
