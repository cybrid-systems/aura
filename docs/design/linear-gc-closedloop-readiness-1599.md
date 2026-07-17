# Linear GC roots + AI closed-loop readiness refine (#1599)

**Issue:** [#1599](https://github.com/cybrid-systems/aura/issues/1599)  
**Refines:** #1478, #1483, #1493, #1499, #1543, #1568, #1593, #1596, #1597  
**Status:** Closed-loop shipped (core in sister issues; this is AC inventory + schema 1599 linkage).

## AC closed-loop map

| AC | Requirement | Surface | Shipped |
|----|-------------|---------|---------|
| 1 | Audit 6 touchpoints + design doc | `docs/design/linear-gc-roots.md` | **#1543/#1568/#1599** |
| 2 | `linear_gc_root_audit_log` + checks total | `query:linear-gc-root-audit-log` schema **1599** | **#1543/#1599** |
| 3 | walk/scan linear + GC roots | `enforce_linear_boundary_consistency` / scan | **#1557/#1568** |
| 4 | `query:ai-closedloop-readiness-stats` health | schema **1599** (lineage 1597…) | **#1470–#1597/#1599** |
| 5 | Adaptive safepoint + depth hist | `query:gc-safepoint-adaptive-stats` schema **1599** | **#1483/#1493/#1599** |
| 6 | Docs + 4+ test ACs under load | `test_linear_gc_closedloop_readiness_1599` | **#1599** |

## Six touchpoints (register / audit)

See `linear-gc-roots.md` Mutation paths table:

1. typed_mutate / Guard exit  
2. invalidate_function  
3. compact_env_frames  
4. JIT hot-swap  
5. fiber steal  
6. GC safepoint  

## Query surfaces

```
(engine:metrics "query:linear-gc-root-audit-log")       ; schema 1599
(engine:metrics "query:ai-closedloop-readiness-stats")  ; schema 1599
(engine:metrics "query:gc-safepoint-adaptive-stats")    ; schema 1599
(engine:metrics "query:linear-boundary-consistency-stats") ; schema 1596
```

### Readiness #1599 linkage keys

| Key | Meaning |
|-----|---------|
| `linear-gc-root-audit-checks` | Audit invocations |
| `linear-live-closure-scans` | Proactive linear scans |
| `mutation_stack_depth_histogram` | Sum of depth hist buckets |
| `orch-health-score` / join / mailbox | From #1597 |
| `health-score` / `slo-breach` / adaptive | From #1593 |

### Adaptive safepoint #1599 keys

| Key | Meaning |
|-----|---------|
| `threshold` / `defer-count` | Adaptive backoff state |
| `avg-mutation-hold-us` | Hold-time adaptive input |
| `mutation_stack_depth_histogram` / `hist-b0`…`b7` | Depth pressure |

## Tests

- `tests/test_linear_gc_closedloop_readiness_1599.cpp` — AC1–6  
- `tests/test_issue_1543.cpp` — audit ring  
- `tests/test_ai_closedloop_*` — readiness lineage  
