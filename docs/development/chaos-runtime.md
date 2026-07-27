# Chaos runtime suite (#2202)

Fixed-seed multi-worker chaos covering **mutate stack × steal × GC safepoint ×
mailbox × panic defer** — the combination path that isolated unit tests miss.

## Run

```bash
# Default CI-class happy path (seed=1, ~8 workers × 64 fibers)
./build/test_chaos_fiber_mutation_gc_mailbox

# Deterministic seed
AURA_CHAOS_SEED=42 ./build/test_chaos_fiber_mutation_gc_mailbox

# Scale (nightly / local soak — AC5)
AURA_CHAOS_WORKERS=16 AURA_CHAOS_FIBERS=128 AURA_CHAOS_STEPS=200 \
  AURA_CHAOS_SEED=7 ./build/test_chaos_fiber_mutation_gc_mailbox

# Fault injection self-test (orphan GcDeferReason detection)
AURA_CHAOS_FAULT=skip_clear_gc_defer ./build/test_chaos_fiber_mutation_gc_mailbox
```

## Env knobs

| Variable | Default | Meaning |
|----------|---------|---------|
| `AURA_CHAOS_SEED` | `1` | RNG seed (CI flake budget) |
| `AURA_CHAOS_WORKERS` | `8` | Scheduler workers |
| `AURA_CHAOS_FIBERS` | `64` | Concurrent fibers |
| `AURA_CHAOS_STEPS` | `80` | Ops per fiber |
| `AURA_CHAOS_NEST_MAX` | `3` | Max nested mutation checkpoints |
| `AURA_CHAOS_FAULT` | empty | Fault inject mode (see below) |

## Invariants (happy path)

1. All fibers complete within wall budget; no stuck GC phase.
2. Process `GcDeferReason` bitmask clear (no orphan panic defer).
3. `defuse_version` has no catastrophic reverse under concurrent samples.
4. Steal skip / deferred counters remain well-defined (unsigned).
5. Mailbox makes progress without permanent deadlock.
6. Yield-under-Guard rejections stay non-parking (#2200).

## Fault injection

| `AURA_CHAOS_FAULT` | Injected bug | Expected detection |
|--------------------|--------------|--------------------|
| `skip_clear_gc_defer` | Arm panic defer without release | Orphan `GcDeferReason` / `should_defer_destructive_gc` |

The suite **detects** the fault and then cleans up so later tests stay green.

## Extending when new P0 gates land

When new production gates ship, add optional end-of-run checks here (keep
happy-path fast; put heavy checks behind env flags):

| Gate | Metric / probe to assert | Issue lineage |
|------|--------------------------|---------------|
| MutationSafetySnapshot steal | `mutation_steal_snapshot_mismatch` == 0 under inject | #2184 |
| Mailbox depth gate | `recv_rejected_in_mutation_boundary` finite; no park under Guard | #2188 |
| Yield-under-held | `yield_while_mutation_held_total` increases only on reject path | #2200 |
| Migration refresh | `fiber_migration_refresh_total` / post-steal closed-loop | #2194 |
| Hold hard timeout | `long_mutation_forced_abort_total` under strict spin | #2199 |
| GC defer bitmask | `defer_reasons_snapshot() == None` after happy path | #2086 / #2088 |

**Pattern:** add a `CHECK` in `check_happy_invariants`, document the env flag
in this table, and if the gate is expensive, gate the check with
`AURA_CHAOS_ASSERT_<NAME>=1`.

## CI

Registered as `test_chaos_fiber_mutation_gc_mailbox` (issue-test + LLVM JIT link).
Default step count keeps wall time well under 120s (AC2). Nightly can raise
`AURA_CHAOS_STEPS` / fiber counts without changing source.
