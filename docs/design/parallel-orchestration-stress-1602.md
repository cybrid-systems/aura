# Parallel orchestration stress (#1602)

**Issue:** [#1602](https://github.com/cybrid-systems/aura/issues/1602)  
**Builds on:** #1584 Fiber::join · #1585 MultiFiberMailbox · #1586 parallel_orch ·
#1587 `(parallel-intend)` · #1588 orch module · #1595 join linear · #1597 closedloop orch

## Goal

High-pressure end-to-end coverage for concurrent mutate + parallel agents +
join + GC so production orchestration regressions surface before release.

## Artifacts

| Artifact | Role |
|----------|------|
| `tests/suite/parallel_orchestration_stress.aura` | E2E Aura suite (CI `./build.py test suite`) |
| `tests/test_parallel_orchestration_stress_1602.cpp` | C++ metrics + true multi-worker stress |
| `tests/test_parallel_orch.cpp` AC9 | Light multi-round extension |
| `tests/fixtures/benchmark_tests.json` | `parallel_intend_small` / `parallel_intend_with_gc` |

## How to run

```bash
# Suite (auto-picked by suite runner; 120s timeout)
./build/aura --load tests/suite/parallel_orchestration_stress.aura
./build.py test suite

# C++ stress
ninja -C build test_parallel_orchestration_stress_1602
./build/test_parallel_orchestration_stress_1602

# Sister unit tests
./build/test_parallel_orch
./build/test_parallel_intend_primitive

# Micro-bench cases (SLO gate)
python3 tests/benchmark.py --json | jq '.[] | select(.name|test("parallel_intend"))'
```

## Interpreting results

### Suite sections

| Section | Pass means |
|---------|------------|
| `parallel-intend-basic` | Batch status ok, ok-count matches |
| `mutate-gc-fiber-batch` | Concurrent mutate + gc-heap + nested fiber:join stable |
| `stress-32-tasks` | Cap=8 batch of 32 mostly succeeds |
| `fiber-join-storm` | Outer fiber:spawn/join arithmetic correct |
| `orch-parallel-intend-alias` | #1588 alias works |
| `metrics-parallel-orch+closedloop` | schema 1586 + closedloop ≥1593 + orch-health |
| `post-gc-binding` | Function binding survives gc-heap |
| `closedloop-readiness-keys` | Fresh metrics after GC still readable |

**Note:** Sample metric **integers before** `gc-heap`. FlatHashTable handles can
be invalidated by aggressive heap compaction; re-query after GC for new hashes.

### C++ metrics

- `g_parallel_orch_stats` — batches / joined / ok / mailbox_posts
- `Fiber::join_wait_us_total` / `join_latency_hist_*` / `join_linear_enforcement_total`
- `query:ai-closedloop-readiness-stats` — `orch-health-score`, `avg-join-latency-us`

Fail if: crash, ok_count collapses, join hangs past timeout, or schema drift.

## Out of scope

- Full ASAN soak (see sanitizer CI jobs)
- Changing parallel_orch core algorithm
