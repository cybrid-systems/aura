# Orchestration ResourceQuota (#1600)

**Issue:** [#1600](https://github.com/cybrid-systems/aura/issues/1600)  
**Builds on:** #1579 (process ResourceQuota), #1590 (hot-path), #1584–#1588 (join / mailbox / parallel / agent_spawn)

## Contract

```
Scheduler::spawn / spawn_with_affinity
  process_resource_quota().check_and_consume(Fibers, 1)
    reject → nullptr
           + fiber_spawn_rejected_total++
           + orchestration_quota_exceeded_total++

agent_spawn (spawn_agent_with_mailbox)
  spawn nullptr → AgentHandle{ok=false, quota_exceeded=true,
                  error="ResourceQuotaExceeded: …"}

parallel_intend / parallel_run
  preflight check_orchestration_fiber_quota(1)
    no remaining capacity → BatchStatus::QuotaExceeded + typed errors
  mid-batch spawn nullptr → fill remaining results with ResourceQuotaExceeded
                          + BatchStatus::QuotaExceeded

Fiber::join
  wait_us aggregated as join_resource_wait_us (join_wait_us_total)
```

## Metrics (`query:resource-quota-stats`, schema **1600**)

| Key | Source |
|-----|--------|
| `fiber_spawn_rejected_total` | process ResourceQuota |
| `orchestration_quota_exceeded_count` | process ResourceQuota |
| `join_resource_wait_us` | `Fiber::join_wait_us_total` |
| `process_fibers_used` / `process_fibers_limit` / remaining | process fibers dim |
| `orch_spawn_gated` | constant 1 |

## Tests

- `tests/test_orch_resource_quota_1600.cpp` — exhaust spawn, parallel_intend, query
- Prior: `test_resource_quota_hotpath`, `test_resource_quota_module`, `test_parallel_orch`

## Related

- `docs/design/resource-quota-hotpath.md`
- `docs/contributing.md` (ResourceQuota section)
