# Primitive Capture-Contract Style Guide

> Issue #671: primitives_detail lambda capture discipline + style
> compliance observability for stdlib-impl primitives.
> Complements the existing [primitives_detail.h](../../src/compiler/primitives_detail.h)
> header contract with the canonical writeup.

## Why this exists

Post the P1/P2 split, every `register_*_primitives` partition has a
shared signature with `PrimRegistrar`, `pairs_`, `string_heap_`,
`error_values_`, `primitive_error_counter`, and `*this` for `ev`.
Lambdas manually capture these; some use the counter, some don't;
some wrap mutate paths in `MutationBoundaryGuard`, some don't.

Without a documented discipline:
- New primitives or AI-Agent-generated extensions risk missing
  `error_counter` → silent or inconsistent error observability.
- Mutate paths without Guard provenance can leave the
  workspace in a partial state during fiber cancellation.
- Read-only hot paths with silent `catch (...)` swallow
  production bugs.

## Required capture discipline (the PRIM_CAPTURE_CONTRACT)

### Mutate paths

1. **Capture `primitive_error_counter`** as a parameter to the
   `register_*_primitives` function. Pass to `make_primitive_error`
   for any error path. Use the `PRIM_ERROR(MSG)` macro — it
   forwards `string_heap`, `error_values`, and
   `primitive_error_counter` to the helper.
2. **Wrap mutate work in `MutationBoundaryGuard`** for fiber-safe
   provenance:

   ```cpp
   bool ok = true;
   aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
   ```

3. **For EDSL-style "real" primitives** (those that call
   `workspace_flat_->mutate_*`), mark dirty explicitly via
   `mark_dirty_upward` so observability surfaces see the propagation.

### Error paths

4. **Always use `PRIM_ERROR(MSG)`** instead of building the error
   value inline. The macro expands to the canonical 4-arg call,
   including the counter bump. See
   `src/compiler/primitives_detail.h` for the macro definition.

5. **For type-mismatch / OOB pre-try validation**, use
   `PRIM_ERROR(MSG)` with a structured message like
   `"<prim-name>: expected N string arguments"`. Do not return
   silent sentinels (`make_int(0)` / `make_void()`) — silent
   sentinels conflate "operation didn't apply" with "user input
   was malformed". See `evaluator_primitives_math.cpp` regex
   primitives for the post-#668 pattern.

### Read-only hot paths

6. **Capture `&ev` or explicit heap refs** as needed. Do not
   silently swallow exceptions in `catch (...)` — let the
   evaluator surface the error or use `PRIM_ERROR(MSG)` for the
   structured path.

## Compile-time helpers

`primitives_detail.h` ships two `constexpr` helpers:

```cpp
PRIM_CAPTURE_HAS_ERROR_COUNTER(true);  // mutate path: pass true if you capture counter
PRIM_CAPTURE_USES_GUARD(true);        // mutate path: pass true if you wrap in Guard
```

These fire a `static_assert` at compile time when `false` is
passed — making the discipline visible during code review
without forcing a runtime check at every call site.

```cpp
add("my-mutate-prim", [&ev, primitive_error_counter](auto a) {
    PRIM_CAPTURE_HAS_ERROR_COUNTER(true);
    PRIM_CAPTURE_USES_GUARD(true);
    bool ok = true;
    aura::compiler::Evaluator::MutationBoundaryGuard guard(ev, &ok);
    // ... mutate logic ...
});
```

## Runtime observability

| Primitive                                | Schema | Source                         |
|------------------------------------------|--------|--------------------------------|
| `(query:primitives-registry-stats)`      | 709    | registry-level summary (7 fields) |
| `(query:primitives-consistency-stats)`   | 671    | capture-discipline axis (7 fields) |
| `(query:primitives-contract-stats)`      | 751    | PRIM_ERROR + capture enforcement (5 fields) |
| `(query:list-soa-hotpath-stats)`         | 752    | list map/filter SoA + intrinsic hot-path (6 fields) |
| `(query:longrunning-infra-stats)`        | 753    | quota/checkpoint/heal/SLO deployment infra (7 fields) |
| `(query:orchestration-llm-bottleneck-stats)` | 754 | LLM-bottleneck adaptive steal + GC safepoint tuning (6 fields) |
| `(query:concurrent-safety-full-cycle-stats)` | 755 | MutationBoundary + steal + AOT + GC full-cycle safety (6 fields) |
| `(query:dead-coercion-elision-stats)`    | 799    | narrow_evidence CastOp elision + zero-overhead savings (5 fields) |
| `(query:linear-postmutate-fidelity-stats)` | 800  | linear ownership post-mutate / steal / EnvFrame fidelity (5 fields) |
| `(query:type-incremental-fidelity-stats)` | 798  | ConstraintSystem incremental fidelity under Guard/steal (5 fields) |
| `(query:eda-infra-stats)`                | 841    | EDA production infrastructure parse/mutate/feedback/co-sim (5 fields) |
| `(query:sv-commercial-emit-fidelity-stats)` | 801 | SV commercial emit roundtrip + dirty re-emit fidelity (5 fields) |
| `(query:sv-verification-self-evolution-stats)` | 802 | feedback-driven SV self-evolution closed-loop (5 fields) |
| `(query:ir-soa-migration-stats)`         | 766    | IR-SoA migration + DirtyAware incremental pipeline (5 fields) |
| `(query:primitives-meta-stats)`          | 669    | meta-introspection axis (5 fields) |

