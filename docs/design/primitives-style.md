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
| `(query:arena-auto-compact-defrag-fiber-stats)` | 767 | Arena auto-compact policy + live defrag + fiber yield (6 fields) |
| `(query:shape-pass-hotpath-stats)`        | 768    | Shape + Pass + Contracts hot-path (5 fields) |
| `(query:registry-extension-stats)`        | 806    | Registry-Extension AI-Agent primitives + auto-validation + SLO (7 fields) |
| `(query:primitive-error-unified-stats)`   | 804    | Stdlib-Registry unified primitive error semantics + recovery SLO (8 fields) |
| `(query:seva-longrunning-concurrent-slo)`  | 803    | SEVA Long-Running Concurrent Verification Evolution SLO (8 fields) |
| `(query:sv-closedloop-slo)`              | 772    | SV Verification closed-loop SLO (6 fields) |
| `(query:workspace-closedloop-fiber-eda-stats)` | 773 | Workspace closed-loop fiber/multi-agent EDA verification (6 fields) |
| `(query:closed-loop-convergence-stats)` | 774 | Verification feedback-driven self-evolution convergence rate (4 fields) |
| `(query:extension-kit-stats)` | 775 | Formal Primitives Extension Kit AI-Agent SLO (4 fields) |
| `(query:primitives-hotpath-slo-stats)` | 776 | Integrated Primitives Hot-Path SLO + Regression Gate (4 fields) |
| `(query:eda-production-readiness)` | 777 | Consolidated EDA Stdlib Production Readiness Roadmap (6 fields) |
| `(query:ffi-call-overhead-stats)` | 778 | FFI call overhead + batch primitive readiness (4 fields) |
| `(query:dirty-region-rendering-stats)` | 779 | Dirty region / delta rendering readiness (4 fields) |
| `(query:jit-rendering-coverage-stats)` | 780 | JIT / hot-update rendering coverage + readiness (4 fields) |
| `(query:zero-copy-framebuffer-stats)` | 781 | Zero-copy byte buffer + framebuffer readiness (4 fields) |
| `(query:terminal-rendering-module-stats)` | 782 | Terminal rendering module + profiling integration readiness (4 fields) |
| `(query:stable-ref-cross-cow-provenance-stats)` | 818 | full provenance + cross-COW auto-resolve (5 fields) |
| `(query:primitives-hotpath-registry-stats)` | 805 | registry + list-apply load samples SLO (6 fields) |
| `(query:error-handling-policy-stats)` | 809 | exception policy + interop counters |
| `(query:fiber-scheduler-init-stats)` | 810 | Fiber/Scheduler AuraResult init path |
| `(query:jit-exception-bridge-stats)` | 811 | guest Raise vs internal Result |
| `(query:orchestration-steal-arena-gc-stats)` | 812 | steal + arena compact + GC yield |
| `(query:guard-error-stats)` | 813 | MutationBoundaryGuard Result path |
| `(query:runtime-production-health)` | 814 | unified production health + self-heal |
| `(query:macro-introduced-provenance-stats)` | 815 | SyntaxMarker→IR provenance |
| `(query:edsl-struct-meta-stats)` | 816 | edsl:define-struct validate meta |
| `(query:dirty-epoch-marker-stats)` | 817 | MacroIntroduced dirty/epoch |
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

### `(query:arena-auto-compact-defrag-fiber-stats)` fields (#767, enhanced #797)

