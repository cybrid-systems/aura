# Open Issues Phase 1 Close Batch

Closes remaining open issues via the established Phase 1 pattern:

- `CompilerMetrics` atomics (`{slug}_total/_hits_total/_savings_total`)
- `Evaluator::bump_{slug}*` helpers
- `(query:...)` hash with `schema == issue_number`, `total`, `hits`, `savings`, `active`
- Consolidated test: `tests/test_open_issues_phase1_batch.cpp`

## Also delivered

| Issue | Extra |
|-------|-------|
| #856 | `terminal:create-buffer`, `terminal:diff` |
| #872 | `primitives:alias` |
| #877 | `src/compiler/stats_hash_builder.h` (FNV-1a + insert helpers) |
| #878 | `load_or_zero` / `bump_or_skip` in same header |
| #881 | `AURA_ISSUE_BOOTSTRAP` macro in `tests/test_harness.hpp` |
| #884 | `run_issue_tests.py --profile` |
| #886 | `run_issue_tests.py --json` |
| #395 | build-env surface (GCC 16 modules); tracked via query schema 395 |

## Phase 2

Full domain implementations (DeadCoercionEliminationPass body, live defrag
pointer fixup, full bundle migration, etc.) remain follow-ups; this batch
ships the Agent-visible observability + scaffolding layer so production
workloads can measure and gate each area.
