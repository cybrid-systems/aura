// evaluator_primitives_observability.cpp — P0 step 15: panic / stats / jit / gc-arena observability
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"
#include "compiler/aura_jit_bridge.h"
#include "observability_metrics.h"
#include "compiler/shape.h"
#include "compiler/value_tags.h"
#include "core/cpp26_contract_stats.h"
#include "core/arena_auto_policy_stats.h"
#include "jit_typed_mutation_stats.h"
#include "shape_jit_pass_closedloop_stats.h"
#include "ci_build_info.h"
#include "primitives_meta.h"
#include "primitives_detail.h"
#include "serve/metrics.h"
#include "hash_meta.h"    // FNV constants (#901)
#include "basis_points.h" // #905

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.compiler.pass_manager;

// Issue #783: C-linkage shims for the refined work-steal
// metrics (outermost vs inner MutationBoundary split +
// cross-fiber safe steal). Defined in fiber.cpp. We
// forward-declare here to avoid pulling in fiber.h (and
// transitively scheduler/worker headers) from this
// translation unit — the shims are tiny extern "C"
// wrappers that just return a std::atomic<uint64_t>::load().
extern "C" std::uint64_t aura_fiber_static_steal_outermost_mutation_boundary_total();
extern "C" std::uint64_t aura_fiber_static_steal_inner_mutation_boundary_deferred_total();
extern "C" std::uint64_t aura_fiber_static_cross_fiber_mutation_safe_steal_total();