- `auto-compact-triggers` — arena stats `auto_alloc_trigger_count` / `auto_triggers` (auto-compact policy fires)
- `frag-reduced-bp` — arena stats `frag_reduced_bp` (basis points × 100; 5000 = 50.00%)
- `live-defrag-savings` — arena stats `defrag_savings_alloc` / `defrag_savings`
- `fiber-yield-during-compact` — `arena_auto_compact_fiber_yield_during_compact_total` (actual fiber yields inside compact/defrag)
- `shape-inval-count` — arena stats `shape_inval_on_compact`
- `defrag-blocked-fibers` — `arena_auto_compact_defrag_blocked_fibers_total` (fibers blocked waiting for defrag)
- `production-readiness` — derived ordinal (#797): 0 = production-ready (auto-policy fires AND fiber-yield observed under sustained load), 1 = partial Phase 1 only (some activity but no fiber-yield / no defrag-blocked surface observed yet), 2 = early-stage (no auto-compact activity yet — service has not exercised the tiered pool hot path). Body AC4 "SLO frag <0.3 under load" maps here as an observable ordinal rather than raw `frag_ratio`.
- `schema` — 767 (drift sentinel; #797 enhances the existing primitive with one derived field instead of creating a new primitive at a different schema, to keep the catalog lean)

Distinct from `(query:arena-auto-compact-stats)` (#685) and `(query:arena-auto-compaction-stats)` (#642): #767 adds 2 truly new counters (`fiber-yield-during-compact` actual-yield vs #685 yield-checks-hit observability-only, and `defrag-blocked-fibers` introducing the hidden defrag-fiber interaction cost metric) on top of the 4 reused arena stats — completes the production auto-compact policy + live defrag + fiber/GC safepoint yield observability surface. #797 enhances the same primitive with the `production-readiness` derived ordinal so the Agent can observe the body AC4 SLO without exposing `frag_ratio` directly; the field costs one branch per call and shares the existing atomics. Live-object-moving defrag (pointer fixup for StableRef/children/GC roots) + actual `WorkerContext` yield coordination + sustained-mutate harness (`tests/test_highperf_arena_live_defrag_auto_compact_fiber_yield.cpp`) remain Phase 2+ follow-up.

### `(query:shape-pass-hotpath-stats)` fields (#768)

- `contract-checks-hotpath` — `shape_pass_contract_checks_hotpath_total` (zero-overhead `contract_assert` / pre / post checks that fired in `inline_shape_of` / history push / dominant compute / record_shape / dirty propagate / shape dispatch hot paths)
- `shape-stability-transitions` — `shape_stability_transitions_total` (dominant-shape transitions recorded by ShapeProfiler; high rate = polymorphic workload)
- `jit-epoch-sync-hits` — `jit_epoch_sync_hits_total` (ShapeProfiler version bumped in sync with `mutation_epoch_` + JIT epoch hint)
- `deopt-targeted-skips` — `deopt_targeted_skips_total` (DirtyAware or `impact_scope` targeted invalidation saved a full recompile)
- `concept-violations-caught` — `concept_violations_caught_total` (static_assert in pipeline templates fired for `JITFriendlyPass` / `DirtyAwarePass` / `SoAView` / `ShapeStablePass` Concept violations)
- `schema` — 768 (drift sentinel)

Distinct from `(query:shape-stability-stats)` (#570), `(query:shape-profiler-stats)` (#492), `(query:pass-pipeline-stats)` (#494), `(query:evalvalue-v2-dispatch-stats)` (#571), and `shape_jit_pass_closedloop_stats` (#744): #768 is the FIRST observability surface that tracks the *production hot-path Contracts coverage + ShapeProfiler epoch sync with JIT/Pass Pipeline + stronger Concept constraints for Dirty/JITFriendly composition* — 5 truly new counters beyond what #570/#492/#494/#571/#744 cover.

### `(query:sv-closedloop-slo)` fields (#772)

- `slo-status` — computed at primitive-call time from current counters + SLO thresholds. 0 = ok (fidelity ≥ 99% AND re-emit latency max ≤ 50 ms AND no explicit breach bumps); 1 = warn (fidelity 95-99% OR latency 50-100 ms); 2 = breach (fidelity < 95% OR latency > 100 ms OR any explicit `bump_sv_slo_breach` fires).
- `emit-parse-success` — `sv_slo_emit_parse_success_total` (numerator for fidelity rate)
- `emit-parse-failure` — `sv_slo_emit_parse_failure_total` (denominator for fidelity rate)
- `reemit-latency-max-us` — `sv_slo_reemit_latency_max_us` (high-water mark of incremental re-emit latency in microseconds; bumped via compare-exchange so only updates when new value exceeds current max)
- `reemit-hits` — `sv_slo_reemit_hits_total` (incremental re-emit trigger count)
- `slo-breach-total` — `sv_slo_breach_total` (cumulative SLO breach counter; any explicit bump forces `slo-status = 2`)
- `schema` — 772 (drift sentinel)

Distinct from `(query:sv-verification-structure-stats)` (#748), `(query:sv-commercial-emit-fidelity-stats)` (#801), and `(query:sv-verification-self-evolution-stats)` (#802): #748 tracks structure mutate / dirty re-emit / emit fidelity pass-fail; #801 tracks commercial emit roundtrip + dirty re-emit; #802 tracks structured self-evolution closed-loop. #772 is the FIRST observability surface that tracks the *production SLO status of the SV verification closed-loop* — the computed slo-status + fidelity rate + re-emit latency max + breach counter — as a deployment-grade dashboard the Agent reads to decide whether the closed-loop is production-ready for commercial VCS / Questa / JasperGold emit acceptance.

### `(query:workspace-closedloop-fiber-eda-stats)` fields (#773)

- `concurrent-query-mutate-success-pct` — derived at primitive-call time from `#762` atomics (0–10000 fixed-point percent × 100; 9900 = 99.00% baseline)
- `cross-cow-ref-validity-pct` — derived from `#762` `cross_cow_ref_valid_total` (0–10000 fixed-point percent × 100)
- `yield-points-hit` — reused `#762` atomic `workspace_closedloop_yield_points_hit_total`
- `shared-mutex-contention-ns` — NEW atomic `workspace_closedloop_shared_mutex_contention_ns_total` (cumulative ns spent in `workspace_mtx_` contention; time-based vs `#762`'s count-based)
- `multi-agent-edit-fidelity` — NEW atomic `workspace_closedloop_multi_agent_edit_fidelity_pct` (0–10000 fixed-point percent × 100; high value = production-ready multi-Agent deployments)
- `stale-ref-prevented-eda-loops` — NEW atomic `workspace_closedloop_stale_ref_prevented_eda_loops_total` (count of cross-COW `StableRef` accesses caught stale + refreshed)
- `schema` — 773 (drift sentinel)

Distinct from `(query:workspace-closedloop-orchestration-stats)` (#762): `#762` ships 4 raw counters (concurrent-query-mutate / cross-cow-ref-valid / yield-points-hit / shared-mutex-contention). `#773` extends with **pct-derived fields** (computed at primitive-call time) + **ns-based contention** (time metric vs count) + **multi-Agent fidelity** (NEW dimension) + **stale-ref prevention** (NEW dimension) — the EDA verification-loop production surface.

### `(query:closed-loop-convergence-stats)` fields (#774)

- `convergence-rate` — derived at primitive-call time from `#802` atomics (0–10000 fixed-point percent × 100; `convergence-hits / closed-loop-rounds × 10000`; 10000 = 100.00% baseline when rounds == 0; integer division to avoid float drift under parallel updates)
- `closed-loop-rounds` — reused `#802` atomic `sv_self_evo_closed_loop_rounds_total` (total feedback parse → mutate → re-verify rounds)
- `convergence-hits` — reused `#802` atomic `sv_self_evo_convergence_hits_total` (successful convergence rounds)
- `feedback-mutate-rounds` — reused `#726` atomic `closed_loop_feedback_mutate_rounds_total` (#726 per-round counter)
- `schema` — 774 (drift sentinel)

Distinct from `(query:closed-loop-reliability-stats)` (#726) and `(query:sv-verification-self-evolution-stats)` (#802): `#726` ships 3 raw counters (ref-drift-prevented / rollback-success / feedback-mutate-rounds) for closed-loop *reliability*; `#802` ships 4 raw counters (feedback-parse-hits / structured-mutate-hits / closed-loop-rounds / convergence-hits) for the self-evolution *volume*. `#774` is the FIRST observability surface that exposes the **convergence_rate** pct the body asks for — a derived metric computed at primitive-call time that an Agent / SEVA controller can read to decide whether the closed-loop is converging in production multi-round SEVA scenarios.

### `(query:extension-kit-stats)` fields (#775)

- `extensions_registered` — reused `#633` atomic `stdlib_extension_count_total` (foundation for AC3 DEFINE_PRIMITIVE macro wire-up; bumped per new extension registered; 0 until AC3 wire-up)
- `contract_violations_caught` — reused `#751` atomic `primitive_capture_violations_total` (# of primitives that failed the capture contract probe; bumped by `prim_record_capture_violation` when no error_counter on a mutate path)
- `meta_completeness_pct` — derived (0–10000 fixed-point percent × 100; `schema_documented_meta_count / slot_count × 10000`; 10000 = 100.00% baseline when slot_count == 0; SLO target >95% = 9500 for Agent-generated extensions)
- `test_skeletons_generated` — reused `#697` atomic `primitive_skeleton_generations_total` (# of `(primitive:generate-skeleton description-string)` invocations; production-path bump)
- `schema` — 775 (drift sentinel)

Distinct from `(query:primitives-extension-stats)` (#697), `(query:primitives-contract-stats)` (#751), and `(query:primitives-meta-stats)` (#669): `#697` ships 8 registry-level counters (eda-meta-backfilled + category-sva + category-verification + category-eda + documented-with-schema + extension-kit-version + registry-slots + skeleton-generations); `#751` ships 4 contract counters (capture-violations + prim-error-hits + style-compliance-pct + capture-contract-version); `#669` ships 4 meta-axis counters (meta-hits + documented-count + schema-documented + total-registered). `#775` is the FIRST observability surface that aggregates the **Agent-facing extension kit SLO composite** — extensions_registered + contract_violations_caught + meta_completeness_pct + test_skeletons_generated — as a single deployment-grade dashboard the Agent reads to decide whether the stdlib extension kit is production-ready.

### `(query:registry-extension-stats)` fields (#806)

- `extensions` — reused `#633` atomic `stdlib_extension_count_total` (# of new primitives registered through any path; 0 until AC3 wire-up)
- `validation-pass` — `registry_extension_validation_passes_total` (NEW atomic #806 introduces the *positive* validation pass count distinct from #775's contract_violations_caught failure count; bumped by `bump_registry_extension_validation_pass()` per successful capture-contract + PrimMeta backfill + schema + safety-flag probe pass)
- `validation-fail` — reused `#751` atomic `primitive_capture_violations_total` (# of primitives that failed the capture contract probe)
- `meta-completeness` — derived (`schema_documented_meta_count / slot_count × 10000`; 10000 = 100.00% baseline when slot_count == 0; SLO target 100% = 10000 for extensions; mirrors #775)
- `slo-validation-pct` — derived (`validation-pass / (validation-pass + validation-fail + 1) × 10000`; 10000 = 100.00% vacuous-true baseline when both counts are 0; SLO target >98% = 9800)
- `extend-registry-safe-active` — hardcoded 0 (Phase 2+ deferred — the actual `(primitive:extend-registry-safe name doc schema [category] [safety] body-expr)` generative primitive + capture-contract auto-probe + PrimMeta backfill + structured-error + Agent prompt patterns + `tests/test_primitives_extension_registry_ai_gen.cpp` harness)
- `recommendation` — derived 0/1/2/3 (0 = production-ready when SLO met + extend primitive active; 1 = near-production when SLO met but extend primitive not yet; 2 = partial Phase 1 when validation-pass seen but not yet SLO; 3 = early-stage when no activity yet)
- `schema` — 806 (drift sentinel)

Distinct from `(query:extension-kit-stats)` (#775): `#806` is the **registry-integration phase** observability surface — distinct from `#775`'s kit SLO composite (`extensions_registered` + `contract_violations_caught` + `meta_completeness_pct` + `test_skeletons_generated`). `#806` adds the *pass* counter (positive signal — what's gone right) where `#775` only tracks the *violation* counter (negative signal — what's gone wrong); the pair enables a true SLO percentage instead of a violation ratio. `#806` also adds the `extend-registry-safe-active` flag + `recommendation` ordinal to make the production-readiness transition visible as a single ordinal without exposing `frag_ratio`-style raw percentages.

### `(query:primitives-hotpath-slo-stats)` fields (#776)

- `current-vs-baseline-pct` — derived from `#614` `stability_score` (0–10000 fixed-point percent × 100; 10000 = 100.00% baseline when stability_score == 100, the no-load production baseline; values < 5000 indicate SLO breach per body SLO "no regression >5%" plus stability_score < 50 = the `#614` "regression" threshold)
- `contract-violations` — reused `#751` atomic `primitive_capture_violations_total` (capture contract enforcement violations under load; body SLO target is 0)
- `fastpath-hit-rate-pct` — derived (0–10000 fixed-point percent × 100; 10000 = 100.00% baseline when `call_total == 0` = no measurement yet, the vacuous-true default mirror `#774` convergence_rate)
- `regression-flag` — derived (1 if `current-vs-baseline-pct < 5000` indicating a >50% stability-score drop = SLO breach, else 0)
- `schema` — 776 (drift sentinel)

Distinct from `(query:primitives-hotpath-stats)` (#614/#584) and `(query:primitives-contract-stats)` (#751): `#614` ships 11 hot-path counters (primitive-call-total + pair-alloc-total + linear-traverse-total + cdr-depth-max + call-rate + alloc-per-call + regex-time-us + stability-score + hotpath-schema + primitives-hotpath-total + primitives-hotpath-recommendation); `#751` ships 4 contract counters (capture-violations + prim-error-hits + style-compliance-pct + capture-contract-version). `#776` is the FIRST observability surface that aggregates the **primitives hot-path SLO composite** — current-vs-baseline-pct (stability_score as fixed-point pct) + contract-violations + fastpath-hit-rate-pct + regression-flag — as a single deployment-grade SLO dashboard the Agent reads to decide whether the stdlib hot-path is production-ready under AI Agent mutation + fiber load.

### `(query:primitive-error-unified-stats)` fields (#804)

- `error-count-total` — reused `#478` atomic `primitive_error_count_` (# of PRIM_ERROR / make_primitive_error invocations; bumped by `bump_primitive_error_count()`)
- `with-provenance` — `primitive_error_with_provenance_total` (NEW atomic #804 — # of errors that filled in `(kind, msg, provenance)` schema; the *good* path the body asks for at 100% coverage; bumped by `bump_primitive_error_with_provenance()`)
- `silent-fallback` — `primitive_error_silent_fallback_total` (NEW atomic #804 — # of ad-hoc returns / catch-alls the body warns against; bumped by the Phase 2+ audit grep-step)
- `error-values-size` — reused `#478` accessor `get_primitive_error_values_size()` (persistent error object arena size)
- `capture-violations` — reused `#751` atomic `primitive_capture_violations_total` (# of primitives that failed the capture contract probe)
- `unified-path-pct` — derived (`with-provenance / error-count-total × 10000`; 10000 = 100.00% baseline when error_count_total == 0 = vacuous-true default; SLO target 100% = 10000 per body "100% primitives use unified path")
- `recovery-hook-invocations` — `primitive_error_recovery_hook_invocations_total` (NEW atomic #804; bumped by `bump_primitive_error_recovery_hook()` at the planned Phase 2+ Guard + retry path recovery-hook firings)
- `unified-error-path-active` — hardcoded 0 (Phase 2+ — the actual PRIM_ERROR audit + make_primitive_error provenance enforcement + registry enforce-unified-path + `(error:structured-make ...)` + recovery hooks in Guard + `tests/test_primitive_error_unified_audit.cpp` harness)
- `schema` — 804 (drift sentinel)

Distinct from `(query:primitive-error-stats)` (#478 pair primitive), `(query:primitives-error-stats)` (#585 coarse hash), and `(query:primitives-contract-stats)` (#751 contract enforcement): those primitives surface per-component error signals individually. `#804` is the FIRST observability surface that aggregates the **unified-error-path SLO composite** — `error-count-total` (reused total) + `with-provenance` (NEW positive path) + `silent-fallback` (NEW negative path the body warns against) + `recovery-hook-invocations` (NEW recovery signal) + `unified-path-pct` (derived 100% target) + `unified-error-path-active` (Phase 2+ flag) — as a single deployment-grade SLO composite the Agent reads to decide whether the stdlib error semantics are production-ready for commercial AI Agent use. The body explicitly cites the gap: "no first-class recovery or structured error objects with schema for AI Agent to inspect/handle uniformly" and "Registry orchestration in evaluator_primitives_registry.cpp does not enforce uniform error contract or meta for errors".

### `(query:seva-longrunning-concurrent-slo)` fields (#803)

- `convergence-rate` — derived (`sv_self_evo_convergence_hits_total / sv_self_evo_closed_loop_rounds_total × 10000`; 10000 = 100.00% baseline when closed_loop_rounds == 0 = vacuous-true default; SLO target >98% = 9800 per body "convergence rate >98% without manual intervention")
- `ref-drift-prevented` — `seva_concurrent_ref_drift_prevented_total` (NEW atomic #803 — # of ref-drift attempts caught + prevented during long-running concurrent SEVA round; bumped by `bump_seva_concurrent_ref_drift_prevented()` when StableNodeRef.refresh_if_stale + auto re-resolve succeeds; distinct from #762 workspace_closedloop_stale_ref_prevented_eda_loops_total which is workspace-level staleness)
- `hygiene-safe-rollback-pct` — derived (`code_as_data_rollback_hygiene_safe_total / (#632 atomic_batch_sv_rollback_total + 1)`) × 10000 (vacuous-true 10000 baseline; SLO target 100% = 10000 per body "hygiene_safe_rollback 100%")
- `steal-during-verification-mutate` — `seva_concurrent_steal_during_verification_mutate_total` (NEW atomic #803 — # of fiber steal events during a verification mutate inside the long-running harness; high-fidelity load metric; bumped by `bump_seva_concurrent_steal_during_verification_mutate()` when fiber steal fires during mutation_stack_ + outermost MutationBoundaryGuard active)
- `dirty-consistency-hits` — `seva_concurrent_dirty_propagation_hits_total` (NEW atomic #803 — no-fail signal; bumped by `bump_seva_concurrent_dirty_propagation_hits()` at the mark_dirty_upward + verify_dirty_ pass-mark during a SEVA round)
- `avg-rounds-to-target` — derived (`closed_loop_rounds / (convergence_hits + 1)`); 0 baseline when no convergence hits; a typical long-running SEVA converges in ~3-7 rounds; rising values = SLO drift / non-convergence
- `longrunning-harness-active` — hardcoded 0 (Phase 2+ — the actual `tests/test_seva_longrunning_concurrent_verification_evolution.cpp` + CI gate step + SLO dashboard + self-heal hooks + SEVA tutorial extension)
- `recommendation` — derived 0/1/2/3 (0 = production-ready when SLO met + harness active; 1 = near-production when SLO met but harness not yet active; 2 = partial Phase 1 when convergence seen but not yet SLO; 3 = early-stage)
- `schema` — 803 (drift sentinel)

Distinct from `(query:sv-verification-self-evolution-stats)` (#802), `(query:closed-loop-reliability-stats)` (#726), `(query:concurrent-safety-full-cycle-stats)` (#755), `(query:workspace-closedloop-fiber-eda-stats)` (#773), `(query:closed-loop-convergence-stats)` (#774), and `(query:full-closedloop-compiler-edsl-fidelity-stats)` (#794): those primitives surface per-component SV-verification or concurrent-safety signals individually. `#803` is the FIRST observability surface that aggregates the **production-scale long-running concurrent SEVA SLO composite** — convergence rate + ref-drift-prevented + hygiene-safe-rollback-pct + steal-during-verification-mutate + dirty-consistency-hits + avg-rounds-to-target + longrunning-harness-active — as a single deployment-grade SLO composite the Agent reads to decide whether the long-running concurrent verification self-evolution harness is production-ready for commercial multi-agent EDA agent deployment at SoC scale. The body explicitly cites the gap: "no single production-scale long-running harness that exercises full multi-agent concurrent SEVA-style verification evolution under realistic fiber steal/GC/AOT/steal-during-boundary load with measurable SLO gates".

### `(query:eda-production-readiness)` fields (#777)

- `m1-completeness-pct` — M1 (basic feedback primitives + emit): (5 found in expected list) × 10000 / 5 (0–10000 fixed-point percent × 100; expected list: `primitive:generate-skeleton` + `verify:parse-coverage-feedback` + `verify:parse-assert-failure` + `verify:parse-formal-cex` + `mutate:from-verification-feedback`)
- `m2-completeness-pct` — M2 (full SV EDSL + dirty re-emit): (4 found / 4) × 10000 (expected list: `query:sv-verification-structure-stats` + `query:sv-commercial-emit-fidelity-stats` + `query:sv-verification-self-evolution-stats` + `query:sv-closedloop-slo`)
- `m3-completeness-pct` — M3 (commercial fidelity + roundtrip + long-running harness): (3 found / 3) × 10000 (expected list: `query:primitives-hotpath-slo-stats` + `compile:inline-pass-stats` + `compile:dead-coercion-stats`)
- `m4-completeness-pct` — M4 (multi-agent concurrent SLOs): (2 found / 2) × 10000 (expected list: `query:workspace-closedloop-orchestration-stats` + `query:workspace-closedloop-fiber-eda-stats`)
- `blocking-issues-count` — fixed count of related open EDA issues (4: `#749` + `#738` + `#725` + `#724` per body; the closed ones `#726` + `#748` + `#772` + `#774` are not counted)
- `recommendation` — derived (0 = production-ready if all milestones ≥ 9500; 1 = near-ready if all ≥ 8000; 2 = in-progress if all ≥ 5000; 3 = early-stage if any < 5000)
- `schema` — 777 (drift sentinel)

Distinct from any individual EDA primitive (`#726`/`#748`/`#772`/`#774`/`#749`/`#738`/`#725`/`724`): each individual primitive covers one specific surface; `#777` is the FIRST observability surface that aggregates the **EDA production readiness composite** — 4 milestone completeness pcts + blocking-issues-count + recommendation — as a single deployment-grade production-readiness dashboard the Agent reads to decide whether the EDA stdlib is production-ready for commercial verification self-evolution.

### `(query:ffi-call-overhead-stats)` fields (#778)

- `ffi-call-count` — read from `ev.get_ffi_call_count()` (alias of `coverage_counters_[8]`; total FFI primitive invocations across c-load + c-func + c-opaque + c-alloc + c-struct-set! + c-struct-ref)
- `batch-ffi-supported` — fixed 0 (the batch FFI primitive is Phase 2+ deferred per body "Add batch FFI primitive or memory view support in `ffi_primitives_impl.cpp`")
- `terminal-batch-write-supported` — fixed 0 (the `terminal-batch-write` primitive is Phase 2+ deferred per body "Provide `terminal-batch-write` or similar high-level primitive that minimizes crossings")
- `recommendation` — derived 0/1/2/3 (0 = production-ready if both batch flags = 1; 1 = partial if one flag = 1; 2 = missing-primitive if both = 0 but `ffi-call-count > 0`; 3 = early-stage if both = 0 and no FFI usage)
- `schema` — 778 (drift sentinel)

Distinct from `(query:ffi-calls-stats)` (#699) and the `#131` FFI primitive extraction: `#131` ships the FFI primitives themselves; `#699` tracks FFI call patterns; `#778` is the FIRST observability surface that tracks FFI call volume + exposes the production-readiness signals for the deferred batch FFI + `terminal-batch-write` work. The actual `ns/op` measurement is in `tests/test_issue_778.cpp` as a benchmark (the production wiring is deferred).

### `(query:dirty-region-rendering-stats)` fields (#779)

- `dirty-region-count` — hardcoded 0 (no existing counter for dirty regions on main; would be bumped by the `(terminal-dirty-region)` primitive when it ships)
- `present-delta-supported` — hardcoded 0 (the `(present-delta)` primitive is Phase 2+ deferred per body "Implement efficient `present-delta` that only outputs changed areas")
- `terminal-dirty-region-supported` — hardcoded 0 (the `(terminal-dirty-region)` primitive is Phase 2+ deferred per body "Add `terminal-dirty-region` tracking primitives")
- `recommendation` — derived 0/1/2/3 (0 = production-ready if both flags = 1; 1 = partial if one flag = 1; 2 = missing-primitive if both = 0 but `dirty-region-count > 0`; 3 = early-stage if both = 0 and no dirty region activity)
- `schema` — 779 (drift sentinel)

Distinct from the existing vector primitives (`vector-set!`, `vector-ref`, `make-vector`, `vector-length`, `vector->list` in `evaluator_primitives_vector.cpp`): the vector primitives provide direct access to contiguous cells but offer no dirty region tracking or delta rendering optimization. `#779` is the FIRST observability surface that exposes the production-readiness signals for the deferred dirty region + `present-delta` work the body asks for. The actual `ns/op` measurement on the vector primitives is in `tests/test_issue_779.cpp` as a benchmark.

### `(query:jit-rendering-coverage-stats)` fields (#780)

- `hotpath-eval-flat-calls` — reused `#441` atomic `hotpath_eval_flat_calls` (total JIT path eval-flat invocations)
- `hotpath-lowering-calls` — reused `#441` atomic `hotpath_lowering_calls` (total JIT lowering invocations)
- `rendering-path-jit-supported` — hardcoded 0 (rendering path JIT is Phase 2+ deferred per body "`present()` and drawing loops remain in interpreted mode or have high overhead")
- `hot-update-rendering-optimized` — hardcoded 0 (hot-update rendering optimization is Phase 2+ deferred per body "Hot-update works for general code but lacks special handling for performance-critical rendering functions")
- `recommendation` — derived 0/1/2/3 (0 = production-ready if both flags = 1; 1 = partial if one flag = 1; 2 = missing-optimization if both = 0 but JIT activity > 0; 3 = early-stage if both = 0 and no JIT activity)
- `schema` — 780 (drift sentinel)

Distinct from `(query:jit-stats)` (#427), `(query:jit-consistency-stats)`, `(query:jit-interpreter-parity-stats)` (#720), and `(query:jit-typed-mutation-stats)` (#746): those primitives cover general JIT metrics, JIT/interpreter consistency, JIT/typed-mutation hot paths, etc. `#780` is the FIRST observability surface that tracks the **JIT coverage for rendering hot paths** (the body items about `present()` and drawing loops remaining in interpreted mode) + exposes the production-readiness signals for the deferred rendering-path JIT + hot-update rendering optimization work.

### `(query:zero-copy-framebuffer-stats)` fields (#781)

- `pair-alloc-total` — reused `#491` atomic `pair_alloc_total` (total pair allocations across list / append / reverse / map / filter; the allocation pressure signal the body identifies)
- `zero-copy-supported` — hardcoded 0 (the `(zero-copy-view)` primitive + byte-buffer primitive with zero-copy semantics is Phase 2+ deferred per body "Enhance or add specialized byte-buffer primitives with zero-copy and view support")
- `ansi-helper-supported` — hardcoded 0 (the `(ansi-sequence-build)` or similar helper primitive is Phase 2+ deferred per body "Provide helpers for efficient ANSI sequence construction")
- `memory-profiling-supported` — hardcoded 0 (the rendering memory profiling primitive is Phase 2+ deferred per body "Add memory profiling for rendering workloads")
- `recommendation` — derived 0/1/2/3 (0 = production-ready if all 3 flags = 1; 1 = partial if any flag = 1; 2 = missing-primitive if all = 0 but `pair-alloc-total > 0`; 3 = early-stage if all = 0 and no allocation activity)
- `schema` — 781 (drift sentinel)

Distinct from the existing memory primitives in `evaluator_primitives_memory.cpp` and vector primitives in `evaluator_primitives_vector.cpp`: those primitives provide direct cell access but offer no zero-copy view, ANSI sequence batching, or rendering memory profiling. `#781` is the FIRST observability surface that tracks the **pair allocation pressure** the body identifies as wasted on per-frame buffer construction + exposes the production-readiness signals for the deferred zero-copy byte-buffer + ANSI helper + memory profiling work. The actual `ns/op` measurement is in `tests/test_issue_781.cpp` as a benchmark.

### `(query:terminal-rendering-module-stats)` fields (#782)

- `core-primitive-count` — live count of expected terminal rendering core primitives registered (4 expected per body: `clear`, `draw-batch`, `present`, `dirty-tracking`; 0 on fresh service because no `evaluator_primitives_terminal.cpp` exists yet — computed via live primitive lookup, mirror `#777` pattern)
- `terminal-module-available` — hardcoded 0 (the `evaluator_primitives_terminal.cpp` module is Phase 2+ deferred per body "no `evaluator_primitives_terminal.cpp` or equivalent module for high-performance terminal/character graphics rendering")
- `shape-profiler-integration-available` — hardcoded 0 (the `shape_profiler.cpp` integration for rendering paths is Phase 2+ deferred per body "Integrate with existing observability and `shape_profiler.cpp`")
- `example-renderer-available` — hardcoded 0 (the minimal high-perf terminal renderer example is Phase 2+ deferred per body "Provide example implementation of a minimal high-perf terminal renderer")
- `recommendation` — derived 0/1/2/3 (0 = production-ready if module + profiler + example all = 1 AND `core-primitive-count == 4`; 1 = partial if any module flag = 1 or `core-primitive-count > 0`; 2 = missing-module if all = 0 but `core-primitive-count > 0`; 3 = early-stage if all = 0 AND `core-primitive-count == 0`)
- `schema` — 782 (drift sentinel)

Distinct from the existing vector + memory + I/O primitives in `evaluator_primitives_vector.cpp` / `_memory.cpp` / `_io.cpp`: those primitives provide general cell access but offer no dedicated terminal rendering module with `clear` / `draw-batch` / `present` / `dirty-tracking` primitives, no `shape_profiler.cpp` integration for rendering paths, and no example high-perf terminal renderer. `#782` is the FIRST observability surface that exposes the **terminal rendering module readiness composite** — `core-primitive-count` (live lookup) + 3 module flags + recommendation — as a single deployment-grade infrastructure-readiness dashboard the Agent reads to decide whether the terminal rendering module is production-ready for the planned cyber cat prototype.

### `(query:envframe-dualpath-mandatory-enforce-stats)` fields (#784)

- `mandatory-enforce-total` — `envframe_mandatory_enforce_total` (NEW atomic, #784; # of `ensure_envframe_dual_path_consistency()` calls at mandatory entry points — `walk_env_frames` / `GCEnvWalkFn` / `materialize_call_env` / post-rollback / fiber steal resume; bumped from `Evaluator::bump_envframe_mandatory_enforce()` at the planned Phase 2+ call sites)
- `mandatory-enforce-desync-total` — `envframe_mandatory_enforce_desync_total` (NEW atomic, #784; # of mandatory `ensure_` calls that detected a length/order mismatch — the primary "did the safety net catch a desync?" signal; bumped from `Evaluator::bump_envframe_mandatory_enforce_desync()` when `ensure_` returns false at a mandatory entry)
- `gc-walk-resync-total` — hardcoded 0 in Phase 1 (the dedicated `envframe_gc_walk_resync_total` atomic is Phase 2+ deferred per body "GCEnvWalkFn + stale handling strengthened to also verify dual-path consistency"; the GC stale-detection signal is already exposed by #756 via `envframe_gc_stale_desync_hits_total`)
- `concurrent-steal-resync-total` — `envframe_concurrent_steal_resync_total` (NEW atomic, #784; # of times a fiber steal resume triggered a re-ensure + version re-stamp; bumped from `Evaluator::bump_envframe_concurrent_steal_resync()` at the planned Phase 2+ `Fiber::resume()` entry)
- `policy-mode` — hardcoded 0 (`log-and-sync` default; the strict-panic vs log-and-sync policy flag + `desync_panic_count` are already exposed by #756 via `envframe_desync_panic_count_total`; Phase 2+ to make policy mode configurable via a setter primitive per body "policy flag (strict_panic vs log_and_sync)")
- `mandatory-call-sites-enabled` — hardcoded 0 (the actual mandatory `ensure_` wiring in `walk_env_frames` / `GCEnvWalkFn` / `materialize_call_env` / post-rollback paths is Phase 2+ deferred per body "Make `ensure_` mandatory (call at start of critical paths)")
- `recommendation` — derived 0/1/2/3 (0 = production-ready strict-panic + wired if `policy-mode == 2 AND mandatory-call-sites-enabled == 1`; 1 = partial if any deferred flag = 1; 2 = Phase 1 only if all deferred flags = 0 but `mandatory-enforce-total > 0 || concurrent-steal-resync-total > 0 || mandatory-enforce-desync-total > 0`; 3 = early-stage if all deferred flags = 0 AND no activity)
- `schema` — 784 (drift sentinel)

Distinct from the existing `#756` `(query:envframe-dualpath-policy-stats)` (desync-panic-count + gc-stale-desync-hits + dualpath-repair + version-mismatch + schema 756) + `#647` `(query:envframe-dualpath-stale-stats-hash)` (cross-fiber-stale + version-mismatch + dualpath-repair + schema 647) + `#731` `(query:envframe-dualpath-stats)` (mirror-write + refresh + consistency-violations + schema 731): those primitives surface the **detection-side** of dual-path reliability (how often was a desync detected, how often did GC walk hit a stale frame, how often did the safety net fire). `#784` is the FIRST observability surface that surfaces the **enforcement-side** of dual-path reliability — was `ensure_` actually called at every critical path? did the call site catch the desync? did concurrent steal trigger a resync? — as a separate per-decision-point signal the Agent consumes to decide whether to trigger `walk_env_frames` re-ensure, `materialize_call_env` re-stamp, or full-subtree rollback under concurrent steal/mutation.

### `(query:aot-concurrent-hotupdate-stats)` fields (#785)

- `concurrent-steal-during-reload` — `aot_concurrent_steal_during_reload_total` (NEW atomic, #785; # of work-steal attempts deferred because the victim fiber was in AOT apply or reload refcount swap was in progress; bumped from `Evaluator::bump_aot_concurrent_steal_during_reload()` at the planned Phase 2+ `WorkerThread::steal()` integration)
- `grace-period-hits` — `aot_grace_period_hits_total` (NEW atomic, #785; # of times the grace period was triggered during reload to allow in-flight `apply_closure` / JIT `GuardShape` to see consistent `func_table`; bumped from `Evaluator::bump_aot_grace_period_hit()` at the planned Phase 2+ `aura_reload_aot_module` before/after swap integration)
- `env-version-sync-on-reload` — `aot_env_version_sync_on_reload_total` (NEW atomic, #785; # of times `EnvFrame::version_` was bumped on reload to coordinate with cross-fiber mutation; bumped from `Evaluator::bump_aot_env_version_sync_on_reload()` at the planned Phase 2+ reload decision + `EnvFrame` sync integration)
- `region-mask-enforced` — hardcoded 0 (Phase 2+ to wire `region_mask` check in `aura_reload_aot_module` reload decision per body "region mask enforced: reload only if `(region_mask & host_mask) != 0`; reject with `region_mismatch` metric")
- `grace-period-implemented` — hardcoded 0 (Phase 2+ to add grace period — atomic or fiber-yield safe delay — before/after swap per body "grace period for refcount swap during concurrent steal/resume")
- `steal-defer-active` — hardcoded 0 (Phase 2+ to wire AOT-specific defer in `is_stealable` or steal loop per body "multi-fiber steal safety during reload — defer if victim in AOT apply or reload in progress")
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if all 3 deferred flags = 1; 1 = partial Phase 2+ if any deferred flag = 1; 2 = Phase 1 only if all deferred flags = 0 but `concurrent-steal-during-reload > 0 || grace-period-hits > 0 || env-version-sync-on-reload > 0`; 3 = early-stage if all deferred flags = 0 AND no activity)
- `schema` — 785 (drift sentinel)

Distinct from the existing `#732` `(query:aot-bridge-stats)` (region + defuse + bridge_epoch tracking + schema 732) + `#708` `(query:aot-reload-stats)` + `(query:aot-checkpoint-version-stats)` (reload attempts / success / stale-rejected / refcount-swaps / region-violations / deopt-on-steal / concurrent-safe-reloads + schema 708) + `#590` `(query:aot-hotupdate-stats)` (hot-update + grace + version-drift + schema 590): those primitives surface the **decision-side** of AOT hot-update (did the reload attempt succeed, was it stale-rejected, was the region mask mismatch detected). `#785` is the FIRST observability surface that surfaces the **concurrency-side** of AOT hot-update — were steals safely deferred during reload? was the grace period triggered to drain in-flight fibers? was the `EnvFrame` version synced to coordinate with cross-fiber mutation? — as a separate per-decision-point signal the Agent consumes to decide whether to trigger region mask enforcement, grace period, or steal defer under concurrent reload pressure.

### `(query:code-as-data-production-health)` fields (#786)

- `sub-primitive-coverage` — live count of 8 expected sub-primitives registered / 8 × 10000 (computed via `ev.primitives_.lookup(name).has_value()` — live lookup, always accurate; mirror `#777` milestone_pct pattern). 0 if none ship.
- `found-sub-primitive-count` — raw count of sub-primitives registered (0..8)
- `fidelity-pct` — hardcoded 10000 in Phase 1 (Phase 2+ to derive from `#759` `code-as-data-maturity-stats` `fidelity-samples - fidelity-drift` / `fidelity-samples` × 10000 when both are available; 10000 = "vacuously true — no measurements yet so can't fail")
- `guard-rollback-hygiene-pct` — hardcoded 10000 in Phase 1 (Phase 2+ to wire to the actual guard rollback path; the body asks for "hygiene_safe_rollback 100%")
- `concurrent-stress-success-pct` — hardcoded 10000 in Phase 1 (Phase 2+ to wire to `#755` `concurrent-safety-full-cycle-stats` or new stress harness)
- `composite-slo-status` — derived 0/1/2/3 (0 = production-ready if coverage == 10000 AND all pcts == 10000; 1 = partial deployment if coverage >= 5000; 2 = early-stage if coverage > 0; 3 = not-started if coverage == 0)
- `recommendation` — derived 0/1/2/3 (0 = production-ready with fidelity gate met if `composite-slo-status == 0 AND fidelity-pct >= 9900`; 1 = partial deployment; 2 = early-stage; 3 = not-started)
- `schema` — 786 (drift sentinel)

Distinct from the existing `#759` `(query:code-as-data-maturity-stats)` (4 fields: fidelity-samples / fidelity-drift / guard-rollback-hygiene-safe / reflect-schema-macro-edsl + schema 759) + `#758` `(query:edsl-reflection-stats)` (validated-edsl / hygiene-invariants-held / schema-fail-by-type / macro-correlated-violations + schema 758) + `#757` `(query:macro-hygiene-provenance-stats)` (provenance-captured / inliner-policy-violations / provenance-violations / hygiene-dirty-impact + schema 757) + `#755` `(query:concurrent-safety-full-cycle-stats)` + `#773` `(query:workspace-closedloop-fiber-eda-stats)` + `#774` `(query:sv-verification-self-evolution-stats)` + `#726` `(query:closed-loop-reliability-stats)`: those primitives surface the **per-component** production-readiness signal individually. `#786` is the FIRST observability surface that surfaces the **consolidated production health composite** — live sub-primitive coverage across all 8 expected sub-primitives + composite SLO status + recommendation — as a single deployment-grade dashboard the Agent reads to decide whether the integrated code-as-data self-evolution loop is production-ready for commercial rollout. The body explicitly cites the gap: "no single unified production dashboard primitive + composite SLO gates + end-to-end fidelity harness".

### `(query:task6-concurrent-fidelity)` fields (#787)

- `sub-primitive-coverage` — live count of 6 expected sub-primitives registered / 6 × 10000 (computed via `ev.primitives_.lookup(name).has_value()` — live lookup, always accurate; mirror `#786` consolidation + `#777` milestone_pct pattern). 0 if none ship.
- `found-sub-primitive-count` — raw count of sub-primitives registered (0..6)
- `hygiene-drift-prevented` — hardcoded 0 in Phase 1 (Phase 2+ to wire to actual post-rollback / post-reload / steal-resume hygiene validation hook per body "In Guard rollback + steal resume + AOT swap success paths, force re-validate macro provenance/hygiene"; the `#757` macro-hygiene-provenance-stats surface already exposes the macro-side signals that feed this)
- `schema-violation-caught-post-rollback` — hardcoded 0 in Phase 1 (Phase 2+ to wire to runtime reflect validate hook per body "runtime reflection schema validation (auto_validate on reconstructed EDSL structs or macro bodies)"; the `#758` edsl-reflection-stats already exposes the validated-edsl / hygiene-invariants-held / schema-fail-by-type / macro-correlated-violations signals that feed this)
- `linear-safe-after-steal-reload` — hardcoded 0 in Phase 1 (Phase 2+ to wire to `linear_ownership_state` consistency check per body "check `linear_ownership_state` consistency"; the IR `linear_ownership_state` + `GuardShape` + `EnvFrame::version_` + `closure_bridge` surface feeds this)
- `epoch-consistent-hits` — hardcoded 0 in Phase 1 (Phase 2+ to wire to `StableNodeRef` / `EnvFrame` version / `bridge_epoch` / `linear_state` consistency check per body "`StableNodeRef` / `EnvFrame` version / `bridge_epoch` / `linear_state` remain consistent across steal/resume + AOT reload + GC safepoint")
- `composite-fidelity-status` — derived 0/1/2/3 (0 = production-ready if coverage == 10000 AND all 4 fidelity signals == 0; 1 = partial if coverage >= 5000; 2 = early-stage if coverage > 0; 3 = not-started if coverage == 0)
- `schema` — 787 (drift sentinel)

Distinct from the existing `#757` macro-hygiene-provenance-stats + `#758` edsl-reflection-stats + `#750` reflection-schema-stats + `#755` concurrent-safety-full-cycle-stats + `#783` orchestration-steal-outermost-stats + `#785` aot-concurrent-hotupdate-stats: those primitives surface the **per-component** fidelity signal individually. `#786` is the **consolidated production health composite** (8 sub-primitives, code-as-data maturity). `#787` is the **consolidated concurrent fidelity composite** (6 sub-primitives, end-to-end fidelity under chaos) — specifically tracking the 4 production-grade fidelity signals the body explicitly lists (hygiene_drift_prevented + schema_violation_caught_post_rollback + linear_safe_after_steal_reload + epoch_consistent_hits) as separate per-decision-point counters the Agent consumes to decide whether to trigger Guard re-validate, steal-resume re-check, or AOT-swap re-validate under concurrent chaos. The body explicitly cites the gap: "no unified test exercising macro expand → EDSL mutate → reflect validate → steal during Guard → AOT reload → rollback → fidelity re-check".

### `(query:ai-native-extension-stats)` fields (#788)

- `sub-primitive-coverage` — live count of 5 expected sub-primitives registered / 5 × 10000 (computed via `ev.primitives_.lookup(name).has_value()` — live lookup, always accurate; mirror `#786` consolidation + `#777` milestone_pct pattern). 0 if none ship.
- `found-sub-primitive-count` — raw count of sub-primitives registered (0..5)
- `validation-pass-rate` — hardcoded 10000 in Phase 1 (Phase 2+ to wire to actual runtime reflect validation hook for `edsl:define-struct` / `extend-struct` / `extend-kit` per body "(edsl:define-struct name doc schema [attrs]) — defines new NodeTag + builders + auto-wires runtime reflect validate + hygiene/linear checks + Guard provenance; returns meta/slot"; the `#758` edsl-reflection-stats already exposes the validated-edsl / hygiene-invariants-held / schema-fail-by-type / macro-correlated-violations signals that feed this)
- `policy-tuning-success-rate` — hardcoded 10000 in Phase 1 (Phase 2+ to wire to actual `macro:set-policy!` hook per body "(macro:set-policy! policy-kw value [target]) — dynamic control of hygiene/inliner from EDSL/AI under Guard"; the `#757` macro-hygiene-provenance-stats already exposes the macro-side signals that feed this)
- `define-struct-success-rate` — hardcoded 10000 in Phase 1 (Phase 2+ to wire to actual `edsl:define-struct` hook per body "Agent prompts → define-struct / set-policy / extend-kit → new capability available in next eval with full safety + observability")
- `contract-compliance-rate` — hardcoded 10000 in Phase 1 (Phase 2+ to wire to actual `extend-kit` auto-validation hook per body "Enhanced (primitive:extend-kit ...) with full auto-contract + meta + validation integration"; the `#751` primitives-contract-stats already exposes the capture-violations signal that feeds this)
- `composite-ai-extension-status` — derived 0/1/2/3 (0 = production-ready if coverage == 10000 AND all 4 fidelity signals == 10000; 1 = partial if coverage >= 5000; 2 = early-stage if coverage > 0; 3 = not-started if coverage == 0). The body explicitly mentions SLO gates "validation_pass >98%, hygiene_held 100%, contract_compliance 100%".
- `schema` — 788 (drift sentinel)

Distinct from the existing `#757` macro-hygiene-provenance-stats + `#758` edsl-reflection-stats + `#750` reflection-schema-stats + `#775` extension-kit-stats + `#751` primitives-contract-stats: those primitives surface the **per-component** AI-extension readiness signal individually. `#786` is the **consolidated production health composite** (8 sub-primitives, code-as-data maturity). `#787` is the **consolidated concurrent fidelity composite** (6 sub-primitives, end-to-end fidelity under chaos). `#788` is the **consolidated AI-Native extensibility composite** (5 sub-primitives, AI Agent generative extensibility) — specifically tracking the 4 AI-extension fidelity signals the body explicitly lists (`validation-pass-rate` + `policy-tuning-success-rate` + `define-struct-success-rate` + `contract-compliance-rate`) as separate per-decision-point SLO gates the Agent consumes to decide whether to trigger AI-driven `(edsl:define-struct ...)` / `(macro:set-policy! ...)` / `(primitive:extend-kit ...)` calls safely. The body explicitly cites the gap: "no first-class, AI-callable primitives or generative kit" and "AI Agents cannot autonomously and safely extend Aura's own macro/EDSL surface for domain needs ... while preserving the Task6 safety guarantees".

### `(query:pattern-index-safe-span-stats)` fields (#789)

- `safe-span-uses` — `pattern_safe_span_uses_total` (NEW atomic, #789; # of `children_safe_view` / `SafePCVSpan` pin calls in the matcher; bumped from `Evaluator::bump_pattern_safe_span_use()` at the planned Phase 2+ `query_matcher.cpp` + `evaluator_primitives_query.cpp` pattern iterator paths wire-up)
- `dangling-prevented` — `pattern_dangling_prevented_total` (NEW atomic, #789; # of times the generation pin check fired and prevented a dangling span — the safety net signal; bumped from `Evaluator::bump_pattern_dangling_prevented()` at the planned Phase 2+ `ast.ixx` `children_safe_view` wire-up)
- `index-hit-rate` — hardcoded 0 in Phase 1 (Phase 2+ to derive from `#760` `pattern_match_index_hits_total / (linear-scans + index-hits) × 10000`; the cross-reference ratio — high = perf win via `tag_arity_index_` fast-path)
- `safe-span-mandate-active` — hardcoded 0 (Phase 2+ to mandate `children_safe_view` in all pattern iterator / where / filter walks per body "Mandate `children_safe_view` / `SafePCVSpan` for all children iteration in pattern match / filter / where; add generation pin check")
- `tag-arity-index-population-active` — hardcoded 0 (Phase 2+ to fully populate `tag_arity_index_` on every structural change + wire fast-path lookup in matcher before linear fallback per body "Fully populate `tag_arity_index_` (hash on tag+arity+marker) on every structural change; wire fast-path lookup in matcher before linear fallback")
- `deep-hygiene-predicate-active` — hardcoded 0 (Phase 2+ to add deep hygiene provenance predicates `:marker MacroIntroduced :provenance macro-def-id` to `QueryExpr` / pattern parser + auto-filter or stamp in matcher under macro context per body "Add support for hygiene provenance predicates ... auto-filter or stamp in matcher under macro context; wire to `clone_macro_body` name_map")
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if all 3 deferred flags = 1; 1 = partial Phase 2+ if any deferred flag = 1; 2 = Phase 1 only if all deferred flags = 0 but `safe-span-uses > 0 || dangling-prevented > 0`; 3 = early-stage if all deferred flags = 0 AND no activity)
- `schema` — 789 (drift sentinel)

Distinct from the existing `#760` `(query:pattern-performance-stats)` (4 fields: linear-scans / index-hits / wildcard-cost / hygiene-filtered + schema 760): that primitive surfaces the **measurement-side** of pattern matcher observability (how often did a linear scan fire, how often did the index hit, how often did wildcard match, how often did hygiene filter). `#789` is the FIRST observability surface that surfaces the **enforcement-side** of pattern matcher production safety — was `SafePCVSpan` actually used? did the generation pin check fire? — as separate per-decision-point signals the Agent consumes to decide whether to trigger `children_safe_view` mandate, `tag_arity_index_` population, or deep hygiene predicate under concurrent mutate on large macro-heavy AST. The body explicitly cites the gap: "missing enforced lifetime-pinned iteration (SafePCVSpan mandate in matcher), full tag_arity_index_ population + lookup in pattern match, and first-class hygiene provenance predicate support".

### `(query:mutate-batch-atomic-stats)` fields (#790)

- `cross-fiber-steals-during-batch` — `atomic_batch_cross_fiber_steals_total` (NEW atomic on Evaluator, #790; # of fiber steals that fired while inside a suppressed atomic batch — counts the *observation* of a steal during batch, not the violation; bumped from `Evaluator::bump_atomic_batch_cross_fiber_steal()` at the planned Phase 2+ `restore_post_yield_or_rollback` + `MutationBoundaryGuard` wire-up)
- `hygiene-violations-in-batch` — `atomic_batch_hygiene_violations_total` (NEW atomic on Evaluator, #790; # of hygiene violations detected during an atomic batch body; bumped from `Evaluator::bump_atomic_batch_hygiene_violation()` at the planned Phase 2+ `hygiene_protected_error` path inside batch wire-up)
- `hygiene-violation-rate` — hardcoded 0 in Phase 1 (Phase 2+ to derive from `hygiene-violations-in-batch / batch-count × 10000`; the cross-reference ratio — high = hygiene drift inside batches)
- `atomic-batch-primitive-active` — hardcoded 0 (Phase 2+ to actually expose `(mutate:atomic-batch [body] :snapshot? #t)` primitive per body "Implement `(mutate:atomic-batch [body] :snapshot? #t)` that acquires outer StructuralMutationGuard + sets suppressed_, executes body (sequence of mutate:*), on success: single bump + optional snapshot ... on fail/panic: full rollback")
- `snapshot-capture-active` — hardcoded 0 (Phase 2+ to actually capture pinned StableNodeRef snapshot per body "Capture/pin affected refs (extend SafePCVSpan or PinnedStableRefSet) during batch; expose in snapshot for post-batch validation")
- `cross-fiber-re-stamp-active` — hardcoded 0 (Phase 2+ to wire `restore_post_yield_or_rollback` + `MutationBoundaryGuard` to re-stamp generation or force refresh pinned StableRefs when inside suppressed batch per body "if inside suppressed batch, re-stamp generation or force refresh pinned StableRefs; coordinate with `checkpoint_yield_boundary`")
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if all 3 deferred flags = 1; 1 = partial Phase 2+ if any deferred flag = 1; 2 = Phase 1 only if all deferred flags = 0 but `cross-fiber-steals-during-batch > 0 || hygiene-violations-in-batch > 0`; 3 = early-stage if all deferred flags = 0 AND no activity)
- `schema` — 790 (drift sentinel)

Distinct from the existing `#761` `(query:mutate-batch-stats)` (8 fields: batch-count / ops-total / rollback-count / ops-per-batch / bumps-saved-total / executed-under-concurrent-fiber / pinned-refs-last-batch / rollback-triggers + schema 761): that primitive surfaces the **per-batch-measurement** layer of atomic batch observability (how many batches fired, how many ops, how many rollbacks, how much churn saved). `#790` is the FIRST observability surface that surfaces the **enforcement-side + cross-fiber safety + AI-exposure** layer — was a steal detected during batch? was a hygiene violation caught inside batch? is the `(mutate:atomic-batch)` primitive actually exposed to AI? is the snapshot capture wired? is the cross-fiber re-stamp active? — as separate per-decision-point signals the Agent consumes to decide whether to trigger `mutation-impact-snapshot` `batch_impact` flag + cross-fiber re-stamp + snapshot capture under concurrent AI mutate. The body explicitly cites the gap: "no first-class exposed primitive `(mutate:atomic-batch)` with user/AI-callable snapshot of pinned StableRefs + unified per-boundary metrics ... + explicit fiber steal handling during suppressed batch".

### `(query:workspace-closedloop-fiber-multi-agent-yield-stats)` fields (#791)

- `autoprop-refs-total` — `workspace_closedloop_autoprop_refs_total` (NEW atomic on CompilerMetrics, #791; # of `StableRef`s auto-propagated/snapshotted across workspace COW/clone/split boundaries; bumped from `Evaluator::bump_workspace_closedloop_autoprop_ref()` at the planned Phase 2+ workspace tree + `is_valid_in` / WeakRef registry paths wire-up per body "On workspace COW/clone/split in primitives or WorkspaceTree, auto-propagate/snapshot active StableRef pins or dirty bits via epoch or weak registry; extend `is_valid_in` / `mark_dirty_upward` to notify cross-boundary")
- `autoprop-dirty-total` — `workspace_closedloop_autoprop_dirty_total` (NEW atomic on CompilerMetrics, #791; # of dirty bits auto-propagated on workspace COW/clone/split boundaries; bumped from `Evaluator::bump_workspace_closedloop_autoprop_dirty()` at the planned Phase 2+ `mark_dirty_upward` cross-boundary notification path wire-up)
- `missed-yield-total` — `workspace_closedloop_missed_yield_total` (NEW atomic on CompilerMetrics, #791; # of times a long walk — pattern matcher / `children_safe` iteration / `mark_dirty_upward` on verification subtrees — missed a yield point; the **negative signal** — high value = yield starvation under concurrent fiber load; bumped from `Evaluator::bump_workspace_closedloop_missed_yield()` at the planned Phase 2+ exhaustive yield instrumentation wire-up per body "Instrument all long walks ... with explicit fiber yield points or safepoint checks")
- `exhaustive-yield-instrumentation-active` — hardcoded 0 (Phase 2+ to wire `Fiber::yield` + `check_gc_safepoint` in `evaluator_primitives_query.cpp` + `mutate.cpp` + workspace paths long walks per body "Instrument all long walks (pattern matcher, children_safe iteration, mark_dirty_upward on SV verification nodes) with explicit fiber yield points or safepoint checks (Fiber::yield or check_gc_safepoint style)")
- `autoprop-active` — hardcoded 0 (Phase 2+ to wire StableRef/dirty auto-propagation across COW/clone/split boundaries per body "auto-propagate/snapshot active StableRef pins or dirty bits via epoch or weak registry"; covers the StableRef + dirty + cross-boundary validation aggregation flag)
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if both deferred flags = 1; 1 = partial Phase 2+ if any deferred flag = 1; 2 = Phase 1 only if both deferred flags = 0 but `autoprop-refs-total > 0 || autoprop-dirty-total > 0 || missed-yield-total > 0`; 3 = early-stage if both deferred flags = 0 AND no activity)
- `schema` — 791 (drift sentinel)

Distinct from the existing `#773` `(query:workspace-closedloop-fiber-eda-stats)` (6 fields: concurrent-query-mutate-success-pct + cross-cow-ref-validity-pct + yield-points-hit + shared-mutex-contention-ns + multi-agent-edit-fidelity + stale-ref-prevented-eda-loops + schema 773): that primitive surfaces the **pct-derived + count + ns** layer of Workspace closed-loop observability. `#791` is the FIRST observability surface that surfaces the **cross-boundary auto-propagation + missed-yield negative signal** layer — were StableRefs auto-propagated across COW/clone/split? were dirty bits auto-propagated? were long walks catching all yield points? — as separate per-decision-point signals the Agent consumes to decide whether to trigger exhaustive yield instrumentation, auto-propagation wire-up, or cross-boundary validation under concurrent multi-Agent EDA verification loops. The body explicitly cites the gap: "missing exhaustive yield instrumentation in EDSL hot paths + automatic cross-boundary (COW/sub-workspace) StableRef/dirty propagation wired into primitives".

### `(query:compiler-invalidate-guard-steal-stats)` fields (#792)

- `deferred-invalidates-total` — `compiler_invalidate_deferred_total` (NEW atomic on CompilerMetrics, #792; # of `invalidate_function` calls deferred when active `MutationBoundaryGuard` depth > 0 — defer epoch bump / re-lower to post-yield boundary; bumped from `Evaluator::bump_compiler_invalidate_deferred()` at the planned Phase 2+ `service.ixx` `invalidate_function` wire-up per body "Add param or query for current fiber's `mutation_stack_depth` ... If depth > 0 or inside Guard, defer epoch bump / re-lower to post-yield boundary or queue; expose `safe_invalidate_at_outermost_boundary()`")
- `version-refresh-hits-total` — `compiler_version_refresh_hits_total` (NEW atomic on CompilerMetrics, #792; # of `bridge_epoch` / `EnvFrame::version_` re-stamp hits on steal resume / `restore_post_yield_or_rollback`; bumped from `Evaluator::bump_compiler_version_refresh_hit()` at the planned Phase 2+ `evaluator_fiber_mutation.cpp` + `apply_closure` / `materialize_call_env` wire-up per body "On steal resume / `restore_post_yield_or_rollback` (if affected by recent invalidate), force `bridge_epoch` / `EnvFrame::version_` re-stamp + `closure_bridge_` refresh for live `IRClosure`; integrate with `GuardShape` `expected_shape` re-validation")
- `guardshape-deopt-on-steal-total` — `compiler_guardshape_deopt_on_steal_total` (NEW atomic on CompilerMetrics, #792; # of `GuardShape` deopts triggered on steal when `bridge_epoch` mismatch detected; bumped from `Evaluator::bump_compiler_guardshape_deopt_on_steal()` at the planned Phase 2+ `aura_jit_bridge.cpp` + JIT hot-swap paths wire-up per body "During refcount swap / hot-reload, if any fiber in `MutationBoundary` or `apply_closure` active, defer or use grace + force `GuardShape` deopt + `linear_state` re-check on affected funcs; wire to `mutation_epoch_`")
- `live-closure-stale-prevented-total` — `compiler_live_closure_stale_prevented_total` (NEW atomic on CompilerMetrics, #792; # of live `IRClosure` stale references prevented via `closure_bridge_` refresh; bumped from `Evaluator::bump_compiler_live_closure_stale_prevented()` at the planned Phase 2+ `apply_closure` dual-path + `bridge_epoch` check wire-up)
- `safe-invalidate-at-outermost-boundary-active` — hardcoded 0 (Phase 2+ to actually expose `safe_invalidate_at_outermost_boundary()` helper per body "expose `safe_invalidate_at_outermost_boundary()`")
- `steal-resume-version-refresh-active` — hardcoded 0 (Phase 2+ to wire force `bridge_epoch` / `EnvFrame::version_` re-stamp + `closure_bridge_` refresh on steal resume)
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if both deferred flags = 1; 1 = partial Phase 2+ if any deferred flag = 1; 2 = Phase 1 only if both deferred flags = 0 but `deferred-invalidates-total > 0 || version-refresh-hits-total > 0 || guardshape-deopt-on-steal-total > 0 || live-closure-stale-prevented-total > 0`; 3 = early-stage if both deferred flags = 0 AND no activity)
- `schema` — 792 (drift sentinel)

Distinct from the existing `mutation-impact + aot-hotupdate-stats` (#708/#590): those primitives surface the **detection-side** of compiler-runtime sync (did the reload attempt happen, was it rejected, was the bridge_epoch mismatch detected). `#792` is the FIRST observability surface that surfaces the **enforcement-side** of compiler-runtime sync — was the invalidate deferred when Guard was held? was the version refresh hit on steal resume? was the GuardShape deopt triggered on steal? was the live closure stale reference prevented? — as separate per-decision-point signals the Agent consumes to decide whether to trigger `safe_invalidate_at_outermost_boundary`, `bridge_epoch` re-stamp, or `GuardShape` deopt under concurrent fiber steal + invalidate load. The body explicitly cites the gap: "no atomic 'safe invalidate point' or forced refresh of live closures/Envs/GuardShape on steal resume or post-rollback".

### `(query:jit-aot-hotswap-fidelity-stats)` fields (#793)

- `deopt-forced-on-reload-total` — `jit_deopt_forced_on_reload_total` (NEW atomic on CompilerMetrics, #793; # of `GuardShape` deopts forced on AOT reload / refcount swap; bumped from `Evaluator::bump_jit_deopt_forced_on_reload()` at the planned Phase 2+ `aura_jit.cpp` + `aura_jit_bridge.cpp` hot-swap path wire-up per body "On successful refcount swap or region reload, if any active fiber holds `MutationBoundary` or has live `GuardShape`/`Apply` on affected func, force deopt (set `generic_block`) or bump `shape_id` / `linear_state` for affected IR")
- `linear-violation-prevented-total` — `jit_linear_violation_prevented_total` (NEW atomic on CompilerMetrics, #793; # of linear ownership violations prevented via JIT runtime version check / `MoveOp` invalidation; bumped from `Evaluator::bump_jit_linear_violation_prevented()` at the planned Phase 2+ `aura_jit.cpp` JIT codegen for `Linear*` wire-up per body "Emit additional runtime checks (`version_` probe or `bridge_epoch` compare) before deopt decision or `MoveOp`")
- `env-version-sync-hits-total` — `jit_env_version_sync_hits_total` (NEW atomic on CompilerMetrics, #793; # of `EnvFrame::version_` sync hits triggered on JIT-executed closure steal resume / post-rollback; bumped from `Evaluator::bump_jit_env_version_sync_hit()` at the planned Phase 2+ `evaluator_fiber_mutation.cpp` + `apply_closure` wire-up per body "On steal resume / post-rollback, for JIT-executed closures, trigger `GuardShape` re-evaluation or linear re-wrap if `version_` or epoch drifted")
- `guardshape-stale-reject-total` — `jit_guardshape_stale_reject_total` (NEW atomic on CompilerMetrics, #793; # of JIT `GuardShape` stale rejections caught when `expected_shape` / `shape_id` mismatch detected at `apply_closure` time; bumped from `Evaluator::bump_jit_guardshape_stale_reject()` at the planned Phase 2+ `ir_executor.ixx` + `evaluator.ixx` `apply_closure` `bridge_epoch` check wire-up per body "`IRInterpreter` handling of `GuardShape`/linear + `apply_closure` (`bridge_epoch` check)")
- `reload-deopt-version-hooks-active` — hardcoded 0 (Phase 2+ to wire reload-deopt version hooks in `aura_jit.cpp` + `aura_jit_bridge.cpp` hot-swap path)
- `jit-emit-runtime-version-checks-active` — hardcoded 0 (Phase 2+ to wire additional runtime checks in JIT codegen for `GuardShape` / `Linear*` ops)
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if both deferred flags = 1; 1 = partial Phase 2+ if any deferred flag = 1; 2 = Phase 1 only if both deferred flags = 0 but `deopt-forced-on-reload-total > 0 || linear-violation-prevented-total > 0 || env-version-sync-hits-total > 0 || guardshape-stale-reject-total > 0`; 3 = early-stage if both deferred flags = 0 AND no activity)
- `schema` — 793 (drift sentinel)

Distinct from the existing `#785` `(query:aot-concurrent-hotupdate-stats)` + `#787` `(query:task6-concurrent-fidelity)` + `#755` `(query:concurrent-safety-full-cycle-stats)`: those primitives surface the **reload-concurrency + per-component fidelity** layer. `#793` is the FIRST observability surface that surfaces the **JIT/AOT hot-swap fidelity for GuardShape + linear + EnvFrame versions** specifically — was a deopt forced on reload? was a linear violation prevented? was an EnvFrame version synced on steal? was a GuardShape stale rejection caught? — as separate per-decision-point signals the Agent consumes to decide whether to trigger reload-deopt version hooks, JIT emit runtime version checks, or `apply_closure` `bridge_epoch` re-validation under concurrent JIT/AOT hot-swap + steal + Guard rollback load. The body explicitly cites the gap: "no integrated 'fidelity checkpoint' in hot-swap/steal paths" and "JIT may execute with stale `expected_shape` or `linear_state` vs current `EnvFrame::version_` / `bridge_epoch`".

### `(query:full-closedloop-compiler-edsl-fidelity-stats)` fields (#794)

- `cross-layer-guardshape-deopt-hits-total` — `cross_layer_guardshape_deopt_hits_total` (NEW atomic on CompilerMetrics, #794; # of times the full closed-loop harness detected `GuardShape` expected vs runtime shape mismatch across the full pipeline; bumped from `Evaluator::bump_cross_layer_guardshape_deopt_hit()` at the planned Phase 2+ `tests/test_full_compiler_edsl_closedloop_fidelity.cpp` wire-up)
- `cross-layer-linear-enforce-success-total` — `cross_layer_linear_enforce_success_total` (NEW atomic on CompilerMetrics, #794; # of times `linear_ownership_state` was respected across compiler + EDSL boundary; bumped from `Evaluator::bump_cross_layer_linear_enforce_success()` at the planned Phase 2+ harness wire-up)
- `cross-layer-epoch-sync-total` — `cross_layer_epoch_sync_total` (NEW atomic on CompilerMetrics, #794; # of times `EnvFrame::version_` + `bridge_epoch` were synchronized across layers; bumped from `Evaluator::bump_cross_layer_epoch_sync()` at the planned Phase 2+ harness wire-up)
- `cross-layer-drift-detections-total` — `cross_layer_drift_detections_total` (NEW atomic on CompilerMetrics, #794; **the negative signal** — # of times the harness detected any cross-layer drift; high value = SLO breach; bumped from `Evaluator::bump_cross_layer_drift_detection()` at the planned Phase 2+ harness wire-up)
- `full-closedloop-harness-active` — hardcoded 0 (Phase 2+ to actually implement `tests/test_full_compiler_edsl_closedloop_fidelity.cpp` per body "New harness `tests/test_full_compiler_edsl_closedloop_fidelity.cpp`: Implement multi-round SEVA-style loop with heavy macro/EDSL mutate under Guard + concurrent fibers + steal injection + AOT reload points; trigger compiler invalidate via mutate; assert after each cycle: GuardShape expected matches runtime shape, linear_ownership_state respected ... EnvFrame version_ consistent, bridge_epoch fresh, StableRef valid, no hygiene drift, Interpreter vs JIT result identical, metrics match SLO")
- `slo-gate-active` — hardcoded 0 (Phase 2+ to wire CI gate + trend dashboard + self-heal hooks per body "Define quantitative gates (fidelity >99.5% over 10k cycles under 8+ fibers + steal/AOT load; zero undetected drift; TSan/ASan clean); add CI step that runs harness and fails PR on breach; publish trend dashboard")
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if both deferred flags = 1; 1 = partial Phase 2+ if any deferred flag = 1; 2 = Phase 1 only if both deferred flags = 0 but `cross-layer-guardshape-deopt-hits-total > 0 || cross-layer-linear-enforce-success-total > 0 || cross-layer-epoch-sync-total > 0 || cross-layer-drift-detections-total > 0`; 3 = early-stage if both deferred flags = 0 AND no activity)
- `schema` — 794 (drift sentinel)

Distinct from the existing `#786` `(query:code-as-data-production-health)` + `#787` `(query:task6-concurrent-fidelity)` + `#755` `(query:concurrent-safety-full-cycle-stats)` + `#792` `(query:compiler-invalidate-guard-steal-stats)` + `#793` `(query:jit-aot-hotswap-fidelity-stats)`: those primitives surface the **per-component** fidelity signal individually (production health, concurrent fidelity, compiler sync, JIT/AOT hot-swap). `#794` is the FIRST observability surface that surfaces the **cross-layer closed-loop harness** fidelity signals specifically — was the GuardShape deopt caught across the full pipeline? was linear enforcement successful across layers? was the epoch synced? was any cross-layer drift detected? — as separate per-decision-point signals the Agent consumes to decide whether to trigger full-cycle re-validation under production self-mod load. The body explicitly cites the gap: "no single unified harness that exercises the full vertical ... Gaps in cross-layer drift only surface in integrated long-running concurrent scenarios".

### `(query:shape-pass-hotpath-contracts-stats)` fields (#795)

- `soa-view-violations-caught-total` — `soa_view_violations_caught_total` (NEW atomic on CompilerMetrics, #795; # of `SoAView` concept `static_assert` violations caught at compile time / runtime; bumped from `Evaluator::bump_soa_view_violations_caught()` at the planned Phase 2+ `pass_manager.ixx` + lowering/JIT `run_incremental_dirty_pipeline` wire-up per body "Define `SoAView` concept (requires const view + `shape_id` consult) and `ShapeStablePass` (requires `stable_shape` consult + `DirtyAware`); `static_assert` in `run_incremental_dirty_pipeline`")
- `shape-stable-pass-violations-total` — `shape_stable_pass_violations_total` (NEW atomic on CompilerMetrics, #795; # of `ShapeStablePass` concept `static_assert` violations caught; bumped from `Evaluator::bump_shape_stable_pass_violations()` at the planned Phase 2+ `pass_manager.ixx` + `dominant_shape` / `ShapePropagationPass` wire-up)
- `targeted-deopt-via-impact-scope-total` — `targeted_deopt_via_impact_scope_total` (NEW atomic on CompilerMetrics, #795; # of targeted deopts via `#741` `impact_scope` instead of global invalidation; bumped from `Evaluator::bump_targeted_deopt_via_impact_scope()` at the planned Phase 2+ `shape_profiler.cpp` deopt hook wire-up per body "consult `DirtyAware` or `#741` `impact_scope` for targeted invalidation instead of global")
- `on-compact-hook-invocations-total` — `on_compact_hook_invocations_total` (NEW atomic on CompilerMetrics, #795; # of Arena compact `on_compact_hook_` invocations that triggered `shape_inval` + dirty cascade; bumped from `Evaluator::bump_on_compact_hook_invocation()` at the planned Phase 2+ `arena.ixx` + `ir_soa.ixx` `on_compact_hook_` wire-up per body "`on_compact_hook_` invoke with `shape_inval` + dirty cascade")
- `concepts-active` — hardcoded 0 (Phase 2+ to actually wire `SoAView` + `ShapeStablePass` concepts + targeted deopt via `impact_scope` + `ShapeProfiler` epoch sync all together — single flag covers all 3+ deferred wire-up areas)
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if `concepts-active == 1`; 1 = partial if any deferred wire-up partially active; 2 = Phase 1 only if `concepts-active == 0` but any of `soa-view-violations-caught-total > 0 || shape-stable-pass-violations-total > 0 || targeted-deopt-via-impact-scope-total > 0 || on-compact-hook-invocations-total > 0`; 3 = early-stage if `concepts-active == 0` AND no activity)
- `schema` — 795 (drift sentinel)

Distinct from the existing `#768` `(query:shape-pass-hotpath-stats)` (5 fields: contract-checks-hotpath / shape-stability-transitions / jit-epoch-sync-hits / deopt-targeted-skips / concept-violations-caught + schema 768): that primitive surfaces the **hot-path observability counters** layer. `#795` is the FIRST observability surface that surfaces the **deep SoA/Pass/JIT contracts + stronger concepts + targeted invalidation + Arena compact hook** layer specifically — were SoAView violations caught? were ShapeStablePass violations caught? was a targeted deopt via `#741` `impact_scope` used? was an Arena compact `on_compact_hook_` invoked? — as separate per-decision-point signals the Agent consumes to monitor the C++26 Contracts/Concepts adoption maturity in the hot allocator/dispatch/SoA/shape paths. The body explicitly cites the gap: "Hot-path Contracts still light in SoA column accessors (view_at/mark), dirty cascade, shape dispatch stability transition, Arena compact hook; no SoAView concept or ShapeStablePass for compile-time enforcement; ShapeProfiler version not wired to mutation_epoch_/JIT bridge_epoch or DirtyAware short-circuit; deopt not consulting impact_scope from #741 for targeted invalidation".

### `(query:ir-soa-full-migration-stats)` fields (#796)

- `soa-instructions-emitted-total` — `ir_soa_instructions_emitted_total` (NEW atomic on CompilerMetrics, #796; # of instructions emitted to `IRFunctionSoA` vs remaining AoS `IRModule` paths; bumped from `Evaluator::bump_ir_soa_instructions_emitted()` at the planned Phase 2+ `lowering_impl.cpp` + JIT emit sites wire-up per body "Complete port of `LoweringState` emit, `ir_executor` traversal, JIT emitter to prefer `IRFunctionSoA` + `IRInstructionView`")
- `dirty-block-skips-total` — `ir_soa_dirty_block_skips_total` (NEW atomic on CompilerMetrics, #796; # of blocks skipped via `DirtyAwarePass` + `run_incremental_dirty_pipeline` short-circuit; bumped from `Evaluator::bump_ir_soa_dirty_block_skips()` at the planned Phase 2+ `service.ixx` `invalidate_function` + lowering/JIT path wire-up per body "Enforce `DirtyAwarePass` + `run_incremental_dirty_pipeline` in `invalidate_function` + JIT recompile; consult `is_block_dirty` / `is_instruction_dirty` + `#741` `impact_scope` for hybrid targeting; short-circuit clean/impact-free blocks")
- `jit-soa-time-ns-total` — `ir_soa_jit_soa_time_ns_total` (NEW atomic on CompilerMetrics, #796; **time-based** — total ns spent in JIT SoA emit path; high value = JIT SoA path actively exercised vs AoS path; bumped from `Evaluator::bump_ir_soa_jit_soa_time_ns()` at the planned Phase 2+ `aura_jit.cpp` SoA emit path wire-up)
- `impact-dirty-hybrid-skips-total` — `ir_soa_impact_dirty_hybrid_skips_total` (NEW atomic on CompilerMetrics, #796; # of skips via hybrid `impact_scope` + `is_block_dirty` targeting — the combined `#741` + `#766` short-circuit count; bumped from `Evaluator::bump_ir_soa_impact_dirty_hybrid_skip()` at the planned Phase 2+ `service.ixx` `invalidate_function` when both `DepGraph` `impact_scope` + SoA block dirty are consulted together per body "consult ... `#741` `impact_scope` for hybrid targeting")
- `clean-block-hit-rate` — hardcoded 0 in Phase 1 (Phase 2+ to derive from `#766` `ir-soa-migration-stats` + dirty block counts; the cross-reference ratio — high = many clean blocks skipped via DirtyAware short-circuit)
- `full-soa-migration-active` — hardcoded 0 (Phase 2+ to actually complete the production-grade migration of `LoweringState` emit + `ir_executor` traversal + JIT emitter to prefer `IRFunctionSoA` + full `pmr` column migration + `DepGraph` integration — single flag covers all deferred wire-up areas)
- `recommendation` — derived 0/1/2/3 (0 = production-ready with all Phase 2+ if `full-soa-migration-active == 1`; 1 = partial if any deferred wire-up partially active; 2 = Phase 1 only if `full-soa-migration-active == 0` but any of `soa-instructions-emitted-total > 0 || dirty-block-skips-total > 0 || jit-soa-time-ns-total > 0 || impact-dirty-hybrid-skips-total > 0`; 3 = early-stage if `full-soa-migration-active == 0` AND no activity)
- `schema` — 796 (drift sentinel)

Distinct from the existing `#766` `(query:ir-soa-migration-stats)` (5 fields + schema 766): that primitive surfaces the **IR-SoA Phase 1** dashboard. `#796` is the FIRST observability surface that surfaces the **end-to-end production migration** signals specifically — were instructions emitted to `IRFunctionSoA`? were dirty blocks skipped via `DirtyAwarePass`? was the JIT SoA emit path exercised? was the hybrid impact+dirty skip consulted? — as separate per-decision-point signals the Agent consumes to monitor production-grade SoA migration in compiler hot paths under concurrent AI self-mod. The body explicitly cites the gap: "IR SoA remains scaffold; no production port of lowering re-emit / eval dispatch / JIT codegen to IRModuleV2 + full DirtyAware short-circuit + complete pmr/arena hosting for all columns; DepGraph impact not consulted for hybrid dirty+impact targeting".

### `(query:orchestration-steal-outermost-stats)` fields (#783)

- `outermost-steal-total` — process-wide lifetime # of successful work-steals at a MutationBoundary point with depth==0 (safe + boundary) — from the new `Fiber::static_steal_outermost_mutation_boundary_count_` atomic, bumped in `WorkerThread::steal()` when the victim yielded at MutationBoundary + `is_at_mutation_boundary_safe()` returns true (depth probe via `aura_evaluator_mutation_stack_depth_from_ptr(mutation_stack_storage_) == 0`)
- `inner-deferred-total` — process-wide lifetime # of steal attempts deferred because the victim held an inner MutationBoundary guard (depth>0 — unsafe to move, would risk deadlock / hygiene drift) — from the new `Fiber::static_steal_inner_mutation_boundary_deferred_count_` atomic, bumped alongside the existing `bump_steal_deferred_mutation_boundary()` coarse counter
- `cross-fiber-safe-steal-total` — process-wide lifetime # of outermost safe steals that crossed between workers — from the new `Fiber::static_cross_fiber_mutation_safe_steal_count_` atomic, bumped on every successful `MutationBoundary + depth==0` cross-fiber steal
- `strict-stable-ref-refresh` — hardcoded 0 (Phase 2+ deferred: actually force StableRef refresh on resume of a stolen outermost fiber per body "On steal of outermost: force StableRef / EnvFrame version refresh on resume")
- `envframe-version-refresh` — hardcoded 0 (Phase 2+ deferred: actually bump `EnvFrame::version_` on resume of a stolen fiber)
- `bias-deferred-outermost-total` — hardcoded 0 (`#754` adaptive bias feature not shipped — would record outermost defers driven by the bias scheduler preferring different priority)
- `recommendation` — derived 0/1/2/3 (0 = production-ready if all 3 Phase 2+ flags = 1; 1 = partial if any Phase 2+ flag = 1; 2 = Phase 1 only if all 3 flags = 0 but `outermost-steal-total > 0 || inner-deferred-total > 0 || cross-fiber-safe-steal-total > 0`; 3 = early-stage if all 3 flags = 0 AND no activity)
- `schema` — 783 (drift sentinel)

Distinct from the existing `(query:orchestration-metrics)` (#451) + `(query:scheduler-mutation-coord-stats)` (#618/#591): those primitives surface the coarse `steal_deferred_mutation_boundary_count_` as one lumped figure (no outermost/inner split) and don't expose the cross-fiber safe steal signal. `#783` is the FIRST observability surface that splits the steal deferral into the production-grade components the body asks for ("separate outermost_deferred vs inner_deferred; expose via `query:orchestration-steal-outermost-stats`"), exposes the `cross_fiber_mutation_safe_steal` counter, and marks the Phase 2+ deferred work (StableRef / EnvFrame version refresh + `#754` bias-driven deferral) as hardcoded "not yet" flags.
### `(query:stable-ref-cross-cow-provenance-stats)` fields (#818)

- `provenance-enforced-hits` — `stable_ref_provenance_enforced_total` (full provenance validate on mutate/query StableRef unpack)
- `cross-cow-refresh-hits` — `stable_ref_cross_cow_refresh_hits_total` (`workspace:resolve-stable-ref` / `validate_or_refresh` success)
- `fiber-workspace-mismatch-prevented` — `stable_ref_fiber_workspace_mismatch_prevented_total` (fiber/workspace id inconsistency caught)
- `steal-auto-refresh-hits` — `stable_ref_steal_auto_refresh_total` (fiber steal/resume auto-refresh)
- `schema` — 818 (drift sentinel)

Distinct from `(query:stable-ref-provenance-sv-stats)` (#641) / `(query:stable-ref-layer-stats)` (#715) / `(query:stable-ref-boundary-stats-hash)` (#738): #818 is the unified full-provenance + cross-COW auto-resolve enforcement dashboard for multi-Agent EDSL orchestration.

### `(query:primitives-hotpath-registry-stats)` fields (#805)

- `fastpath-hit-rate-pct` — derived `primitive_fastpath_hits_total / primitive_call_total × 10000`
- `ns-per-apply` — `hotpath_registry_ns_accum_total / hotpath_registry_apply_samples_total`
- `linear-cost` — `hotpath_registry_linear_cost_total` (cdr-walk sample cost)
- `extension-reg-ns` — `hotpath_registry_extension_reg_ns_total` (registry extension probe cost)
- `bench-runs` — `hotpath_registry_bench_runs_total`
- `schema` — 805 (drift sentinel)

Distinct from `(query:primitives-hotpath-slo-stats)` (#776): #776 is stability-score + regression-flag composite; #805 is **sample-based** registry/list-apply load metrics (ns/apply, extension reg cost) for CI SLO gates under mutation+fiber load.

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