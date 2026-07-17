# Long-running recovery latency + stall budget

**Issues:** #1583 (instrumentation + stats), #1207 (scaffold)  
**Surfaces:** panic-restore, quota-reject, `query:longrunning-recovery-stats`, `query:production-health`

## Problem

panic-restore and quota-reject lacked high-resolution timing and a stall
budget, so “zero-stall” commercial SLO and SEVA reactions could not be
quantified.

## Contract

```
panic restore / quota reject
  ──chrono──► record_recovery_latency_us(us, kind)
                ├─ latency_us_total / samples / max
                ├─ 9-bucket histogram → p50 / p99 (upper edges)
                └─ if us > stall_budget_us → stall_violations++

default stall_budget_us = 5000
set via Evaluator::set_recovery_stall_budget_us
```

| Metric / field | Role |
|----------------|------|
| `longrunning_recovery_stall_budget_us` | Configurable budget |
| `longrunning_recovery_latency_us_total` | Sum latency |
| `longrunning_recovery_samples` | Sample count |
| `longrunning_recovery_stall_violations_total` | Over-budget count |
| histogram[9] | p50/p99 approximation |

## query:longrunning-recovery-stats (schema 1583)

`stall-budget-us`, `samples`, `panic-samples`, `quota-samples`,
`latency-us-total`, `latency-us-max`, `latency-us-avg`,
`latency-p50-us`, `latency-p99-us`, `stall-violations`, `schema`.

## production-health (#1215 + #1583)

Adds `recovery-stall-violations`, `recovery-p99-us`, `stall-budget-us`;
score also subtracts stall violations.

## Tests

`tests/test_longrunning_recovery_latency.cpp` — AC1–AC6.