### `(query:longrunning-infra-stats)` fields (#753)

- `quota-violations` — `longrunning_quota_violations_total`
- `checkpoint-success` — `longrunning_checkpoint_success_total` (panic-checkpoint save/commit)
- `heal-triggers` — `longrunning_heal_triggers_total` (successful panic-restore self-heal)
- `resource-trend` — `longrunning_resource_trend_total` (resource:quota-check samples)
- `deployment-slo-hits` — `longrunning_deployment_slo_hits_total` (within-quota checks)
- `infra-events-total` — sum of the five counters above
- `schema` — 753 (drift sentinel)

Production primitives: `(resource:quota-set kind limit)`, `(resource:quota-get kind)`, `(resource:quota-check kind current)` where `kind` is `"memory"`, `"fibers"`, or `"time"` (limit `0` = unlimited).

### `(query:orchestration-llm-bottleneck-stats)` fields (#754)

- `outermost-preferred` — `AdaptiveStealStats::outermost_preferred` (work-steal bias toward outermost MutationBoundary)
- `backoff-triggers` — `AdaptiveStealStats::deferred_pressure_boosts` (StealBudget alertness raises under defer pressure)
- `llm-tail-reduction` — `AdaptiveStealStats::llm_tail_reductions` (Explicit/OperationBoundary steals under LLM tail pressure)
- `gc-safepoint-adapted` — `orchestration_llm_gc_safepoint_adapted_total` (GC safepoint deferral under MutationBoundary)
- `orchestration-events-total` — sum of the four counters above
- `schema` — 754 (drift sentinel)