namespace aura::compiler::primitives_detail {

// Issues #810/#811 process-wide init / guest-exception counters (defined in serve/fiber.cpp,
// aura_jit_runtime.cpp).
extern "C" std::uint64_t aura_fiber_init_aura_result_ok_total();
extern "C" std::uint64_t aura_fiber_init_aura_result_err_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_ok_total();
extern "C" std::uint64_t aura_scheduler_init_aura_result_err_total();
extern "C" std::uint64_t aura_jit_guest_exception_bridge_total();

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

// Issue #560: canonical list of every registered *-stats observability
// primitive. Single source of truth for both (stats:list) and
// (stats:count). Adding a new (query:*-stats) primitive is one entry here
// — the count and the enumeration both update automatically.
//
// The Aura stdlib (`lib/std/stats.aura`) mirrors this list at require
// time, so AI agents can also call these primitives by name. When you add
// a new entry here, also add a matching line to `lib/std/stats.aura` so the
// EDSL discoverability surface stays in sync.
const std::vector<std::string> kObservabilityStatsPrimitives = {
    // Issue #543 — EnvFrame dual-path
    "query:envframe-dualpath-stats",
    // Issue #547 — Pattern + MacroIntroduced hygiene
    "query:pattern-index-stats",
    // Issue #490 — lazy vs eager pattern-index rebuild observability
    "query:pattern-index-rebuild-stats",
    "query:pattern-hygiene-stats",
    // Issue #486 — MacroIntroduced hygiene decision hash
    "query:macro-hygiene-stats",
    // Issue #548 — Panic-checkpoint lifecycle
    "query:panic-checkpoint-lifecycle-stats",
    // Issue #549 — Self-evolution stability
    "query:self-evolution-stability-stats",
    // Issue #550 — Typed mutation + dirty impact
    "query:typed-mutation-stats",
    "query:dirty-impact",
    // Issue #573 — Task2 solve_delta + narrowing incremental
    "query:typed-incremental-stats",
    // Issue #608 — Incremental type reliability
    "query:type-incremental-stats",
    // Issue #798 — ConstraintSystem incremental fidelity under Guard/steal
    "query:type-incremental-fidelity-stats",
    // Issue #509 — solve_delta touched_roots soundness
    "query:constraint-delta-stats",
    // Issue #628 — solve_delta clean-conflict safety
    "query:solve-delta-safety-stats",
    // Issue #467 — Per-node occurrence-dirty + blame chain
    "query:occurrence-stats",
    // Issue #495 — Task2 refinement closed-loop pillars
    "query:task2-refinement-stats",
    // Issue #609 — Occurrence narrow post-mutate recovery
    "query:occurrence-narrow-stats",
    // Issue #576 — Task2 occurrence blame + provenance
    "query:occurrence-blame-stats",
    // Issue #577 — Task2 ADT exhaustiveness + match narrowing
    "query:adt-exhaustiveness-stats",
    // Issue #454 — Reflection-to-EDSL bridge (FlatAST/marker)
    "query:reflect-edsl-bridge-stats",
    // Issue #551 — Reflect post-mutate
    "query:reflect-postmutate-stats",
    // Issue #593 — AST→query→IR MacroIntroduced hygiene closed loop
    "query:pattern-ir-hygiene-closed-loop-stats",
    // Issue #594 — Static reflection self-mod validation hook
    "query:reflection-selfmod-stats",
    // Issue #596 — Guard + panic checkpoint + reflect closed loop
    "query:guard-panic-reflect-stats",
    // Issue #599 — Compiler root epoch/version + GC synergy
    "query:compiler-root-stats",
    // Issue #600 — Per-block dirty + impact scope + closure bridge synergy
    "query:incremental-closure-stats",
    // Issue #741 — impact_scope → closure_bridge + EnvFrame version re-stamp
    "query:incremental-closure-bridge-stats",
    // Issue #654 — Macro hygiene vs fiber/panic/AOT/SoA cross-cutting gaps
    "query:macro-hygiene-fiber-panic-stats",
    // Issue #655 — EDSL core stability COW/atomic/query/mutate gaps
    "query:edsl-core-stability-stats",
    // Issue #657 — Compiler core incremental self-mod gaps
    "query:compiler-core-incremental-stats",
    // Issue #658 — High-perf C++26 Arena/SoA/Value/Shape/Pass gaps
    "query:highperf-cpp26-stats",
    // Issue #742 — C++26 Contracts + consteval hot-path invariants
    "query:cpp26-contracts-stats",
    // Issue #743 — Arena auto-compact + defrag + fiber safepoint + dirty/Shape
    "query:arena-auto-policy-stats",
    // Issue #744 — Shape → dirty → Pass → JIT deopt/recompile closed loop
    "query:shape-jit-pass-closedloop-stats",
    // Issue #745 — Dynamic reverify limit + Occurrence priority in solve_delta
    "query:constraint-reverify-occurrence-stats",
    // Issue #746 — JIT typed-mutation narrow_evidence / linear / L2 propagation
    "query:jit-typed-mutation-stats",
    // Issue #747 — Linear + Occurrence predicate-branch safety under typed mutate
    "query:linear-occurrence-mutate-stats",
    // Issue #748 — SV verification EDSL structured mutate + dirty re-emit closed-loop
    "query:sv-verification-structure-stats",
    // Issue #801 — SV commercial emit fidelity stats
    "query:sv-commercial-emit-fidelity-stats",
    // Issue #802 — SV verification self-evolution closed-loop stats
    "query:sv-verification-self-evolution-stats",
    // Issue #805 — registry + list apply hot-path load SLO
    "query:primitives-hotpath-registry-stats",
    // Issue #803 — SEVA Long-Running Concurrent Verification
    // Evolution SLO observability (P0 EDA-SV-verification-
    // production long-running concurrent multi-agent harness
    // foundation; consolidates/non-duplicates #794 + #755 + #773
    // + #774 + #802 + #748; non-duplicative with each component
    // primitive). #803 is the FIRST observability surface that
    // tracks the *production-scale long-running concurrent SEVA
    // SLO composite* — convergence_rate (derived from #802),
    // ref_drift_prevented (#803 NEW atomic),
    // hygiene_safe_rollback_pct (derived from #759 +
    // #632 atomic_batch_sv_rollback_total),
    // steal_during_verification_mutate (#803 NEW atomic),
    // dirty_consistency_hits (#803 NEW atomic),
    // avg_rounds_to_target (derived from #802), and the
    // Phase 2+ longrunning-harness-active flag — as a single
    // deployment-grade SLO composite the Agent reads to decide
    // whether the long-running concurrent SEVA production
    // harness is ready for commercial multi-agent deployment.
    "query:seva-longrunning-concurrent-slo",
    // Issue #818 — StableNodeRef full provenance + cross-COW enforcement
    "query:stable-ref-cross-cow-provenance-stats",
    "query:stable-ref-provenance-stats",
    // Issue #1565 — Capability Effects enforcement + provenance binding
    "query:capability-effect-stats",
    // Issue #1566 — multi-tenant WorkspaceIsolationPolicy
    "query:tenant-isolation-stats",
    // Issue #1567 — mutation audit WAL persist + crash recovery
    "query:audit-wal-stats",
    "query:mutation-audit-log",
    // Issue #1568 — linear boundary consistency closed-loop
    "query:linear-boundary-consistency-stats",
    // Issues #809–#817 Phase 1 unified production/error/macro surfaces
    "query:error-handling-policy-stats",
    "query:fiber-scheduler-init-stats",
    "query:jit-exception-bridge-stats",
    "query:orchestration-steal-arena-gc-stats",
    "query:guard-error-stats",
    "query:runtime-production-health",
    "query:macro-introduced-provenance-stats",
    "query:edsl-struct-meta-stats",
    "query:dirty-epoch-marker-stats",
    // Issues #819–#829 Phase 1
    "query:pattern-hygiene-provenance-stats",
    "query:mutate-atomic-batch-e2e-stats",
    "query:jit-fiber-exception-stats",
    "query:l2-specialization-deopt-stats",
    "query:opcode-coverage-deopt-stats",
    "query:terminal-render-production-stats",
    "query:render-ffi-buffer-stats",
    "query:render-hotpath-stats",
    "query:shape-value-hotpath-contracts-stats",
    "query:ir-soa-full-enforcement-stats",
    "query:arena-live-defrag-stats",
    // Issue #1518 — live compact + relocate + deopt coordination
    "query:arena-live-compact-stats",
    // Remaining open issues Phase 1 batch
    "query:pass-shape-epoch-stats",
    "query:edsl-hotpath-real-stats",
    "query:dead-coercion-elim-stats",
    "query:occurrence-renarrow-stats",
    "query:linear-escape-mutate-stats",
    "query:typed-mutate-coercion-stats",
    "query:fiber-epoch-type-safety-stats",
    "query:sv-verification-feedback-mutate-stats",
    "query:seva-longrunning-harness-v2-stats",
    "query:typed-mutation-audit-stats",
    "query:stable-ref-full-provenance-v2-stats",
    "query:longrunning-ai-infra-stats",
    "query:ai-native-meta-extension-stats",
    "query:orchestration-telemetry-pipeline-stats",
    "query:per-fiber-exception-state-stats",
    "query:aot-hotswap-pipeline-stats",
    "query:macro-hygiene-query-provenance-v2-stats",
    "query:reflection-edsl-extension-v2-stats",
    "query:self-evolution-hygiene-dirty-epoch-stats",
    "query:sv-verification-feedback-closedloop-stats",
    "query:pattern-defuse-hygiene-full-stats",
    "query:stable-ref-mutation-log-hardening-stats",
    "query:dirtyaware-impact-enforcement-v2-stats",
    "query:live-irclosure-envframe-gc-stats",
    "query:source-marker-linear-consistency-stats",
    "query:terminal-buffer-diff-present-stats",
    "query:render-observability-v2-stats",
    "query:render-jit-soa-hotpath-stats",
    "query:arena-live-defrag-full-v2-stats",
    "query:ir-soa-dirty-hybrid-full-v2-stats",
    "query:value-shape-consteval-full-v2-stats",
    "query:defuse-infer-partial-stats",
    "query:ownership-escape-postmutate-stats",
    "query:typed-mutation-audit-pass-stats",
    "query:sv-backend-emit-bidirectional-stats",
    "query:large-sv-pattern-defuse-stats",
    "query:longrunning-stable-ref-dirty-stats",
    "query:sv-eda-primitives-cluster-stats",
    "query:primitives-resource-quota-fiber-stats",
    "query:declarative-primitive-registry-stats",
    "query:primitives-namespace-alias-stats",
    "query:guard-steal-gc-safety-v2-stats",
    "query:dirtyaware-ir-cache-consistency-stats",
    "query:stats-builder-refactor-stats",
    "query:load-or-zero-helper-stats",
    "query:cpp26-modernization-sweep-stats",
    "query:metrics-meta-reflection-stats",
    "query:test-harness-bootstrap-stats",
    "query:bundle-codegen-decouple-stats",
    "query:test-bundle-migration-stats",
    "query:test-profile-flag-stats",
    "query:test-harness-module-stats",
    "query:test-json-report-stats",
    "query:gcc16-modules-buildenv-stats",
    // Issue #750 — Runtime reflection schema validation for macro/EDSL mutate
    "query:reflection-schema-stats",
    // Issue #659 — Type system typed-mutate incremental gaps
    "query:typesystem-typed-mutate-stats",
    // Issue #673 — Unified Runtime Observability Layer (P1)
    // cross-module correlation primitive
    "query:runtime-observability-correlated-stats",
    // Issue #674 — Closed-loop self-evolution chaos stress
    // outcome classifier primitive
    "query:self-evolution-chaos-stats",
    // Issue #597 — Macro+reflect+self-evo combined loop
    "query:macro-reflect-self-evo-stats",
    // Issue #488 — Guard impact snapshot hash
    "query:mutation-impact-snapshot",
    // Issue #504 — Guard impact log for AI decision loops
    "query:mutation-boundary-log",
    "query:mutation-boundary-hold-stats",
    // Issue #1504 — first-class safe yield + depth instrumentation
    "query:mutation-boundary-depth",
    "query:mutation-boundary-safe-yield",
    "query:mutation-boundary-safe-yield-stats",
    // Issue #1591 — fairness dashboard + per-fiber depth alias
    "query:mutation-boundary-fairness-stats",
    "query:per-fiber-mutation-depth-stats",
    // Issue #1592 — post-steal EnvFrame/StableNodeRef/linear closed loop
    "query:post-steal-closed-loop-stats",
    // Issue #1598 / #1604 / #1607 — apply + unified invalidation dual-epoch
    "query:epoch-apply-hotpath-stats",
    // Issue #1595 — join / mailbox / parallel_intend linear + StableNodeRef
    "query:join-linear-enforcement-stats",
    "ast:yield-at-boundary",
    // Issue #595 — Marker/dirty/epoch/Guard self-evo loop
    "query:self-evolution-loop-stats",
    // Issue #415 — DirtyReason verification categories +
    // mark_dirty_upward propagation synthesis
    "query:dirty-reason-propagation-stats",
    // Issue #517 — Consolidated 3-pillar production priority meta
    "query:consolidated-production-priority-stats",
    // Issue #520 — Consolidated Top 5 production roadmap synthesis
    "query:production-roadmap-stats",
    // Issue #514 — Task6 Top 3 production-readiness synthesis
    "query:ir-hygiene-stats",
    "query:pattern-marker-stats",
    "query:task6-production-readiness-stats",
    // Issue #441 — Consolidated compiler/runtime P0 synthesis
    "query:compiler-runtime-production-readiness-stats",
    // Issue #634 — Commercial production readiness P0 synthesis
    "query:commercial-production-readiness-stats",
    // Issue #635 — Macro+reflect+self-evo commercial closed-loop
    "query:macro-reflect-self-evo-commercial-stats",
    // Issue #636 — EDSL workspace query/mutate commercial closed-loop
    "query:edsl-query-mutate-commercial-stats",
    // Issue #619 — Task6 macro+reflect+self-evo follow-up
    "query:macro-reflect-self-evo-followup-stats",
    // Issue #602 — Prompt6 memory-safety matrix
    "query:prompt6-violation-count",
    "query:prompt6-safety-score",
    // Issue #570 — ShapeProfiler stability + deopt
    "query:shape-stability-stats",
    // Issue #492 — ShapeProfiler structured deopt/stability hash
    "query:shape-profiler-stats",
    // Issue #493 — EDSL hot-path bottleneck breakdown hash
    "query:hotpath-bottleneck-stats",
    // Issue #494 — Pass pipeline yield + dirty short-circuit hash
    "query:pass-pipeline-stats",
    // Issue #496 — Native SV NodeTag census + mutate counters
    "query:sv-node-stats",
    // Issue #571 — EvalValue v2 dispatch + contracts
    "query:value-dispatch-stats",
    // Issue #506 — IR SoA + dirty-aware Pass hotpath adoption
    "query:soa-hotpath-adoption-stats",
    // Issue #404 — IR SoA Phase 3 block_dirty incremental lowering
    "query:ir-soa-incremental-stats",
    // Issue #403 — IRInstruction rich metadata interpreter/JIT
    "query:ir-metadata-stats",
    // Issue #607 — Task4 high-perf hot-path matrix
    "query:task4-hotpath-safety-score",
    "query:task4-cache-locality-win",
    "query:task4-mutation-stability",
    // Issue #552 — EDSL stability
    "query:edsl-stability-stats",
    // Issue #553 — Atomic batch + mutation log (int sum)
    "query:mutation-log-stats",
    // Issue #1362 — mutation log compaction observability hash
    "query:mutation-log-compact-stats",
    // Issue #529 — Atomic batch + Guard rollback closed loop
    "query:atomic-batch-rollback-stats",
    // Issue #527 — StableNodeRef cross-COW/fiber closed loop
    "query:stable-ref-cow-fiber-stats",
    // Issue #400 — sym_id/structural rollback coverage
    "query:mutation-rollback-coverage-stats",
    // Issue #554 — Pattern index timing (same name as #547; unified)
    // Issue #555 — Typed mutation Task1
    "query:typed-mutation-stats-task1",
    // Issue #556 — EDSL concurrency safety
    "query:edsl-concurrency-stats",
    // Issue #531 — Closure env safety
    "query:closure-env-safety-stats",
    // Issue #610 — Linear ownership post-mutate validation
    "query:linear-ownership-mutation-stats",
    // Issue #638 — Linear + GuardShape runtime safety post-mutate
    "query:linear-ownership-safety-stats",
    // Issue #800 — linear post-mutate fidelity stats
    "query:linear-postmutate-fidelity-stats",
    // Issue #598 — Runtime linear enforcement + invalidate hook
    "query:linear-ownership-runtime-stats",
    // Issue #575 — Task2 PerDefUse incremental linear ownership
    "query:linear-ownership-incremental-stats",
    // Pre-existing (Issue #288, #391, #447, #457, #459)
    "query:query-stats",
    "query:stale-ref-stats",
    // Issue #489 — StableNodeRef enforcement in mutate/query hot paths
    "query:stability-stats",
    "query:atomic-batch-stats",
    "query:stable-ref-stats",
    // Issue #470 — StableNodeRef 4-field hash
    "query:stable-ref-stats-hash",
    // Issue #497 — Long-session StableRef lifecycle hash
    "query:stable-ref-lifecycle-stats",
    // Issue #498 — AI-native primitive metadata + skeleton ergonomics
    "query:primitive-metadata",
    // Issue #499 — EDA foundation primitives module observability
    "query:eda-foundation-stats",
    // Issue #841 — EDA production infrastructure stats
    "query:eda-infra-stats",
    // Issue #500 — Work-stealing + MutationBoundary depth observability
    "query:work-steal-stats",
    "query:fiber-migration-stats",
    "query:mutation-coordination-stats",
    "query:envframe-stale-stats",
    "query:envframe-bump-stats",
    "query:dirty-subtree",
    "query:epoch-stats",
    "query:macro-introduced",
    "query:by-marker",
    // Compile: stats (Issue #560 enumeration source of truth)
    "compile:compiler-cache-stats",
    "compile:compiler-incremental-stats",
    "compile:typecheck-stats",
    "compile:jit-stats",
    // Issue #491 — JIT opcode coverage + hot-swap safety hash
    "query:jit-stats-hash",
    "compile:arena-stats",
    "compile:dead-coercion-stats",
    // Issue #574 — coercion elimination summary
    "query:coercion-elim-stats",
    // Issue #468 — DeadCoercionEliminationPass zero-overhead
    "query:dead-coercion-zerooverhead-stats",
    "compile:per-defuse-index-stats",
    "compile:mutator-dispatch-stats",
    "compile:mutation-impact-stats",
    "compile:inline-pass-stats",
    "compile:type-cache-stats",
    "compile:dirty-impact-stats",
    // Primitive error (Issue #478)
    "query:primitive-error-stats",
    // Issue #583 — Registry + core primitives hot-path stats
    "query:primitives-stats",
    // Issue #480 — Self-describing primitive metadata closed loop
    "query:primitive-meta-stats",
    // Issue #405 — Arena auto-compaction orchestration signals
    "query:arena-compaction-stats",
    // Issue #406 — Pass Pipeline + Contracts hot-path stats
    "query:pass-contracts-stats",
    // Issue #407 — ShapeProfiler burst/deopt storm observability
    "query:shape-deopt-burst-stats",
    // Issue #408 — EDSL dirty propagation cost observability
    "query:dirty-propagation-cost-stats",
    // Issue #471 — SV-scale dirty propagation
    "query:dirty-propagation-stats",
    // Issue #414 — Long-term generation_/epoch management
    "query:generation-epoch-stats",
    // Issue #416 — AST SoA column compaction observability
    "query:ast-column-compaction-stats",
    // Issue #417 — MutationBoundary cross-TU invariant stats
    "query:mutation-boundary-invariant-stats",
    // Issue #418 — EnvFrame dual-path + stale policy stats
    "query:envframe-dualpath-stale-stats",
    // Issue #419 — Modular defuse_version + AOT dispatch stats
    "query:defuse-version-stats",
    // Issue #420 — MacroIntroduced end-to-end hygiene contract
    "query:macro-hygiene-contract-stats",
    // Issue #421 — query:pattern recursive MacroIntroduced filter
    "query:pattern-macro-filter-stats",
    // Issue #422 — Mutate-path hygiene violation detection
    "query:hygiene-violation-stats",
    // Issue #423 — query:pattern structural pre-index
    "query:pattern-structural-index-stats",
    // Issue #424 — StableNodeRef WorkspaceTree COW safety
    "query:stable-ref-workspace-tree-stats",
    // Issue #428 — closure bridge + bridge_epoch drift
    "query:closure-stats",
    // Issue #739 — atomic epoch visibility under fiber steal
    "query:closure-epoch-concurrency-stats",
    // Issue #429 — IR SoA live dirty state
    "query:soa-dirty-stats",
    // Issue #430 — arena compaction hash variant
    "query:arena-compaction-stats-hash",
    // Issue #431 — C++26 Contracts/Concepts/consteval density
    "query:cxx26-invariants",
    // Issue #440 — consolidated Task 1 EDSL readiness
    "query:edsl-readiness",
    // Issue #444 — strategy evolution controller pheromone stats
    "query:strategy-evolution-stats",
    // Issue #445 — SEVA audit log (OpenClaw integration)
    "query:seva-audit-log",
    // Issue #446 — SEVA demo with metrics
    "seva:run-demo-with-metrics",
    // Issue #450 (sub-issue #441) — primitive perf stats
    "query:primitive-perf-stats",
    // Issue #452 — AOT hot-update + region filtering
    "query:aot-stats",
    // Issue #1516 — per-function AOT + EH coverage production stats
    "compile:aot-stats",
    // Issue #1517 — SoAView concept enforcement + EDSL migration
    "query:soa-view-enforcement-stats",
    // Issue #462 — ShapeAwareFoldingPass
    "query:shape-folding-stats",
    // Issue #463 — SoA Phase 2 adoption
    "query:soa-adoption-stats",
    // Issue #675 — CI reproducibility + sanitizer gates
    "query:ci-reproducibility-stats",
    // Issue #676 — sandbox capability + mutation audit
    "query:security-stats",
    "query:mutation-audit-log",
    // Issue #464 — Arena auto-compaction lifecycle
    "query:arena-auto-stats",
    // Issue #677 — deployment / health / metrics export
    "query:deployment-stats",
    // Issue #465 — C++26 hot-path contracts + consteval
    "query:cxx26-hotpath-invariants",
    // Issue #507 — Task4 hot-path Contracts + consteval sites
    "query:task4-hotpath-contracts",
    // Issue #678 — PCV span lifetime safety in query layer
    "query:span-lifetime-stats",
    // Issue #679 — nested Guard + atomic-batch rollback alignment
    "query:nested-guard-atomic-stats",
    // Issue #680 — Define mutate IR/JIT/bridge invalidation
    "query:define-mutate-ir-invalidation-stats",
    // Issue #681 — compiler IRClosure/bridge epoch enforcement
    "query:compiler-closure-inval-stats",
    // Issue #682 — compiler IRClosure/EnvId GC root coordination
    "query:compiler-gc-root-stats",
    // Issue #683 — linear ownership + GC safepoint / steal integration
    "query:linear-ownership-gc-stats",
    // Issue #684 — IRSoA full wiring incremental stats
    "query:irsoa-incremental-stats",
    // Issue #685 — arena auto-compact policy + defrag/shape synergy
    "query:arena-auto-compact-stats",
    // Issue #686 — ShapeProfiler ring + Value dispatch + Pass dirty wiring
    "query:shape-value-pass-stats",
    // Issue #688 — Linear OwnershipEnv post-mutate typed-mutation
    "query:linear-ownership-typed-mutate-stats",
    // Issue #689 — Occurrence typing deep predicate + provenance
    "query:occurrence-typing-mutate-stats",
    // Issue #690 — Constraint typed-mutation reverify + blame
    "query:constraint-typed-mutate-stats",
    "query:constraint-delta-blame-stats",
    // Issue #691 — CoercionMap + NarrowingRecord provenance
    "query:coercion-narrowing-stats",
    // Issue #692 — ADT exhaustiveness + pattern provenance typed-mutation
    "query:adt-exhaustiveness-typed-mutate-stats",
    // Issue #693 — Hardware backend SV commercial closed-loop
    "query:hardware-backend-sv-closedloop-stats",
    // Issue #694 — SVA structured AST tags + mutate stats
    "query:sv-sva-structure-stats",
    // Issue #695 — EDA-SV verification closed-loop stress
    "query:eda-sv-closedloop-stress-stats",
    // Issue #510 — EDA verification interop + feedback stats
    "query:eda-verification-stats",
    // Issue #511 — Workspace snapshot + checkpoint persistence stats
    "query:workspace-snapshot-stats",
    // Issue #512 — Runtime orchestration production-readiness stats
    "query:runtime-orchestration-stats",
    // Issue #513 — AOT hot-reload consolidated observability
    "query:aot-hot-reload-stats",
    // Issue #522 — AOT production hot-reload deployment tracker
    "query:aot-production-reload-stats",
    // Issue #523 — EnvFrame dual-path production safety tracker
    "query:envframe-production-safety-stats",
    // Issue #524 — Macro+hygiene production closed-loop tracker
    "query:macro-production-hygiene-stats",
    // Issue #525 — Guard post-mutate impact + reflect validation tracker
    "query:guard-production-impact-stats",
    // Issue #528 — Pattern index + hygiene production tracker
    "query:pattern-production-index-stats",
    // Issue #530 — Incremental re-lower + ir_cache/JIT production tracker
    "query:incremental-production-relower-stats",
    // Issue #532 — JIT opcode coverage + IR consistency + hot-swap safety
    "query:jit-consistency-stats",
    // Issue #533 — children_ columnar + IR SoA hot-path production tracker
    "query:soa-production-columnar-stats",
    // Issue #534 — Arena auto-compaction + defrag safepoint coordination
    "query:arena-production-compaction-stats",
    // Issue #535 — C++26 Contracts + consteval hot-path production tracker
    "query:contracts-production-hotpath-stats",
    // Issue #539 — SV verification feedback → structured mutate closed loop
    "query:sv-production-verification-stats",
    // Issue #540 — StableNodeRef + generation/mutation_log EDA stability
    "query:eda-stability-stats",
    // Issue #541 — query:pattern + DefUseIndex + hygiene SV verification
    "query:pattern-sv-verification-stats",
    // Issue #557 — Top 5 commercial test-coverage cluster tracker
    "query:top5-commercial-coverage-stats",
    // Issue #567 — Primitive governance + stdlib layering closing metrics
    "query:primitives-governance-stats",
    // Issue #568 — FlatAST children_ columnar SoA migration completion
    "query:soa-children-columnar-migration-stats",
    // Issue #1520 — children_ SoA + region dense lookup stats
    "query:children-column-stats",
    // Issue #1521 — ShapeProfiler + Arena compact synergy
    "query:shape-arena-compact-stats",
    // Issue #569 — Arena auto-compact + defrag + fiber safepoint completion
    "query:arena-auto-compact-defrag-stats",
    // Issue #572 — Pass Pipeline DirtyAware + fold short-circuit completion
    "query:pass-pipeline-dirtyaware-stats",
    // Issue #578 — Structured SV IR + query/mutate + dirty propagation completion
    "query:sv-structured-edsl-stats",
    // Issue #661 — SV InterfaceIR + ModportIR structure observability
    "query:sv-interface-structure-stats",
    // Issue #579 — Verification feedback → structured mutate closed-loop
    "query:verification-feedback-loop-stats",
    // Issue #580 — Hardware backend emit maturity + commercial interop
    "query:hardware-backend-stats",
    // Issue #581 — StableNodeRef + dirty propagation SV SoC scalability
    "query:stable-ref-sv-scale-stats",
    // Issue #582 — EDA SV concurrency + atomic batch + fiber safety
    "query:eda-concurrency-stats",
    // Issue #583 — Registry + core primitives hot-path observability hash
    "query:primitives-registry-core-stats",
    // Issue #584 — Primitives hot-path AI-agent stress observability
    "query:primitives-hotpath-stats",
    // Issue #585 — Unified primitive error handling + recovery observability
    "query:primitives-error-stats",
    // Issue #586 — EDA/infrastructure primitives registry extension observability
    "query:eda-primitives-stats",
    // Issue #587 — AI-native primitives development support observability
    "query:primitives-ai-native-stats",
    // Issue #591 — Scheduler steal/GC safepoint mutation coordination observability
    "query:scheduler-mutation-coord-stats",
    // Issue #515 — Consolidated Top 5 P0 production-readiness tracker
    "query:consolidated-p0-production-stats",
    // Issue #516 — Prompt6 memory/ownership/GC safety tracker
    "query:prompt6-memory-safety-stats",
    // Issue #519 — EDSL/EDA/SV verification closed-loop tracker
    "query:edsl-eda-sv-closedloop-stats",
    // Issue #521 — Multi-fiber orchestration + MutationBoundary safety
    "query:multi-fiber-orchestration-stats",
    // Issue #697 — Declarative primitives extension kit
    "query:primitives-extension-stats",
    // Issue #709 — Registry fast dispatch + capture discipline
    "query:primitives-registry-stats",
    // Issue #710 — verify_tool/diagnostic Guard + StableRef wiring
    "query:verify-tool-guard-stats",
    // Issue #698 — Hardware backend commercial interop
    "query:hardware-backend-commercial-stats",
    // Issue #663 — Hardware backend SV-specific observability
    // (verbatim-name view of the issue body's Action #4)
    "query:hardware-backend-sv-stats",
    // Issue #664 — SV DefUse incremental observability (P1)
    "query:sv-defuse-stats",
    // Issue #665 — SV stability observability (P1)
    "query:sv-stability-stats",
    // Issue #667 — list/map/filter apply hot-path
    // observability (P1 stdlib-impl)
    "query:primitives-apply-stats",
    // Issue #668 — math regex primitive error observability
    // (P1 stdlib-impl error consistency)
    "query:primitives-regex-error-stats",
    // Issue #669 — primitives meta introspection stats
    // (P1 stdlib-impl AI-native introspection)
    "query:primitives-meta-stats",
    // Issue #671 — primitives capture consistency stats
    // (P1 stdlib-impl consistency)
    "query:primitives-consistency-stats",
    // Issue #751 — PRIM_ERROR / capture contract enforcement stats
    "query:primitives-contract-stats",
    // Issue #804 — unified primitive error semantics + recovery
    // observability (P0 stdlib-Registry reliability foundation;
    // refines/consolidates #585 + #751 + #775 + #478; non-
    // duplicative with #585 query:primitives-error-stats
    // coarse hash + #478 query:primitive-error-stats pair
    // primitive + #751 query:primitives-contract-stats
    // contract enforcement + #775 query:extension-kit-stats
    // capture contract validation + #806 registry-extension
    // primitives). #804 is the FIRST observability surface
    // that tracks the *unified-error-path SLO composite* —
    // 100% primitives use unified path + zero silent fallback
    // errors under load — as a single deployment-grade SLO
    // composite the Agent reads to decide whether the
    // stdlib error semantics are production-ready for
    // commercial AI Agent use.
    "query:primitive-error-unified-stats",
    // Issue #752 — list/vector map/filter SoA hot-path stats
    "query:list-soa-hotpath-stats",
    // Issue #753 — long-running deployment infra stats
    "query:longrunning-infra-stats",
    // Issue #1583 / #1207 — recovery latency stall budget + p50/p99 SLO
    "query:longrunning-recovery-stats",
    // Issue #1585 — MultiFiberMailbox production stats
    "query:mf-mailbox-stats",
    // Issue #1586 — parallel_orch / parallel_intend stats
    "query:parallel-orch-stats",
    // Issue #1588 — unified src/orch module stats
    "query:orch-module-stats",
    // Issue #1589 — TypedMutationAudit production trail
    "query:typed-mutation-audit-trail",
    // Issue #754 — LLM-bottleneck orchestration adaptive stats
    "query:orchestration-llm-bottleneck-stats",
    // Issue #755 — concurrent safety full-cycle integration stats
    "query:concurrent-safety-full-cycle-stats",
    // Issue #672 — linear ownership + GuardShape runtime
    // invariant enforcement observability (P0 production
    // safety)
    "query:linear-ownership-enforcement-stats",
    // Issue #740 — linear JIT L2 post-invalidate safety
    "query:linear-jit-safety-stats",
    // Issue #687 — DeadCoercionEliminationPass + IR-interpreter
    // identity fast-path dashboard (P0 zero-overhead gradual typing)
    "query:dead-coercion-elim-stats",
    // Issue #799 — narrow_evidence-driven dead-coercion elision stats
    "query:dead-coercion-elision-stats",
    // Issue #706 — Scheduler StealBudget adaptive bias
    "query:scheduler-stealbudget-adaptive-stats",
    // Issue #652 / #707 — Per-fiber stack/checkpoint pool
    "query:per-fiber-stack-pool-stats",
    // Issue #1483 — Per-fiber mutation stack depth observability
    // (sister to per-fiber-stack-pool-stats above; reads the
    // per_fiber_mutation_stack_depth_max + _current_max atomics
    // wired at evaluator_fiber_mutation.cpp:316 + :454 sites).
    "query:per-fiber-mutation-stack-stats",
    // Issue #1483 — Adaptive GC safepoint threshold observability
    // (sister to per-fiber-mutation-stack-stats above; reads the
    // safepoint_adaptive_threshold + _defer_count atomics wired at
    // request_gc_safepoint() evaluator.ixx:4191 area via the C4
    // exponential-backoff heuristic (a)).
    "query:gc-safepoint-adaptive-stats",
    // Issue #708 — AOT hot-reload refcount + checkpoint version
    "query:aot-reload-stats",
    "query:aot-checkpoint-version-stats",
    // Issue #712 — Subtree-level reflect validation for MacroIntroduced nodes
    "query:macro-reflect-validation-stats",
    // Issue #713 — JIT/AOT/Interpreter macro-hygiene violation counters
    "query:macro-jit-hygiene-stats",
    // Issue #728 — Unified structured error + provenance + recovery
    // observability (non-duplicative with #478 / #585 coarse error
    // stats; #728 tracks per-decision-point unified-model signals).
    "query:unified-error-stats",
    // Issue #731 — Arena + SoA + EnvFrame concurrent compaction
    // safety observability (non-duplicative with #722 / #743 / #604
    // coarse arena stats; #731 tracks per-decision-point concurrent
    // safety signals).
    "query:arena-concurrent-compact-stats",
    // Issue #732 — AOT hot-reload safe-swap at MutationBoundary
    // observability (non-duplicative with #708 / #644 / #590 coarse
    // AOT stats; #732 tracks per-decision-point safe-swap signals).
    "query:aot-safe-swap-boundary-stats",
    // Issue #733 — Macro SyntaxMarker propagation + IR/JIT hygiene
    // enforcement observability (non-duplicative with #714 / #455 /
    // #373 coarse macro stats; #733 tracks per-decision-point
    // marker propagation + IR/JIT enforcement signals).
    "query:ir-marker-hygiene-stats",
    // Issue #735 — MacroIntroduced provenance in StableNodeRef +
    // targeted dirty/rollback for macro subtrees observability
    // (non-duplicative with #714 / #717 / #392 / #373 / #733 / #750
    // coarse macro/Guard/IR/reflect stats; #735 tracks per-decision-
    // point macro provenance + targeted macro-subtree handling
    // signals).
    "query:macro-provenance-stats",
    // Issue #756 — EnvFrame dual-path consistency enforcement +
    // desync panic policy + GCEnvWalkFn stale handling observability
    // (non-duplicative with #647 / #418 coarse dual-path stats;
    // #756 tracks per-decision-point desync-panic + GCEnvWalkFn
    // stale under concurrent steal/mutate signals).
    "query:envframe-dualpath-policy-stats",
    // Issue #757 — Fine-grained MacroIntroduced provenance tracking
    // + dynamic inliner policy + AI-queryable hygiene violation
    // correlation observability (non-duplicative with #654 / #458 /
    // #373 / #750 coarse macro stats; #757 tracks per-decision-
    // point fine-grained provenance + dynamic inliner policy +
    // per-macro correlation signals).
    "query:macro-hygiene-provenance-stats",
    // Issue #758 — Runtime auto_validate bridge for user-defined
    // EDSL structs (DEFINE_STRUCT / custom nodes) under
    // MutationBoundaryGuard with macro hygiene invariant
    // correlation observability (non-duplicative with #750
    // general macro schema stats; #758 tracks per-decision-point
    // EDSL struct validate + macro hygiene invariant correlation
    // signals).
    "query:edsl-reflection-stats",
    // Issue #759 — Unified 'code-as-data' closed-loop maturity
    // observability (composite health score for the integrated
    // macro + reflect + EDSL self-evo loop; production readiness
    // dashboard for Task6: marker propagation fidelity, Guard
    // rollback hygiene safety, reflection schema coverage on
    // macro/EDSL subtrees, concurrent fiber stress success).
    "query:code-as-data-maturity-stats",
    // Issue #760 — query:pattern performance + hygiene fidelity
    // observability (linear scans vs index hits, wildcard cost,
    // deep hygiene predicate activity for large macro-heavy
    // concurrent AI pattern-mutate loops; non-duplicative with
    // #757 / #758 / #759 coarse observability).
    "query:pattern-performance-stats",
    // Issue #761 — End-to-end atomic batch mutate + suppressed
    // generation bump + cross-fiber safety observability for
    // reliable multi-step AI iterative edits; non-duplicative
    // with #755 concurrent Guard, #749 StableRef COW, #737
    // atomic batch proposal — but #761 is the FIRST observability
    // surface that tracks the batch lifecycle + suppressed bump
    // count + cross-fiber steals during suppressed batch +
    // hygiene violations caught within batch boundary as
    // per-decision-point counters.
    "query:mutate-batch-stats",
    // Issue #762 — Workspace '锁定-导航-修改-执行' closed-loop
    // orchestration observability under concurrent fiber +
    // multi-Agent parallel edits (concurrent query/mutate,
    // cross-COW StableRef validity, yield points, shared_mutex
    // contention); non-duplicative with #749 StableRef COW, #755
    // Guard concurrent stress, #754 LLM-bottleneck adaptive
    // scheduling.
    "query:workspace-closedloop-orchestration-stats",
    // Issue #763 — Runtime linear_ownership_state enforcement +
    // GC root registration for IRClosure/EnvFrame in invalidate_
    // function and live-closure paths (linear ownership + GC +
    // compiler maturation observability; non-duplicative with
    // the existing query:linear-ownership-gc-stats GC layer and
    // #747/#741/#756/#749 — #763 is the FIRST observability
    // surface that tracks the compiler IRClosure + EnvFrame +
    // invalidate runtime linear enforcement composite).
    "query:linear-ownership-gc-compiler-stats",
    // Issue #764 — Arena AST / shared_ptr<FlatAST> lifetime safety
    // vs GC-managed Env/Closure in closure_bridge_ under
    // incremental re-lower + mutation (non-duplicative with #741
    // DepGraph/bridge/Env version, #731 Arena concurrent compact,
    // #749 StableRef COW, #756 EnvFrame dual — #764 is the FIRST
    // observability surface that tracks the compiler Arena AST /
    // shared_ptr<FlatAST> lifetime vs GC-managed Env/Closure in
    // closure_bridge_ composite).
    "query:compiler-arena-closure-lifetime-stats",
    // Issue #765 — Full DepEntry quote/lambda tracking + impact_
    // scope propagation to bridge_epoch bump, EnvFrame version
    // re-stamp and linear state refresh in LoweringState/
    // invalidate (refine/extend #741, non-duplicative — #765 is
    // the FIRST observability surface that tracks the incremental
    // compilation safety for quote/lambda/closure-heavy defines
    // composite specifically: DepEntry quote/lambda hit, bridge_
    // epoch bump on impact, EnvFrame version refresh, linear state
    // refreshed).
    "query:incremental-quote-lambda-linear-stats",
    // Issue #766 — IR-SoA migration observability + DirtyAware
    // incremental pipeline (P0 high-perf C++26 DOD/SoA
    // foundation; refines #167/#463/#741; non-duplicative with
    // #729 soa-hotpath-stats and #765 incremental-quote-lambda-
    // linear-stats). #766 is the FIRST observability surface
    // that tracks the *production migration of IRModuleV2 +
    // DirtyAware incremental pipeline* — IR SoA column counts /
    // DirtyAware short-circuit hits / pmr column utilization /
    // JIT SoA codegen time — as separate per-decision-point
    // counters the Agent consumes to monitor the SoA migration +
    // DirtyAware short-circuit production-readiness under
    // incremental AI mutation flows.
    "query:ir-soa-migration-stats",
    // Issue #767 — Arena Auto-Compact Policy + Live Defrag + Fiber/GC
    // Safepoint Yield observability (P0 high-perf C++26 Arena
    // foundation; completes #300 P1 + #685 + #731; non-duplicative
    // with #685 query:arena-auto-compact-stats and #642 query:arena-
    // auto-compaction-stats). #767 is the FIRST observability surface
    // that tracks the *production auto-compact policy + live defrag
    // + fiber yield during compact + defrag blocked fibers* — 2
    // truly new counters beyond what #685/#642 cover — as separate
    // per-decision-point counters the Agent consumes to decide whether
    // to tune the threshold, force defrag, or trust the auto-compact
    // policy under sustained AI mutation load.
    "query:arena-auto-compact-defrag-fiber-stats",
    // Issue #768 — Shape + Pass + Contracts hot-path observability
    // (P0 high-perf C++26 Contracts/Concepts adoption foundation;
    // builds on #507 hot-path Contracts; non-duplicative with
    // #570 query:shape-stability-stats, #492 query:shape-profiler-
    // stats, #494 query:pass-pipeline-stats, #571 query:evalvalue-
    // v2-dispatch-stats, #744 shape_jit_pass_closedloop_stats).
    // #768 is the FIRST observability surface that tracks the
    // *production hot-path Contracts coverage + ShapeProfiler
    // epoch sync with JIT/Pass Pipeline + stronger Concept
    // constraints for Dirty/JITFriendly composition* —
    // contract_checks_hotpath (zero-overhead debug catches in
    // SoA/dirty/shape dispatch), shape_stability_transitions
    // (proxy for "how often the dominant shape flipped"),
    // jit_epoch_sync_hits (ShapeProfiler version bumped in sync
    // with mutation_epoch_ + JIT epoch hint), deopt_targeted_
    // skips (DirtyAware or impact_scope targeted invalidation
    // saved a full recompile), concept_violations_caught
    // (static_assert in pipeline templates fired) — as separate
    // per-decision-point counters the Agent consumes to monitor
    // the speculative opt + debug layer production-readiness
    // under AI mutation churn.
    "query:shape-pass-hotpath-stats",
    // Issue #772 — SV Verification closed-loop SLO observability
    // (P0 EDA production standard foundation; consolidates/refines
    // #693/#724/#725/#726/#748; non-duplicative with #748 query:
    // sv-verification-structure-stats, #801 query:sv-commercial-
    // emit-fidelity-stats, #802 query:sv-verification-self-
    // evolution-stats). #772 is the FIRST observability surface
    // that tracks the *production SLO status of the SV verification
    // closed-loop* — fidelity rate (>99% threshold) + re-emit
    // latency max (the bound threshold) + breach counter + a
    // computed slo-status field the Agent can read to decide
    // whether the closed-loop is production-ready for commercial
    // VCS/Questa/JasperGold emit acceptance.
    "query:sv-closedloop-slo",
    // Issue #773 — Workspace closed-loop fiber/multi-agent EDA
    // verification orchestration observability (P0 high-perf
    // C++26 concurrent Workspace foundation; refines/consolidates
    // #762/#749/#755/#760; non-duplicative with #762 query:
    // workspace-closedloop-orchestration-stats). #773 is the
    // FIRST observability surface that tracks the *production
    // Workspace closed-loop orchestration under fiber + multi-Agent
    // EDA verification loops* — pct-derived concurrent_query_mutate
    // / cross_cow_ref_validity + ns-based shared_mutex_contention
    // + multi_agent_edit_fidelity + stale_ref_prevented_in_eda_loops
    // — as separate per-decision-point counters the Agent consumes
    // to decide whether to tune Workspace locking, force COW
    // propagation, or trust the fiber/Guard orchestration under
    // sustained AI concurrent multi-Agent verification load.
    "query:workspace-closedloop-fiber-eda-stats",
    // Issue #774 — Verification feedback-driven closed-loop
    // self-evolution convergence rate (derived pct from
    // #802 convergence-hits / closed-loop-rounds × 10000)
    // + closed-loop-rounds + convergence-hits + feedback-
    // mutate-rounds composite. Non-duplicative with #726
    // (closed-loop-reliability-stats, ref-drift/rollback/
    // feedback-mutate-rounds) and #802 (sv-verification-
    // self-evolution-stats, feedback-parse/structured-
    // mutate/closed-loop-rounds/convergence-hits). #774
    // adds the deployment-grade convergence_rate pct the
    // body asks for as a parallel companion surface.
    "query:closed-loop-convergence-stats",
    // Issue #775 — Formal Primitives Extension Kit for AI
    // Agent safe generation, registration, contract
    // enforcement + auto-meta + test template observability
    // (P0 stdlib AI-native surface). Non-duplicative with
    // #697 (primitives-extension-stats, runtime counters
    // with 8 fields including eda-meta-backfilled +
    // category-sva + category-verification + category-eda
    // + documented-with-schema + extension-kit-version +
    // registry-slots + skeleton-generations), #751
    // (primitives-contract-stats, capture-violations +
    // prim-error-hits + style-compliance-pct + capture-
    // contract-version), #669 (primitives-meta-stats,
    // meta-hits + documented-count + schema-documented +
    // total-registered). #775 is the FIRST observability
    // surface that aggregates the Agent-facing extension
    // kit SLO composite (extensions_registered + contract_
    // violations_caught + meta_completeness_pct + test_
    // skeletons_generated) as a single deployment-grade
    // dashboard.
    "query:extension-kit-stats",
    // Issue #806 — Registry-Extension surface for AI Agent
    // safe stdlib extension via registry (P0 stdlib
    // AI-native extension observability foundation;
    // refines/consolidates #775 Extension Kit + #711 +
    // #480; non-duplicative with #775 query:extension-kit-
    // stats and #633 query:stdlib-compiler-demands-stats).
    // #806 is the FIRST observability surface that tracks
    // the *registry-integration pass counter (extensions /
    // validation_pass / meta_completeness) + slo-validation
    // pct + extend-registry-safe-active* — the registry
    // integration phase of the AI Native stdlib extension
    // story — as a single deployment-grade dashboard the
    // Agent reads to decide whether the
    // `(primitive:extend-registry-safe ...)` auto-validation
    // pipeline is production-ready.
    "query:registry-extension-stats",
    // Issue #776 — Integrated Primitives Hot-Path
    // Benchmark Suite + Mutation/Fiber-Load Regression
    // Gate with Quantitative SLOs observability
    // dashboard. Non-duplicative with #614/#584
    // (primitives-hotpath-stats, 11 fields including
    // primitive-call-total + pair-alloc-total +
    // linear-traverse-total + cdr-depth-max + call-rate +
    // alloc-per-call + regex-time-us + stability-score +
    // hotpath-schema + primitives-hotpath-total +
    // primitives-hotpath-recommendation), #751
    // (primitives-contract-stats, 4 contract counters).
    // #776 is the FIRST observability surface that
    // aggregates the primitives hot-path SLO composite
    // (current-vs-baseline-pct + contract-violations +
    // fastpath-hit-rate-pct + regression-flag) as a
    // single deployment-grade SLO dashboard the Agent
    // reads to decide whether the stdlib hot-path is
    // production-ready under AI Agent mutation + fiber
    // load.
    "query:primitives-hotpath-slo-stats",
    // Issue #777 — Consolidated EDA Infrastructure
    // Primitives Production Readiness Roadmap +
    // Milestone Tracker with Measurable Fidelity/SLO
    // Gates observability. Non-duplicative with #726
    // /#748/#772/#774/#749/#738/#725/#724 individual
    // EDA primitives — #777 is the FIRST observability
    // surface that aggregates the EDA production
    // readiness composite (4 milestone completeness
    // pcts derived from primitive lookup + fixed
    // blocking-issues-count + recommendation) as a
    // single deployment-grade production-readiness
    // dashboard the Agent reads to decide whether the
    // EDA stdlib is production-ready for commercial
    // verification self-evolution.
    "query:eda-production-readiness",
    // Issue #778 — FFI call overhead observability
    // for batch terminal output + rendering engine
    // hot-path (P1 perf surface). Non-duplicative with
    // #131 (FFI primitive extraction) and #699 (ffi-
    // calls-stats). #778 is the FIRST observability
    // surface that tracks the FFI call volume at the
    // primitive-call layer (c-load + c-func + c-opaque
    // + c-alloc + c-struct-set! + c-struct-ref all
    // increment coverage_counters_[8]) + exposes the
    // production-readiness signals for the deferred
    // batch FFI primitive + (terminal-batch-write)
    // work the body asks for.
    "query:ffi-call-overhead-stats",
    // Issue #779 — Dirty region / delta rendering
    // observability for terminal rendering engine
    // (P2 perf surface). Non-duplicative with the
    // existing vector primitives in
    // evaluator_primitives_vector.cpp. #779 is the
    // FIRST observability surface that exposes the
    // production-readiness signals for the deferred
    // dirty region + present-delta work the body asks
    // for.
    "query:dirty-region-rendering-stats",
    // Issue #780 — JIT / hot-update coverage
    // observability for rendering hot paths
    // (present/draw) (P2 perf surface). Non-duplicative
    // with the existing (query:jit-stats) #427 +
    // (query:jit-consistency-stats) +
    // (query:jit-interpreter-parity-stats) #720 +
    // (query:jit-typed-mutation-stats) #746. #780 is
    // the FIRST observability surface that tracks the
    // JIT coverage for rendering hot paths + exposes
    // the production-readiness signals for the deferred
    // rendering-path JIT + hot-update rendering
    // optimization work the body asks for.
    "query:jit-rendering-coverage-stats",
    // Issue #781 — High-performance byte buffer +
    // zero-copy primitives observability for
    // framebuffer management (P2 perf surface).
    // Non-duplicative with the existing memory
    // primitives in evaluator_primitives_memory.cpp
    // + vector primitives in evaluator_primitives
    // _vector.cpp. #781 is the FIRST observability
    // surface that tracks the pair allocation
    // pressure that the body identifies as wasted
    // on per-frame buffer construction + exposes the
    // production-readiness signals for the deferred
    // zero-copy byte-buffer + ANSI sequence helper
    // + memory profiling work the body asks for.
    "query:zero-copy-framebuffer-stats",
    // Issue #782 — Dedicated terminal rendering
    // primitives module + profiling integration
    // observability (P2 infrastructure surface).
    // Non-duplicative with the existing vector +
    // memory + I/O primitives in
    // evaluator_primitives_vector.cpp / _memory.cpp
    // / _io.cpp. #782 is the FIRST observability
    // surface that exposes the production-readiness
    // signals for the deferred evaluator_primitives
    // _terminal.cpp module + core rendering primitives
    // (clear, draw-batch, present, dirty tracking) +
    // shape_profiler integration + example terminal
    // renderer the body asks for. Uses live primitive
    // lookup pattern (mirror #777) to count how many
    // of the 4 expected core primitives are registered.
    "query:terminal-rendering-module-stats",

    // Issue #783: orchestration steal outermost stats.
    // Splits the coarse steal_deferred_mutation_boundary
    // _count_ metric into "outermost safe steal" +
    // "inner deferred" + "cross-fiber safe steal", plus
    // 3 hardcoded "not yet" flags for the deferred
    // Phase 2+ work (strict StableRef refresh on
    // resume + EnvFrame version refresh + #754 bias).
    "query:orchestration-steal-outermost-stats",

    // Issue #784: EnvFrame dual-path mandatory
    // enforcement observability (Non-duplicative
    // refinement of #756 envframe-dualpath-policy-stats +
    // #647 envframe-dualpath-stale-stats-hash + #731
    // envframe-dualpath-stats). Surfaces 3 NEW atomics
    // for mandatory ensure_ call sites + concurrent
    // steal resync + GC walk resync, plus 2 hardcoded
    // "not yet" flags for the Phase 2+ deferred work
    // (configurable policy mode + mandatory call site
    // wiring + panic-on-detected-desync enforcement).
    "query:envframe-dualpath-mandatory-enforce-stats",

    // Issue #785: AOT concurrent hot-update
    // observability. Surfaces 3 NEW atomics for the
    // concurrent steal + grace period + EnvFrame
    // version sync under multi-agent multi-fiber
    // concurrent reload. 3 hardcoded "not yet" flags
    // for Phase 2+ deferred work (region mask
    // enforcement + grace period implementation +
    // steal-defer coordination).
    "query:aot-concurrent-hotupdate-stats",

    // Issue #786: code-as-data production health
    // composite (consolidation of #759/#758/#757 +
    // #755/#773/#774 etc. non-duplicative refinement).
    // 0 NEW atomics — pure composite that uses live
    // primitive lookup to aggregate the 8 expected
    // sub-primitives + derives composite SLO status
    // (parallel companion pattern, mirror #777).
    "query:code-as-data-production-health",

    // Issue #787: end-to-end hygiene + schema +
    // linear ownership fidelity under fiber steal +
    // AOT hot-reload + Guard rollback chaos
    // (Consolidate #757/#758/#750/#755/#783/#785
    // non-duplicative). 0 NEW atomics — pure
    // consolidation composite (mirror #786) using
    // live primitive lookup + 4 hardcoded "not yet"
    // fidelity signals for Phase 2+ deferred work.
    "query:task6-concurrent-fidelity",

    // Issue #788: AI-Native extensibility
    // (define-struct / set-policy! / extend-kit)
    // observability consolidation. 0 NEW atomics —
    // consolidation composite (mirror #786/#787)
    // using live primitive lookup to aggregate 5
    // expected sub-primitives (#757/#758/#750/#775/
    // #751) + 4 hardcoded "not yet" AI-extension
    // fidelity signals (validation-pass-rate /
    // policy-tuning-success-rate /
    // define-struct-success-rate /
    // contract-compliance-rate).
    "query:ai-native-extension-stats",

    // Issue #789: query:pattern SafePCVSpan mandate
    // + tag_arity_index_ hot-path + deep :marker
    // provenance predicate enforcement observability
    // (Refine/Consolidate #760 non-duplicative). 2
    // NEW atomics + 5 hardcoded "not yet" flags for
    // Phase 2+ deferred work (SafePCVSpan mandate
    // wire-up + tag_arity_index_ fast-path population
    // + deep hygiene predicate support).
    "query:pattern-index-safe-span-stats",

    // Issue #790: mutate:atomic-batch + pinned
    // StableNodeRef snapshot + per-boundary
    // observability + cross-fiber safety
    // (Refine/Consolidate #737/#761 non-duplicative).
    // 2 NEW Evaluator atomics + 2 NEW bump helpers +
    // 2 NEW accessors + 1 NEW primitive that
    // exposes the new fields + 4 hardcoded "not yet"
    // flags for Phase 2+ deferred work (atomic-batch
    // primitive exposure + snapshot capture + cross-
    // fiber re-stamp + mutation-impact batch flag).
    "query:mutate-batch-atomic-stats",

    // Issue #791: exhaustive fiber yield-point
    // instrumentation + automatic StableRef/dirty
    // cross-boundary propagation observability
    // (Refine/Consolidate #773/#762 non-duplicative).
    // 3 NEW CompilerMetrics atomics + 3 NEW bump
    // helpers on Evaluator + 1 NEW primitive that
    // exposes the new fields + 2 hardcoded "not yet"
    // flags for Phase 2+ deferred work (exhaustive
    // yield instrumentation + auto-propagation
    // active).
    "query:workspace-closedloop-fiber-multi-agent-yield-stats",

    // Issue #792: compiler invalidate_function +
    // mutation_epoch_ synchronization with outermost
    // MutationBoundaryGuard depth + live IRClosure /
    // EnvFrame / GuardShape version refresh under
    // concurrent fiber steal (Non-duplicative
    // refinement of #783/#755/#784/#787). 4 NEW
    // CompilerMetrics atomics + 4 NEW bump helpers
    // on Evaluator + 1 NEW primitive that exposes
    // the new fields + 2 hardcoded "not yet" flags
    // for Phase 2+ deferred work (safe invalidate at
    // outermost boundary + steal-resume version
    // refresh).
    "query:compiler-invalidate-guard-steal-stats",

    // Issue #793: JIT/AOT hot-swap + GuardShape +
    // linear + EnvFrame version_ consistency
    // observability (Non-duplicative consolidation/
    // refinement of #785/#787/#755). 4 NEW
    // CompilerMetrics atomics + 4 NEW bump helpers
    // on Evaluator + 1 NEW primitive that exposes
    // the new fields + 2 hardcoded "not yet" flags
    // for Phase 2+ deferred work (reload-deopt-
    // version-hooks-active + jit-emit-runtime-
    // version-checks-active).
    "query:jit-aot-hotswap-fidelity-stats",

    // Issue #794: full closed-loop compiler + EDSL
    // fidelity observability (Non-duplicative
    // consolidation/refinement of #786/#787/
    // #755/#792/#793). 4 NEW CompilerMetrics
    // atomics + 4 NEW bump helpers on Evaluator +
    // 1 NEW primitive that exposes the new fields
    // + 2 hardcoded "not yet" flags for Phase 2+
    // deferred work (full closed-loop harness
    // active + SLO gate active).
    "query:full-closedloop-compiler-edsl-fidelity-stats",

    // Issue #795: deep hot-path Contracts + stronger
    // SoAView/ShapeStablePass Concepts +
    // ShapeProfiler JIT Epoch Sync + Dirty
    // Propagation observability (Non-duplicative
    // refinement of #768/#507/#766/#767/#741).
    // 4 NEW CompilerMetrics atomics + 4 NEW bump
    // helpers on Evaluator + 1 NEW primitive that
    // exposes the new fields + 1 hardcoded "not
    // yet" flag for Phase 2+ deferred work
    // (concepts + epoch sync all deferred).
    "query:shape-pass-hotpath-contracts-stats",

    // Issue #796: end-to-end IR SoA full migration +
    // DirtyAware short-circuit + DepGraph
    // integration observability (Non-duplicative
    // extension of #766/#741). 4 NEW
    // CompilerMetrics atomics + 4 NEW bump helpers
    // on Evaluator + 1 NEW primitive that exposes
    // the new fields + 2 hardcoded "not yet" flags
    // for Phase 2+ deferred work (full SoA
    // migration active + clean-block-hit-rate +
    // pmr utilization).
    "query:ir-soa-full-migration-stats",
    // Issues #923–#940: stdlib production review
    "query:stdlib-production-review-stats",
    "query:primitive-tier-stats",
    // Issues #941–#967: self-evo + bugfix dashboards
    "query:self-evo-pipeline-stats",
    "query:bugfix-941-967-stats",
    // Issues #985–#1013: production hardening
    "query:production-hardening-985-1013-stats",
    // Issues #1014–#1046: production stability + bugfix
    "query:production-stability-1014-1046-stats",
    "query:serve-health",
    // Issues #1047–#1071: hygiene / type / mutate safety
    "query:production-safety-1047-1071-stats",
    // Issues #1072–#1096: security / metrics / concurrency
    "query:production-hardening-1072-1096-stats",
    // Issues #1097–#1122: serialize / fold / serve safety
    "query:production-safety-1097-1122-stats",
    // Issues #1123–#1140: final open-issue sweep
    "query:production-sweep-1123-1140-stats",
    // Issue #1450 / #1449 Phase 1: residual public *-stats still on
    // Primitives registry (not query:/compile: legacy-only). Catalogued
    // so (stats:list)/(stats:get)/(engine:metrics) can discover them.
    "arena:adaptive-stats",
    "arena:defrag-stats",
    "ast:generation-stats",
    "ast:node-lifecycle-stats",
    "ast:post-restore-stats",
    "closure:free-stats",
    "compile:bidirectional-stats",
    "gc-arena-stats",
    "gc-stats",
    "type-registry-stats",
};


// Issue #909: shared helper (was local lambda in register_eval; used across peels).
EvalValue ObservabilityPrims::meta_to_pair(Evaluator& ev, const PrimMeta& m) {
    // Issue #926: include perf_tier + security_level for Agent discovery.
    auto schema_idx = ev.string_heap_.size();
    ev.string_heap_.push_back(m.schema);
    auto cat_idx = ev.string_heap_.size();
    ev.string_heap_.push_back(m.category);
    auto doc_idx = ev.string_heap_.size();
    ev.string_heap_.push_back(m.doc);
    auto pid0 = ev.pairs_.size();
    ev.pairs_.push_back({make_string(cat_idx), make_string(schema_idx)});
    auto pid1 = ev.pairs_.size();
    ev.pairs_.push_back({make_string(doc_idx), make_pair(pid0)});
    auto pid2 = ev.pairs_.size();
    ev.pairs_.push_back({make_int(m.security_level), make_pair(pid1)});
    auto pid3 = ev.pairs_.size();
    ev.pairs_.push_back({make_int(m.perf_tier), make_pair(pid2)});
    auto pid4 = ev.pairs_.size();
    ev.pairs_.push_back({make_int(m.safety_flags), make_pair(pid3)});
    auto pid5 = ev.pairs_.size();
    ev.pairs_.push_back({make_bool(m.pure), make_pair(pid4)});
    auto pid6 = ev.pairs_.size();
    ev.pairs_.push_back({make_int(m.arity), make_pair(pid5)});
    return make_pair(pid6);
}

// ── Issue #909: thin observability registration dispatchers ──
const std::vector<std::string>& ObservabilityPrims::stats_primitives() {
    return kObservabilityStatsPrimitives;
}

// Issue #1439: private map for query:/compile:*-stats hash builders.
// Not exposed via Primitives / api-reference; only engine:metrics + stats:list catalog.
namespace {
    std::unordered_map<std::string, PrimFn>& legacy_stats_impls() {
        static std::unordered_map<std::string, PrimFn> m;
        return m;
    }
} // namespace

bool ObservabilityPrims::is_legacy_stats_name(std::string_view name) {
    // Multi-arg observability APIs that require node-id / name args must
    // stay public — (stats:get)/(engine:metrics) cannot forward those args.
    static constexpr std::string_view kMultiArgPublic[] = {
        "compile:per-symbol-dirty-stats",
        "compile:func-block-dirty-count",
        "compile:block-dirty-count",
        "dirty:reasons",     // (dirty:reasons node-id)
        "dirty:ppa-reasons", // (dirty:ppa-reasons node-id)
    };
    for (auto m : kMultiArgPublic) {
        if (name == m)
            return false;
    }

    // Issue #1439 / #1449 / #1450: zero-arity *-stats are facade-only
    // (register_stats_impl). Public add() surface must not grow dashboards.
    if (name.ends_with("-stats") || name.ends_with("-stats-hash"))
        return true;
    if (name.find("-stats-") != std::string_view::npos)
        return true;

    // Issue #1449 demotion batch: zero-arity query:* SLO / health / score
    // dashboards are agent-facing via (stats:get) / (engine:metrics), not
    // as public primitives. Keeps SlimSurface moving toward ≤420.
    if (name.starts_with("query:")) {
        const auto rest = name.substr(6);
        // Explicit high-traffic dashboard names (not covered by suffix rules).
        // Zero-arity dashboards only. Multi-arg discovery APIs
        // (primitives-meta, primitives-by-category, …) stay public
        // because (stats:get)/(engine:metrics) cannot pass name args.
        static constexpr std::string_view kExplicit[] = {
            "orchestration-metrics",
            "primitive-fastpath-per-prim",
            "primitive-metadata",
            "primitive-list-with-meta",
            "primitives-meta-catalog",
            "narrowings-at-mutation",
            "cxx26-invariants",
            "cxx26-hotpath-invariants",
            "prompt6-violation-count",
            "prompt6-safety-score",
            "task4-hotpath-safety-score",
            "task4-cache-locality-win",
            "task4-mutation-stability",
            "task4-hotpath-contracts",
            "task6-concurrent-fidelity",
            "seva-longrunning-concurrent-slo",
            "sv-closedloop-slo",
            "edsl-readiness",
            "eda-production-readiness",
            "code-as-data-production-health",
            "runtime-production-health",
            "production-health",
            "serve-health",
        };
        for (auto e : kExplicit) {
            if (rest == e)
                return true;
        }
        // Suffix patterns for future dashboards.
        if (rest.ends_with("-health") || rest.ends_with("-readiness") || rest.ends_with("-slo") ||
            rest.ends_with("-score") || rest.ends_with("-fidelity") ||
            rest.ends_with("-invariants") || rest.ends_with("-win") ||
            rest.ends_with("-contracts") || rest.ends_with("-stability") ||
            rest.ends_with("-snapshot") || rest.ends_with("-histogram") ||
            rest.ends_with("-effectiveness") || rest.ends_with("-available") ||
            rest.ends_with("-audit-log") || rest == "tag-arity-count" ||
            rest == "epoch-delta-since-last-query")
            return true;
    }
    // Issue #1449 batch 2: dirty:* and render-*-samples are observability.
    if (name.starts_with("dirty:"))
        return true;
    if (name.starts_with("render-") &&
        (name.ends_with("-samples") || name == "render-hotpath-depth" ||
         name.ends_with("-histogram")))
        return true;
    return false;
}

void ObservabilityPrims::register_stats_impl(std::string name, PrimFn fn) {
    legacy_stats_impls()[std::move(name)] = std::move(fn);
}

std::optional<PrimFn> ObservabilityPrims::lookup_stats_impl(std::string_view name) {
    auto& m = legacy_stats_impls();
    auto it = m.find(std::string(name));
    if (it == m.end())
        return std::nullopt;
    return it->second;
}

bool ObservabilityPrims::stats_impl_registered(std::string_view name) {
    return lookup_stats_impl(name).has_value();
}

void ObservabilityPrims::register_eval_all(PrimRegistrar add, Evaluator& ev) {
    register_eval_p0(add, ev);
    register_eval_p1(add, ev);
    register_eval_p2(add, ev);
    register_eval_p3(add, ev);
    register_eval_p4(add, ev);
    register_eval_p5(add, ev);
    register_eval_p6(add, ev);
    register_eval_p7(add, ev);
    register_eval_p8(add, ev);
    register_eval_p9(add, ev);
    register_eval_p10(add, ev);
    register_eval_p11(add, ev);
    register_eval_p12(add, ev);
    register_eval_p13(add, ev);
    register_eval_p14(add, ev);
    register_eval_p15(add, ev);
    register_eval_p16(add, ev);
    register_eval_p17(add, ev);
    register_eval_p18(add, ev);
    register_eval_p19(add, ev);
    register_eval_p20(add, ev);
    register_eval_p21(add, ev);
    register_eval_p22(add, ev);
    register_eval_p23(add, ev);
    register_eval_p24(add, ev);
    register_eval_p25(add, ev);
    register_eval_p26(add, ev);
    register_eval_p27(add, ev);
    register_eval_p28(add, ev);
    register_eval_p29(add, ev);
    register_eval_p30(add, ev);
    register_eval_p31(add, ev);
    register_eval_p32(add, ev);
    register_eval_p33(add, ev);
    register_eval_p34(add, ev);
    register_eval_p35(add, ev);
    register_eval_p36(add, ev);
    register_eval_p37(add, ev);
    register_eval_p38(add, ev);
    register_eval_p39(add, ev);
    register_eval_p40(add, ev);
    register_eval_p41(add, ev);
    register_eval_p42(add, ev);
    register_eval_p43(add, ev);
    register_eval_p44(add, ev);
    register_eval_p45(add, ev);
    register_eval_p46(add, ev);
    register_eval_p47(add, ev);
    register_eval_p48(add, ev);
    register_eval_p49(add, ev);
    register_eval_p50(add, ev);
    register_eval_p51(add, ev);
    register_eval_p52(add, ev);
    register_eval_p53(add, ev);
    register_eval_p54(add, ev);
    register_eval_p55(add, ev);
    register_eval_p56(add, ev);
    register_eval_p57(add, ev);
    register_eval_p58(add, ev);
    register_eval_p59(add, ev);
    register_eval_p60(add, ev);
    register_eval_p61(add, ev);
    register_eval_p62(add, ev);
    register_eval_p63(add, ev);
    register_eval_p64(add, ev);
    register_eval_p65(add, ev);
    register_eval_p66(add, ev);
    register_eval_p67(add, ev);
    register_eval_p68(add, ev);
    register_eval_p69(add, ev);
    register_eval_p70(add, ev);
    register_eval_p71(add, ev);
    register_eval_p72(add, ev);
    register_eval_p73(add, ev);
    register_eval_p74(add, ev);
    register_eval_p75(add, ev);
    register_eval_p76(add, ev);
    register_eval_p77(add, ev);
    register_eval_p78(add, ev);
    register_eval_p79(add, ev);
    register_eval_p80(add, ev);
    register_eval_p81(add, ev);
    register_eval_p82(add, ev);
    register_eval_p83(add, ev);
    register_eval_p84(add, ev);
    register_eval_p85(add, ev);
    register_eval_p86(add, ev);
    register_eval_p87(add, ev);
    register_eval_p88(add, ev);
    register_eval_p89(add, ev);
    register_eval_p90(add, ev);
    register_eval_p91(add, ev);
    register_eval_p92(add, ev);
    register_eval_p93(add, ev);
    register_eval_p94(add, ev);
    register_eval_p95(add, ev);
    register_eval_p96(add, ev);
    register_eval_p97(add, ev);
    register_eval_p98(add, ev);
    register_eval_p99(add, ev);
    register_eval_p100(add, ev);
    register_eval_p101(add, ev);
    register_eval_p102(add, ev);
    register_eval_p103(add, ev);
    register_eval_p104(add, ev);
}

void ObservabilityPrims::register_jit_all(PrimRegistrar add, Evaluator& ev) {
    register_jit_p0(add, ev);
    register_jit_p1(add, ev);
    register_jit_p2(add, ev);
    register_jit_p3(add, ev);
    register_jit_p4(add, ev);
    register_jit_p5(add, ev);
    register_jit_p6(add, ev);
    register_jit_p7(add, ev);
    register_jit_p8(add, ev);
    register_jit_p9(add, ev);
    register_jit_p10(add, ev);
    register_jit_p11(add, ev);
    register_jit_p12(add, ev);
    register_jit_p13(add, ev);
    register_jit_p14(add, ev);
    register_jit_p15(add, ev);
    register_jit_p16(add, ev);
    register_jit_p17(add, ev);
    register_jit_p18(add, ev);
    register_jit_p19(add, ev);
    register_jit_p20(add, ev);
    register_jit_p21(add, ev);
    register_jit_p22(add, ev);
    register_jit_p23(add, ev);
    register_jit_p24(add, ev);
    register_jit_p25(add, ev);
    register_jit_p26(add, ev);
    register_jit_p27(add, ev);
    register_jit_p28(add, ev);
    register_jit_p29(add, ev);
    register_jit_p30(add, ev);
    register_jit_p31(add, ev);
    register_jit_p32(add, ev);
    register_jit_p33(add, ev);
    register_jit_p34(add, ev);
    register_jit_p35(add, ev);
    register_jit_p36(add, ev);
    register_jit_p37(add, ev);
    register_jit_p38(add, ev);
    register_jit_p39(add, ev);
    register_jit_p40(add, ev);
    register_jit_p41(add, ev);
    register_jit_p42(add, ev);
    register_jit_p43(add, ev);
    register_jit_p44(add, ev);
    register_jit_p45(add, ev);
    register_jit_p46(add, ev);
    register_jit_p47(add, ev);
    register_jit_p48(add, ev);
    register_jit_p49(add, ev);
    register_jit_p50(add, ev);
    register_jit_p51(add, ev);
    register_jit_p52(add, ev);
    register_jit_p53(add, ev);
    register_jit_p54(add, ev);
    register_jit_p55(add, ev);
    register_jit_p56(add, ev);
    register_jit_p57(add, ev);
    register_jit_p58(add, ev);
    register_jit_p59(add, ev);
    register_jit_p60(add, ev);
    register_jit_p61(add, ev);
    register_jit_p62(add, ev);
    register_jit_p63(add, ev);
    register_jit_p64(add, ev);
    register_jit_p65(add, ev);
    register_jit_p66(add, ev);
    register_jit_p67(add, ev);
    register_jit_p68(add, ev);
    register_jit_p69(add, ev);
    register_jit_p70(add, ev);
    register_jit_p71(add, ev);
    register_jit_p72(add, ev);
    register_jit_p73(add, ev);
    register_jit_p74(add, ev);
    register_jit_p75(add, ev);
    register_jit_p76(add, ev);
    register_jit_p77(add, ev);
    register_jit_p78(add, ev);
    register_jit_p79(add, ev);
    register_jit_p80(add, ev);
    register_jit_p81(add, ev);
    register_jit_p82(add, ev);
    register_jit_p83(add, ev);
    register_jit_p84(add, ev);
    register_jit_p85(add, ev);
    register_jit_p86(add, ev);
    register_jit_p87(add, ev);
    register_jit_p88(add, ev);
    register_jit_p89(add, ev);
    register_jit_p90(add, ev);
    register_jit_p91(add, ev);
    register_jit_p92(add, ev);
    register_jit_p93(add, ev);
    register_jit_p94(add, ev);
    register_jit_p95(add, ev);
    register_jit_p96(add, ev);
    register_jit_p97(add, ev);
    register_jit_p98(add, ev);
    register_jit_p99(add, ev);
    register_jit_p100(add, ev);
    register_jit_p101(add, ev);
    register_jit_p102(add, ev);
    register_jit_p103(add, ev);
    register_jit_p104(add, ev);
    register_jit_p105(add, ev);
    register_jit_p106(add, ev);
    register_jit_p107(add, ev);
    register_jit_p108(add, ev);
    register_jit_p109(add, ev);
    register_jit_p110(add, ev);
    register_jit_p111(add, ev);
    register_jit_p112(add, ev);
    register_jit_p113(add, ev);
    // Issue #1434: mark top-20 stats as deprecated after full registration.
    ObservabilityPrims::mark_p1b_top_stats_deprecated(ev);
    // Issue #1450: residual public *-stats aliases (arena/gc/ast/…).
    ObservabilityPrims::mark_residual_public_stats_deprecated(ev);
}

// Issue #1434 / P1b: top-20 query:*-stats → prefer (engine:metrics "…").
// Names pinned by scripts/find_top_stats.py (usage rank). Still registered
// for back-compat; PrimMeta.deprecated surfaces them under api-reference
// *deprecated* section.
void ObservabilityPrims::mark_p1b_top_stats_deprecated(Evaluator& ev) {
    static constexpr const char* kTop20[] = {
        "query:envframe-dualpath-stats",
        "query:self-evolution-closedloop-stats",
        "query:macro-reflect-validation-stats",
        "query:macro-jit-hygiene-stats",
        "query:pattern-index-stats",
        "query:stable-ref-layer-stats",
        "query:arena-auto-compact-defrag-fiber-stats",
        "query:pattern-stats",
        "query:typed-mutation-stats",
        "query:fiber-boundary-violation-stats",
        "query:value-dispatch-stats",
        "query:incremental-relower-stats",
        "query:edsl-reflection-stats",
        "query:panic-checkpoint-lifecycle-stats",
        "query:jit-interpreter-parity-stats",
        "query:closure-env-epoch-safety-stats",
        "query:arena-integration-stats",
        "query:pattern-hygiene-stats",
        "query:macro-provenance-stats",
        "query:envframe-dualpath-policy-stats",
    };
    for (const char* name : kTop20) {
        const auto slot = ev.primitives_.slot_for_name(name);
        if (slot >= ev.primitives_.slot_count())
            continue;
        PrimMeta meta = ev.primitives_.meta_for_slot(slot);
        meta.deprecated = true;
        meta.category = "deprecated";
        const std::string hint =
            std::string("DEPRECATED (#1434): prefer (engine:metrics \"") + name + "\")";
        if (meta.doc.empty())
            meta.doc = hint;
        else if (meta.doc.find("DEPRECATED") == std::string::npos)
            meta.doc = hint + ". " + meta.doc;
        ev.primitives_.set_meta_for_name(name, std::move(meta));
    }
}

// Issue #1450 / #1449 Phase 1: residual public *-stats that stayed on
// Primitives (arena/ast/gc/type-registry/…). Mark deprecated so dispatch
// telemetry counts debt; bodies still run for test/compat.
void ObservabilityPrims::mark_residual_public_stats_deprecated(Evaluator& ev) {
    static constexpr const char* kResidual[] = {
        "arena:adaptive-stats",        "arena:defrag-stats",     "ast:generation-stats",
        "ast:node-lifecycle-stats",    "ast:post-restore-stats", "closure:free-stats",
        "compile:bidirectional-stats", "gc-arena-stats",         "gc-stats",
        "type-registry-stats",
    };
    static_assert(sizeof(kResidual) / sizeof(kResidual[0]) < 50,
                  "AC2: residual public stats aliases must stay <50");
    for (const char* name : kResidual) {
        const auto slot = ev.primitives_.slot_for_name(name);
        if (slot >= ev.primitives_.slot_count())
            continue;
        PrimMeta meta = ev.primitives_.meta_for_slot(slot);
        meta.deprecated = true;
        meta.category = "deprecated";
        const std::string hint = std::string("DEPRECATED (#1450): prefer (stats:get \"") + name +
                                 "\") or (engine:metrics \"" + name + "\")";
        if (meta.doc.empty())
            meta.doc = hint;
        else if (meta.doc.find("DEPRECATED") == std::string::npos)
            meta.doc = hint + ". " + meta.doc;
        ev.primitives_.set_meta_for_name(name, std::move(meta));
    }
}

void register_eval_observability_primitives(PrimRegistrar add, Evaluator& ev) {
    // P2b: bulk eval-side stats only in full mode (s0 skips).
    if (full_primitives_enabled())
        ObservabilityPrims::register_eval_all(add, ev);
}

void register_jit_arena_primitives(PrimRegistrar add, Evaluator& ev) {
    // P2b: full JIT/obs stats in full mode; s0 only metrics facade.
    if (full_primitives_enabled()) {
        ObservabilityPrims::register_jit_all(add, ev);
    } else {
        ObservabilityPrims::register_metrics_facade(add, ev);
        // s0: top-20 names usually unregistered — mark is a no-op for missing.
        ObservabilityPrims::mark_p1b_top_stats_deprecated(ev);
        ObservabilityPrims::mark_residual_public_stats_deprecated(ev);
    }
}

} // namespace aura::compiler::primitives_detail
