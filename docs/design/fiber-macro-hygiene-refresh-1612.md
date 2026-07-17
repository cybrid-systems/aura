# Fiber resume / steal / GC MacroIntroduced hygiene refresh (#1612)

**Issue:** [#1612](https://github.com/cybrid-systems/aura/issues/1612)  
**Builds on:** #1608 · #1592 · #1490 · #1497 · #1609 · #1610  
**Status:** Production closed-loop (macro-specific layer on post-steal path).

## Contract

```
Fiber::resume() / steal / GC compact
  └─ complete_post_resume_steal_refresh  (#1608)
        ├─ refresh_stale_frames_after_steal
        ├─ probe_and_repin_linear_on_steal
        ├─ auto_restamp StableNodeRef (Steal)
        ├─ refresh_stale_macro_frames          // #1612
        │     └─ marker drift repair + MacroIntroduced pin refresh
        └─ probe_and_repin_macro_provenance    // #1612
              └─ ensure_valid_or_refresh on MacroIntroduced pins

on_arena_compact_hook (GC)
  ├─ re_pin_cow_children_from_snapshot
  ├─ refresh_stale_macro_frames
  └─ probe_and_repin_macro_provenance
```

## Metrics (`query:post-steal-closed-loop-stats`, schema **1612**)

| Key | Meaning |
|-----|---------|
| `macro_stale_ref_prevented` | Marker/pin drift repairs |
| `macro_provenance_repin_total` | MacroIntroduced pin repins |
| `macro-refresh-invoke-count` | `refresh_stale_macro_frames` calls |
| `macro-refresh-helper-wired` | 1 |
| `gc-compact-macro-refresh-wired` | 1 |
| `schema` | **1612** (lineage 1608/1592) |

## Tests

| File | Role |
|------|------|
| `tests/test_fiber_macro_hygiene_refresh_1612.cpp` | **#1612** AC |
| `tests/test_fiber_resume_post_steal_1608.cpp` | Lineage accepts schema 1612 |

## Related

- `docs/design/fiber-resume-post-steal-1608.md`
- `docs/design/query-pattern-hygiene-1609.md`