Distinct from `(query:scheduler-stealbudget-adaptive-stats)` (#706): #754 is the orchestration/LLM-bottleneck closed-loop dashboard; #706 is the StealBudget adaptive-bias summary.

### `(query:concurrent-safety-full-cycle-stats)` fields (#755)

- `steal-boundary-success` — `concurrent_safety_steal_boundary_success_total` (safe-boundary steal probe)
- `aot-reload-at-guard` — `concurrent_safety_aot_reload_at_guard_total` (AOT version drift at guard transfer)
- `gc-safepoint-during-steal` — `concurrent_safety_gc_safepoint_during_steal_total` (GC coordination on fiber migration)
- `recovery-success` — `concurrent_safety_recovery_success_total` (successful panic-checkpoint restore)
- `safety-events-total` — sum of the four counters above
- `schema` — 755 (drift sentinel)

Distinct from `(query:self-evolution-chaos-stats)` (#674): #674 classifies chaos-harness outcomes; #755 tracks the integrated steal/AOT/GC/recovery full-cycle path.

### `(query:dead-coercion-elision-stats)` fields (#799)

- `elided-casts` — `dead_coercion_elision_elided_casts_total` (DeadCoercionEliminationPass static elisions)
- `evidence-hit-rate` — derived percentage from evidence hits vs castop emissions
- `narrowing-stable-paths` — `dead_coercion_elision_narrowing_stable_paths_total` (Rule 6 + TypeSpec narrow skips)
- `runtime-check-savings` — `dead_coercion_elision_runtime_check_savings_total` (compile elim + IR fast-path)
- `schema` — 799 (drift sentinel)

Distinct from `(query:dead-coercion-elim-stats)` (#687): #799 is the narrow_evidence elision closed-loop dashboard for typed mutation; #687 is the general zero-overhead elimination summary.

### `(query:linear-postmutate-fidelity-stats)` fields (#800)

- `post-rollback-revalidate-hits` — `linear_postmutate_post_rollback_revalidate_total` (OwnershipEnv re-validate after rollback/steal)
- `escape-violations-prevented` — `linear_postmutate_escape_violations_prevented_total` (caught use-after-move / escape violations)
- `guard-boundary-linear-safe` — `linear_postmutate_guard_boundary_linear_safe_total` (linear invariant held at Guard/steal probe)
- `env-version-sync` — `linear_postmutate_env_version_sync_total` (EnvFrame version_ validated under materialize/steal)
- `schema` — 800 (drift sentinel)

Distinct from `(query:linear-ownership-gc-compiler-stats)` (#763): #800 tracks post-mutate fidelity under Guard/steal/rollback; #763 tracks compiler IRClosure GC root registration.

### `(query:type-incremental-fidelity-stats)` fields (#798)

- `cross-delta-blame-complete` — `type_incremental_cross_delta_blame_complete_total` (cross-delta conflicts with auditable `active_mutation_id` blame chain)
- `reverify-truncated-under-guard` — `type_incremental_reverify_truncated_under_guard_total` (clean-constraint reverify scan capped while MutationBoundary active)
- `epoch-sync-hits` — `type_incremental_epoch_sync_hits_total` (touched-root / narrow delta marks under Guard boundary)
- `blame-chain-length` — `type_incremental_blame_chain_length_total` (cumulative blame chain steps on cross-delta hits)
- `schema` — 798 (drift sentinel)

Distinct from `(query:type-incremental-stats)` (#608): #798 tracks Guard/steal/MutationBoundary coordination and blame completeness; #608 is the general incremental type reliability sum.

### `(query:eda-infra-stats)` fields (#841)

- `parse-success-hits` — `eda_infra_parse_success_total` (successful SV/SVA parse via `eda:parse-netlist` / `eda:load-sv`)
- `structured-mutate-hits` — `eda_infra_structured_mutate_total` (Guard + StableRef structured SVA/RTL mutate)
- `feedback-ingest-hits` — `eda_infra_feedback_ingest_total` (structured verification feedback ingest)
- `cosim-invoke-hits` — `eda_infra_cosim_invoke_total` (co-simulation bridge via `eda:invoke-simulator` / `eda:ingest-result`)
- `schema` — 841 (drift sentinel)

Distinct from `(query:eda-foundation-stats)` (#499): #841 tracks production closed-loop reliability (parse/mutate/feedback/co-sim); #499 is the foundational primitive call totals.

### `(query:sv-commercial-emit-fidelity-stats)` fields (#801)

- `emit-parse-success-hits` — `sv_commercial_emit_parse_success_total` (`validate_sv_emit` roundtrip pass)
- `roundtrip-mismatch-prevented` — `sv_commercial_emit_roundtrip_mismatch_prevented_total` (local validator caught drift)
- `dirty-reemit-hits` — `sv_commercial_emit_dirty_reemit_total` (dirty-triggered incremental re-emit)
- `commercial-tool-compatible-hits` — `sv_commercial_emit_tool_compatible_total` (emit + commercial stub accepted)
- `schema` — 801 (drift sentinel)

Distinct from `(query:sv-verification-structure-stats)` (#748): #801 tracks commercial-tool interop fidelity; #748 tracks structural mutate + emit pass/fail totals.

### `(query:sv-verification-self-evolution-stats)` fields (#802)

- `feedback-parse-hits` — `sv_self_evo_feedback_parse_total` (`verify:parse-coverage-feedback` / assert / formal-cex)
- `structured-mutate-hits` — `sv_self_evo_structured_mutate_total` (`mutate:from-verification-feedback` successes)
- `closed-loop-rounds` — `sv_self_evo_closed_loop_rounds_total` (orchestrated self-evolution rounds)
- `convergence-hits` — `sv_self_evo_convergence_hits_total` (successful feedback→mutate→re-verify rounds)
- `schema` — 802 (drift sentinel)

Distinct from `(query:closed-loop-reliability-stats)` (#726): #802 tracks structured feedback parse + mutate orchestration; #726 tracks ref-drift/rollback/feedback-round reliability.

### `(query:ir-soa-migration-stats)` fields (#766)

- `soa-instructions-emitted` — `ir_soa_instructions_emitted_total` (cumulative `IRModuleV2::add_instruction` calls)
- `dirty-block-skips` — `ir_soa_dirty_block_skips_total` (DirtyAware short-circuit block skips when `is_block_dirty==0`)
- `clean-block-hit-rate` — `ir_soa_clean_block_hit_rate_pct` (0–10000 fixed-point percent × 100 of blocks clean at re-lower entry)
- `pmr-column-utilization` — `ir_soa_pmr_column_utilization_pct` (0–10000 fixed-point percent × 100 of SoA column capacity in use)
- `jit-soa-codegen-time-ns` — `ir_soa_jit_codegen_time_ns_total` (cumulative SoA codegen ns in `aura_jit.cpp`)
- `schema` — 766 (drift sentinel)

Distinct from `(query:soa-hotpath-stats)` (#729) and `(query:incremental-quote-lambda-linear-stats)` (#765): #766 tracks the production migration of `IRModuleV2` + `DirtyAware` incremental pipeline (cache-locality recovery under AI mutation load); #729 tracks SoA list/cdr-walk hot-path telemetry; #765 tracks incremental quote/lambda/closure compile safety.

### `(query:list-soa-hotpath-stats)` fields (#752)

- `chain-traversals` — `list_chain_traversals_total` (cdr-walk steps in map/filter/foldl)
- `soa-hits` — `list_soa_hits_total` (primitive fast-dispatch in list hot loops)
- `intrinsic-dispatches` — `list_intrinsic_dispatches_total` (slot_lookup_fast wins)
- `estimated-cache-misses` — `list_estimated_cache_misses_total` (advisory pointer-chase estimate)
- `hotpath-events-total` — sum of the four counters above
- `schema` — 752 (drift sentinel)

Prefer passing primitive refs (not closures) to `map`/`filter`/`foldl` when the fn is a pure builtin — the intrinsic/SoA-eligible path records `soa-hits` and avoids extra `estimated-cache-misses` from closure dispatch.

### `(query:primitives-contract-stats)` fields (#751)

- `capture-violations` — `primitive_capture_violations_total`
- `prim-error-hits` — `prim_error_unified_total` (PRIM_ERROR / `make_primitive_error` path)
- `style-compliance-pct` — derived compliance percentage
- `capture-contract-version` — `kPrimCaptureContractVersion` (currently 2)
- `schema` — 751 (drift sentinel)

### `(query:primitives-consistency-stats)` fields

- `capture-violations-detected` — `primitive_capture_violations_total`
  bumped by `prim_record_capture_violation` when a primitive fails
  the capture contract.
- `style-compliance-pct` — derived `(slots - capture_violations) / slots * 100`.
  100 means every primitive passes the contract.
- `registry-slots` — `primitives_.slot_count()`.
- `documented-count` — `primitives_.documented_meta_count()`.
- `capture-contract-version` — `kPrimCaptureContractVersion` from
  `primitives_detail.h`. Bump when the contract changes so the
  Agent can detect drift.
- `recommended-action` — 0 = no action, 1 = backfill missing
  meta, 2 = audit capture contract. Triggered when
  `capture_violations > 0` or `documented < slots`.
- `schema` — 671 (drift sentinel).

## Audit checklist

For every new `register_*_primitives` partition:

- [ ] Mutate primitives capture `primitive_error_counter` and use
      `PRIM_ERROR(MSG)` for error paths.
- [ ] Mutate primitives wrap work in `MutationBoundaryGuard`.
- [ ] Pre-try validation (type-mismatch / OOB) uses `PRIM_ERROR(MSG)`
      instead of silent sentinels.
- [ ] Read-only hot paths don't silently `catch (...)` —
      either let it propagate or use `PRIM_ERROR(MSG)`.
- [ ] Compiles with `PRIM_CAPTURE_HAS_ERROR_COUNTER(true)` +
      `PRIM_CAPTURE_USES_GUARD(true)` at the top of every
      mutate lambda body.
- [ ] Test covers: success path, error path (counter bumped),
      pre-try validation path (counter bumped, not silent).
- [ ] If EDSL-style (calls `workspace_flat_->mutate_*`):
      `mark_dirty_upward` is called for dirty propagation.

## Related

- `src/compiler/primitives_detail.h` — header contract + macro
  definitions
- `src/compiler/observability_metrics.h` — `primitive_capture_violations_total`
  atomic
- `src/compiler/evaluator.ixx` — `MutationBoundaryGuard` definition
- `src/compiler/primitives_meta.h` — `DEFINE_PRIMITIVE_META` macro
- `docs/design/primitive-vs-stdlib-decision-framework.md` —
  which primitives belong in C++ vs stdlib `.aura`