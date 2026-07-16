# Linear Post-Mutate Stress Testing

**Issue:** #1544 (parent #1478 AC #5)  
**Related:** #1538 (combined pipeline), #1515 (GC/steal probes), #1525 (multi-fiber), #1543 (GC root audit)

## Goal

Exercise the **linear post-mutate contract** under production-shaped load:

1. Long-running AI multi-round self-modification (`typed_mutate` / dual-epoch)
2. GC pressure (`request_gc_safepoint` immediate path)
3. Concurrent fiber steal (`probe_linear_ownership_on_fiber_steal`)

## Harness (`tests/test_issue_1544.cpp`)

| AC | What |
|----|------|
| AC1 | **10 000** iterations of mutate → safepoint → fiber-steal |
| AC2 | `linear_post_mutate_enforcements` monotonic across the loop |
| AC3 | Injected **Moved** frames still fail `linear_post_mutate_enforce`; `linear_ownership_violation_prevented` advances by the expected count |
| AC4 | Concurrent steal workers while main mutates (no crash) |
| AC5 | Completes under **60s** (CI-friendly) |

### Per-iteration sequence

```
AI mutation:
  - every iter: public_atomic_bump_epochs_and_stamp_bridge (dual-epoch write)
  - every 50th: public_typed_mutate rebind (full combined pipeline #1538)
GC:  request_gc_safepoint()          // sync_linear_roots + audit
Fiber: probe_linear_ownership_on_fiber_steal()
Enforce: linear_post_mutate_enforce(owned_frame)  // quiet Owned scan
```

Full `typed_mutate` on every iter would exceed the 60s CI budget on module-heavy
builds; dual-epoch bump is the write-side of AI self-modify (#1476) and is
paired with periodic full rebind so both halves stay warm. Hot loops avoid
per-iter CHECK I/O (accumulate counters, assert once).

### Moved detection under pressure

Every 250 iters the harness:

1. Allocates an EnvFrame with a **Moved** binding
2. Calls `linear_post_mutate_enforce` → expects `false`
3. Asserts `linear_ownership_violation_prevented` grew

This keeps the real SoA scan (#1539) honest under epoch/GC churn.

## Counters watched

| Counter | Expectation |
|---------|-------------|
| `linear_post_mutate_enforcements` | Non-decreasing; grows by ≥1 per enforce call |
| `linear_ownership_violation_prevented` | Grows exactly when Moved frames are scanned |
| `linear_post_mutate_pipeline_total` | Grows on typed_mutate samples |
| `linear_steal_enforced` / GC root audits | Non-decreasing (side effects of steal/safepoint) |

## ASan / TSan

- Default CI: normal build, 60s wall-clock budget in-test.
- Optional: rebuild with `build_asan` / TSan and re-run `test_issue_1544`.
  The test is single-process and avoids intentional data races on non-atomic
  Evaluator state (steal workers only call the probe entry that takes its own
  locks).

## Extending

- Raise `kIters` for nightlies via env `AURA_STRESS_ITERS`.
- Add true fiber spawn via serve scheduler when a dedicated fiber stress
  binary is needed (see #1525 multi-worker patterns).
