# tests/fiber/

Fiber / orchestration / concurrent-unit regression drivers.

Prefer extending a batch binary over adding a new `test_*.cpp` here.

## Default-build batches (CI)

| Target | Role |
|--------|------|
| `test_fiber_strategy_evolve_batch` | strategy/evolve unit ACs (mutexes, merr, analytics parse) |
| `test_fiber_synthesize_batch` | synthesize: JSON parse / PRNG / try_probe / find-after-parens |
| `test_fiber_concurrent_unit_batch` | fingerprint atomic + intend closure/heap slots |

## On-demand orch batches (`EXCLUDE_FROM_ALL`)

| Target | Role |
|--------|------|
| `test_fiber_orch_core_batch` | Fiber::join, mailbox, orch spawn/obs/stable-ref, closedloop, safepoint |
| `test_fiber_orch_parallel_quota_batch` | parallel_orch / intend / stress + resource quota |

```bash
ninja -C build test_fiber_orch_core_batch test_fiber_orch_parallel_quota_batch
```

## Standalone (custom / heavy / flaky under co-link)

| File | Why separate |
|------|----------------|
| `test_concurrent.cpp` | Large concurrency model suite (custom cmake) |
| `test_jit_concurrent_compile.cpp` | Custom JIT concurrent compile target |
| `test_per_fiber_stack_pool_high_concurrency.cpp` | jit_late1 bundle member |
| `test_query_namespace_audit.cpp` | query: demotion audit (registered) |
| `test_set_arena_atomic_owner.cpp` | Concurrent arena owner stress (double-free if co-linked) |
| `test_stress_alloc_storage_lock.cpp` | Multi-minute 100k cons stress |
| `test_terminal_concurrent.cpp` | Terminal registry stress (heap corruption if co-linked) |
| `test_self_heal_policy_engine.cpp` | Policy engine ACs |
| `test_production_sweep.cpp` | Fiber production flag sweep |

Schema-only orch gates also live in `tests/domain/test_domain_gates_batch.cpp`.
