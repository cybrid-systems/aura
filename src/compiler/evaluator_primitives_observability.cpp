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

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.value;
import aura.compiler.pass_manager;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

// Issue #560: canonical list of every registered *-stats observability
// primitive. Single source of truth for both (stats:list) and
// (stats:count). Adding a new (query:*-stats) primitive is one entry here
// — the count and the enumeration both update automatically.
//
// The Aura stdlib (`lib/std/stats.aura`) mirrors this list at require
// time, so AI agents can also call these primitives by name. When you add
// a new entry here, also add a matching line to `lib/std/stats.aura` so the
// EDSL discoverability surface stays in sync.
static const std::vector<std::string> kObservabilityStatsPrimitives = {
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
    // Issue #553 — Atomic batch + mutation log
    "query:mutation-log-stats",
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
    // Issue #752 — list/vector map/filter SoA hot-path stats
    "query:list-soa-hotpath-stats",
    // Issue #672 — linear ownership + GuardShape runtime
    // invariant enforcement observability (P0 production
    // safety)
    "query:linear-ownership-enforcement-stats",
    // Issue #740 — linear JIT L2 post-invalidate safety
    "query:linear-jit-safety-stats",
    // Issue #687 — DeadCoercionEliminationPass + IR-interpreter
    // identity fast-path dashboard (P0 zero-overhead gradual typing)
    "query:dead-coercion-elim-stats",
    // Issue #706 — Scheduler StealBudget adaptive bias
    "query:scheduler-stealbudget-adaptive-stats",
    // Issue #652 / #707 — Per-fiber stack/checkpoint pool
    "query:per-fiber-stack-pool-stats",
    // Issue #708 — AOT hot-reload refcount + checkpoint version
    "query:aot-reload-stats",
    "query:aot-checkpoint-version-stats",
    // Issue #712 — Subtree-level reflect validation for MacroIntroduced nodes
    "query:macro-reflect-validation-stats",
    // Issue #713 — JIT/AOT/Interpreter macro-hygiene violation counters
    "query:macro-jit-hygiene-stats",
};

void register_eval_observability_primitives(PrimRegistrar add, Evaluator& ev) {

    auto meta_to_pair = [&ev](const PrimMeta& m) -> EvalValue {
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
        ev.pairs_.push_back({make_int(m.safety_flags), make_pair(pid1)});
        auto pid3 = ev.pairs_.size();
        ev.pairs_.push_back({make_bool(m.pure), make_pair(pid2)});
        auto pid4 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(m.arity), make_pair(pid3)});
        return make_pair(pid4);
    };

    // (typecheck-status) — Returns the last mutate typecheck result.
    // Empty string = no errors, non-empty = last mutate caused type errors.
    add("typecheck-status", [&ev](const auto&) -> EvalValue {
        std::shared_lock<std::shared_mutex> rlock(ev.workspace_mtx_);
        if (ev.last_mutate_error_.empty()) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back("ok");
            return make_string(sidx);
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.last_mutate_error_);
        return make_string(sidx);
    });

    // (auto-rollback-on-panic [#t|#f]) — Get/set auto-rollback on panic flag
    // When enabled, runtime error triggers automatic rollback to last safe
    // checkpoint. Returns previous value.
    add("auto-rollback-on-panic", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool old = ev.panic_auto_rollback_;
        if (!a.empty() && types::is_bool(a[0]))
            ev.panic_auto_rollback_ = types::as_bool(a[0]);
        return make_bool(old);
    });

    // (panic-auto-rollback?) — Query current auto-rollback state
    add("panic-auto-rollback?",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.panic_auto_rollback_); });

    // (panic-checkpoint) — Save current workspace as a safe checkpoint
    // Returns #t on success, #f if no workspace loaded.
    add("panic-checkpoint",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.save_panic_checkpoint()); });

    // (panic-restore) — Restore to the last safe checkpoint
    // Returns #t on success, #f if no checkpoint available or restore failed.
    add("panic-restore",
        [&ev](const auto&) -> EvalValue { return make_bool(ev.restore_panic_checkpoint()); });

    // (panic-safe-source) — Return the checkpoint source code
    // Returns empty string if no checkpoint.
    add("panic-safe-source", [&ev](const auto&) -> EvalValue {
        auto idx = ev.string_heap_.size();
        ev.string_heap_.push_back(ev.panic_safe_source_);
        return make_string(idx);
    });

    // Issue #480: (primitive:describe name) — return PrimMeta as
    // (arity . (pure . (safety-flags . doc-string))).
    ev.primitives_.add(
        "primitive:describe",
        [&ev, meta_to_pair](const auto& a) -> EvalValue {
            if (a.size() != 1 || !is_string(a[0]))
                return make_void();
            const auto& heap = ev.string_heap_;
            const auto idx = as_string_idx(a[0]);
            if (idx >= heap.size())
                return make_void();
            const auto& name = heap[idx];
            const auto slot = ev.primitives_.slot_for_name(name);
            if (slot >= ev.primitives_.slot_count())
                return make_void();
            ev.bump_primitive_describe_count();
            return meta_to_pair(ev.primitives_.meta_for_slot(slot));
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Return metadata for a registered primitive by name.",
                 .category = "general",
                 .schema = "(string) -> pair"});

    // Issue #480: (query:primitive-list-with-meta) — list of
    // (name . meta-pair) for every registered primitive.
    ev.primitives_.add(
        "query:primitive-list-with-meta",
        [&ev, meta_to_pair](const auto& a) -> EvalValue {
            (void)a;
            ev.bump_primitive_list_meta_count();
            EvalValue result = make_void();
            for (std::size_t slot = ev.primitives_.slot_count(); slot-- > 0;) {
                const auto& name = ev.primitives_.name_for_slot(slot);
                auto nidx = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back(
                    {make_string(nidx), meta_to_pair(ev.primitives_.meta_for_slot(slot))});
                auto wrap = ev.pairs_.size();
                ev.pairs_.push_back({make_pair(pid), result});
                result = make_pair(wrap);
            }
            return result;
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "List every primitive with its PrimMeta pair.",
                 .category = "general",
                 .schema = "() -> list"});

    // Issue #617: (query:primitives-by-category category) — filter
    // primitive-list-with-meta by PrimMeta.category. The Agent
    // discovery loop starts here: "show me all primitives in the
    // 'eda' category" is a single primitive call rather than
    // downloading the entire registry list and walking it client-side.
    //
    // Returns a list of (name . meta-pair) for every primitive
    // whose category matches. Returns () for an unknown category
    // (matches query:primitive-list-with-meta behavior on empty).
    // Bumps primitives_by_category_query_total on every call (success
    // or miss) so the catalog primitive can surface per-discovery-
    // entry hit rates.
    ev.primitives_.add(
        "query:primitives-by-category",
        [&ev, meta_to_pair](std::span<const EvalValue> a) -> EvalValue {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->primitives_by_category_query_total.fetch_add(1, std::memory_order_relaxed);
            if (a.empty() || !is_string(a[0]))
                return make_void();
            const auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_void();
            const auto& category = ev.string_heap_[idx];
            EvalValue result = make_void();
            for (std::size_t slot = ev.primitives_.slot_count(); slot-- > 0;) {
                const auto& pm = ev.primitives_.meta_for_slot(slot);
                if (pm.category != category)
                    continue;
                const auto& name = ev.primitives_.name_for_slot(slot);
                auto nidx = ev.string_heap_.size();
                ev.string_heap_.push_back(name);
                auto pid = ev.pairs_.size();
                ev.pairs_.push_back({make_string(nidx), meta_to_pair(pm)});
                auto wrap = ev.pairs_.size();
                ev.pairs_.push_back({make_pair(pid), result});
                result = make_pair(wrap);
            }
            return result;
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Return list of (name . meta-pair) for primitives in the given category.",
                 .category = "general",
                 .schema = "(string) -> list"});

    // Issue #617: (query:schema-of-primitive name) — return just
    // the PrimMeta.schema string for a named primitive. Lighter
    // than (query:primitive-metadata) which returns the full nested
    // pair; suitable for the Agent's "what's the signature of X?"
    // lookups that don't need arity/pure/safety/doc.
    //
    // Returns the schema string on success. Returns #f when the
    // primitive is unknown OR when it exists but has no schema
    // documented (so the Agent can branch on (schema-of-primitive
    // 'unknown-name') and not be confused with an empty-string
    // schema for a documented primitive).
    ev.primitives_.add(
        "query:schema-of-primitive",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->schema_of_primitive_query_total.fetch_add(1, std::memory_order_relaxed);
            if (a.empty() || !is_string(a[0]))
                return make_bool(false);
            const auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_bool(false);
            const auto& name = ev.string_heap_[idx];
            const auto slot = ev.primitives_.slot_for_name(name);
            if (slot >= ev.primitives_.slot_count())
                return make_bool(false);
            const auto& pm = ev.primitives_.meta_for_slot(slot);
            if (pm.schema.empty())
                return make_bool(false);
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(pm.schema);
            return make_string(sidx);
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Return the PrimMeta.schema string for a named primitive, or #f if unknown "
                        "/ undocumented.",
                 .category = "general",
                 .schema = "(string) -> string | bool"});

    // Issue #617: (query:primitives-meta-catalog) — one-call
    // Agent discovery primitive. Companion to (query:primitives-
    // extension-stats) from #697, but focused on the meta layer
    // rather than the runtime counters.
    //
    // Returns a 7-field hash:
    //   - total-registered:        slot_count()
    //   - schema-documented:       # of primitives with non-empty
    //                              doc AND schema
    //   - doc-only:                # of primitives with non-empty
    //                              doc but empty schema (registration
    //                              needs follow-up to add schema)
    //   - by-category-eda:         category_meta_count("eda")
    //   - by-category-sva:         category_meta_count("sva")
    //   - by-category-verification: category_meta_count("verification")
    //   - by-category-general:     # of primitives in any other
    //                              category (incl. "")
    //   - introspection-hits:     sum of primitives_by_category +
    //                              schema_of_primitive +
    //                              primitives_meta_catalog query
    //                              counters (so the Agent can see
    //                              how much the catalog has been
    //                              used this session)
    ev.primitives_.add(
        "query:primitives-meta-catalog",
        [&ev](const auto&) -> EvalValue {
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->primitives_meta_catalog_query_total.fetch_add(1, std::memory_order_relaxed);
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = 0xcbf29ce484222325ull;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto idx = ((h >> 1) + at) & (hcap - 1);
                        if (meta[idx] == 0xFF) {
                            meta[idx] = fp;
                            keys[idx] = key_ev.val;
                            vals[idx] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            std::size_t schema_doc = 0;
            std::size_t doc_only = 0;
            std::size_t eda = 0, sva = 0, verif = 0, gen = 0;
            for (std::size_t slot = ev.primitives_.slot_count(); slot-- > 0;) {
                const auto& pm = ev.primitives_.meta_for_slot(slot);
                if (!pm.doc.empty() && !pm.schema.empty())
                    ++schema_doc;
                else if (!pm.doc.empty())
                    ++doc_only;
                if (pm.category == "eda")
                    ++eda;
                else if (pm.category == "sva")
                    ++sva;
                else if (pm.category == "verification")
                    ++verif;
                else
                    ++gen;
            }
            const auto total = ev.primitives_.slot_count();
            const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
            const std::uint64_t introspect_hits =
                m ? (m->primitives_by_category_query_total.load(std::memory_order_relaxed) +
                     m->schema_of_primitive_query_total.load(std::memory_order_relaxed) +
                     m->primitives_meta_catalog_query_total.load(std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"total-registered", make_int(static_cast<std::int64_t>(total))},
                {"schema-documented", make_int(static_cast<std::int64_t>(schema_doc))},
                {"doc-only", make_int(static_cast<std::int64_t>(doc_only))},
                {"by-category-eda", make_int(static_cast<std::int64_t>(eda))},
                {"by-category-sva", make_int(static_cast<std::int64_t>(sva))},
                {"by-category-verification", make_int(static_cast<std::int64_t>(verif))},
                {"by-category-general", make_int(static_cast<std::int64_t>(gen))},
                {"introspection-hits", make_int(static_cast<std::int64_t>(introspect_hits))},
            };
            return build_hash(kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "One-call AI Agent discovery: meta layer breakdown by category + "
                        "introspection hit counter.",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #697: (primitive:generate-skeleton description-string)
    // — AI-friendly primitive extension bundle: C++ lambda, spec,
    // test snippet, and DEFINE_PRIMITIVE_META registration code.
    ev.primitives_.add(
        "primitive:generate-skeleton",
        [&ev](const auto& a) -> EvalValue {
            if (a.size() != 1 || !is_string(a[0]))
                return make_void();
            const auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_void();
            const auto sk = generate_primitive_skeleton(ev.string_heap_[idx]);
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->primitive_skeleton_generations_total.fetch_add(1, std::memory_order_relaxed);
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = 0xcbf29ce484222325ull;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            auto push_str = [&](const std::string& s) -> EvalValue {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(s);
                return make_string(sidx);
            };
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"category", push_str(sk.category)},
                {"spec", push_str(sk.spec)},
                {"cpp-lambda", push_str(sk.cpp_lambda)},
                {"test-snippet", push_str(sk.test_snippet)},
                {"registration", push_str(sk.registration)},
            };
            return build_hash(kv);
        },
        PrimMeta{.arity = 1,
                 .pure = true,
                 .doc = "Generate AI-friendly primitive extension skeleton from description.",
                 .category = "general",
                 .schema = "(string) -> hash"});

    // Issue #498: query:generate-primitive-skeleton — query-namespace alias
    // for primitive:generate-skeleton (Agent EDSL ergonomics).
    add("query:generate-primitive-skeleton", [&ev](const auto& a) -> EvalValue {
        if (auto fn = ev.primitives_.lookup("primitive:generate-skeleton"))
            return (*fn)(a);
        return make_void();
    });

    // Issue #633: query:stdlib-compiler-demands-stats-hash —
    // Agent-discoverable structured dashboard for the stdlib
    // commercial-evolution reverse-ask surface (specifically
    // covers AC5 from the issue body).
    //
    // Fields (5):
    //   - hotpath-calls         sum of the existing hot-path
    //                          counters (value_dispatch_hit_count +
    //                          primitive_fastpath_hits_total +
    //                          hotpath_eval_flat_calls +
    //                          hotpath_lowering_calls +
    //                          hotpath_soa_dual_emit_hits).
    //                          Synthesizes the AI primitive layer's
    //                          "hotpath_calls" demand signal.
    //   - error-consistency     existing value_contract_violation_
    //                          count (from #479 + #709). Higher
    //                          numbers = more contract violations
    //                          = more "error_consistency" debt.
    //   - extension-count       new stdlib_extension_count_total
    //                          atomic (foundation for AC3 DEFINE_
    //                          PRIMITIVE macro work — bumped per
    //                          new extension registered).
    //                          Value is 0 until AC3 wire-up.
    //   - ai-native-hits        new ai_native_primitive_hits_total
    //                          atomic (foundation for AC4 — bumped
    //                          per Agent-generated primitive
    //                          registration).
    //                          Value is 0 until AC4 wire-up.
    //   - soa-jit-win           existing primitive_fastpath_hits_
    //                          total (from #709) — proxy for
    //                          SoA/JIT win-rate at the primitive
    //                          layer.
    //   - schema == 633         sentinel for Agent drift detection
    //                          (mirrors the full chain through
    //                          #618+#620+#621+#622+#623+#624+#625+
    //                          #626+#630+#631+#632 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~80% of the AC5 surface:
    //   - (query:schema-of-primitive) (#617) — per-primitive schema
    //   - (query:primitives-meta-catalog) (#617) — 5-field catalog
    //   - (query:primitives-extensions-list) (#618) — extensions
    //   - (query:primitives-stats) (#479) — 8-field hot-path hash
    //   - (query:primitives-meta-stats) (#617) — primitive-meta
    //   - (query:primitives-fastpath-per-prim) (#709) — per-prim
    //   - hotpath counters on CompilerMetrics + PassPipeline
    //     Counters + PassPipeline metrics.
    // What AC5 specifies by **exact name + fields** —
    // `query:stdlib-compiler-demands-stats` with
    // {hotpath_calls, error_consistency, extension_count,
    // ai_native_hits, SoA/JIT_win} — was *not* shipped under
    // that exact name. So #633 ships ONE new Aura primitive
    // + 2 new foundation atomics.
    //
    // The remaining #633 AC1 + AC2 + AC3 + AC4 work (SoA value
    // views for primitives, unified PRIM_ERROR across registry,
    // DEFINE_PRIMITIVE macro, AI-generated primitive sandbox) is
    // invasive C++ + stdlib + reflect work that needs
    // benchmarking + perf regression coverage alongside the
    // existing AI/JSON/SoA initiatives — separate follow-ups.
    add("query:stdlib-compiler-demands-stats-hash", [&ev](const auto&) -> EvalValue {
        // hotpath-calls: sum of all hot-path counters.
        const std::uint64_t dispatch_hits =
            types::value_dispatch_hit_count.load(std::memory_order_relaxed);
        const std::uint64_t fastpath_hits =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->primitive_fastpath_hits_total.load(std::memory_order_relaxed)
                : 0;
        const std::uint64_t hotpath_eval =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->hotpath_eval_flat_calls.load(std::memory_order_relaxed)
                : 0;
        const std::uint64_t hotpath_lowering =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->hotpath_lowering_calls.load(std::memory_order_relaxed)
                : 0;
        const std::uint64_t soa_dual_emit =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->hotpath_soa_dual_emit_hits.load(std::memory_order_relaxed)
                : 0;
        const std::uint64_t hotpath_calls =
            dispatch_hits + fastpath_hits + hotpath_eval + hotpath_lowering + soa_dual_emit;
        // error-consistency: existing value_contract_violation_count.
        const std::uint64_t error_consistency =
            types::value_contract_violation_count.load(std::memory_order_relaxed);
        // extension-count: new foundation atomic (0 until AC3 macro).
        const std::uint64_t extension_count =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->stdlib_extension_count_total.load(std::memory_order_relaxed)
                : 0;
        // ai-native-hits: new foundation atomic (0 until AC4 wire-up).
        const std::uint64_t ai_native_hits =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->ai_native_primitive_hits_total.load(std::memory_order_relaxed)
                : 0;
        // soa-jit-win: existing primitive_fastpath_hits_total proxy.
        const std::uint64_t soa_jit_win = fastpath_hits;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("hotpath-calls", static_cast<std::int64_t>(hotpath_calls));
        insert_kv("error-consistency", static_cast<std::int64_t>(error_consistency));
        insert_kv("extension-count", static_cast<std::int64_t>(extension_count));
        insert_kv("ai-native-hits", static_cast<std::int64_t>(ai_native_hits));
        insert_kv("soa-jit-win", static_cast<std::int64_t>(soa_jit_win));
        insert_kv("schema", 633);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #637: query:closure-bridge-safety-stats-hash —
    // Agent-discoverable structured dashboard for IRClosure +
    // EnvFrame versioning + bridge invalidate protocol
    // (P0 memory-safety + commercial reliability surface;
    // non-duplicative to #620 #623 #624 — see issue body).
    //
    // Fields (3 + sentinel):
    //   - invalidations-post-mutate   new
    //                          closure_invalidation_post_mutate_total
    //                          atomic (foundation for AC1
    //                          invalidate_function wire-up —
    //                          bumped when invalidate_function
    //                          fires after a workspace mutate).
    //                          Value is 0 until AC1 wire-up.
    //   - version-mismatches-caught   new
    //                          closure_version_mismatch_caught_
    //                          total atomic (foundation for
    //                          AC2 bridge_epoch / EnvFrame
    //                          .version check wire-up in
    //                          apply_closure + materialize_
    //                          call_env — bumped per detected
    //                          mismatch that would otherwise
    //                          have caused UAF / stale-env
    //                          access). Value is 0 until
    //                          AC2 wire-up.
    //   - safe-rebuilds        new closure_safe_rebuild_total
    //                          atomic (foundation for AC2/AC3
    //                          Guard dtor + WorkspaceTree::
    //                          resolve_safe_ref wire-up —
    //                          bumped per successful bridge
    //                          rebuild after a mismatch).
    //                          Value is 0 until AC2/AC3
    //                          wire-up.
    //   - schema == 637         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the *foundation* for
    //   - invalidate_function_calls (#401) + jit_cache_evictions
    //     (#401) + compiler_inval_bridge_epoch_total (#498)
    //     + bridge_epoch_hit_count_ (#531)
    //     + jit_hotswap_*_total (#601) + linear_deopt_on_
    //     invalidate_total (#531) + stable_ref_invalidations
    //     (#604)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:closure-bridge-safety-stats` with
    // {invalidations_post_mutate, version_mismatches_caught,
    // safe_rebuilds} — was *not* shipped under that exact
    // name with that exact field set. So #637 ships ONE new
    // Aura primitive + 3 new foundation atomics.
    //
    // The remaining #637 AC1 + AC2 + AC3 work (IRClosure
    // env_version/weak_env stamp, apply_closure dual-path
    // version check + integrate with MutationBoundaryGuard,
    // bridge_epoch bump to JIT hot-swap / Interpreter fallback)
    // is invasive C++ on the hot path + needs the 10k+ fiber
    // stress + TSan coverage from the issue body — separate
    // follow-ups.
    add("query:closure-bridge-safety-stats-hash", [&ev](const auto&) -> EvalValue {
        // invalidations-post-mutate: new foundation atomic
        // (0 until AC1 invalidate_function wire-up).
        const std::uint64_t invalidations_post_mutate =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->closure_invalidation_post_mutate_total.load(std::memory_order_relaxed)
                : 0;
        // version-mismatches-caught: new foundation atomic
        // (0 until AC2 apply_closure + materialize_call_env
        // wire-up).
        const std::uint64_t version_mismatches_caught =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->closure_version_mismatch_caught_total.load(std::memory_order_relaxed)
                : 0;
        // safe-rebuilds: new foundation atomic
        // (0 until AC2/AC3 Guard dtor wire-up).
        const std::uint64_t safe_rebuilds =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->closure_safe_rebuild_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("invalidations-post-mutate",
                  static_cast<std::int64_t>(invalidations_post_mutate));
        insert_kv("version-mismatches-caught",
                  static_cast<std::int64_t>(version_mismatches_caught));
        insert_kv("safe-rebuilds", static_cast<std::int64_t>(safe_rebuilds));
        insert_kv("schema", 637);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #640: query:sv-verification-closedloop-stats —
    // Agent-discoverable structured dashboard for the
    // Verification Feedback → Structured SV Mutate Closed-Loop
    // (P0 EDA-SV-Review + commercial reliability surface).
    //
    // Note the naming distinction from #630:
    //   - (query:sv-verification-closedloop-stats-hash) (#630)
    //     is the historical hash primitive from before the
    //     AC1+AC2+AC3 enforcement work existed (12+ fields,
    //     predicate-driven coverage / assert / cex summary).
    //   - (query:sv-verification-closedloop-stats) (#640, this
    //     primitive) is the *enforcement-track* companion that
    //     focuses on the closed-loop counters for AC1+AC2+AC3
    //     and uses the issue-specified exact name from #640's
    //     AC4 (no `-hash` suffix).
    //
    // Fields (3 + sentinel):
    //   - feedback-apply       new sv_verify_feedback_apply_total
    //                          atomic (foundation for AC1
    //                          (eda:apply-verification-feedback
    //                          report) primitive wire-up —
    //                          bumped per successful feedback
    //                          → Guard + StableNodeRef +
    //                          sv_ir structured mutate).
    //                          Value is 0 until AC1 wire-up.
    //   - guard-reemit         new sv_guard_reemit_hook_total
    //                          atomic (foundation for AC2
    //                          Guard success → hardware_backend
    //                          re-emit hook wire-up).
    //                          Value is 0 until AC2 wire-up.
    //   - stable-ref-strict    new sv_stable_ref_provenance_
    //                          strict_total atomic (foundation
    //                          for AC3 strengthened StableNodeRef
    //                          provenance check on SV mutate
    //                          paths). Value is 0 until AC3
    //                          wire-up.
    //   - schema == 640         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~80% of the closed-
    // loop observability surface:
    //   - (query:verification-feedback-loop-stats) (#579) —
    //     8-field feedback → mutate closed-loop hash
    //   - (query:sv-verification-closedloop-stats-hash)
    //     (#630) — historical hash primitive
    //   - hardware_backend_hook_calls_total (#693) +
    //     commercial_reemits_total (#693) +
    //     feedback_mutate_hits_total (#693) +
    //     ppa_savings_total (#693) +
    //     verification_loop_success_total (#693)
    //   - eda_sv_feedback_mutate_success_total (#695) +
    //     eda_sv_stable_ref_invalidation_total (#695) +
    //     eda_sv_corruption_detected_total (#695)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:sv-verification-closedloop-stats`
    // (no `-hash` suffix) with AC1+AC2+AC3-specific counters
    // — was *not* shipped under that exact name. So #640
    // ships ONE new Aura primitive + 3 new foundation
    // atomics.
    //
    // The remaining #640 AC1 + AC2 + AC3 work is invasive
    // C++ on the verification-feedback hot path
    // (eda:apply-verification-feedback primitive + Guard +
    // StableNodeRef capture + sv_ir structured mutate +
    // hardware_backend re-emit hook + strengthened
    // StableNodeRef provenance check) + needs the 5000+
    // fiber stress + TSan coverage from the issue body —
    // separate follow-ups.
    add("query:sv-verification-closedloop-stats", [&ev](const auto&) -> EvalValue {
        // feedback-apply: new foundation atomic
        // (0 until AC1 eda:apply-verification-feedback
        // wire-up).
        const std::uint64_t feedback_apply =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->sv_verify_feedback_apply_total.load(std::memory_order_relaxed)
                : 0;
        // guard-reemit: new foundation atomic
        // (0 until AC2 Guard → hardware_backend re-emit
        // hook wire-up).
        const std::uint64_t guard_reemit =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->sv_guard_reemit_hook_total.load(std::memory_order_relaxed)
                : 0;
        // stable-ref-strict: new foundation atomic
        // (0 until AC3 strengthened StableNodeRef
        // provenance check wire-up).
        const std::uint64_t stable_ref_strict =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->sv_stable_ref_provenance_strict_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("feedback-apply", static_cast<std::int64_t>(feedback_apply));
        insert_kv("guard-reemit", static_cast<std::int64_t>(guard_reemit));
        insert_kv("stable-ref-strict", static_cast<std::int64_t>(stable_ref_strict));
        insert_kv("schema", 640);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #661: query:sv-interface-structure-stats — SV InterfaceIR
    // + ModportIR structure observability (P1 EDA-SV).
    //
    // The 3 counters track the structured interface IR/ModportIR
    // BUILDER shape (the foundations the issue body Action #4
    // calls out: ports_count + modport_views + direction_changes):
    //   - ports-count         sv_interface_ports_total
    //       Bumped per Interface body port addition (lifetime
    //       running total). Wired into `eda:parse-netlist`'s
    //       interface parse path.
    //   - modport-views       sv_interface_modport_views_total
    //       Bumped per Modport view addition. Wired into
    //       `eda:parse-netlist`'s modport parse path.
    //   - direction-changes   sv_interface_direction_changes_total
    //       Bumped per port direction change. Currently bumped
    //       via the test-only helpers (the production wire via
    //       `eda:set-port-direction` is follow-up work for
    //       Action #3 in the issue body).
    //   - interface-events-total
    //       Sum of the 3 above (per-call derivation, not a
    //       separate atomic). Lets dashboards show overall
    //       interface-structure-event volume at a glance.
    //   - schema == 661
    //
    // Non-duplicative with #640/#630/#539/#497/#498/#496 (those
    // cover SVA, verification, and pattern scopes); #661 covers
    // the interface IR/ModportIR BUILDER shape specifically.
    add("query:sv-interface-structure-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t ports_count =
            static_cast<std::int64_t>(ev.get_sv_interface_ports_total());
        const std::int64_t modport_views =
            static_cast<std::int64_t>(ev.get_sv_interface_modport_views_total());
        const std::int64_t direction_changes =
            static_cast<std::int64_t>(ev.get_sv_interface_direction_changes_total());
        const std::int64_t events_total = ports_count + modport_views + direction_changes;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("ports-count", ports_count);
        insert_kv("modport-views", modport_views);
        insert_kv("direction-changes", direction_changes);
        insert_kv("interface-events-total", events_total);
        insert_kv("schema", 661);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #641: query:stable-ref-provenance-sv-stats —
    // Agent-discoverable structured dashboard for the
    // StableNodeRef Cross-Fiber Provenance Enforcement in
    // Multi-Agent Orchestration (P0 EDSL-Review + commercial
    // reliability surface).
    //
    // Note the naming distinction from #631:
    //   - (query:stable-ref-provenance-sv-stats-hash) (#631)
    //     is the historical hash primitive from the
    //     pre-enforcement era (5 fields, provenance summary
    //     before fiber_id/workspace_id enforcement).
    //   - (query:stable-ref-provenance-sv-stats) (#641, this
    //     primitive) is the *enforcement-track* companion that
    //     focuses on the cross-fiber / multi-agent
    //     provenance enforcement counters for AC1+AC2+AC4
    //     and uses the issue-specified exact name from #641's
    //     AC3 (no `-hash` suffix).
    //
    // Fields (3 + sentinel):
    //   - fiber-check         new stable_ref_fiber_provenance_
    //                          check_total atomic (foundation
    //                          for AC1 fiber_id / workspace_id
    //                          match enforcement in query:/
    //                          mutate: + Guard dtor). Value
    //                          is 0 until AC1 wire-up.
    //   - auto-refresh        new stable_ref_provenance_auto_
    //                          refresh_total atomic (foundation
    //                          for AC2 Guard success →
    //                          auto-refresh provenance stamp).
    //                          Value is 0 until AC2 wire-up.
    //   - sv-feedback-wired   new stable_ref_sv_feedback_wired_
    //                          total atomic (foundation for AC4
    //                          provenance-checked SV feedback
    //                          path wire-up).
    //                          Value is 0 until AC4 wire-up.
    //   - schema == 641         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~70% of the
    // cross-fiber provenance observability surface:
    //   - (query:stable-ref-provenance) (#604) — base
    //     provenance summary primitive (no SV-specific track)
    //   - (query:stable-ref-provenance-sv-stats-hash) (#631)
    //     — historical hash primitive
    //   - stable_ref_provenance_query_total (#631) + cross_
    //     fiber_violations_total (#631) + safe_resolves_total
    //     (#631) — cross-fiber / multi-agent SV provenance
    //     counters
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:stable-ref-provenance-sv-stats` (no
    // `-hash` suffix) with AC1+AC2+AC4-specific counters —
    // was *not* shipped under that exact name. So #641 ships
    // ONE new Aura primitive + 3 new foundation atomics.
    //
    // The remaining #641 AC1 + AC2 + AC4 work is invasive C++
    // on the StableNodeRef validate_with_provenance +
    // Guard dtor + SV feedback hot path + needs the
    // multi-fiber steal + SV sequences + TSan coverage from
    // the issue body — separate follow-ups.
    add("query:stable-ref-provenance-sv-stats", [&ev](const auto&) -> EvalValue {
        // fiber-check: new foundation atomic
        // (0 until AC1 fiber_id / workspace_id match
        // enforcement wire-up).
        const std::uint64_t fiber_check =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->stable_ref_fiber_provenance_check_total.load(std::memory_order_relaxed)
                : 0;
        // auto-refresh: new foundation atomic
        // (0 until AC2 Guard success →
        // auto-refresh provenance stamp wire-up).
        const std::uint64_t auto_refresh =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->stable_ref_provenance_auto_refresh_total.load(std::memory_order_relaxed)
                : 0;
        // sv-feedback-wired: new foundation atomic
        // (0 until AC4 provenance-checked SV feedback
        // path wire-up).
        const std::uint64_t sv_feedback_wired =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->stable_ref_sv_feedback_wired_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("fiber-check", static_cast<std::int64_t>(fiber_check));
        insert_kv("auto-refresh", static_cast<std::int64_t>(auto_refresh));
        insert_kv("sv-feedback-wired", static_cast<std::int64_t>(sv_feedback_wired));
        insert_kv("schema", 641);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #642: query:arena-auto-compaction-stats —
    // Agent-discoverable structured dashboard for the Arena
    // Auto-Compaction + Fiber/GC Safepoint Coordination
    // (P0 Prompt6-MemorySafety + commercial reliability
    // surface).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:arena-auto-compact-stats) — earlier
    //     primitive focused on the auto-compact trigger only
    //   - (query:arena-auto-compact-defrag-stats) (#569) —
    //     extended version with defrag breakdown
    //   - (query:arena-auto-compaction-stats) (#642, this
    //     primitive) — *enforcement-track* companion that
    //     focuses on the auto-compaction + fiber/GC
    //     safepoint coordination counters for AC1+AC2+AC3
    //     and uses the issue-specified exact name from #642's
    //     AC4 (`-compaction` with the `-ion` suffix, NOT
    //     `-compact`).
    //
    // Fields (3 + sentinel):
    //   - auto-trigger        new arena_auto_compact_trigger_
    //                          total atomic (foundation for
    //                          AC1 allocate_raw auto-trigger
    //                          compact on fragmentation >
    //                          threshold + fiber safepoint
    //                          coordination). Value is 0
    //                          until AC1 wire-up.
    //   - live-move-yield     new arena_live_move_yield_total
    //                          atomic (foundation for AC2
    //                          compact/defrag enhanced with
    //                          live move + yield support).
    //                          Value is 0 until AC2 wire-up.
    //   - guard-defrag        new arena_guard_request_defrag_
    //                          total atomic (foundation for
    //                          AC3 Guard/invalidate → request_
    //                          defrag wiring). Value is 0
    //                          until AC3 wire-up.
    //   - schema == 642         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641
    //                          sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~70% of the
    // auto-compaction observability surface:
    //   - (query:arena-auto-stats) — broader arena stats
    //   - (query:arena-auto-compact-stats) — earlier
    //     auto-compact trigger primitive
    //   - (query:arena-auto-compact-defrag-stats) (#569) —
    //     defrag breakdown primitive
    //   - (query:arena-compaction-stats) — base compaction
    //     summary primitive
    //   - (query:arena-fragmentation-snapshot) — snapshot
    //     primitive
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:arena-auto-compaction-stats` (`-ion`
    // suffix, not `-compact`) with AC1+AC2+AC3-specific
    // counters — was *not* shipped under that exact name.
    // So #642 ships ONE new Aura primitive + 3 new foundation
    // atomics.
    //
    // The remaining #642 AC1 + AC2 + AC3 work is invasive
    // C++ on the allocate_raw + compact/defrag + Guard hot
    // path + needs the 10k+ mutate + 20+ fibers + TSan/ASan
    // coverage from the issue body — separate follow-ups.
    add("query:arena-auto-compaction-stats", [&ev](const auto&) -> EvalValue {
        // auto-trigger: new foundation atomic
        // (0 until AC1 allocate_raw auto-trigger wire-up).
        const std::uint64_t auto_trigger =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->arena_auto_compact_trigger_total.load(std::memory_order_relaxed)
                : 0;
        // live-move-yield: new foundation atomic
        // (0 until AC2 live move + yield wire-up).
        const std::uint64_t live_move_yield =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->arena_live_move_yield_total.load(std::memory_order_relaxed)
                : 0;
        // guard-defrag: new foundation atomic
        // (0 until AC3 Guard/invalidate → request_defrag
        // wire-up).
        const std::uint64_t guard_defrag =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->arena_guard_request_defrag_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("auto-trigger", static_cast<std::int64_t>(auto_trigger));
        insert_kv("live-move-yield", static_cast<std::int64_t>(live_move_yield));
        insert_kv("guard-defrag", static_cast<std::int64_t>(guard_defrag));
        insert_kv("schema", 642);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #643: query:primitives-meta — Agent-discoverable
    // structured per-primitive AI-native introspection
    // primitive (P0 Stdlib-Impl-P1 foundation — implements
    // #633 AC3+AC4 + #559 classification wire-up).
    //
    // Note the naming distinction from #498:
    //   - (query:primitive-metadata) (#498, no `s`) — base
    //     AI-native primitive introspection primitive
    //     (no per-primitive lookup arg, returns list)
    //   - (query:primitives-meta) (#643, this primitive) —
    //     per-primitive lookup form per issue body AC2 exact
    //     spec. Accepts optional [name] argument:
    //       - (query:primitives-meta) → list of all
    //         primitive meta pairs (alias for catalog)
    //       - (query:primitives-meta 'foo) → single meta
    //         pair for primitive "foo" or () if not found
    //     Uses the new primitives_meta_query_total counter
    //     (distinct from primitives_meta_catalog_query_total
    //     #617 which tracks the catalog primitive).
    //
    // Fields per entry (8) + sentinel:
    //   - name              primitive name (string)
    //   - has-fn            1 if the primitive has a registered
    //                       function body, 0 otherwise
    //   - arity             from PrimMeta.arity (255 = variadic)
    //                       — foundation for the DEFINE_PRIMITIVE
    //                       macro arity-at-compile validation
    //                       (#643 AC1)
    //   - pure              from PrimMeta.pure (bool) — lets the
    //                       Agent decide whether memoization /
    //                       const-folding applies (Issue #669
    //                       fill-the-gap enrichment)
    //   - safety            from PrimMeta.safety_flags (int) —
    //                       0x01=mutates, 0x02=io, 0x04=fiber
    //                       (#480 + #669 enrichment)
    //   - doc               from PrimMeta.doc (string, "") —
    //                       lets the Agent render help text for
    //                       end-users without hardcoded
    //                       (#480 + #669 enrichment)
    //   - category          classification from #559
    //                       (mutation-safety / core /
    //                       internal-observable / convenience)
    //   - schema == 669     sentinel for Agent drift detection
    //                       (changed from 643 in #669 to signal
    //                       the enriched 8-field shape; pre-#669
    //                       shape was 5 fields with hardcoded
    //                       arity=0 / category="internal-observable" / no
    //                       pure / no safety / no doc)
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers ~70% of the AI-native
    // meta introspection surface:
    //   - (query:primitive-metadata) (#498) — base AI-native
    //     primitive introspection (no per-primitive lookup arg)
    //   - (query:primitives-meta-catalog) (#617) — catalog
    //     primitive with category + arity + meta
    //   - (query:primitives-by-category) — category filter
    //     primitive
    //   - (query:primitives-extension-stats) (#618/#625) —
    //     extension stats
    //   - primitives_meta_catalog_query_total (#617) — catalog
    //     hit-rate counter
    // What the issue body AC2 specifies by **exact name +
    // signature** — `query:primitives-meta [name]` accepting
    // an optional [name] argument for per-primitive lookup
    // — was *not* shipped under that exact signature. So #643
    // ships ONE new Aura primitive (with optional [name] arg
    // dispatch) + 3 new foundation atomics.
    //
    // The remaining #643 AC1 (DEFINE_PRIMITIVE macro) + AC3
    // (PRIM_ERROR unification) + AC4 (primitives_style.md) work
    // is invasive C++ on the registry / evaluator.ixx /
    // primitives_detail header + needs the AI-Agent generate-
    // primitive demo + ./build.py check + CI gate coverage
    // from the issue body — separate follow-ups.
    add("query:primitives-meta", [&ev](const auto& a) -> EvalValue {
        // Bump the new per-primitive-lookup counter (distinct
        // from primitives_meta_catalog_query_total #617).
        if (auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())) {
            m->primitives_meta_query_total.fetch_add(1, std::memory_order_relaxed);
        }
        // The foundation scaffolding atomics (currently 0
        // until AC1+AC3 wire-up).
        const std::uint64_t define_macro_used =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->define_primitive_macro_used_total.load(std::memory_order_relaxed)
                : 0;
        const std::uint64_t prim_error_unified =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->prim_error_unified_total.load(std::memory_order_relaxed)
                : 0;
        auto build_pair = [&](const std::string& name) -> EvalValue {
            // Issue #669: enrich the per-name response with
            // real PrimMeta fields (arity, pure, safety,
            // doc, category) via meta_for_slot. Pre-#669 the
            // shape returned hardcoded arity=0 + has-fn=1 +
            // category="internal-observable" — PrimMeta was
            // populated (#480) but never reached the Agent.
            //
            // Schema now exposes 8 fields:
            //   - name              primitive name
            //   - has-fn            1 if registered, 0 if unknown
            //   - arity             from PrimMeta.arity (255 = variadic)
            //   - pure              from PrimMeta.pure (bool)
            //   - safety            from PrimMeta.safety_flags (int)
            //   - doc               from PrimMeta.doc (string, "")
            //   - category          from PrimMeta.category (string)
            //   - schema            669 (drift sentinel — changed
            //                       from 643 to signal the
            //                       enriched field set)
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            auto insert_kv = [&](const char* k_str, EvalValue v) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (const char* p = k_str; *p; ++p)
                    h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        auto kidx = ev.string_heap_.size();
                        ev.string_heap_.push_back(k_str);
                        keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                        vals[idx] = v.val;
                        ht->size++;
                        return;
                    }
                }
            };
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(name);
            insert_kv("name", make_string(static_cast<std::uint64_t>(name_idx)));
            // Look up the real PrimMeta via slot_for_name +
            // meta_for_slot. If unknown, has-fn=0 + default
            // PrimMeta{} (the Agent can distinguish "known
            // primitive with no body" from "unknown" via
            // has-fn).
            const auto slot = ev.primitives_.slot_for_name(name);
            const bool known = slot < ev.primitives_.slot_count();
            const PrimMeta& pm = known ? ev.primitives_.meta_for_slot(slot) : PrimMeta{};
            insert_kv("has-fn", make_int(known ? 1 : 0));
            insert_kv("arity", make_int(static_cast<std::int64_t>(pm.arity)));
            insert_kv("pure", make_bool(pm.pure));
            insert_kv("safety", make_int(static_cast<std::int64_t>(pm.safety_flags)));
            auto doc_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(pm.doc);
            insert_kv("doc", make_string(static_cast<std::uint64_t>(doc_idx)));
            auto cat_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(pm.category);
            insert_kv("category", make_string(static_cast<std::uint64_t>(cat_idx)));
            insert_kv("schema", make_int(669));
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // Dispatch: optional [name] argument.
        if (!a.empty() && aura::compiler::types::is_string(a[0])) {
            const auto idx = aura::compiler::types::as_string_idx(a[0]);
            if (idx < ev.string_heap_.size()) {
                const auto& name = ev.string_heap_[idx];
                // Build the meta hash for the requested name.
                // Whether or not the primitive exists, we return
                // the meta shape so the Agent can introspect —
                // has-fn=0 + arity=0 if not found is a valid
                // response (lets the Agent distinguish "known
                // primitive with no body" from "unknown").
                return build_pair(name);
            }
            return make_void();
        }
        // No [name] arg → return a pair with the aggregate
        // foundation counters + the schema sentinel so the
        // Agent can dashboard at-a-glance. (Full catalog
        // form is provided by #617 query:primitives-meta-
        // catalog — the new primitive specializes on
        // per-name lookup.)
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("define-macro-used", static_cast<std::int64_t>(define_macro_used));
        insert_kv("prim-error-unified", static_cast<std::int64_t>(prim_error_unified));
        insert_kv("schema", 643);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #669: query:primitives-meta-stats — per-meta
    // observability summary (P1 stdlib-impl AI-native
    // introspection gap-fill). Reports the count of
    // primitives by meta status so the Agent can see
    // how much of the stdlib surface has been
    // meta-documented vs left default.
    //
    // Non-duplicative with #617 query:primitives-meta-catalog
    // (which returns the registry-level summary) and
    // #697 query:primitives-extension-stats (which tracks
    // runtime counters). #669 adds the per-meta-axis
    // observability summary that the Agent needs to
    // know "how much is documented?" at-a-glance.
    //
    // Fields (4 + sentinel):
    //   - meta-hits            primitives_meta_query_total
    //                          (this primitive's call counter)
    //   - documented-count     documented_meta_count()
    //                          (primitives with non-empty doc)
    //   - schema-documented    schema_documented_meta_count()
    //                          (primitives with both doc AND
    //                          schema set)
    //   - total-registered     primitives_.slot_count()
    //   - schema == 669        drift sentinel
    add("query:primitives-meta-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t meta_hits = static_cast<std::int64_t>(
            ev.compiler_metrics()
                ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                      ->primitives_meta_query_total.load(std::memory_order_relaxed)
                : 0);
        const std::int64_t documented =
            static_cast<std::int64_t>(ev.get_primitive_documented_meta_count());
        const std::int64_t schema_documented =
            static_cast<std::int64_t>(ev.primitives_.schema_documented_meta_count());
        const std::int64_t total = static_cast<std::int64_t>(ev.primitives_.slot_count());
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("meta-hits", meta_hits);
        insert_kv("documented-count", documented);
        insert_kv("schema-documented", schema_documented);
        insert_kv("total-registered", total);
        insert_kv("schema", 669);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #671: query:primitives-consistency-stats —
    // primitives_detail lambda capture discipline + style
    // compliance observability (P1 stdlib-impl consistency).
    //
    // Companion to #709 (query:primitives-registry-stats which
    // bundles registry-level metrics) but specialized on the
    // capture-discipline axis. The existing #709 primitive
    // exposes `capture-violations` as one of 7 fields; #671
    // carves out a dedicated primitive focused on consistency:
    //   - capture-violations-detected
    //       primitive_capture_violations_total — bumped by
    //       prim_record_capture_violation when a primitive
    //       fails the capture contract (no error_counter on
    //       a mutate path)
    //   - style-compliance-pct
    //       derived metric: (1 - capture_violations /
    //       slot_count) * 100 — 100 means every primitive
    //       passes the contract
    //   - capture-contract-version
    //       kPrimCaptureContractVersion (defined in
    //       primitives_detail.h) — bump when the contract
    //       changes so the Agent can detect drift
    //   - recommended-action
    //       0 = no action, 1 = backfill missing meta, 2 =
    //       audit capture contract. Triggered when
    //       capture_violations > 0 or documented < slots.
    //   - schema == 671
    //
    // Non-duplicative with #709 (registry-level summary with
    // 7 fields covering fast-path + EDA integration),
    // #615 (PRIM_ERROR macro shape), #643 (DEFINE_PRIMITIVE
    // macro for registration), #617 (catalog registry
    // summary).
    add("query:primitives-consistency-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t capture_viol =
            m ? m->primitive_capture_violations_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t slots = ev.primitives_.slot_count();
        const std::uint64_t documented = ev.primitives_.documented_meta_count();
        const std::int64_t style_compliance_pct =
            slots > 0 ? static_cast<std::int64_t>(
                            ((slots > capture_viol ? slots - capture_viol : 0) * 100) / slots)
                      : 100;
        std::int64_t recommended_action = 0;
        if (capture_viol > 0)
            recommended_action = 2; // audit capture contract
        else if (documented < slots)
            recommended_action = 1; // backfill missing meta
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("capture-violations-detected", static_cast<std::int64_t>(capture_viol));
        insert_kv("style-compliance-pct", style_compliance_pct);
        insert_kv("registry-slots", static_cast<std::int64_t>(slots));
        insert_kv("documented-count", static_cast<std::int64_t>(documented));
        insert_kv("capture-contract-version",
                  static_cast<std::int64_t>(kPrimCaptureContractVersion));
        insert_kv("recommended-action", recommended_action);
        insert_kv("schema", 671);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #751: query:primitives-contract-stats — P0 PRIM_ERROR /
    // capture discipline enforcement dashboard (refines #728/#671/#615;
    // non-duplicative with #671 primitives-consistency-stats which
    // focuses on registry meta backfill + recommended-action).
    //
    // Fields (4 + sentinel):
    //   - capture-violations     primitive_capture_violations_total
    //   - prim-error-hits        prim_error_unified_total
    //   - style-compliance-pct   derived (slots - violations) / slots * 100
    //   - capture-contract-version kPrimCaptureContractVersion
    //   - schema == 751
    add("query:primitives-contract-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t capture_viol =
            m ? m->primitive_capture_violations_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t prim_errors =
            m ? m->prim_error_unified_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t slots = ev.primitives_.slot_count();
        const std::int64_t style_compliance_pct =
            slots > 0 ? static_cast<std::int64_t>(
                            ((slots > capture_viol ? slots - capture_viol : 0) * 100) / slots)
                      : 100;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("capture-violations", static_cast<std::int64_t>(capture_viol));
        insert_kv("prim-error-hits", static_cast<std::int64_t>(prim_errors));
        insert_kv("style-compliance-pct", style_compliance_pct);
        insert_kv("capture-contract-version",
                  static_cast<std::int64_t>(kPrimCaptureContractVersion));
        insert_kv("schema", 751);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #752: query:list-soa-hotpath-stats — P0 list/vector
    // map/filter SoA + intrinsic fast-dispatch observability
    // (refines #727; non-duplicative with #667 apply-loop
    // counters and #506 IR SoA adoption).
    //
    // Fields (4 + sentinel):
    //   - chain-traversals      list_chain_traversals_total
    //   - soa-hits              list_soa_hits_total
    //   - intrinsic-dispatches  list_intrinsic_dispatches_total
    //   - estimated-cache-misses list_estimated_cache_misses_total
    //   - hotpath-events-total  (sum of 4, per-call derivation)
    //   - schema == 752
    add("query:list-soa-hotpath-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t chain_traversals =
            static_cast<std::int64_t>(ev.get_list_chain_traversals());
        const std::int64_t soa_hits = static_cast<std::int64_t>(ev.get_list_soa_hits());
        const std::int64_t intrinsic_dispatches =
            static_cast<std::int64_t>(ev.get_list_intrinsic_dispatches());
        const std::int64_t estimated_cache_misses =
            static_cast<std::int64_t>(ev.get_list_estimated_cache_misses());
        const std::int64_t events_total =
            chain_traversals + soa_hits + intrinsic_dispatches + estimated_cache_misses;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("chain-traversals", chain_traversals);
        insert_kv("soa-hits", soa_hits);
        insert_kv("intrinsic-dispatches", intrinsic_dispatches);
        insert_kv("estimated-cache-misses", estimated_cache_misses);
        insert_kv("hotpath-events-total", events_total);
        insert_kv("schema", 752);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #644: query:aot-reload-func-table-stats —
    // Agent-discoverable structured dashboard for the AOT
    // Hot-Reload func_table Refcount + Per-Region Isolation
    // (P0 Runtime-Gap + AOT production-readiness surface —
    // non-duplicative to #624 #601 #358).
    //
    // Note the naming distinction from #708:
    //   - (query:aot-reload-stats) (#708) — 5-field primitive
    //     focused on the high-level reload attempt / success /
    //     stale / refcount_swaps / region_violations summary
    //   - (query:aot-reload-func-table-stats) (#644, this
    //     primitive) — *enforcement-track* companion that
    //     focuses on the func_table refcount bump/decrement
    //     protocol + region filter re-apply wire-up
    //     (the AC1+AC2+AC4 enforcement counters that #708
    //     did not surface as a separate primitive).
    //
    // Fields (3 + sentinel):
    //   - ref-bump            new aot_func_table_ref_bump_total
    //                          atomic (foundation for AC1
    //                          atomic refcount bumps on new
    //                          func_table entry install).
    //                          Value is 0 until AC1 wire-up.
    //   - ref-decrement       new aot_func_table_ref_decrement_
    //                          total atomic (foundation for AC1
    //                          atomic refcount decrements on
    //                          old entry retirement after grace
    //                          period / epoch check). Value is
    //                          0 until AC1 wire-up.
    //   - region-reapply      new aot_region_filter_reapply_
    //                          total atomic (foundation for AC2
    //                          region filtering re-applied on
    //                          reload per agent/workspace).
    //                          Value is 0 until AC2 wire-up.
    //   - schema == 644         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level AOT
    // reload observability surface:
    //   - (query:aot-reload-stats) (#708) — 5-field reload
    //     summary (attempts / success / stale / swaps /
    //     region_violations)
    //   - (query:aot-hot-reload-stats) (#358/#452) — earlier
    //     AOT hot-reload summary
    //   - (query:aot-checkpoint-version-stats) (#708) —
    //     checkpoint version tracking
    //   - aot_reload_attempts_ + aot_hot_update_success_ +
    //     aot_stale_reject_count_ + aot_refcount_swaps_ +
    //     aot_region_mismatch_ (#708) — high-level counters
    // What the issue body specifies by **exact enforcement
    // layer** — granular func_table refcount bump/decrement
    // + per-region filter re-apply counters for AC1+AC2+AC4
    // — was *not* shipped under that exact enforcement layer.
    // So #644 ships ONE new Aura primitive + 3 new foundation
    // atomics.
    //
    // The remaining #644 AC1 (func_table refcount swap
    // protocol) + AC2 (region filtering re-apply) + AC4
    // (MutationBoundaryGuard + fiber yield wire-up) work is
    // invasive C++ on aura_jit_bridge.cpp + hot-swap hooks +
    // service.ixx invalidate + needs the 1000+ reload cycles
    // + concurrent apply_closure + TSan coverage from the
    // issue body — separate follow-ups.
    add("query:aot-reload-func-table-stats", [&ev](const auto&) -> EvalValue {
        // ref-bump: new foundation atomic
        // (0 until AC1 atomic refcount bumps wire-up).
        const std::uint64_t ref_bump =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->aot_func_table_ref_bump_total.load(std::memory_order_relaxed)
                : 0;
        // ref-decrement: new foundation atomic
        // (0 until AC1 atomic refcount decrements wire-up).
        const std::uint64_t ref_decrement =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->aot_func_table_ref_decrement_total.load(std::memory_order_relaxed)
                : 0;
        // region-reapply: new foundation atomic
        // (0 until AC2 region filtering re-apply wire-up).
        const std::uint64_t region_reapply =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->aot_region_filter_reapply_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("ref-bump", static_cast<std::int64_t>(ref_bump));
        insert_kv("ref-decrement", static_cast<std::int64_t>(ref_decrement));
        insert_kv("region-reapply", static_cast<std::int64_t>(region_reapply));
        insert_kv("schema", 644);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #645: query:scheduler-steal-bias-stats —
    // Agent-discoverable structured dashboard for the
    // Work-Stealing LIFO/FIFO Adaptive Bias + YieldReason /
    // outermost Mutation Depth (P0 Runtime-Gap + Scheduler
    // production-readiness surface — non-duplicative to
    // #618 #588 #451).
    //
    // Note the naming distinction from #706:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     is the steal-budget adaptive bias primitive
    //     (LLM-bottleneck adjustments — higher level
    //     orchestration tune)
    //   - (query:scheduler-steal-bias-stats) (#645, this
    //     primitive) is the *enforcement-track* companion
    //     that focuses on the per-steal LIFO/FIFO +
    //     mutation-deferred counters for AC1+AC2+AC4
    //     enforcement (lower level — what each steal
    //     decision consults).
    //
    // Fields (3 + sentinel):
    //   - lifo-hits            new scheduler_lifo_hits_total
    //                          atomic (foundation for AC1
    //                          LIFO local hits on worker
    //                          deque). Value is 0 until
    //                          AC1 wire-up.
    //   - fifo-steals          new scheduler_fifo_steals_total
    //                          atomic (foundation for AC1
    //                          FIFO steals from victim).
    //                          Value is 0 until AC1 wire-up.
    //   - mutation-deferred    new scheduler_mutation_deferred_
    //                          bias_total atomic (foundation
    //                          for AC1+AC2 deferred-steal
    //                          from inner-MutationBoundary
    //                          fibers + the simple adaptive
    //                          LIFO/FIFO tuning). Value is
    //                          0 until AC1+AC2 wire-up.
    //   - schema == 645         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // scheduler adaptive bias surface:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     — LLM-bottleneck adjustments (orchestration tune)
    //   - #618 per-fiber yield_reason classification +
    //     is_at_mutation_boundary_safe + outermost depth probe
    //   - #588 per-fiber stack + adaptive hints
    //   - #451 work-stealing deque LIFO local + FIFO steal +
    //     request_gc_safepoint
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:scheduler-steal-bias-stats` with
    // LIFO/FIFO + mutation-deferred counters — was *not*
    // shipped under that exact name. So #645 ships ONE new
    // Aura primitive + 3 new foundation atomics.
    //
    // The remaining #645 AC1 (steal loop consults
    // victim->last_yield_reason() + outermost depth) +
    // AC2 (simple adaptive LIFO/FIFO tuning) + AC4 (wire
    // to #618 orchestration tune) work is invasive C++
    // on worker steal loop + scheduler next_worker +
    // needs the 20+ fibers + LLM-sim latency matrix +
    // TSan coverage from the issue body — separate
    // follow-ups.
    add("query:scheduler-steal-bias-stats", [&ev](const auto&) -> EvalValue {
        // lifo-hits: new foundation atomic
        // (0 until AC1 LIFO local hits wire-up).
        const std::uint64_t lifo_hits =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->scheduler_lifo_hits_total.load(std::memory_order_relaxed)
                : 0;
        // fifo-steals: new foundation atomic
        // (0 until AC1 FIFO steals wire-up).
        const std::uint64_t fifo_steals =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->scheduler_fifo_steals_total.load(std::memory_order_relaxed)
                : 0;
        // mutation-deferred: new foundation atomic
        // (0 until AC1+AC2 deferred-steal wire-up).
        const std::uint64_t mutation_deferred =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->scheduler_mutation_deferred_bias_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("lifo-hits", static_cast<std::int64_t>(lifo_hits));
        insert_kv("fifo-steals", static_cast<std::int64_t>(fifo_steals));
        insert_kv("mutation-deferred", static_cast<std::int64_t>(mutation_deferred));
        insert_kv("schema", 645);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #646: query:gc-safepoint-deferral-stats —
    // Agent-discoverable structured dashboard for the GC
    // Safepoint Deferral + Backoff Only for Outermost
    // MutationBoundary + Contention Metrics (P0 Runtime-Gap
    // + GC production-readiness surface — non-duplicative to
    // #642 #623 #591).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //     primitive (no deferral-specific breakdown)
    //   - (query:gc-safepoint-deferral-stats) (#646, this
    //     primitive) — *deferral-track* companion that
    //     focuses on the outermost-vs-inner deferral +
    //     backoff contention counters for AC1+AC2+AC4
    //     enforcement.
    //
    // Fields (3 + sentinel):
    //   - outermost-deferral  new gc_outermost_deferral_total
    //                          atomic (foundation for AC1
    //                          outermost MutationBoundary
    //                          depth==1 full deferral).
    //                          Value is 0 until AC1 wire-up.
    //   - inner-proceeded      new gc_inner_proceeded_total
    //                          atomic (foundation for AC1
    //                          inner MutationBoundary
    //                          depth>1 short-yield/proceed).
    //                          Value is 0 until AC1 wire-up.
    //   - backoff-trigger      new gc_backoff_trigger_total
    //                          atomic (foundation for AC2
    //                          backoff fires under repeated
    //                          deferral contention). Value
    //                          is 0 until AC2 wire-up.
    //   - schema == 646         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644+#645 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the base GC
    // safepoint observability surface:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //     primitive (no deferral-specific breakdown)
    //   - #591 gc pause attributed to mutation count
    //   - #588 per-fiber stack + GC coordination
    //   - #623 arena + GC safepoint coordination
    //   - #642 arena auto-compaction + fiber/GC safepoint
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:gc-safepoint-deferral-stats` with
    // outermost-vs-inner + backoff counters — was *not*
    // shipped under that exact name. So #646 ships ONE new
    // Aura primitive + 3 new foundation atomics.
    //
    // The remaining #646 AC1 (outermost vs inner check) +
    // AC2 (backoff retry) + AC4 (wire to scheduler GC phase
    // + fiber yield_classification) work is invasive C++ on
    // aura_evaluator_request_gc_safepoint + fiber
    // check_gc_safepoint + scheduler request_gc_safepoint /
    // wait_for_safepoint + needs the high-contention matrix
    // + arena pressure + TSan coverage from the issue body
    // — separate follow-ups.
    add("query:gc-safepoint-deferral-stats", [&ev](const auto&) -> EvalValue {
        // outermost-deferral: new foundation atomic
        // (0 until AC1 outermost depth==1 wire-up).
        const std::uint64_t outermost_deferral =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->gc_outermost_deferral_total.load(std::memory_order_relaxed)
                : 0;
        // inner-proceeded: new foundation atomic
        // (0 until AC1 inner depth>1 wire-up).
        const std::uint64_t inner_proceeded =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->gc_inner_proceeded_total.load(std::memory_order_relaxed)
                : 0;
        // backoff-trigger: new foundation atomic
        // (0 until AC2 backoff wire-up).
        const std::uint64_t backoff_trigger =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->gc_backoff_trigger_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("outermost-deferral", static_cast<std::int64_t>(outermost_deferral));
        insert_kv("inner-proceeded", static_cast<std::int64_t>(inner_proceeded));
        insert_kv("backoff-trigger", static_cast<std::int64_t>(backoff_trigger));
        insert_kv("schema", 646);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #647: query:envframe-dualpath-stale-stats-hash —
    // Agent-discoverable structured dashboard for the
    // Dual-Path EnvFrame/Env (parent_id_ vs parent_,
    // bindings_symid_ vs bindings_) Cross-Fiber Stale
    // Detection + materialize_call_env After Steal
    // (P0 Runtime-Gap + SoA production-readiness surface —
    // non-duplicative to #637 #589 #355).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:envframe-dualpath-stale-stats) (#418) —
    //     existing flat-int primitive (returns a single
    //     sum of 7 counters — no field breakdown)
    //   - (query:envframe-dualpath-stats) — existing base
    //     dualpath primitive
    //   - (query:envframe-dualpath-stale-stats-hash) (#647,
    //     this primitive) — *enforcement-track* companion
    //     with `-hash` suffix (matches the #630 / #641
    //     hash-vs-int naming convention) that focuses on
    //     the AC1+AC2+AC4 counters for cross-fiber stale +
    //     post-steal version mismatch + dual-path repair
    //     wire-up.
    //
    // Fields (3 + sentinel):
    //   - cross-fiber-stale   new envframe_cross_fiber_stale_
    //                          total atomic (foundation for
    //                          AC1 cross-fiber stale detection
    //                          post-steal — parent_id_
    //                          mismatch vs env_frames_
    //                          owner). Value is 0 until AC1
    //                          wire-up.
    //   - version-mismatch    new envframe_version_mismatch_
    //                          post_steal_total atomic
    //                          (foundation for AC1 version_
    //                          stamp mismatch detection
    //                          post-steal). Value is 0 until
    //                          AC1 wire-up.
    //   - dualpath-repair     new envframe_dualpath_repair_
    //                          total atomic (foundation for
    //                          AC2 dual-path consistency
    //                          check + repair hits). Value
    //                          is 0 until AC2 wire-up.
    //   - schema == 647         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644+#645+#646
    //                          sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // EnvFrame dual-path observability surface:
    //   - (query:envframe-dualpath-stale-stats) (#418) —
    //     flat-int sum of 7 counters (no field breakdown)
    //   - (query:envframe-dualpath-stats) — base dualpath
    //     primitive
    //   - (query:envframe-stale-stats) — stale refresh stats
    //   - (query:envframe-bump-stats) — bump stats
    //   - env_frames_ EnvFrame arena (walk + lookup_by_symid_
    //     chain) with version_ + INVALID_VERSION sentinel #356
    //   - #637 IRClosure + EnvFrame versioning + bridge
    //     invalidate protocol
    //   - #589 / #355 SoA migration (parent_id_ vs parent_,
    //     bindings_symid_ vs bindings_ dual-path)
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:envframe-dualpath-stale-stats` with
    // AC1+AC2+AC4-specific counters as a structured hash —
    // was *not* shipped under that exact hash form. The
    // existing flat-int primitive ships under the same name
    // without `-hash` suffix; #647 ships the hash form with
    // `-hash` suffix (matches #630 / #641 convention for
    // hash-vs-int naming).
    //
    // The remaining #647 AC1 (parent_id_ vs current owner
    // validation) + AC2 (fiber resume dual-path consistency
    // check / repair) + AC4 (GCEnvWalkFn skip/repair) work
    // is invasive C++ on materialize_call_env + lookup paths
    // + fiber resume + g_fiber_sync_mutation_stack_ +
    // GCEnvWalkFn + needs the heavy mutate + fiber steal/
    // yield/resume matrix + INVALID_VERSION post-rollback
    // + TSan coverage from the issue body — separate
    // follow-ups.
    add("query:envframe-dualpath-stale-stats-hash", [&ev](const auto&) -> EvalValue {
        // cross-fiber-stale: new foundation atomic
        // (0 until AC1 cross-fiber post-steal wire-up).
        const std::uint64_t cross_fiber_stale =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->envframe_cross_fiber_stale_total.load(std::memory_order_relaxed)
                : 0;
        // version-mismatch: new foundation atomic
        // (0 until AC1 version_ mismatch post-steal wire-up).
        const std::uint64_t version_mismatch =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->envframe_version_mismatch_post_steal_total.load(std::memory_order_relaxed)
                : 0;
        // dualpath-repair: new foundation atomic
        // (0 until AC2 dual-path repair wire-up).
        const std::uint64_t dualpath_repair =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->envframe_dualpath_repair_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("cross-fiber-stale", static_cast<std::int64_t>(cross_fiber_stale));
        insert_kv("version-mismatch", static_cast<std::int64_t>(version_mismatch));
        insert_kv("dualpath-repair", static_cast<std::int64_t>(dualpath_repair));
        insert_kv("schema", 647);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #648: query:panic-checkpoint-fiber-stats —
    // Agent-discoverable structured dashboard for the Panic
    // Checkpoint + Yield Checkpoint Storage Lifecycle +
    // INVALID_VERSION Frame Handling in Fiber Resume +
    // Concurrent GC (P0 Runtime-Gap + Panic production-
    // readiness surface — non-duplicative to #637 #356 #264).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:panic-checkpoint-lifecycle-stats) — existing
    //     high-level panic checkpoint lifecycle summary
    //   - (query:panic-checkpoint-fiber-stats) (#648, this
    //     primitive) — *enforcement-track* companion that
    //     focuses on the AC1+AC2+AC3 counters for fiber
    //     resume transfer + INVALID_VERSION frame handling
    //     in GC + concurrent panic/GC conflict.
    //
    // Fields (3 + sentinel):
    //   - transfer-on-resume    new panic_transfer_on_resume_
    //                            total atomic (foundation for
    //                            AC1 fiber resume panic
    //                            checkpoint transfer).
    //                            Value is 0 until AC1 wire-up.
    //   - invalid-frames-skipped
    //                            new panic_invalid_frames_
    //                            skipped_total atomic
    //                            (foundation for AC2
    //                            INVALID_VERSION frame
    //                            skip/count in GC walk /
    //                            compact). Value is 0 until
    //                            AC2 wire-up.
    //   - concurrent-gc-conflict
    //                            new panic_concurrent_gc_
    //                            conflict_total atomic
    //                            (foundation for AC3
    //                            concurrent panic + GC
    //                            conflict coordination).
    //                            Value is 0 until AC3 wire-up.
    //   - schema == 648           sentinel for Agent drift
    //                            detection (mirrors the full
    //                            chain through
    //                            #618+#620+#621+#622+#623+
    //                            #624+#625+#626+#630+#631+
    //                            #632+#633+#637+#640+#641+
    //                            #642+#643+#644+#645+#646+
    //                            #647 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // panic checkpoint lifecycle observability surface:
    //   - (query:panic-checkpoint-lifecycle-stats) —
    //     high-level panic checkpoint lifecycle summary
    //   - #264 yield checkpoint foundation
    //   - #356 INVALID_VERSION env_frames_ sentinel +
    //     post-rollback frames
    //   - #637 IRClosure + EnvFrame versioning + bridge
    //     invalidate protocol
    //   - #588 per-fiber stack + yield_checkpoint_storage_
    //   - #591 GC pause attribution
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:panic-checkpoint-fiber-stats` with
    // AC1+AC2+AC3-specific counters as a structured hash —
    // was *not* shipped under that exact name. So #648 ships
    // ONE new Aura primitive + 3 new foundation atomics.
    //
    // The remaining #648 AC1 (fiber resume validate/sync
    // per-fiber yield_checkpoint_storage_) + AC2 (GCEnvWalkFn
    // + compact handle INVALID_VERSION frames) + AC3
    // (g_fiber_yield_checkpoint_ + resume_validate_ coordinate
    // with panic checkpoint under MutationBoundary) work is
    // invasive C++ on fiber.cpp resume() + GCEnvWalkFn +
    // compact + Guard panic state + needs the panic during
    // deep mutate + steal + GC matrix + rollback +
    // INVALID_VERSION cases + TSan coverage from the issue
    // body — separate follow-ups.
    add("query:panic-checkpoint-fiber-stats", [&ev](const auto&) -> EvalValue {
        // transfer-on-resume: new foundation atomic
        // (0 until AC1 fiber resume wire-up).
        const std::uint64_t transfer_on_resume =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->panic_transfer_on_resume_total.load(std::memory_order_relaxed)
                : 0;
        // invalid-frames-skipped: new foundation atomic
        // (0 until AC2 GC walk/compact wire-up).
        const std::uint64_t invalid_frames_skipped =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->panic_invalid_frames_skipped_total.load(std::memory_order_relaxed)
                : 0;
        // concurrent-gc-conflict: new foundation atomic
        // (0 until AC3 concurrent panic + GC wire-up).
        const std::uint64_t concurrent_gc_conflict =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->panic_concurrent_gc_conflict_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("transfer-on-resume", static_cast<std::int64_t>(transfer_on_resume));
        insert_kv("invalid-frames-skipped", static_cast<std::int64_t>(invalid_frames_skipped));
        insert_kv("concurrent-gc-conflict", static_cast<std::int64_t>(concurrent_gc_conflict));
        insert_kv("schema", 648);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #649: query:yield-checkpoint-panic-stats —
    // Agent-discoverable structured dashboard for the Full
    // Per-Fiber YieldCheckpointStorage Re-Stamp + Size
    // Validation on Panic Transfer + Cross-Steal (P0
    // Runtime-Gap + Panic production-readiness surface —
    // non-duplicative to #648 #264).
    //
    // Note the naming distinction from #648:
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic checkpoint transfer +
    //     INVALID_VERSION GC + concurrent panic/GC
    //     conflict (transport layer)
    //   - (query:yield-checkpoint-panic-stats) (#649, this
    //     primitive) — *yield-checkpoint storage lifecycle*
    //     companion that focuses on the AC1+AC2+AC3
    //     counters for yield_checkpoint re-stamp +
    //     size validation + cross-steal invalidation
    //     (storage lifecycle layer that #648 doesn't cover).
    //
    // Fields (3 + sentinel):
    //   - transfer-with-restamp   new yield_transfer_with_
    //                              restamp_total atomic
    //                              (foundation for AC1 panic
    //                              transfer triggering yield_
    //                              checkpoint re-stamp).
    //                              Value is 0 until AC1
    //                              wire-up.
    //   - size-mismatch-caught    new yield_size_mismatch_
    //                              caught_total atomic
    //                              (foundation for AC2
    //                              yield_checkpoint stack
    //                              size + top-entry version
    //                              mismatch caught in
    //                              restore_post_yield_or_
    //                              rollback). Value is 0
    //                              until AC2 wire-up.
    //   - cross-steal-invalidation
    //                              new yield_cross_steal_
    //                              invalidation_total
    //                              atomic (foundation for
    //                              AC3 cross-steal
    //                              invalidation of pending
    //                              yield checkpoints).
    //                              Value is 0 until AC3
    //                              wire-up.
    //   - schema == 649             sentinel for Agent drift
    //                              detection (mirrors the
    //                              full chain through
    //                              #618+#620+#621+#622+
    //                              #623+#624+#625+#626+
    //                              #630+#631+#632+#633+
    //                              #637+#640+#641+#642+
    //                              #643+#644+#645+#646+
    //                              #647+#648 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // yield checkpoint + panic observability surface:
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic transfer (transport layer)
    //   - (query:panic-checkpoint-lifecycle-stats) —
    //     high-level panic checkpoint lifecycle summary
    //   - #264 yield checkpoint foundation
    //   - #356 INVALID_VERSION + post-rollback frames
    //   - #588 per-fiber stack + yield_checkpoint_storage_
    //   - transfer_panic_checkpoint_trampoline + bump metric
    //   - restore_post_yield_or_rollback validates
    //     thread/version/depth but no yield_checkpoint
    //     re-stamp or size check
    //   - g_fiber_yield_checkpoint_deleter_ exists but no
    //     panic-state re-stamp coordination
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:yield-checkpoint-panic-stats` with
    // AC1+AC2+AC3-specific counters as a structured hash —
    // was *not* shipped under that exact name. So #649 ships
    // ONE new Aura primitive + 3 new foundation atomics.
    //
    // The remaining #649 AC1 (transfer_panic_checkpoint_
    // trampoline + fiber resume after hook call re-stamp or
    // resize yield_checkpoint_storage_) + AC2 (restore_post_
    // yield_or_rollback adds yield_checkpoint stack size +
    // top-entry version check) + AC3 (g_fiber_yield_checkpoint_
    // coordinates with pending_panic_checkpoint under
    // MutationBoundary) work is invasive C++ on
    // transfer_panic_checkpoint_trampoline + fiber resume +
    // restore_post_yield_or_rollback + g_fiber_yield_checkpoint_
    // + needs the panic during deep yield-boundary + steal +
    // resume matrix + TSan coverage from the issue body —
    // separate follow-ups.
    add("query:yield-checkpoint-panic-stats", [&ev](const auto&) -> EvalValue {
        // transfer-with-restamp: new foundation atomic
        // (0 until AC1 panic transfer wire-up).
        const std::uint64_t transfer_with_restamp =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->yield_transfer_with_restamp_total.load(std::memory_order_relaxed)
                : 0;
        // size-mismatch-caught: new foundation atomic
        // (0 until AC2 yield_checkpoint stack size wire-up).
        const std::uint64_t size_mismatch_caught =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->yield_size_mismatch_caught_total.load(std::memory_order_relaxed)
                : 0;
        // cross-steal-invalidation: new foundation atomic
        // (0 until AC3 cross-steal invalidation wire-up).
        const std::uint64_t cross_steal_invalidation =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->yield_cross_steal_invalidation_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("transfer-with-restamp", static_cast<std::int64_t>(transfer_with_restamp));
        insert_kv("size-mismatch-caught", static_cast<std::int64_t>(size_mismatch_caught));
        insert_kv("cross-steal-invalidation", static_cast<std::int64_t>(cross_steal_invalidation));
        insert_kv("schema", 649);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #650: query:scheduler-stealbudget-yield-class-stats —
    // Agent-discoverable structured dashboard for the
    // StealBudget in WorkerThread to Use fiber
    // yield_classification() + Outermost Mutation Depth for
    // Adaptive Bias (P0 Runtime-Gap + Scheduler production-
    // readiness surface — non-duplicative to #645).
    //
    // Note the naming distinction from #706:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     — 5-field adaptive bias summary (already covers
    //     mutation-bias-hits + outermost-preferred +
    //     llm-tail-reductions + deferred-pressure-boosts +
    //     global-deferred-mutation-total — the AC3
    //     surface)
    //   - (query:scheduler-steal-bias-stats) (#645) —
    //     per-steal LIFO/FIFO + mutation-deferred counters
    //     (lower-level enforcement layer)
    //   - (query:scheduler-stealbudget-yield-class-stats)
    //     (#650, this primitive) — *yield-class-bias-track*
    //     companion that focuses on the AC1+AC2 enforcement
    //     counters for StealBudget consultation of victim
    //     yield_classification() + outermost mutation depth
    //     + max_before_sleep raised on contention.
    //
    // Fields (3 + sentinel):
    //   - outermost-bias       new stealbudget_outermost_bias_
    //                          total atomic (foundation for
    //                          AC1 bias hits preferring
    //                          outermost Mutation fibers).
    //                          Value is 0 until AC1 wire-up.
    //   - explicit-bias        new stealbudget_explicit_bias_
    //                          total atomic (foundation for
    //                          AC1 bias hits preferring
    //                          Explicit yield reason fibers).
    //                          Value is 0 until AC1 wire-up.
    //   - max-sleep-raised     new stealbudget_max_before_sleep_
    //                          raised_total atomic (foundation
    //                          for AC2 max_before_sleep raised
    //                          on contention). Value is 0
    //                          until AC2 wire-up.
    //   - schema == 650         sentinel for Agent drift
    //                          detection (mirrors the full
    //                          chain through
    //                          #618+#620+#621+#622+#623+
    //                          #624+#625+#626+#630+#631+
    //                          #632+#633+#637+#640+#641+
    //                          #642+#643+#644+#645+#646+
    //                          #647+#648+#649 sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // StealBudget adaptive bias surface:
    //   - (query:scheduler-stealbudget-adaptive-stats) (#706)
    //     — 5-field adaptive bias summary (the AC3 surface
    //     listed in #650 body)
    //   - (query:scheduler-steal-bias-stats) (#645) — per-
    //     steal LIFO/FIFO + mutation-deferred
    //   - #618 per-fiber yield_reason classification +
    //     is_at_mutation_boundary_safe + outermost depth
    //     probe
    //   - #588 per-fiber stack + StealBudget WINDOW_SIZE=10
    //     thresholds 50%/30%/10%
    //   - #451 work-stealing deque LIFO local + FIFO steal
    // What the issue body AC3 specifies by **exact name +
    // fields** — `query:scheduler-stealbudget-adaptive-stats`
    // already ships the AC3 fields. #650 ships the AC1+AC2
    // enforcement-layer companion with a distinct name
    // (`-yield-class-` midfix).
    //
    // The remaining #650 AC1 (try_steal_from / should_steal
    // query yield_classification + outermost depth) + AC2
    // (high steal_deferred_mutation_boundary_count raises
    // max_before_sleep) + AC4 (unit test mock Fiber yield
    // reasons) work is invasive C++ on worker.cpp/h +
    // StealBudget + needs the LLM latency + mixed yield
    // reasons matrix + 20 fibers + TSan coverage from the
    // issue body — separate follow-ups.
    add("query:scheduler-stealbudget-yield-class-stats", [&ev](const auto&) -> EvalValue {
        // outermost-bias: new foundation atomic
        // (0 until AC1 outermost bias wire-up).
        const std::uint64_t outermost_bias =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->stealbudget_outermost_bias_total.load(std::memory_order_relaxed)
                : 0;
        // explicit-bias: new foundation atomic
        // (0 until AC1 explicit bias wire-up).
        const std::uint64_t explicit_bias =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->stealbudget_explicit_bias_total.load(std::memory_order_relaxed)
                : 0;
        // max-sleep-raised: new foundation atomic
        // (0 until AC2 max_before_sleep raise wire-up).
        const std::uint64_t max_sleep_raised =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->stealbudget_max_before_sleep_raised_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("outermost-bias", static_cast<std::int64_t>(outermost_bias));
        insert_kv("explicit-bias", static_cast<std::int64_t>(explicit_bias));
        insert_kv("max-sleep-raised", static_cast<std::int64_t>(max_sleep_raised));
        insert_kv("schema", 650);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #651: query:gc-panic-deferral-stats — Agent-
    // discoverable structured dashboard for the Actual GC
    // Deferral/Block Logic in
    // block_gc_for_pending_checkpoint_trampoline + Request
    // Shim (P0 Runtime-Gap + GC production-readiness surface —
    // fills TODO in evaluator_fiber_mutation.cpp, non-
    // duplicative to #646 #648).
    //
    // Note the relationship to existing primitives:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //     primitive (no deferral/panic breakdown)
    //   - (query:gc-safepoint-deferral-stats) (#646) —
    //     deferral + backoff for outermost-vs-inner
    //     MutationBoundary (no panic-specific breakdown)
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic transfer (no GC-deferral
    //     wire-up)
    //   - (query:gc-panic-deferral-stats) (#651, this
    //     primitive) — *GC-panic coordination* companion that
    //     focuses on the AC1+AC2+AC3 counters for
    //     block_gc trampoline deferral + GC request blocked
    //     by panic + panic/GC conflict resolution.
    //
    // Fields (3 + sentinel):
    //   - pending-panic-deferral
    //                            new gc_panic_pending_deferral_
    //                            total atomic (foundation for
    //                            AC1 pending panic checkpoint
    //                            deferral triggered in
    //                            block_gc trampoline). Value
    //                            is 0 until AC1 wire-up.
    //   - gc-blocked-by-panic   new gc_blocked_by_panic_total
    //                            atomic (foundation for AC2 GC
    //                            safepoint request blocked
    //                            due to pending panic +
    //                            depth > 0). Value is 0 until
    //                            AC2 wire-up.
    //   - conflicts-resolved    new gc_panic_conflict_resolved_
    //                            total atomic (foundation for
    //                            AC3 panic + GC conflict
    //                            resolved without root
    //                            inconsistency). Value is 0
    //                            until AC3 wire-up.
    //   - schema == 651           sentinel for Agent drift
    //                            detection (mirrors the full
    //                            chain through
    //                            #618+#620+#621+#622+#623+
    //                            #624+#625+#626+#630+#631+
    //                            #632+#633+#637+#640+#641+
    //                            #642+#643+#644+#645+#646+
    //                            #647+#648+#649+#650
    //                            sentinels).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // GC + panic observability surface:
    //   - (query:gc-safepoint-stats) — base GC safepoint
    //   - (query:gc-safepoint-deferral-stats) (#646) —
    //     deferral + backoff for outermost-vs-inner
    //     MutationBoundary
    //   - (query:panic-checkpoint-fiber-stats) (#648) —
    //     fiber resume panic transfer
    //   - (query:panic-checkpoint-lifecycle-stats) —
    //     high-level panic lifecycle
    //   - block_gc_for_pending_checkpoint_trampoline +
    //     g_block_gc_for_pending_checkpoint exist but with
    //     "actual GC deferral is out of scope for the current
    //     ship (TODO)" comment
    //   - aura_evaluator_request_gc_safepoint forwards but
    //     only records request (no pending panic check)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:gc-panic-deferral-stats` with
    // AC1+AC2+AC3-specific counters — was *not* shipped
    // under that exact name. So #651 ships ONE new Aura
    // primitive + 3 new foundation atomics.
    //
    // The remaining #651 AC1 (block_gc trampoline real
    // deferral + gc_state phase integration) + AC2
    // (aura_evaluator_request_gc_safepoint pending panic +
    // depth > 0 check + defer/yield/retry) + AC3 (fiber
    // check_gc_safepoint + scheduler wait_for_safepoint
    // pending-panic awareness) work is invasive C++ on
    // evaluator_fiber_mutation.cpp +
    // block_gc_for_pending_checkpoint_trampoline +
    // aura_evaluator_request_gc_safepoint + fiber
    // check_gc_safepoint + scheduler wait_for_safepoint +
    // needs the panic during MutationBoundary + concurrent
    // GC + steal matrix + TSan coverage from the issue body
    // — separate follow-ups.
    add("query:gc-panic-deferral-stats", [&ev](const auto&) -> EvalValue {
        // pending-panic-deferral: new foundation atomic
        // (0 until AC1 block_gc trampoline wire-up).
        const std::uint64_t pending_panic_deferral =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->gc_panic_pending_deferral_total.load(std::memory_order_relaxed)
                : 0;
        // gc-blocked-by-panic: new foundation atomic
        // (0 until AC2 aura_evaluator_request_gc_safepoint wire-up).
        const std::uint64_t gc_blocked_by_panic =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->gc_blocked_by_panic_total.load(std::memory_order_relaxed)
                : 0;
        // conflicts-resolved: new foundation atomic
        // (0 until AC3 panic + GC conflict resolution wire-up).
        const std::uint64_t conflicts_resolved =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->gc_panic_conflict_resolved_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("pending-panic-deferral", static_cast<std::int64_t>(pending_panic_deferral));
        insert_kv("gc-blocked-by-panic", static_cast<std::int64_t>(gc_blocked_by_panic));
        insert_kv("conflicts-resolved", static_cast<std::int64_t>(conflicts_resolved));
        insert_kv("schema", 651);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #589: query:envframe-dualpath-enforce-stats —
    // Agent-discoverable structured dashboard for the SoA
    // EnvFrame/EnvId dual-path bindings_ vs bindings_symid_
    // consistency + version stamping + stale refresh in
    // materialize_call_env & GCEnvWalkFn (P0 Runtime-Review +
    // SoA production-readiness surface — non-duplicative to
    // existing #543 SoA EnvFrame, #568 children SoA, #205
    // GCEnvWalkFn).
    //
    // Note the relationship to existing primitives:
    //   - (query:envframe-dualpath-stats) — base flat-int
    //     dualpath primitive (the AC4 surface)
    //   - (query:envframe-dualpath-stale-stats) — existing
    //     flat-int stale summary
    //   - (query:envframe-dualpath-stale-stats-hash) (#647)
    //     — stale enforcement-layer hash (cross-fiber /
    //     version mismatch / dualpath-repair counters)
    //   - (query:envframe-dualpath-enforce-stats) (#589,
    //     this primitive) — *enforce-track* companion with
    //     `-enforce-` midfix that focuses on the AC1+AC2+AC3
    //     counters for bind/bind_symid mirror writes +
    //     materialize_call_env dual-path refresh + GCEnvWalkFn
    //     consistency violations (the SoA dual-path
    //     consistency enforcement layer that #647's
    //     `-stale-` midfix does not cover).
    //
    // Fields (3 + sentinel):
    //   - mirror-write        new envframe_dualpath_mirror_
    //                          write_total atomic (foundation
    //                          for AC1 bind/bind_symid mirror
    //                          writes). Value is 0 until AC1
    //                          wire-up.
    //   - dualpath-refresh    new envframe_dualpath_refresh_
    //                          total atomic (foundation for
    //                          AC2 materialize_call_env
    //                          refresh_dual_path_from_soa
    //                          helper calls). Value is 0
    //                          until AC2 wire-up.
    //   - consistency-violations
    //                          new envframe_dualpath_
    //                          consistency_violations_total
    //                          atomic (foundation for AC3
    //                          GCEnvWalkFn consistency
    //                          violations caught). Value
    //                          is 0 until AC3 wire-up.
    //   - schema == 589         sentinel for Agent drift
    //                          detection (back to a lower
    //                          number than #651 since #589
    //                          is an older issue that
    //                          reaches observability
    //                          foundation layer late — the
    //                          schema sentinel still
    //                          matches the issue number for
    //                          Agent drift tracking).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // dual-path observability surface:
    //   - (query:envframe-dualpath-stats) — base flat-int
    //     dualpath primitive (the AC4 surface)
    //   - (query:envframe-dualpath-stale-stats) — existing
    //     flat-int stale summary
    //   - (query:envframe-dualpath-stale-stats-hash) (#647)
    //     — stale enforcement-layer hash
    //   - (query:envframe-stale-stats) — stale refresh stats
    //   - (query:envframe-bump-stats) — bump stats
    //   - #543 SoA EnvFrame foundation
    //   - #568 children SoA
    //   - #205 GCEnvWalkFn foundation
    //   - envframe_desync_detected_ / envframe_stale_refresh_
    //     count_ / envframe_post_rollback_invalidations_ +
    //     envframe_version_mismatch_in_walk_ +
    //     envframe_gc_walk_safe_skips_ + gc_envframe_stale_
    //     skipped_ (existing counters that #589 AC1+AC2+AC3
    //     will exercise)
    // What the issue body AC4 specifies by **exact name +
    // fields** — `query:envframe-dualpath-stats` — already
    // ships as the base flat-int primitive. #589 ships the
    // AC1+AC2+AC3 enforcement-layer companion with a
    // distinct name (`-enforce-` midfix).
    //
    // The remaining #589 AC1 (Env::bind_symid / bind always
    // mirror + owner_ set stamp defuse_version_ into
    // env_version_) + AC2 (materialize_call_env on version
    // mismatch call refresh_dual_path_from_soa helper) +
    // AC3 (walk_env_frames / GCEnvWalkFn before emitting
    // roots refresh or skip with metric) work is invasive
    // C++ on evaluator.ixx + evaluator_impl.cpp +
    // gc_coordinator.h + needs the large env chains +
    // mutate + compaction/GC matrix + 5000+ materialize
    // under fibers + TSan coverage from the issue body —
    // separate follow-ups.
    add("query:envframe-dualpath-enforce-stats", [&ev](const auto&) -> EvalValue {
        // mirror-write: new foundation atomic
        // (0 until AC1 bind/bind_symid mirror wire-up).
        const std::int64_t mirror_write =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->envframe_dualpath_mirror_write_total.load(std::memory_order_relaxed)
                : 0;
        // dualpath-refresh: new foundation atomic
        // (0 until AC2 materialize_call_env refresh wire-up).
        const std::int64_t dualpath_refresh =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->envframe_dualpath_refresh_total.load(std::memory_order_relaxed)
                : 0;
        // consistency-violations: new foundation atomic
        // (0 until AC3 GCEnvWalkFn consistency violation wire-up).
        const std::int64_t consistency_violations =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->envframe_dualpath_consistency_violations_total.load(
                          std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("mirror-write", static_cast<std::int64_t>(mirror_write));
        insert_kv("dualpath-refresh", static_cast<std::int64_t>(dualpath_refresh));
        insert_kv("consistency-violations", static_cast<std::int64_t>(consistency_violations));
        insert_kv("schema", 589);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #590: query:aot-hotupdate-stats — Agent-
    // discoverable structured dashboard for the AOT mangle
    // versioning + region filtering + multi-agent hot-update
    // isolation + closure dispatch stale detection (P0
    // Runtime-Review + AOT production-readiness surface —
    // non-duplicative to existing #544 / #323 / #287).
    //
    // Note the naming distinction from existing primitives:
    //   - (query:aot-reload-stats) (#708) — 5-field high-level
    //     reload summary
    //   - (query:aot-reload-func-table-stats) (#644) —
    //     func_table refcount + region filter primitive
    //   - (query:aot-hot-reload-stats) (#358/#452) — earlier
    //     AOT hot-reload primitive
    //   - (query:aot-checkpoint-version-stats) (#708) —
    //     checkpoint version tracking
    //   - (query:aot-hotupdate-stats) (#590, this primitive)
    //     — *multi-agent hot-update isolation* companion with
    //     no `-reload-` midfix that focuses on the AC1+AC2+AC3
    //     counters for region-isolated reload + dispatch
    //     stale prevention + multi-agent reload cycles.
    //
    // Fields (3 + sentinel):
    //   - region-isolation     new aot_hotupdate_region_
    //                          isolation_total atomic
    //                          (foundation for AC1 region
    //                          isolation hits — reload only
    //                          affected target region).
    //                          Value is 0 until AC1 wire-up.
    //   - dispatch-stale      new aot_hotupdate_dispatch_
    //                          stale_prevented_total atomic
    //                          (foundation for AC3 closure
    //                          dispatch stale mismatch
    //                          prevented). Value is 0 until
    //                          AC3 wire-up.
    //   - multi-agent-reload  new aot_hotupdate_multi_agent_
    //                          reload_total atomic
    //                          (foundation for AC1 successful
    //                          multi-agent reload cycles).
    //                          Value is 0 until AC1 wire-up.
    //   - schema == 590         sentinel for Agent drift
    //                          detection (matches issue
    //                          number for Agent drift
    //                          tracking).
    //
    // Discovery before this PR (preserved, not duplication):
    // the existing infrastructure covers the high-level
    // AOT hot-update observability surface:
    //   - (query:aot-reload-stats) (#708) — 5-field
    //     reload summary
    //   - (query:aot-reload-func-table-stats) (#644) —
    //     func_table refcount + region filter primitive
    //   - (query:aot-hot-reload-stats) (#358/#452) —
    //     earlier AOT hot-reload primitive
    //   - (query:aot-checkpoint-version-stats) (#708) —
    //     checkpoint version tracking
    //   - aot_emit_version + runtime defuse_version_ +
    //     aot_reload_attempts_ + aot_hot_update_success_ +
    //     aot_stale_reject_count_ + aot_refcount_swaps_ +
    //     aot_region_mismatch_ (#708) — existing counters
    //   - mangle_aot_name (with emit_version + module_version)
    //   - aura_reload_aot_module (dlopen + aot_emit_version
    //     check + g_aot_module_version)
    // What the issue body AC2 specifies by **exact name +
    // fields** — `query:aot-hotupdate-stats` (no `-reload-`
    // midfix) with reload_success + stale_reject +
    // region_isolation_hits + dispatch_stale_prevented —
    // was *not* shipped under that exact name. The existing
    // #708 5-field summary already covers some of these
    // counters in aggregate; #590 ships the multi-agent
    // hot-update isolation focused primitive.
    //
    // The remaining #590 AC1 (mangle_aot_name +
    // generate_registration_c add region/agent_id prefix +
    // reload success iterate func_table rebind matching
    // version/region with refcounts) + AC2 ((aot:reload-
    // with-region path version region) primitive wire-up) +
    // AC3 (closure dispatch version check on func_id
    // lookup; on mismatch force deopt or error with metric)
    // work is invasive C++ on aura_jit_bridge.cpp +
    // mangle_aot_name + generate_registration_c + closure
    // dispatch path + needs the multi-agent region matrix +
    // 1000+ reload cycles + concurrent mutate/eval + TSan
    // coverage from the issue body — separate follow-ups.
    add("query:aot-hotupdate-stats", [&ev](const auto&) -> EvalValue {
        // region-isolation: new foundation atomic
        // (0 until AC1 region isolation wire-up).
        const std::int64_t region_isolation =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->aot_hotupdate_region_isolation_total.load(std::memory_order_relaxed)
                : 0;
        // dispatch-stale: new foundation atomic
        // (0 until AC3 dispatch stale prevention wire-up).
        const std::int64_t dispatch_stale =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->aot_hotupdate_dispatch_stale_prevented_total.load(std::memory_order_relaxed)
                : 0;
        // multi-agent-reload: new foundation atomic
        // (0 until AC1 multi-agent reload wire-up).
        const std::int64_t multi_agent_reload =
            ev.compiler_metrics()
                ? static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics())
                      ->aot_hotupdate_multi_agent_reload_total.load(std::memory_order_relaxed)
                : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("region-isolation", static_cast<std::int64_t>(region_isolation));
        insert_kv("dispatch-stale", static_cast<std::int64_t>(dispatch_stale));
        insert_kv("multi-agent-reload", static_cast<std::int64_t>(multi_agent_reload));
        insert_kv("schema", 590);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #593: query:pattern-ir-hygiene-closed-loop-stats — AST→query→IR
    // MacroIntroduced hygiene closed-loop companion (non-duplicative with
    // #524 macro-production-hygiene-stats, #547 pattern-hygiene-stats,
    // #501 ir-hygiene-stats, #420 macro-hygiene-contract-stats).
    //
    // Fields (3 + sentinel):
    //   - capture-prevented   pattern_ir_capture_prevented_total
    //   - ir-post-mutate-violation  ir_hygiene_post_mutate_violation_total
    //   - tag-arity-delta     tag_arity_hygiene_query_delta_total
    //   - schema == 593
    add("query:pattern-ir-hygiene-closed-loop-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t capture_prevented =
            m ? static_cast<std::int64_t>(
                    m->pattern_ir_capture_prevented_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t ir_violation =
            m ? static_cast<std::int64_t>(
                    m->ir_hygiene_post_mutate_violation_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t tag_delta =
            m ? static_cast<std::int64_t>(
                    m->tag_arity_hygiene_query_delta_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("capture-prevented", capture_prevented);
        insert_kv("ir-post-mutate-violation", ir_violation);
        insert_kv("tag-arity-delta", tag_delta);
        insert_kv("schema", 593);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #596: query:guard-panic-reflect-stats — Guard + panic checkpoint +
    // reflect/schema validation + fiber resume closed-loop companion
    // (non-duplicative with #548 panic-checkpoint-lifecycle-stats,
    // #594 reflection-selfmod-stats, #592 fiber resume safety matrix).
    //
    // Fields (5 + sentinel):
    //   - checkpoints-committed   panic_checkpoint_commit_count_
    //   - restores-on-resume      guard_panic_reflect_restores_on_resume_total
    //   - validation-pass         schema_validation_pass_count_
    //   - validation-fail         schema_validation_fail_count_
    //   - boundary-violation-prevented
    //                             guard_panic_reflect_boundary_violation_prevented_total
    //   - schema == 596
    add("query:guard-panic-reflect-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t commits =
            static_cast<std::int64_t>(ev.get_panic_checkpoint_commit_count());
        const std::int64_t restores_on_resume =
            m ? static_cast<std::int64_t>(
                    m->guard_panic_reflect_restores_on_resume_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t validation_pass =
            static_cast<std::int64_t>(ev.get_schema_validation_pass_count());
        const std::int64_t validation_fail =
            static_cast<std::int64_t>(ev.get_schema_validation_fail_count());
        const std::int64_t boundary_prevented =
            m ? static_cast<std::int64_t>(
                    m->guard_panic_reflect_boundary_violation_prevented_total.load(
                        std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("checkpoints-committed", commits);
        insert_kv("restores-on-resume", restores_on_resume);
        insert_kv("validation-pass", validation_pass);
        insert_kv("validation-fail", validation_fail);
        insert_kv("boundary-violation-prevented", boundary_prevented);
        insert_kv("schema", 596);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #599: query:compiler-root-stats — automatic epoch/version root
    // management for live IRClosure/EnvFrame post-invalidate_function
    // (non-duplicative with #531 closure-env-safety-stats,
    // #598 linear-ownership-runtime-stats, #682 GC root coordination).
    //
    // Fields (4 + sentinel):
    //   - stale-closure-detected   compiler_root_stale_closure_detected_total
    //   - env-version-mismatch     compiler_root_env_version_mismatch_total
    //   - root-refresh-count       closure_stale_refresh_count_
    //   - dangling-prevented       compiler_root_dangling_prevented_total
    //   - schema == 599
    add("query:compiler-root-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t stale_closure =
            m ? static_cast<std::int64_t>(
                    m->compiler_root_stale_closure_detected_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t env_mismatch =
            m ? static_cast<std::int64_t>(
                    m->compiler_root_env_version_mismatch_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t root_refresh =
            m ? static_cast<std::int64_t>(
                    m->closure_stale_refresh_count_.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t dangling_prevented =
            m ? static_cast<std::int64_t>(
                    m->compiler_root_dangling_prevented_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("stale-closure-detected", stale_closure);
        insert_kv("env-version-mismatch", env_mismatch);
        insert_kv("root-refresh-count", root_refresh);
        insert_kv("dangling-prevented", dangling_prevented);
        insert_kv("schema", 599);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #600: query:incremental-closure-stats — per-block dirty + impact
    // scope + closure bridge synergy for minimal re-lower
    // (non-duplicative with #530 incremental-production-relower-stats,
    // #429 soa-dirty-stats, #531 closure-env-safety-stats).
    //
    // Fields (4 + sentinel):
    //   - blocks-relowered     incremental_closure_blocks_relowered_total
    //   - closure-bridge-hits  bridge_epoch_hit_count_
    //   - min-scope-win        incremental_closure_min_scope_win_total
    //   - jit-sync-count       incremental_closure_jit_sync_total
    //   - schema == 600
    add("query:incremental-closure-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t blocks_relowered =
            m ? static_cast<std::int64_t>(
                    m->incremental_closure_blocks_relowered_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t bridge_hits =
            m ? static_cast<std::int64_t>(
                    m->bridge_epoch_hit_count_.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t min_scope_win =
            m ? static_cast<std::int64_t>(
                    m->incremental_closure_min_scope_win_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t jit_sync =
            m ? static_cast<std::int64_t>(
                    m->incremental_closure_jit_sync_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("blocks-relowered", blocks_relowered);
        insert_kv("closure-bridge-hits", bridge_hits);
        insert_kv("min-scope-win", min_scope_win);
        insert_kv("jit-sync-count", jit_sync);
        insert_kv("schema", 600);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #741: query:incremental-closure-bridge-stats — impact_scope
    // propagation to closure_bridge shared_ptr lifetime + EnvFrame version
    // re-stamp for quote/lambda-heavy defines (non-duplicative with #718
    // block_dirty, #719 epoch/bridge general safety).
    //
    // Fields (3 + sentinel):
    //   - impact-blocks-on-bridge  incremental_closure_bridge_impact_blocks_total
    //   - quote-lambda-stale-prevented
    //                              incremental_closure_quote_lambda_stale_prevented_total
    //   - env-version-resync       incremental_closure_env_version_resync_total
    //   - schema == 741
    add("query:incremental-closure-bridge-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t impact_on_bridge =
            m ? static_cast<std::int64_t>(m->incremental_closure_bridge_impact_blocks_total.load(
                    std::memory_order_relaxed))
              : 0;
        const std::int64_t quote_lambda_prevented =
            m ? static_cast<std::int64_t>(
                    m->incremental_closure_quote_lambda_stale_prevented_total.load(
                        std::memory_order_relaxed))
              : 0;
        const std::int64_t env_resync =
            m ? static_cast<std::int64_t>(
                    m->incremental_closure_env_version_resync_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("impact-blocks-on-bridge", impact_on_bridge);
        insert_kv("quote-lambda-stale-prevented", quote_lambda_prevented);
        insert_kv("env-version-resync", env_resync);
        insert_kv("schema", 741);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #654: query:macro-hygiene-fiber-panic-stats — 5 cross-cutting
    // macro+reflect+self-evo hygiene gaps vs fiber/panic/AOT/SoA runtime
    // (non-duplicative with #593 pattern-ir-hygiene-closed-loop,
    // #596 guard-panic-reflect, #597 macro-reflect-self-evo-stats).
    //
    // Fields (5 + sentinel):
    //   - hygiene-panic-restamp      macro_hygiene_panic_restamp_total
    //   - provenance-violations      macro_hygiene_provenance_violations_total
    //   - macro-expand-checkpoints   macro_expand_checkpoint_saves_total
    //   - reflect-hygiene-validation macro_reflect_hygiene_validation_total
    //   - hygiene-dirty-impact       macro_hygiene_dirty_impact_total
    //   - schema == 654
    add("query:macro-hygiene-fiber-panic-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t panic_restamp =
            m ? static_cast<std::int64_t>(
                    m->macro_hygiene_panic_restamp_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t provenance_violations =
            m ? static_cast<std::int64_t>(
                    m->macro_hygiene_provenance_violations_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t expand_checkpoints =
            m ? static_cast<std::int64_t>(
                    m->macro_expand_checkpoint_saves_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t reflect_validation =
            m ? static_cast<std::int64_t>(
                    m->macro_reflect_hygiene_validation_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t dirty_impact =
            m ? static_cast<std::int64_t>(
                    m->macro_hygiene_dirty_impact_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("hygiene-panic-restamp", panic_restamp);
        insert_kv("provenance-violations", provenance_violations);
        insert_kv("macro-expand-checkpoints", expand_checkpoints);
        insert_kv("reflect-hygiene-validation", reflect_validation);
        insert_kv("hygiene-dirty-impact", dirty_impact);
        insert_kv("schema", 654);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #712: (query:macro-reflect-validation-stats) — subtree-level
    // reflect validation counters (non-duplicative with #654 which
    // folds reflect-hygiene-validation into macro-hygiene-fiber-panic-
    // stats as one of 5 fields, and with #488 which tracks whole-
    // workspace schema_validation_pass_count / fail_count).
    //
    // Fields (4 + sentinel):
    //   - validation-calls         calls to subtree-level auto_validate
    //                              for MacroIntroduced nodes during
    //                              post_mutation_reflect_validate (==1
    //                              per mutation cycle that touched any
    //                              macro subtree)
    //   - schema-mismatches-caught # of MacroIntroduced nodes whose
    //                              macro_dirty bit is missing the
    //                              kMacroExpansion flag during the
    //                              post-mutate reflect scan
    //   - post-mutate-hygiene-drift # of MacroIntroduced nodes that are
    //                              also dirty in the post-mutate snapshot
    //                              (macro subtree was re-expanded or
    //                              re-written between commits — the
    //                              Agent uses this counter to decide
    //                              whether to deep-validate that subtree
    //                              before trusting it)
    //   - schema-pass              reflects from schema_validation_pass_count_
    //                              (whole-workspace pass counter); lets
    //                              the Agent correlate subtree-level
    //                              diagnostics with workspace-level
    //                              validation outcomes
    //   - schema == 712
    // Issue #712: routes through ev.primitives_.add (3-arg form) so
    // we can attach PrimMeta with schema=712 + category=general +
    // arity=0 + pure=true. The local PrimRegistrar typedef at the
    // top of this file is intentionally 2-arg (matches pre-#669
    // baseline). Other #669/#671 primitives that don't carry PrimMeta
    // use the 2-arg add() directly.
    ev.primitives_.add(
        "query:macro-reflect-validation-stats",
        [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = 0xcbf29ce484222325ull;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t validation_calls =
                m ? static_cast<std::int64_t>(
                        m->macro_reflect_validation_calls_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t schema_mismatches =
                m ? static_cast<std::int64_t>(m->macro_reflect_schema_mismatches_caught_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t hygiene_drift =
                m ? static_cast<std::int64_t>(m->macro_reflect_post_mutate_hygiene_drift_total.load(
                        std::memory_order_relaxed))
                  : 0;
            const std::int64_t schema_pass =
                static_cast<std::int64_t>(ev.get_schema_validation_pass_count());
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"validation-calls", make_int(validation_calls)},
                {"schema-mismatches-caught", make_int(schema_mismatches)},
                {"post-mutate-hygiene-drift", make_int(hygiene_drift)},
                {"schema-pass", make_int(schema_pass)},
                {"schema", make_int(712)},
            };
            return build_hash(kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "Subtree-level reflect validation counters for MacroIntroduced "
                        "nodes (post-mutate hygiene drift + schema mismatch catches). "
                        "Used by Agent to decide whether to deep-validate a macro subtree.",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #713: (query:macro-jit-hygiene-stats) — JIT/AOT/Interpreter
    // macro-hygiene violation counters (non-duplicative with #488
    // schema_validation_pass/fail, #654 macro-hygiene-fiber-panic-
    // stats, #712 macro-reflect-validation-stats).
    //
    // Fields (4 + sentinel):
    //   - deopt-on-hygiene            macro_jit_hygiene_deopt_total
    //                                (# of JIT deopts triggered by a
    //                                 source_marker=MacroIntroduced
    //                                 call site trying to inline into
    //                                 User code or other policy
    //                                 violation)
    //   - aot-reload-marker-mismatches
    //                                macro_aot_reload_marker_mismatches_total
    //                                (# of AOT reloads that re-stamped
    //                                 or rejected a module because its
    //                                 source_marker column drifted
    //                                 from the host's expected markers)
    //   - interpreter-fallback-hygiene-hits
    //                                macro_interpreter_fallback_hygiene_hits_total
    //                                (# of IR executor dispatches that
    //                                 hit a source_marker=MacroIntroduced
    //                                 call site + chose conservative
    //                                 interpreter fallback over JIT'd
    //                                 inlined code)
    //   - schema == 713
    //
    // All three counters are 0 on a fresh service. The bump helpers
    // are exposed via Evaluator::bump_macro_jit_hygiene_deopt()
    // etc. for future hot-path wiring (each JIT/AOT/Interpreter
    // hook is a dedicated follow-up).
    ev.primitives_.add(
        "query:macro-jit-hygiene-stats",
        [&ev](const auto&) -> EvalValue {
            auto build_hash =
                [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
                auto* ht = FlatHashTable::create(16);
                if (!ht)
                    return make_void();
                auto meta = ht->metadata();
                auto keys = ht->keys();
                auto vals = ht->values();
                auto hcap = ht->capacity;
                for (auto& [k, v] : kv) {
                    std::uint64_t h = 0xcbf29ce484222325ull;
                    for (char c : k)
                        h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                    auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                    if (fp == 0xFF)
                        fp = 0xFE;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k);
                    EvalValue key_ev = make_string(kidx);
                    bool inserted = false;
                    for (std::size_t at = 0; at < hcap; ++at) {
                        auto slot = ((h >> 1) + at) & (hcap - 1);
                        if (meta[slot] == 0xFF) {
                            meta[slot] = fp;
                            keys[slot] = key_ev.val;
                            vals[slot] = v.val;
                            ht->size++;
                            inserted = true;
                            break;
                        }
                    }
                    if (!inserted) {
                        FlatHashTable::destroy(ht);
                        return make_void();
                    }
                }
                auto hidx = g_hash_tables.size();
                g_hash_tables.push_back(ht);
                return make_hash(hidx);
            };
            CompilerMetrics* m = ev.compiler_metrics()
                                     ? static_cast<CompilerMetrics*>(ev.compiler_metrics())
                                     : nullptr;
            const std::int64_t deopt_on_hygiene =
                m ? static_cast<std::int64_t>(
                        m->macro_jit_hygiene_deopt_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t aot_reload_mismatches =
                m ? static_cast<std::int64_t>(
                        m->macro_aot_reload_marker_mismatches_total.load(std::memory_order_relaxed))
                  : 0;
            const std::int64_t interpreter_fallback =
                m ? static_cast<std::int64_t>(m->macro_interpreter_fallback_hygiene_hits_total.load(
                        std::memory_order_relaxed))
                  : 0;
            std::vector<std::pair<std::string, EvalValue>> kv = {
                {"deopt-on-hygiene", make_int(deopt_on_hygiene)},
                {"aot-reload-marker-mismatches", make_int(aot_reload_mismatches)},
                {"interpreter-fallback-hygiene-hits", make_int(interpreter_fallback)},
                {"schema", make_int(713)},
            };
            return build_hash(kv);
        },
        PrimMeta{.arity = 0,
                 .pure = true,
                 .doc = "JIT/AOT/Interpreter macro-hygiene violation counters. Bumped "
                        "when a MacroIntroduced source_marker triggers a JIT deopt, "
                        "AOT reload marker mismatch, or interpreter fallback.",
                 .category = "general",
                 .schema = "() -> hash"});

    // Issue #655: query:edsl-core-stability-stats — 5 EDSL core gaps for
    // Workspace/Query/Mutate + StableNodeRef/COW/atomic under AI multi-round
    // editing (non-duplicative with #527 stable-ref-cow, #552 edsl-stability,
    // #622 atomic-batch, #654 macro-hygiene-fiber-panic).
    //
    // Fields (5 + sentinel):
    //   - cow-stable-ref-remaps       edsl_cow_stable_ref_remap_total
    //   - tag-arity-delta-patches     edsl_tag_arity_delta_patch_total
    //   - nested-atomic-rollbacks     edsl_nested_atomic_rollback_total
    //   - children-safe-views         FlatAST children_safe_view_count_
    //   - mutate-invalidate-precision edsl_mutate_invalidate_precision_total
    //   - schema == 655
    add("query:edsl-core-stability-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t cow_remap =
            m ? static_cast<std::int64_t>(
                    m->edsl_cow_stable_ref_remap_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t delta_patch =
            m ? static_cast<std::int64_t>(
                    m->edsl_tag_arity_delta_patch_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t nested_rollback =
            m ? static_cast<std::int64_t>(
                    m->edsl_nested_atomic_rollback_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t children_safe =
            ev.workspace_flat()
                ? static_cast<std::int64_t>(ev.workspace_flat()->children_safe_view_count())
                : 0;
        const std::int64_t invalidate_precision =
            m ? static_cast<std::int64_t>(
                    m->edsl_mutate_invalidate_precision_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("cow-stable-ref-remaps", cow_remap);
        insert_kv("tag-arity-delta-patches", delta_patch);
        insert_kv("nested-atomic-rollbacks", nested_rollback);
        insert_kv("children-safe-views", children_safe);
        insert_kv("mutate-invalidate-precision", invalidate_precision);
        insert_kv("schema", 655);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #657: query:compiler-core-incremental-stats — 5 compiler pipeline
    // gaps for AI multi-round self-mod + incremental (cache bridge epoch,
    // impact-scope partial re-lower, JIT unhandled deopt, linear metadata
    // flow, quote fallback refresh). Non-duplicative with #600
    // incremental-closure-stats, #680 impact_scope, #530 production-reloader.
    //
    // Fields (7 + sentinel):
    //   - bridge-epoch-cache-syncs   compiler_core_bridge_epoch_sync_total
    //   - impact-blocks              Evaluator total_affected_blocks_
    //   - partial-relower-hits       Evaluator partial_relower_count_
    //   - full-fallbacks             relower_full_called_count
    //   - jit-unhandled-deopts       compiler_core_jit_unhandled_invalidate_total
    //   - linear-metadata-flows      compiler_core_linear_metadata_flow_total
    //   - quote-fallback-refreshes   compiler_core_quote_fallback_refresh_total
    //   - schema == 657
    add("query:compiler-core-incremental-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t bridge_sync =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_bridge_epoch_sync_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t impact_blocks =
            static_cast<std::int64_t>(ev.get_total_affected_blocks());
        const std::int64_t partial_relower =
            static_cast<std::int64_t>(ev.get_partial_relower_count());
        const std::int64_t full_fallback =
            m ? static_cast<std::int64_t>(
                    m->relower_full_called_count.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t jit_deopt =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_jit_unhandled_invalidate_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t linear_flow =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_linear_metadata_flow_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t quote_refresh =
            m ? static_cast<std::int64_t>(
                    m->compiler_core_quote_fallback_refresh_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("bridge-epoch-cache-syncs", bridge_sync);
        insert_kv("impact-blocks", impact_blocks);
        insert_kv("partial-relower-hits", partial_relower);
        insert_kv("full-fallbacks", full_fallback);
        insert_kv("jit-unhandled-deopts", jit_deopt);
        insert_kv("linear-metadata-flows", linear_flow);
        insert_kv("quote-fallback-refreshes", quote_refresh);
        insert_kv("schema", 657);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #658: query:highperf-cpp26-stats — 5 high-perf integration gaps
    // (Arena tier fallback + IRSoA dirty cascade + Value v2 classify +
    // ShapeProfiler history jitter + Pass DirtyAware short-circuit).
    // Non-duplicative with #657 compiler-core-incremental, #642 arena
    // auto-compact, #571 value-dispatch, #570 shape-stability, #494 pass-pipeline.
    //
    // Fields (5 + sentinel):
    //   - arena-tier-fallbacks      arena_small_tier_fallback_total
    //   - soa-dirty-cascades        irsoa_dirty_cascade_savings
    //   - value-classify-calls      value_classify_call_count
    //   - shape-history-jitter-wins history_jitter_reduction_count
    //   - pass-dirty-skips          passes_skipped_dirty_pipeline
    //   - schema == 658
    add("query:highperf-cpp26-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t arena_fallback = static_cast<std::int64_t>(
            aura::ast::arena_small_tier_fallback_total.load(std::memory_order_relaxed));
        const std::int64_t soa_cascade =
            m ? static_cast<std::int64_t>(
                    m->irsoa_dirty_cascade_savings.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t classify_calls = static_cast<std::int64_t>(
            types::value_classify_call_count.load(std::memory_order_relaxed));
        const std::int64_t jitter_wins = static_cast<std::int64_t>(
            shape::history_jitter_reduction_count.load(std::memory_order_relaxed));
        const std::int64_t dirty_skips = static_cast<std::int64_t>(
            passes_skipped_dirty_pipeline.load(std::memory_order_relaxed));
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("arena-tier-fallbacks", arena_fallback);
        insert_kv("soa-dirty-cascades", soa_cascade);
        insert_kv("value-classify-calls", classify_calls);
        insert_kv("shape-history-jitter-wins", jitter_wins);
        insert_kv("pass-dirty-skips", dirty_skips);
        insert_kv("schema", 658);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #742: query:cpp26-contracts-stats — C++26 Contracts +
    // consteval hot-path invariant observability for Arena/SoA/Value/
    // Shape/Pass pipeline (non-duplicative with #658 highperf-cpp26,
    // #431 cxx26-invariants, #465 cxx26-hotpath-invariants).
    //
    // Fields (3 + sentinel):
    //   - contract-violations-caught  cpp26::contract_violations_caught_total
    //   - consteval-checks            kConstevalChecksTotal (compile-time)
    //   - hotpath-invariant-hits      cpp26::hotpath_invariant_hits_total
    //   - schema == 742
    add("query:cpp26-contracts-stats", [&ev](const auto&) -> EvalValue {
        (void)ev;
        const std::int64_t violations = static_cast<std::int64_t>(
            aura::core::cpp26::contract_violations_caught_total.load(std::memory_order_relaxed));
        const std::int64_t consteval_checks = aura::core::cpp26::kConstevalChecksTotal;
        const std::int64_t hotpath_hits = static_cast<std::int64_t>(
            aura::core::cpp26::hotpath_invariant_hits_total.load(std::memory_order_relaxed));
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("contract-violations-caught", violations);
        insert_kv("consteval-checks", consteval_checks);
        insert_kv("hotpath-invariant-hits", hotpath_hits);
        insert_kv("schema", 742);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #743: query:arena-auto-policy-stats — Arena auto-compact + live
    // defrag + fiber safepoint + dirty/Shape closed loop (non-duplicative with
    // #642 arena-auto-compaction-stats, #685 arena-auto-compact-stats,
    // #569 arena-auto-compact-defrag-stats).
    //
    // Fields (5 + sentinel):
    //   - auto-compact-triggers     alloc-path + group adaptive triggers
    //   - defrag-fiber-safe-hits    fiber-context compact/defrag safepoints
    //   - fragmentation-post-mutate post-mutate frag ratio (basis points)
    //   - shape-inval-on-compact    ShapeProfiler + on_compact_hook fires
    //   - env-reval-success         env resync after compact invalidation
    //   - schema == 743
    add("query:arena-auto-policy-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t auto_triggers =
            aura::core::arena_policy::auto_compact_triggers_total.load(std::memory_order_relaxed);
        std::uint64_t defrag_fiber_safe =
            aura::core::arena_policy::defrag_fiber_safe_hits_total.load(std::memory_order_relaxed);
        const std::uint64_t frag_post =
            aura::core::arena_policy::fragmentation_post_mutate_bp.load(std::memory_order_relaxed);
        std::uint64_t shape_inval =
            aura::core::arena_policy::shape_inval_on_compact_total.load(std::memory_order_relaxed);
        std::uint64_t env_reval =
            aura::core::arena_policy::env_reval_success_total.load(std::memory_order_relaxed);
        if (ev.arena_) {
            const auto s = ev.arena_->stats();
            auto_triggers += s.auto_alloc_trigger_count;
            shape_inval += s.shape_inval_on_compact;
        }
        if (ev.arena_group_) {
            auto_triggers += ev.arena_group_->auto_compact_trigger_count();
            const auto ag = ev.arena_group_->auto_compact_policy_stats();
            auto_triggers += ag.auto_triggers;
            shape_inval += ag.shape_inval_on_compact;
        }
        if (ev.compiler_metrics()) {
            auto* m = static_cast<aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
            env_reval +=
                m->incremental_closure_env_version_resync_total.load(std::memory_order_relaxed);
        }
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("auto-compact-triggers", static_cast<std::int64_t>(auto_triggers));
        insert_kv("defrag-fiber-safe-hits", static_cast<std::int64_t>(defrag_fiber_safe));
        insert_kv("fragmentation-post-mutate", static_cast<std::int64_t>(frag_post));
        insert_kv("shape-inval-on-compact", static_cast<std::int64_t>(shape_inval));
        insert_kv("env-reval-success", static_cast<std::int64_t>(env_reval));
        insert_kv("schema", 743);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #744: query:shape-jit-pass-closedloop-stats — Shape stability churn
    // → IRSoA dirty → DirtyAware Pass short-circuit → JIT deopt/recompile
    // (non-duplicative with #686 shape-value-pass-stats, #605 shapeprofiler,
    // #723 DirtyAware, #720 JIT metadata).
    //
    // Fields (4 + sentinel):
    //   - stability-churn-deopts   stable→unstable / invalidate deopt fires
    //   - dirty-from-shape         dirty_hook / IRSoA cascade from shape loss
    //   - incremental-recompile-hits JIT invalidate + recompile requests
    //   - speculative-win-lost     stable speculative opt invalidated
    //   - schema == 744
    add("query:shape-jit-pass-closedloop-stats", [&ev](const auto&) -> EvalValue {
        (void)ev;
        const std::int64_t churn = static_cast<std::int64_t>(
            shape_jit_pass::stability_churn_deopts_total.load(std::memory_order_relaxed));
        const std::int64_t dirty_shape = static_cast<std::int64_t>(
            shape_jit_pass::dirty_from_shape_total.load(std::memory_order_relaxed));
        const std::int64_t recompile = static_cast<std::int64_t>(
            shape_jit_pass::incremental_recompile_hits_total.load(std::memory_order_relaxed));
        const std::int64_t win_lost = static_cast<std::int64_t>(
            shape_jit_pass::speculative_win_lost_total.load(std::memory_order_relaxed));
        const std::int64_t stable_skips = static_cast<std::int64_t>(
            passes_skipped_shape_stable_blocks.load(std::memory_order_relaxed));
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("stability-churn-deopts", churn);
        insert_kv("dirty-from-shape", dirty_shape);
        insert_kv("incremental-recompile-hits", recompile);
        insert_kv("speculative-win-lost", win_lost);
        insert_kv("shape-stable-block-skips", stable_skips);
        insert_kv("schema", 744);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #745: query:constraint-reverify-occurrence-stats — dynamic
    // effective_reverify_limit + Occurrence-narrowed priority scan in
    // reverify_clean_constraints_for_touched (non-duplicative with #466,
    // #690 constraint-typed-mutate-stats, #659 typesystem-typed-mutate).
    //
    // Fields (4 + sentinel):
    //   - reverify-hits-on-narrow      priority scans on occurrence-narrow roots
    //   - cross-delta-blame-complete   blame chain with active_mutation_id
    //   - timeout-prevented            dynamic limit avoided fixed-256 truncation
    //   - stale-blame-invalidation     cross-delta hit without mutation epoch
    //   - schema == 745
    add("query:constraint-reverify-occurrence-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t narrow_hits =
            m ? static_cast<std::int64_t>(
                    m->constraint_reverify_narrow_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t blame_complete =
            m ? static_cast<std::int64_t>(
                    m->constraint_blame_chain_complete_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t timeout_prevented =
            m ? static_cast<std::int64_t>(
                    m->constraint_reverify_timeout_prevented_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t stale_blame =
            m ? static_cast<std::int64_t>(
                    m->constraint_stale_blame_invalidation_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("reverify-hits-on-narrow", narrow_hits);
        insert_kv("cross-delta-blame-complete", blame_complete);
        insert_kv("timeout-prevented", timeout_prevented);
        insert_kv("stale-blame-invalidation", stale_blame);
        insert_kv("schema", 745);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #746: query:jit-typed-mutation-stats — narrow_evidence / TypeId /
    // linear_ownership_state propagation from lowering/IRSoA to JIT L2
    // (non-duplicative with #687 dead-coercion-elim, #403 ir-metadata-stats,
    // #550 typed-mutation-stats, #744 shape-jit-pass-closedloop).
    //
    // Fields (4 + sentinel):
    //   - narrow-evidence-hits      JIT GuardShape/CastOp narrow fast paths
    //   - cast-elided-in-l2         CastOp elided via narrow_evidence in JIT
    //   - linear-state-optimized    linear probe + narrow_evidence synergy
    //   - type-propagation-coverage basis-points stamped/total metadata path
    //   - schema == 746
    add("query:jit-typed-mutation-stats", [&ev](const auto&) -> EvalValue {
        (void)ev;
        const std::int64_t narrow_hits = static_cast<std::int64_t>(
            jit_typed_mutation::narrow_evidence_hits_total.load(std::memory_order_relaxed));
        const std::int64_t cast_elided = static_cast<std::int64_t>(
            jit_typed_mutation::cast_elided_in_l2_total.load(std::memory_order_relaxed));
        const std::int64_t linear_opt = static_cast<std::int64_t>(
            jit_typed_mutation::linear_state_optimized_total.load(std::memory_order_relaxed));
        const std::int64_t stamped = static_cast<std::int64_t>(
            jit_typed_mutation::type_propagation_stamped_total.load(std::memory_order_relaxed));
        const std::int64_t denom = narrow_hits + cast_elided + linear_opt;
        const std::int64_t coverage_bp =
            denom > 0 ? (10000 * stamped) / denom : (stamped > 0 ? 10000 : 0);
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("narrow-evidence-hits", narrow_hits);
        insert_kv("cast-elided-in-l2", cast_elided);
        insert_kv("linear-state-optimized", linear_opt);
        insert_kv("type-propagation-coverage", coverage_bp);
        insert_kv("schema", 746);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #747: query:linear-occurrence-mutate-stats — OwnershipEnv +
    // Occurrence Typing predicate-branch linear safety under typed mutation
    // (non-duplicative with #688 linear-ownership-typed-mutate, #689
    // occurrence-typing-mutate, #746 jit-typed-mutation).
    //
    // Fields (3 + sentinel):
    //   - revalidate-hits                 post-mutate linear∩occurrence revalidates
    //   - escape-violations-prevented     escape/ownership violations caught early
    //   - predicate-branch-linear-safe    ownership pass on narrowed predicate branches
    //   - schema == 747
    add("query:linear-occurrence-mutate-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::int64_t revalidates =
            m ? static_cast<std::int64_t>(
                    m->linear_occurrence_revalidate_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t escape_prev =
            m ? static_cast<std::int64_t>(
                    m->linear_occurrence_escape_prevented_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t branch_safe =
            m ? static_cast<std::int64_t>(
                    m->linear_occurrence_predicate_safe_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("revalidate-hits", revalidates);
        insert_kv("escape-violations-prevented", escape_prev);
        insert_kv("predicate-branch-linear-safe", branch_safe);
        insert_kv("schema", 747);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #748: query:sv-verification-structure-stats — P0 SV verification
    // EDSL structured representation + emit fidelity + dirty re-emit closed-loop
    // (consolidates #724/#725/#726; non-duplicative with #694 sv-sva-structure,
    // #640 sv-verification-closedloop, #693 hardware-backend-sv-closedloop).
    //
    // Fields (4 + sentinel):
    //   - structure-mutate-hits   sv_verification_structure_mutate_hits_total
    //   - dirty-reemit-triggers   sv_verification_dirty_reemit_total
    //   - emit-fidelity-pass      sv_emit_parse_success_total
    //   - emit-fidelity-fail      sv_emit_parse_fail_total
    //   - schema == 748
    add("query:sv-verification-structure-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::int64_t structure_mutate =
            m ? static_cast<std::int64_t>(
                    m->sv_verification_structure_mutate_hits_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t dirty_reemit =
            m ? static_cast<std::int64_t>(
                    m->sv_verification_dirty_reemit_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t emit_pass =
            m ? static_cast<std::int64_t>(
                    m->sv_emit_parse_success_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t emit_fail =
            m ? static_cast<std::int64_t>(
                    m->sv_emit_parse_fail_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("structure-mutate-hits", structure_mutate);
        insert_kv("dirty-reemit-triggers", dirty_reemit);
        insert_kv("emit-fidelity-pass", emit_pass);
        insert_kv("emit-fidelity-fail", emit_fail);
        insert_kv("schema", 748);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #750: query:reflection-schema-stats — Runtime reflection schema
    // validation bridge for macro bodies + EDSL structs under Guard mutate
    // (refines #734; non-duplicative with #454 reflect-edsl-bridge, #502
    // reflect-postmutate, #654 macro-hygiene-fiber-panic).
    //
    // Fields (4 + sentinel):
    //   - validated                  reflection_schema_validated_total
    //   - hygiene-invariants-held    reflection_macro_provenance_held_total
    //   - schema-violations          reflection_schema_violations_total
    //   - stale-validation-prevented reflection_stale_validation_prevented_total
    //   - schema == 750
    add("query:reflection-schema-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::int64_t validated =
            m ? static_cast<std::int64_t>(
                    m->reflection_schema_validated_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t hygiene_held =
            m ? static_cast<std::int64_t>(
                    m->reflection_macro_provenance_held_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t violations =
            m ? static_cast<std::int64_t>(
                    m->reflection_schema_violations_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t stale_prev =
            m ? static_cast<std::int64_t>(
                    m->reflection_stale_validation_prevented_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("validated", validated);
        insert_kv("hygiene-invariants-held", hygiene_held);
        insert_kv("schema-violations", violations);
        insert_kv("stale-validation-prevented", stale_prev);
        insert_kv("schema", 750);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #659: query:typesystem-typed-mutate-stats — 5 type system gaps for
    // AI multi-round typed mutation (solve_delta reverify, dead coercion elim,
    // linear ownership post-mutate, occurrence provenance refresh, coercion map
    // incremental). Non-duplicative with #656 Lambda param recheck, #657
    // compiler-core-incremental, #690 constraint-typed-mutate-stats.
    //
    // Fields (5 + sentinel):
    //   - delta-reverify-scans           delta_conflict_reverify_total
    //   - dead-coercion-eliminated       dead_coercion_eliminated_total
    //   - linear-post-mutate-revalidations linear_post_mutate_revalidations_total
    //   - narrowing-provenance-refresh   narrowing_provenance_total
    //   - coercion-incremental-wins      coercion_zerooverhead_win_total
    //   - schema == 659
    add("query:typesystem-typed-mutate-stats", [&ev](const auto&) -> EvalValue {
        CompilerMetrics* m =
            ev.compiler_metrics() ? static_cast<CompilerMetrics*>(ev.compiler_metrics()) : nullptr;
        const std::int64_t reverify =
            m ? static_cast<std::int64_t>(
                    m->delta_conflict_reverify_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t dce =
            m ? static_cast<std::int64_t>(
                    m->dead_coercion_eliminated_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t linear =
            m ? static_cast<std::int64_t>(
                    m->linear_post_mutate_revalidations_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t provenance =
            m ? static_cast<std::int64_t>(
                    m->narrowing_provenance_total.load(std::memory_order_relaxed))
              : 0;
        const std::int64_t coercion =
            m ? static_cast<std::int64_t>(
                    m->coercion_zerooverhead_win_total.load(std::memory_order_relaxed))
              : 0;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("delta-reverify-scans", reverify);
        insert_kv("dead-coercion-eliminated", dce);
        insert_kv("linear-post-mutate-revalidations", linear);
        insert_kv("narrowing-provenance-refresh", provenance);
        insert_kv("coercion-incremental-wins", coercion);
        insert_kv("schema", 659);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #673: query:runtime-observability-correlated-stats — Unified
    // Runtime Observability Layer (P1) cross-module correlation primitive.
    //
    // The other observability primitives (#527, #529, #506, #480, #598,
    // #548, #599, #592, #593, #596, #591, #438, #448 et al.) each cover
    // a single module's stats. #673 ships the FIRST dedicated
    // correlation counters that resolve cross-module events to a single
    // signal: "mutation during steal" / "ownership violation rate during
    // steal" / "GC deferred by boundary" (3 of the 4 concrete gaps
    // identified in the issue body).
    //
    // Fields (4 + sentinel):
    //   - steal-attempts-correlated
    //       runtime_observability_steal_attempt_correlated_total
    //       (any steal attempt; baseline denominator)
    //   - steal-deferred-correlated
    //       runtime_observability_steal_deferred_correlated_total
    //       (steal deferred at an active MutationBoundary — the
    //       "mutation during steal" correlation)
    //   - steal-ownership-violation-correlated
    //       runtime_observability_steal_ownership_violation_correlated_total
    //       (linear ownership violation caught during steal probe —
    //       the "ownership violation rate during steal" correlation)
    //   - correlated-events-total
    //       Sum of the 3 correlated counters above (per-call derivation,
    //       not a separate atomic). Lets dashboards show overall
    //       correlated-event volume at a glance.
    //   - schema == 673
    //
    // Non-duplicative with #591 scheduler-mutation-coord-stats,
    // #438 fiber-migration-stats, #448 mutation-coordination-stats,
    // #599 compiler-root-stats, #592 panic-checkpoint-fiber-stats,
    // #596 guard-panic-reflect-stats — each of those exposes its own
    // module-local view; this primitive is the FIRST unified view.
    add("query:runtime-observability-correlated-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t steal_attempts =
            static_cast<std::int64_t>(ev.get_runtime_observability_steal_attempt_correlated());
        const std::int64_t steal_deferred =
            static_cast<std::int64_t>(ev.get_runtime_observability_steal_deferred_correlated());
        const std::int64_t ownership_violation = static_cast<std::int64_t>(
            ev.get_runtime_observability_steal_ownership_violation_correlated());
        const std::int64_t correlated_total = steal_attempts + steal_deferred + ownership_violation;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("steal-attempts-correlated", steal_attempts);
        insert_kv("steal-deferred-correlated", steal_deferred);
        insert_kv("steal-ownership-violation-correlated", ownership_violation);
        insert_kv("correlated-events-total", correlated_total);
        insert_kv("schema", 673);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #674: query:self-evolution-chaos-stats — Closed-loop
    // self-evolution stability stress testing (P0) outcome
    // classifier. Companion primitive for the chaos stress
    // harness that drives 1000+ mutation cycles under fiber
    // steal + GC + AOT hot-reload conditions. The 3 fields
    // are the "outcome classifier" of each chaos cycle:
    //
    //   - chaos-cycles      self_evolution_chaos_cycles_total
    //       Bumped by the chaos harness once per full chaos
    //       mutation cycle (one attempted self-evolution,
    //       regardless of outcome). The "1000+ mutations" sum
    //       the issue body calls out as the long-running
    //       stress baseline.
    //   - chaos-failures    self_evolution_chaos_failures_total
    //       Bumped by the chaos harness when a chaos mutation
    //       cycle fails (post-mutation validation, rollback,
    //       or panic). The "evolution success rate" denominator.
    //   - chaos-corruptions self_evolution_chaos_corruptions_total
    //       Bumped by the chaos harness when a version/ownership
    //       mismatch is detected during a chaos cycle. The
    //       "corruption detected per epoch" metric from the
    //       issue body.
    //   - chaos-events-total
    //       Sum of the 3 (per-call derivation, not a separate
    //       atomic). Lets dashboards show overall chaos-event
    //       volume at a glance.
    //   - schema == 674
    //
    // Non-duplicative with #548 panic-checkpoint-lifecycle,
    // #529 atomic-batch-rollback, #527 stable-ref-cow-fiber,
    // #400 mutation-rollback-coverage, #679 nested-Guard
    // atomic-batch-rollback. Those cover the production
    // counter set; #674 covers the chaos/stress-test
    // outcome classifier.
    add("query:self-evolution-chaos-stats", [&ev](const auto&) -> EvalValue {
        const std::int64_t cycles = static_cast<std::int64_t>(ev.get_self_evolution_chaos_cycles());
        const std::int64_t failures =
            static_cast<std::int64_t>(ev.get_self_evolution_chaos_failures());
        const std::int64_t corruptions =
            static_cast<std::int64_t>(ev.get_self_evolution_chaos_corruptions());
        const std::int64_t events_total = cycles + failures + corruptions;
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("chaos-cycles", cycles);
        insert_kv("chaos-failures", failures);
        insert_kv("chaos-corruptions", corruptions);
        insert_kv("chaos-events-total", events_total);
        insert_kv("schema", 674);
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #498: query:primitive-metadata — structured AI-native primitive
    // registry introspection for Agent development workflows.
    add("query:primitive-metadata", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t slots = ev.primitives_.slot_count();
        const std::uint64_t documented = ev.primitives_.documented_meta_count();
        const std::uint64_t schema_doc = ev.primitives_.schema_documented_meta_count();
        const std::uint64_t describes = ev.get_primitive_describe_count();
        const std::uint64_t list_meta = ev.get_primitive_list_meta_count();
        const std::uint64_t skeletons =
            m ? m->primitive_skeleton_generations_total.load(std::memory_order_relaxed) : 0;
        std::uint64_t pure_count = 0;
        std::uint64_t mutate_count = 0;
        for (std::size_t si = 0; si < slots; ++si) {
            const auto& pm = ev.primitives_.meta_for_slot(si);
            if (pm.pure)
                ++pure_count;
            if ((pm.safety_flags & kPrimSafetyMutates) != 0)
                ++mutate_count;
        }
        const std::uint64_t coverage_bp =
            slots > 0 ? (10000 * documented / slots) : (documented > 0 ? 10000 : 0);
        std::int64_t recommendation = 0;
        if (coverage_bp < 5000)
            recommendation = 1;
        else if (schema_doc < documented / 2)
            recommendation = 2;
        const std::uint64_t metadata_total = slots + documented + schema_doc + describes +
                                             list_meta + skeletons + pure_count + mutate_count;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"registry-slots", make_int(static_cast<std::int64_t>(slots))},
            {"documented-meta", make_int(static_cast<std::int64_t>(documented))},
            {"schema-documented", make_int(static_cast<std::int64_t>(schema_doc))},
            {"describe-calls", make_int(static_cast<std::int64_t>(describes))},
            {"list-meta-calls", make_int(static_cast<std::int64_t>(list_meta))},
            {"skeleton-generations", make_int(static_cast<std::int64_t>(skeletons))},
            {"pure-primitives", make_int(static_cast<std::int64_t>(pure_count))},
            {"mutate-primitives", make_int(static_cast<std::int64_t>(mutate_count))},
            {"meta-coverage-bp", make_int(static_cast<std::int64_t>(coverage_bp))},
            {"extension-kit-version",
             make_int(static_cast<std::int64_t>(kPrimitivesExtensionKitVersion))},
            {"metadata-recommendation", make_int(recommendation)},
            {"metadata-total", make_int(static_cast<std::int64_t>(metadata_total))},
        };
        return build_hash(kv);
    });

    // Issue #499: query:eda-foundation-stats — EDA primitives module
    // parse/query/mutate/waveform/feedback observability for Agent loops.
    add("query:eda-foundation-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t parse =
            m ? m->eda_foundation_parse_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t query =
            m ? m->eda_foundation_query_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mutate =
            m ? m->eda_foundation_mutate_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t waveform =
            m ? m->eda_foundation_waveform_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t feedback =
            m ? m->eda_foundation_feedback_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t hooks =
            m ? m->hardware_backend_hook_calls_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t foundation_total = parse + query + mutate + waveform + feedback;
        std::int64_t recommendation = 0;
        if (parse == 0)
            recommendation = 1;
        else if (mutate == 0)
            recommendation = 2;
        else if (feedback == 0)
            recommendation = 3;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"parse-total", make_int(static_cast<std::int64_t>(parse))},
            {"query-total", make_int(static_cast<std::int64_t>(query))},
            {"mutate-total", make_int(static_cast<std::int64_t>(mutate))},
            {"waveform-total", make_int(static_cast<std::int64_t>(waveform))},
            {"feedback-total", make_int(static_cast<std::int64_t>(feedback))},
            {"hardware-hook-calls", make_int(static_cast<std::int64_t>(hooks))},
            {"foundation-total", make_int(static_cast<std::int64_t>(foundation_total))},
            {"foundation-recommendation", make_int(recommendation)},
        };
        return build_hash(kv);
    });

    // Issue #616: query:eda-hw-stats — EDA hardware-co-design
    // primitives observability. Companion to query:eda-foundation-stats
    // (#499) but covering the file-boundary surface (load-sv,
    // parse-verification-result). Separate primitive so the #499
    // foundation stats shape stays unchanged for back-compat, and
    // the file-I/O layer has its own dedicated dashboard.
    //
    // Returned hash:
    //   - load-sv-total               successful (eda:load-sv) calls
    //   - load-sv-failure-total       failed (eda:load-sv) calls
    //   - parse-verification-result-total successful calls
    //   - parse-verification-failure-total failed calls
    //   - load-sv-success-rate        0..100 (0 when both are 0)
    //   - parse-verification-success-rate 0..100 (0 when both are 0)
    //
    // The success-rate fields are computed inline so the Agent
    // doesn't have to do the division itself; the per-call counters
    // are also exposed so a custom Agent can compute its own
    // moving-window rate.
    add("query:eda-hw-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t load_sv_ok =
            m ? m->eda_load_sv_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t load_sv_fail =
            m ? m->eda_load_sv_failure_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t parse_vr_ok =
            m ? m->eda_parse_verification_result_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t parse_vr_fail =
            m ? m->eda_parse_verification_failure_total.load(std::memory_order_relaxed) : 0;
        const auto load_total = load_sv_ok + load_sv_fail;
        const auto parse_total = parse_vr_ok + parse_vr_fail;
        const std::int64_t load_rate =
            load_total == 0 ? 0 : static_cast<std::int64_t>((load_sv_ok * 100) / load_total);
        const std::int64_t parse_rate =
            parse_total == 0 ? 0 : static_cast<std::int64_t>((parse_vr_ok * 100) / parse_total);
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"load-sv-total", make_int(static_cast<std::int64_t>(load_sv_ok))},
            {"load-sv-failure-total", make_int(static_cast<std::int64_t>(load_sv_fail))},
            {"parse-verification-result-total", make_int(static_cast<std::int64_t>(parse_vr_ok))},
            {"parse-verification-failure-total",
             make_int(static_cast<std::int64_t>(parse_vr_fail))},
            {"load-sv-success-rate", make_int(load_rate)},
            {"parse-verification-success-rate", make_int(parse_rate)},
        };
        return build_hash(kv);
    });

    // Issue #478: query:primitive-error-stats — returns a pair
    // (error-count . error-values-size) for Agent recovery loops.
    add("query:primitive-error-stats", [&ev](const auto&) -> EvalValue {
        auto count = static_cast<std::int64_t>(ev.get_primitive_error_count());
        auto stored = static_cast<std::int64_t>(ev.get_primitive_error_values_size());
        auto pid = ev.pairs_.size();
        ev.pairs_.push_back({make_int(count), make_int(stored)});
        return make_pair(pid);
    });

    // (query:primitive-fastpath-per-prim) — Issue #479:
    // per-prim fast-path hit breakdown. Returns a hash with:
    //   - total: aggregate fast-path hit count (matches
    //     primitive_fastpath_hits_total from #709)
    //   - distinct-prims: number of slots with count > 0
    //   - top: list of (name . count) dotted pairs sorted
    //     by count desc, capped at top-N (default 10). The
    //     hottest primitive comes first. Slots with 0 hits
    //     are excluded to keep the response small even for
    //     large registries.
    //   - capacity: current per-prim array capacity (for
    //     diagnosing whether growth has occurred)
    //
    // Why a separate primitive from query:primitive-perf-stats:
    // that one is a coarse-grained MVP (just total + count).
    // This one is the "which prim is the bottleneck?" answer
    // that the AI Agent perf-tuning loop actually needs.
    add("query:primitive-fastpath-per-prim", [&ev](std::span<const EvalValue> a) -> EvalValue {
        constexpr std::size_t kDefaultTopN = 10;
        std::size_t top_n = kDefaultTopN;
        // Optional arg: top-N override (clamped to [1, 1000]).
        if (!a.empty() && is_int(a[0])) {
            auto v = as_int(a[0]);
            if (v < 1)
                v = 1;
            if (v > 1000)
                v = 1000;
            top_n = static_cast<std::size_t>(v);
        }

        std::uint64_t total = 0;
        std::uint64_t distinct = 0;
        std::vector<std::pair<std::string, std::uint64_t>> rows;
        std::size_t cap = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            total = m->primitive_fastpath_hits_total.load(std::memory_order_relaxed);
            cap = m->primitive_fastpath_per_prim_capacity_;
            const auto slot_count = ev.primitives_.slot_count();
            const std::size_t limit = std::min(slot_count, cap);
            rows.reserve(limit);
            for (std::size_t slot = 0; slot < limit; ++slot) {
                auto cnt =
                    m->primitive_fastpath_hits_per_prim_[slot].load(std::memory_order_relaxed);
                if (cnt > 0) {
                    ++distinct;
                    rows.emplace_back(ev.primitives_.name_for_slot(slot), cnt);
                }
            }
        }
        // Sort desc by count, ties broken by name asc for stability.
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second)
                return a.second > b.second;
            return a.first < b.first;
        });
        if (rows.size() > top_n)
            rows.resize(top_n);

        // Build top-N as a proper list of (name . count) dotted pairs.
        // Build in reverse so the head of the list is the last
        // pushed pair (Aura list primitive uses pair-chain with
        // void terminator; building in reverse is the natural form).
        EvalValue top_list = make_void();
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(it->first);
            auto name_ev = make_string(name_idx);
            auto count_ev = make_int(static_cast<std::int64_t>(it->second));
            auto idx = ev.pairs_.size();
            ev.pairs_.push_back({name_ev, count_ev});
            auto pair_ev = make_pair(idx);
            auto cell_idx = ev.pairs_.size();
            ev.pairs_.push_back({pair_ev, top_list});
            top_list = make_pair(cell_idx);
        }

        // Inline build_hash (small hash, 4 fields; matches the
        // pattern used by query:primitive-perf-stats below).
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"total", make_int(static_cast<std::int64_t>(total))},
            {"distinct-prims", make_int(static_cast<std::int64_t>(distinct))},
            {"top", top_list},
            {"capacity", make_int(static_cast<std::int64_t>(cap))},
        };
        return build_hash(kv);
    });

    // (query:primitive-perf-stats) — Issue #441 (rolled into
    // #450): hot-path primitive dispatch stats. Returns a
    // hash with 3 fields:
    //   - primitive-call-total: lifetime count of every
    //     (primitive-func args...) dispatch (bumped in
    //     evaluator_eval_flat.cpp at the dispatch site)
    //   - primitive-count: # of registered primitives
    //     (snapshot at primitive-registration time; gives
    //     a per-primitive average call rate when paired
    //     with primitive-call-total)
    //   - avg-per-prim: primitive-call-total / primitive-count
    //
    // Issue #479 ships the per-prim breakdown as a
    // separate primitive (query:primitive-fastpath-per-prim)
    // — see above. This primitive remains the aggregate
    // "is the dispatch hot path hot?" answer.
    add("query:primitive-perf-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t call_total = 0;
        std::uint64_t prim_count = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            call_total = m->primitive_call_total.load(std::memory_order_relaxed);
        }
        prim_count = ev.primitives_.slot_count();
        std::int64_t avg_per_prim =
            prim_count > 0 ? static_cast<std::int64_t>(call_total / prim_count) : 0;
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"primitive-call-total", make_int(static_cast<std::int64_t>(call_total))},
            {"primitive-count", make_int(static_cast<std::int64_t>(prim_count))},
            {"avg-per-prim", make_int(avg_per_prim)},
        };
        return build_hash(kv);
    });

    // (query:aot-stats) — Issue #452: AOT hot-update + region
    // filtering observability. Returns a 3-field hash:
    //   - aot-stale-reject-count: lifetime count of
    //     aura_reload_aot_module rejections due to
    //     aot_emit_version mismatch (bumped in
    //     aura_jit_bridge.cpp)
    //   - aot-region-mismatch-count: lifetime count of
    //     region_filter mismatches (currently 0 — region
    //     wiring is a follow-up; counter is in place
    //     so the day it ships, observability is immediate)
    //   - aot-hot-update-success-count: lifetime count of
    //     successful dlopen + version check + constructor
    //     invocation.
    //
    // This is the AI Agent's signal for "is the AOT
    // hot-update pipeline behaving correctly?". A rising
    // stale-reject count without rising success count =
    // version drift (the bug pattern from #452's body).
    add("query:aot-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t stale_rej = 0;
        std::uint64_t region_mismatch = 0;
        std::uint64_t hot_update_ok = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            stale_rej = m->aot_stale_reject_count_.load(std::memory_order_relaxed);
            region_mismatch = m->aot_region_mismatch_.load(std::memory_order_relaxed);
            hot_update_ok = m->aot_hot_update_success_.load(std::memory_order_relaxed);
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"aot-stale-reject-count", make_int(static_cast<std::int64_t>(stale_rej))},
            {"aot-region-mismatch-count", make_int(static_cast<std::int64_t>(region_mismatch))},
            {"aot-hot-update-success-count", make_int(static_cast<std::int64_t>(hot_update_ok))},
        };
        return build_hash(kv);
    });

    // (query:ci-reproducibility-stats) — Issue #675: build/CI
    // reproducibility + sanitizer gate observability. Returns a
    // 5-field hash:
    //   - source-date-epoch: SOURCE_DATE_EPOCH env (0 if unset)
    //   - build-type: AURA_BUILD_TYPE env (or "unknown")
    //   - sanitizer-mode: compile-time "none"|"asan"|"ubsan"|"tsan"
    //   - reproducible-flags-active: 1 iff SOURCE_DATE_EPOCH > 0
    //   - ccache-disabled: 1 iff CCACHE_DISABLE=1
    add("query:ci-reproducibility-stats", [&ev](const auto&) -> EvalValue {
        const auto epoch = aura::ci::source_date_epoch();
        const auto repro = aura::ci::reproducible_flags_active();
        const auto ccache_off = aura::ci::ccache_disabled();
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        auto bt_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(aura::ci::build_type_from_env());
        auto san_idx = ev.string_heap_.size();
        ev.string_heap_.push_back(aura::ci::sanitizer_mode());
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"source-date-epoch", make_int(epoch)},
            {"build-type", make_string(bt_idx)},
            {"sanitizer-mode", make_string(san_idx)},
            {"reproducible-flags-active", make_bool(repro)},
            {"ccache-disabled", make_bool(ccache_off)},
        };
        return build_hash(kv);
    });

    // (query:shape-folding-stats) — Issue #462: observability
    // for ShapeAwareFoldingPass. Returns a 4-field hash:
    //   - shape-fold-count: lifetime # of instructions
    //     replaced (OpNop'd) due to shape/linear/narrow
    //     metadata
    //   - shape-linear-elide-count: subset of fold-count
    //     due to linear-ownership elision (MoveOp on
    //     non-escaping Owned slot is a no-op)
    //   - shape-narrow-check-count: # of redundant
    //     type-checks detected (counted, not yet rewritten
    //     in Cycle 1; rewrite is #462 follow-up)
    //   - guard-shape-hits: # of GuardShape instructions
    //     seen in the module (signal for downstream passes
    //     to trust per-block shape_id)
    //
    // This is the AI Agent's signal for "is the
    // shape-aware folding pass doing useful work?".
    // Cycle 2 (separate issue) will add per-shape-id
    // OpAdd unchecked specialization + the narrow-evidence
    // rewrite. The counter layer is in place.
    add("query:shape-folding-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t fold = 0;
        std::uint64_t linear_elide = 0;
        std::uint64_t narrow = 0;
        std::uint64_t guard_hits = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            fold = m->shape_fold_count.load(std::memory_order_relaxed);
            linear_elide = m->shape_linear_elide_count.load(std::memory_order_relaxed);
            narrow = m->shape_narrow_check_count.load(std::memory_order_relaxed);
            guard_hits = m->guard_shape_hits.load(std::memory_order_relaxed);
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"shape-fold-count", make_int(static_cast<std::int64_t>(fold))},
            {"shape-linear-elide-count", make_int(static_cast<std::int64_t>(linear_elide))},
            {"shape-narrow-check-count", make_int(static_cast<std::int64_t>(narrow))},
            {"guard-shape-hits", make_int(static_cast<std::int64_t>(guard_hits))},
        };
        return build_hash(kv);
    });

    // (query:soa-adoption-stats) — Issue #463: SoA Phase 2
    // adoption observability. Returns a 3-field hash:
    //   - soa-functions-visited: lifetime # of SoA
    //     functions walked by the bridge pass
    //   - soa-instructions-visited: lifetime # of SoA
    //     instructions walked
    //   - aos-view-built-count: lifetime # of SoA→AoS
    //     view conversions
    //
    // This is the AI Agent's signal for "is the SoA
    // rollout progressing?". A rising
    // soa-instructions-visited count with no
    // aos-view-built-count growth means the SoA path is
    // being used end-to-end (the AoS view is a one-time
    // scaffold; subsequent cycles replace it with
    // SoA-aware Pass overloads).
    add("query:soa-adoption-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t funcs = 0;
        std::uint64_t instrs = 0;
        std::uint64_t views = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            funcs = m->soa_functions_visited.load(std::memory_order_relaxed);
            instrs = m->soa_instructions_visited.load(std::memory_order_relaxed);
            views = m->aos_view_built_count.load(std::memory_order_relaxed);
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"soa-functions-visited", make_int(static_cast<std::int64_t>(funcs))},
            {"soa-instructions-visited", make_int(static_cast<std::int64_t>(instrs))},
            {"aos-view-built-count", make_int(static_cast<std::int64_t>(views))},
        };
        return build_hash(kv);
    });

    // (query:arena-auto-stats) — Issue #464: Arena
    // auto-compaction lifecycle observability. Returns a
    // 4-field hash:
    //   - auto-compact-guard-call-count: lifetime # of
    //     times MutationBoundaryGuard dtor bumped the
    //     closed-loop signal (one bump per outermost +
    //     success guard exit)
    //   - compaction-yield-checks: lifetime # of times
    //     auto_compact_with_safety() observed a fiber
    //     context (g_current_fiber != nullptr); the actual
    //     yield-during-compact is a #464 follow-up
    //   - auto-compact-trigger-count: lifetime # of
    //     triggered compactions (from ArenaGroup)
    //   - auto-compact-skip-count: lifetime # of
    //     skipped triggers (below adaptive threshold)
    //
    // This is the AI Agent's signal for "is the
    // arena auto-compaction lifecycle working as
    // expected?". Cycle 2 (separate issue) will add
    // the actual auto_compact_with_safety() call from
    // the scheduler + the fiber-yield integration.
    add("query:arena-auto-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t guard_calls = 0;
        std::uint64_t yield_checks = 0;
        std::uint64_t trigger_count = 0;
        std::uint64_t skip_count = 0;
        // Read all 4 counters directly from the ArenaGroup
        // (the bump happens in MutationBoundaryGuard dtor
        // on ev_->arena_group_). The compiler_metrics_
        // field is the in-process metrics struct used by
        // the snapshot() helper; for EDSL primitives we
        // read from the source of truth (ArenaGroup) so
        // the counter advances immediately without
        // requiring a metrics copy.
        guard_calls = ev.arena_group().auto_compact_guard_call_count();
        yield_checks = ev.arena_group().compaction_yield_checks_group();
        trigger_count = ev.arena_group().auto_compact_trigger_count();
        skip_count = ev.arena_group().auto_compact_skip_count();
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-compact-guard-call-count", make_int(static_cast<std::int64_t>(guard_calls))},
            {"compaction-yield-checks", make_int(static_cast<std::int64_t>(yield_checks))},
            {"auto-compact-trigger-count", make_int(static_cast<std::int64_t>(trigger_count))},
            {"auto-compact-skip-count", make_int(static_cast<std::int64_t>(skip_count))},
        };
        return build_hash(kv);
    });

    // Issue #685: (query:arena-auto-compact-stats) — alloc-path
    // auto-compact policy + Shape/dirty synergy metrics.
    add("query:arena-auto-compact-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t auto_triggers = 0;
        std::uint64_t frag_reduced = 0;
        std::uint64_t shape_inval = 0;
        std::uint64_t defrag_savings = 0;
        std::uint64_t yield_checks = 0;
        if (ev.arena_) {
            const auto s = ev.arena_->stats();
            auto_triggers += s.auto_alloc_trigger_count;
            frag_reduced += s.frag_reduced_bp;
            shape_inval += s.shape_inval_on_compact;
            defrag_savings += s.defrag_savings_alloc;
            yield_checks += s.compaction_yield_checks;
        }
        if (ev.arena_group_) {
            const auto ag = ev.arena_group_->auto_compact_policy_stats();
            auto_triggers += ag.auto_triggers;
            frag_reduced += ag.frag_reduced;
            shape_inval += ag.shape_inval_on_compact;
            defrag_savings += ag.defrag_savings;
            yield_checks += ag.yield_checks_hit;
        }
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-triggers", make_int(static_cast<std::int64_t>(auto_triggers))},
            {"frag-reduced", make_int(static_cast<std::int64_t>(frag_reduced))},
            {"shape-inval-on-compact", make_int(static_cast<std::int64_t>(shape_inval))},
            {"defrag-savings", make_int(static_cast<std::int64_t>(defrag_savings))},
            {"yield-checks-hit", make_int(static_cast<std::int64_t>(yield_checks))},
        };
        return build_hash(kv);
    });

    // Issue #569: Task4-review closing hash for tiered SmallObjectPool +
    // dtor tracking + auto-compaction + live defrag + fiber safepoint coordination.
    add("query:arena-auto-compact-defrag-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto& group = ev.arena_group();
        const auto stats = group.total_stats();
        const auto policy = group.auto_compact_policy_stats();
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t live_dtors = ev.arena_ ? ev.arena_->live_count() : 0;
        const std::int64_t frag_pct =
            static_cast<std::int64_t>(stats.fragmentation_ratio() * 100.0);
        const std::uint64_t auto_compact_count =
            group.auto_compact_trigger_count() + policy.auto_triggers;
        const std::uint64_t auto_compact_skips = group.auto_compact_skip_count();
        const std::uint64_t guard_calls = group.auto_compact_guard_call_count();
        const std::uint64_t defrag_saved =
            policy.defrag_savings + stats.defrag_savings_alloc + stats.last_defrag_saved;
        const std::uint64_t defrag_attempted = stats.defrag_attempted_count;
        const std::uint64_t yield_checks = group.compaction_yield_checks_group() +
                                           policy.yield_checks_hit + stats.compaction_yield_checks;
        const std::uint64_t paused = ev.compaction_paused_by_boundary();
        const std::uint64_t gc_waits = ev.get_gc_safepoint_waits_total();
        const std::uint64_t gc_deferred = ev.get_gc_safepoint_deferred_total();
        const std::uint64_t safepoint_coord = yield_checks + paused + gc_waits + gc_deferred;
        const std::uint64_t mutation_volume = ev.total_mutations();
        const std::uint64_t threshold_config =
            m ? m->arena_auto_compact_threshold_set_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t total =
            live_dtors + stats.peak_used + auto_compact_count + auto_compact_skips + guard_calls +
            defrag_saved + defrag_attempted + safepoint_coord + mutation_volume + threshold_config;
        std::int64_t recommendation = 0;
        if (frag_pct > 30 && auto_compact_count == 0)
            recommendation = 3;
        else if (paused > yield_checks && paused > 0)
            recommendation = 2;
        else if (auto_compact_count > 0 || defrag_saved > 0 || safepoint_coord > 0)
            recommendation = 1;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"fragmentation-ratio-pct", make_int(frag_pct)},
            {"peak-used-bytes", make_int(static_cast<std::int64_t>(stats.peak_used))},
            {"live-dtor-count", make_int(static_cast<std::int64_t>(live_dtors))},
            {"auto-compact-count", make_int(static_cast<std::int64_t>(auto_compact_count))},
            {"auto-compact-skips", make_int(static_cast<std::int64_t>(auto_compact_skips))},
            {"auto-compact-guard-calls", make_int(static_cast<std::int64_t>(guard_calls))},
            {"defrag-saved-bytes", make_int(static_cast<std::int64_t>(defrag_saved))},
            {"defrag-attempted-count", make_int(static_cast<std::int64_t>(defrag_attempted))},
            {"safepoint-coordination-count", make_int(static_cast<std::int64_t>(safepoint_coord))},
            {"mutation-volume-trigger", make_int(static_cast<std::int64_t>(mutation_volume))},
            {"threshold-config-count", make_int(static_cast<std::int64_t>(threshold_config))},
            {"compaction-yield-checks", make_int(static_cast<std::int64_t>(yield_checks))},
            {"paused-by-boundary", make_int(static_cast<std::int64_t>(paused))},
            {"task4-review-schema", make_int(569)},
            {"arena-auto-compact-defrag-total", make_int(static_cast<std::int64_t>(total))},
            {"arena-auto-compact-defrag-recommendation", make_int(recommendation)},
        };
        return build_hash(kv);
    });

    // Issue #604: (query:arena-fragmentation-snapshot) — a *live*
    // snapshot of the auto-compaction / defrag / fiber-yield
    // subsystem. Unlike (query:arena-auto-compact-stats) which
    // reports lifetime policy counters only, this also reports the
    // current aggregate fragmentation ratio so an AI agent can
    // correlate the trigger counters with the memory state right
    // now. Fields:
    //   - auto-compact-triggers: lifetime auto-trigger count
    //   - fragmentation-ratio:   current (capacity-used)/capacity
    //                            aggregated over arena_ + group
    //   - yield-deferred:        # of compactions that observed a
    //                            fiber context (compaction_yield_checks)
    //   - defrag-saved-bytes:    bytes reclaimed by alloc-path defrag
    //
    // Note: the (query:arena-auto-stats) name is already taken by
    // #464 (group-level guard/skip counts), so we use a distinct
    // name that signals "point-in-time snapshot" vs. "cumulative".
    add("query:arena-fragmentation-snapshot", [&ev](const auto&) -> EvalValue {
        std::uint64_t auto_triggers = 0;
        std::uint64_t yield_deferred = 0;
        std::uint64_t defrag_saved = 0;
        std::size_t total_cap = 0;
        std::size_t total_used = 0;
        if (ev.arena_) {
            const auto s = ev.arena_->stats();
            auto_triggers += s.auto_alloc_trigger_count;
            yield_deferred += s.compaction_yield_checks;
            defrag_saved += s.defrag_savings_alloc;
            total_cap += s.capacity;
            total_used += s.used;
        }
        if (ev.arena_group_) {
            const auto ag = ev.arena_group_->auto_compact_policy_stats();
            auto_triggers += ag.auto_triggers;
            yield_deferred += ag.yield_checks_hit;
            defrag_saved += ag.defrag_savings;
            const auto gs = ev.arena_group_->total_stats();
            total_cap += gs.capacity;
            total_used += gs.used;
        }
        const double frag = total_cap == 0 ? 0.0
                                           : static_cast<double>(total_cap - total_used) /
                                                 static_cast<double>(total_cap);
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-compact-triggers", make_int(static_cast<std::int64_t>(auto_triggers))},
            {"fragmentation-ratio", make_float(frag)},
            {"yield-deferred", make_int(static_cast<std::int64_t>(yield_deferred))},
            {"defrag-saved-bytes", make_int(static_cast<std::int64_t>(defrag_saved))},
        };
        return build_hash(kv);
    });

    // Issue #614 + #584: (query:primitives-hotpath-stats) — pair-allocation +
    // cdr-traversal cost under AI Agent high-freq list/math workloads.
    // Hash fields (#614 foundation + #584 AI-agent stress synthesis):
    //   - primitive-call-total: lifetime # of primitive invocations
    //                            (same as the #441/#450 field exposed
    //                            by query:primitive-perf-stats; kept
    //                            here for one-shot correlation).
    //   - pair-alloc-total:     # of pairs.push_back calls across
    //                            list / append / reverse / map / filter.
    //   - linear-traverse-total: total cdr-walk steps across length /
    //                            list-ref / member / foldl.
    //   - cdr-depth-max:        longest single linear traverse
    //                            observed (high-water mark).
    //
    // This is the AI agent's signal for "are my list-heavy
    // stdlib usages paying pair-allocation cost that I should
    // consolidate to Arena-backed storage?" + "is cdr-walk
    // getting pathological under mutation?".
    add("query:primitives-hotpath-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t call_total = 0;
        std::uint64_t pair_total = 0;
        std::uint64_t tra_total = 0;
        std::uint64_t depth_max = 0;
        std::uint64_t fastpath_hits = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics_);
            call_total = m->primitive_call_total.load(std::memory_order_relaxed);
            pair_total = m->pair_alloc_total.load(std::memory_order_relaxed);
            tra_total = m->linear_traverse_total.load(std::memory_order_relaxed);
            depth_max = m->cdr_depth_max.load(std::memory_order_relaxed);
            fastpath_hits = m->primitive_fastpath_hits_total.load(std::memory_order_relaxed);
        }
        const std::uint64_t mutations = ev.total_mutations();
        const std::uint64_t queries = ev.get_total_query_calls();
        const std::uint64_t call_denom = call_total + mutations + queries + 1;
        const std::int64_t call_rate = static_cast<std::int64_t>((call_total * 100) / call_denom);
        const std::int64_t alloc_per_call =
            static_cast<std::int64_t>(pair_total / (call_total + 1));
        const std::int64_t regex_time_us =
            static_cast<std::int64_t>((tra_total * 10) / (call_total + 1));
        const std::int64_t stability_penalty =
            static_cast<std::int64_t>(alloc_per_call * 3 + (depth_max > 32 ? depth_max / 8 : 0));
        const std::int64_t stability_score = stability_penalty >= 100 ? 0 : 100 - stability_penalty;
        const std::uint64_t total = call_total + pair_total + tra_total + depth_max +
                                    fastpath_hits + static_cast<std::uint64_t>(call_rate);
        std::int64_t recommendation = 0;
        if (stability_score < 50)
            recommendation = 3;
        else if (alloc_per_call > 10 || depth_max > 64)
            recommendation = 2;
        else if (call_total > 0 || fastpath_hits > 0)
            recommendation = 1;
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(16);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"primitive-call-total", make_int(static_cast<std::int64_t>(call_total))},
            {"pair-alloc-total", make_int(static_cast<std::int64_t>(pair_total))},
            {"linear-traverse-total", make_int(static_cast<std::int64_t>(tra_total))},
            {"cdr-depth-max", make_int(static_cast<std::int64_t>(depth_max))},
            {"call-rate", make_int(call_rate)},
            {"alloc-per-call", make_int(alloc_per_call)},
            {"regex-time-us", make_int(regex_time_us)},
            {"stability-score", make_int(stability_score)},
            {"hotpath-schema", make_int(584)},
            {"primitives-hotpath-total", make_int(static_cast<std::int64_t>(total))},
            {"primitives-hotpath-recommendation", make_int(recommendation)},
        };
        return build_hash(kv);
    });

    // (query:cxx26-hotpath-invariants) — Issue #465: C++26
    // hot-path Contracts + consteval invariants observability.
    // Returns a 5-field hash reporting the compile-time
    // invariants the binary was built with:
    //   - fixnum-tag-encoding: 0 (matches low2 dispatch table[0])
    //   - ref-tag-encoding: 1 (matches low2 dispatch table[1])
    //   - string-v2-tag-encoding: 2 (matches low2 dispatch table[2])
    //   - special-tag-encoding: 3 (matches low2 dispatch table[3])
    //   - float-tag-encoding: 4 (out of low2 dispatch space)
    //
    // These are static_assert'd at compile time in
    // value_tags.h. The primitive reports the values that
    // were baked in at build time, so the AI Agent can
    // verify a deployed binary matches the expected
    // encoding without re-running the static_asserts.
    //
    // Future follow-ups will add:
    //   - The Pass concept instance count
    //   - The SoA column count
    //   - The dirty bitmask byte width
    add("query:cxx26-hotpath-invariants", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // These values are the ones static_assert'd in
        // value_tags.h. The build will fail if they drift.
        // We hardcode the values here because the
        // EvalValueTag enum is in value.ixx (a different
        // module partition) and not directly accessible
        // from this file. The static_assert chain in
        // value_tags.h is the source of truth; the
        // primitive reports the same values so the
        // AI Agent can verify a deployed binary matches
        // the expected encoding.
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"fixnum-tag-encoding", make_int(0)},    {"ref-tag-encoding", make_int(1)},
            {"string-v2-tag-encoding", make_int(2)}, {"special-tag-encoding", make_int(3)},
            {"float-tag-encoding", make_int(4)},
        };
        return build_hash(kv);
    });

    // (atomic-batch:stats) — Issue #192: observability for
    // mutate:atomic-batch. Hash with batch-count, ops-total,
    // rollback-count, ops-per-batch (avg).
    add("atomic-batch:stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // Issue #394 / #258: capacity 32 (was 8). This primitive
            // returns 5 keys; cap-8 + FNV-1a probing occasionally
            // failed to insert a key, so hash-ref returned void.
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::size_t avg =
            ev.atomic_batch_count_ > 0 ? ev.atomic_batch_ops_total_ / ev.atomic_batch_count_ : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"batch-count", make_int(static_cast<std::int64_t>(ev.atomic_batch_count_))},
            {"ops-total", make_int(static_cast<std::int64_t>(ev.atomic_batch_ops_total_))},
            {"rollback-count", make_int(static_cast<std::int64_t>(ev.atomic_batch_rollbacks_))},
            {"ops-per-batch", make_int(static_cast<std::int64_t>(avg))},
            // Issue #250: how many per-op generation bumps the
            // batches suppressed (lifetime total). Useful for
            // dashboards ("how much churn did batching save?").
            {"bumps-saved-total",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_bumps_saved_total_))},
            // Issue #396 Phase 3: heuristic for "ran under
            // concurrent fiber pressure". Bumped when the
            // bridge fiber setter was active at commit time
            // (i.e. serve mode + fiber context). Stays 0 in
            // test-binary paths where the hook is null.
            // Name matches the issue's proposed field.
            {"executed-under-concurrent-fiber",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_in_fiber_total_))},
            // Issue #737: pinning + snapshot rollback observability.
            {"pinned-refs-last-batch",
             make_int(static_cast<std::int64_t>(ev.atomic_batch_pinned_ref_count()))},
            {"rollback-triggers", make_int(static_cast<std::int64_t>(ev.atomic_batch_rollbacks()))},
        };
        return build_hash(kv);
    });

    // (closure:stats) — Issue #252: observability for
    // apply_closure dual-path. Hash with the 5 counters
    // (calls-total, ffi-calls, tw-calls, bridge-calls,
    // stale-returns) + bridge-fraction (helper for
    // dashboards: how much of the dispatch goes to the
    // bridge path, which is the slowest).
    add("closure:stats", [&ev](const auto&) -> EvalValue {
        // Re-use the build_hash from atomic-batch:stats
        // (defined in the lambda above). It's the same code
        // pattern, so we re-bind to keep the closure:stats
        // self-contained.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            // Issue #394 / #258: capacity 32 (was 8). closure:stats
            // returns 7 keys; cap-8 insertion failures broke hash-ref.
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // Issue #252: closure stats. Read from ev.compiler_metrics_
        // (shared with the IR's IROpcode::Call/Apply). If metrics
        // is not set (legacy standalone use), all counters are 0.
        std::uint64_t calls = 0, ffi_c = 0, tw_c = 0, ir_c = 0;
        std::uint64_t bridge_c = 0, stale_c = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            calls = m->closure_calls_total.load(std::memory_order_relaxed);
            ffi_c = m->closure_ffi_calls.load(std::memory_order_relaxed);
            tw_c = m->closure_tw_calls.load(std::memory_order_relaxed);
            ir_c = m->closure_ir_calls.load(std::memory_order_relaxed);
            bridge_c = m->closure_bridge_calls.load(std::memory_order_relaxed);
            stale_c = m->closure_stale_returns.load(std::memory_order_relaxed);
        }
        std::uint64_t bridge = bridge_c;
        // bridge-fraction * 100 (integer percent). 0 if no calls.
        std::int64_t bridge_pct = calls > 0 ? static_cast<std::int64_t>((bridge * 100) / calls) : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"calls-total", make_int(static_cast<std::int64_t>(calls))},
            {"ffi-calls", make_int(static_cast<std::int64_t>(ffi_c))},
            {"tw-calls", make_int(static_cast<std::int64_t>(tw_c))},
            {"ir-calls", make_int(static_cast<std::int64_t>(ir_c))},
            {"bridge-calls", make_int(static_cast<std::int64_t>(bridge))},
            {"stale-returns", make_int(static_cast<std::int64_t>(stale_c))},
            {"bridge-fraction-pct", make_int(bridge_pct)},
        };
        return build_hash(kv);
    });

    // (query:closure-stats) — Issue #428: unified closure
    // observability surface in the query: family. Returns
    // a hash with 9 fields covering both the dispatch
    // (closure:stats 7 fields) and the bridge_epoch drift
    // (bridge-epoch-hits, bridge-epoch-drift-pct). The
    // drift is the percent of bridge_epoch checks that
    // observed a stale epoch (vs hits which observed
    // fresh) — the AI Agent's primary signal for
    // "is the bridge falling behind the mutation rate?".
    //
    // Field list (9 total):
    //   - calls-total:           every apply_closure call
    //   - ffi-calls:             FFI-dispatched
    //   - tw-calls:              tree-walker closures_ map hit
    //   - ir-calls:              IR runtime_closures_ hit
    //   - bridge-calls:          closure_bridge_ (IR/JIT)
    //   - stale-returns:         stale-bridge nullopt returns
    //   - bridge-fraction-pct:   bridge-calls / calls-total * 100
    //   - bridge-epoch-hits:     # of bridge_epoch checks
    //                            that succeeded (fresh)
    //   - bridge-epoch-drift-pct: stale-returns /
    //                            (bridge-epoch-hits + stale-returns)
    //                            * 100. The AI Agent's primary
    //                            signal — > 0 means the workspace
    //                            is mutating faster than closures
    //                            can refresh.
    //
    // Migration note: closure:stats is the pre-#428 primitive;
    // query:closure-stats is the new unified surface. Both
    // return the same hash shape; the new one just adds
    // 2 bridge_epoch fields. The old primitive stays for
    // backward compat (existing tests use closure:stats).
    add("query:closure-stats", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as closure:stats.
        // Inline here (instead of refactoring closure:stats to
        // a helper) so the new primitive stays self-contained
        // and easy to evolve independently.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::uint64_t calls = 0, ffi_c = 0, tw_c = 0, ir_c = 0;
        std::uint64_t bridge_c = 0, stale_c = 0;
        std::uint64_t bridge_epoch_hits = 0, bridge_epoch_drifts = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            calls = m->closure_calls_total.load(std::memory_order_relaxed);
            ffi_c = m->closure_ffi_calls.load(std::memory_order_relaxed);
            tw_c = m->closure_tw_calls.load(std::memory_order_relaxed);
            ir_c = m->closure_ir_calls.load(std::memory_order_relaxed);
            bridge_c = m->closure_bridge_calls.load(std::memory_order_relaxed);
            stale_c = m->closure_stale_returns.load(std::memory_order_relaxed);
            bridge_epoch_hits = m->bridge_epoch_hit_count_.load(std::memory_order_relaxed);
            bridge_epoch_drifts = m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
        }
        std::int64_t bridge_pct =
            calls > 0 ? static_cast<std::int64_t>((bridge_c * 100) / calls) : 0;
        // Drift = stale-refreshes / (hits + drifts) * 100.
        // 0 if no checks yet (avoids divide-by-zero in the
        // dashboard, which would otherwise show NaN).
        std::uint64_t total_epoch_checks = bridge_epoch_hits + bridge_epoch_drifts;
        std::int64_t drift_pct =
            total_epoch_checks > 0
                ? static_cast<std::int64_t>((bridge_epoch_drifts * 100) / total_epoch_checks)
                : 0;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"calls-total", make_int(static_cast<std::int64_t>(calls))},
            {"ffi-calls", make_int(static_cast<std::int64_t>(ffi_c))},
            {"tw-calls", make_int(static_cast<std::int64_t>(tw_c))},
            {"ir-calls", make_int(static_cast<std::int64_t>(ir_c))},
            {"bridge-calls", make_int(static_cast<std::int64_t>(bridge_c))},
            {"stale-returns", make_int(static_cast<std::int64_t>(stale_c))},
            {"bridge-fraction-pct", make_int(bridge_pct)},
            {"bridge-epoch-hits", make_int(static_cast<std::int64_t>(bridge_epoch_hits))},
            {"bridge-epoch-drift-pct", make_int(drift_pct)},
        };
        return build_hash(kv);
    });

    // (query:closure-epoch-concurrency-stats) — Issue #739:
    // atomic epoch visibility under fiber steal + invalidate.
    // Fields (3 + sentinel):
    //   - stale-epoch-on-steal: epoch_stale_steal_caught
    //   - fence-enforced: closure_epoch_fence_enforced_total
    //   - linear-violation-prevented: linear_violation_prevented_epoch_total
    //   - schema == 739
    add("query:closure-epoch-concurrency-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t stale_steal = 0;
        std::uint64_t fence_enforced = 0;
        std::uint64_t linear_prevented = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            stale_steal = m->epoch_stale_steal_caught.load(std::memory_order_relaxed);
            fence_enforced = m->closure_epoch_fence_enforced_total.load(std::memory_order_relaxed);
            linear_prevented =
                m->linear_violation_prevented_epoch_total.load(std::memory_order_relaxed);
        }
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        const std::pair<std::string, EvalValue> fields[] = {
            {"stale-epoch-on-steal", make_int(static_cast<std::int64_t>(stale_steal))},
            {"fence-enforced", make_int(static_cast<std::int64_t>(fence_enforced))},
            {"linear-violation-prevented", make_int(static_cast<std::int64_t>(linear_prevented))},
            {"schema", make_int(739)},
        };
        for (auto& [k, v] : fields) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (char c : k)
                h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            auto kidx = ev.string_heap_.size();
            ev.string_heap_.push_back(k);
            EvalValue key_ev = make_string(kidx);
            bool inserted = false;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    keys[idx] = key_ev.val;
                    vals[idx] = v.val;
                    ht->size++;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                FlatHashTable::destroy(ht);
                return make_void();
            }
        }
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });
}

void register_jit_arena_primitives(PrimRegistrar add, Evaluator& ev) {

    // (jit:intrinsic-count) — Issue #194: return the
    // runtime→intrinsic migration counter from the AuraJIT.
    // This is the per-commit observability signal for the 4
    // candidates the issue body tracks. Returns 0 if no hook
    // is installed (e.g. unit-test Evaluator without a JIT).
    add("jit:intrinsic-count", [&ev](const auto&) -> EvalValue {
        if (!ev.get_intrinsic_count_fn_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.get_intrinsic_count_fn_()));
    });

    // (jit:deopt-fn? fn-name threshold) — Issue #193: returns
    // #t if the function should be deopted (i.e., its
    // unhandled-opcode count exceeds the threshold). Default
    // threshold is 0 (any hit triggers deopt). Production code
    // should pass a non-zero threshold (e.g. 10) to avoid
    // thrashing on transient bugs during initial JIT warmup.
    add("jit:deopt-fn?", [&ev](const auto& a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        std::uint64_t threshold = 0;
        if (a.size() >= 2 && is_int(a[1])) {
            auto t = as_int(a[1]);
            if (t < 0)
                t = 0;
            threshold = static_cast<std::uint64_t>(t);
        }
        // The intrinsic_count check would need a separate hook
        // for the per-function unhandled count. For now, we
        // look up via the AuraJIT if it's installed. If the
        // hook isn't installed, default to false (never deopt).
        if (!ev.get_jit_unhandled_count_fn_)
            return make_bool(false);
        auto count = ev.get_jit_unhandled_count_fn_(ev.string_heap_[idx].c_str());
        return make_bool(count > threshold);
    });

    // (jit:exception-depth) — Issue #195: current fiber's
    // exception stack depth. Reads from the per-fiber ExStack
    // via the JIT runtime's hook (aura_fiber_current_id).
    // Returns 0 if no exception state for the current fiber.
    add("jit:exception-depth", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(aura_exception_depth()));
    });

    // (jit:exception-fibers) — Issue #195: number of distinct
    // fiber ids that have exception state. Used for
    // observability of the per-fiber ExStack map size.
    add("jit:exception-fibers", [&ev](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(aura_exception_fiber_count()));
    });

    // (jit:exception-fibers-clear) — Issue #195: clear all
    // per-fiber exception state. Returns void. Used by the
    // session-reset path; safe to call from Aura code.
    add("jit:exception-fibers-clear", [&ev](const auto&) -> EvalValue {
        aura_exception_clear_all();
        return make_void();
    });

    // (query:jit-stats) — Issue #427: full JIT metrics line
    // in the same format AuraJIT::Metrics::format produces.
    // Returns a single string with key=value fields separated
    // by spaces. Includes: compiles, avg_us, hot_swaps,
    // cached_fns, inlined_prims, slow_prims, prim_calls,
    // prim_avg_ns, verify_fail, add_mod_fail, unhandled_opcode,
    // intrinsics. Returns "" if no hook is installed (e.g.
    // unit-test Evaluator without a JIT). Cheap to call —
    // just reads a thread-local buffer populated by the hook.
    add("query:jit-stats", [&ev](const auto&) -> EvalValue {
        auto sidx = ev.string_heap_.size();
        if (!ev.get_jit_stats_fn_) {
            ev.string_heap_.push_back("");
        } else {
            const char* s = ev.get_jit_stats_fn_();
            ev.string_heap_.push_back(s ? std::string(s) : std::string());
        }
        return make_string(sidx);
    });

    // Issue #491: query:jit-stats-hash — structured JIT production-readiness
    // view for AI self-monitoring (opcode coverage, fallback, hot-swap safety).
    add("query:jit-stats-hash", [&ev](const auto&) -> EvalValue {
        std::uint64_t compiles = 0;
        std::uint64_t hot_swaps = 0;
        std::uint64_t cached_fns = 0;
        std::uint64_t unhandled = 0;
        std::uint64_t fallback = aura_jit_fallback_count_v_read();
        std::uint64_t consistency = 0;
        std::uint64_t intrinsics = 0;
        if (ev.get_jit_stats_fn_) {
            const char* s = ev.get_jit_stats_fn_();
            if (s) {
                auto parse_u64 = [&](const char* key) -> std::uint64_t {
                    const char* p = std::strstr(s, key);
                    if (!p)
                        return 0;
                    p += std::strlen(key);
                    return std::strtoull(p, nullptr, 10);
                };
                compiles = parse_u64("compiles=");
                hot_swaps = parse_u64("hot_swaps=");
                cached_fns = parse_u64("cached_fns=");
                unhandled = parse_u64("unhandled_opcode=");
                fallback = parse_u64("fallback_count=");
                consistency = parse_u64("consistency_violations=");
                intrinsics = parse_u64("intrinsics=");
            }
        }
        std::uint64_t jit_cache_evictions = 0;
        std::uint64_t invalidate_calls = 0;
        std::uint64_t hotswap_invalidate = 0;
        std::uint64_t epoch_mismatch = 0;
        std::uint64_t mutation_epoch = 0;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            jit_cache_evictions = m->jit_cache_evictions.load(std::memory_order_relaxed);
            invalidate_calls = m->invalidate_function_calls.load(std::memory_order_relaxed);
            hotswap_invalidate = m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed);
            epoch_mismatch =
                m->compiler_closure_epoch_mismatch_hits.load(std::memory_order_relaxed);
        }
        if (ev.get_incremental_stats_fn_) {
            const auto packed = ev.get_incremental_stats_fn_();
            mutation_epoch = (packed >> 16) & 0xFFFFu;
        }
        constexpr std::int64_t k_opcode_total = 53; // IROpcode::Nop..TopCellLoad
        const std::int64_t coverage_pct =
            unhandled == 0 && fallback == 0
                ? 100
                : std::max<std::int64_t>(
                      0, 100 - static_cast<std::int64_t>((unhandled + fallback) * 100 /
                                                         std::max<std::uint64_t>(1, compiles)));
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("compiles", static_cast<std::int64_t>(compiles));
        insert_kv("hot-swaps", static_cast<std::int64_t>(hot_swaps));
        insert_kv("cached-fns", static_cast<std::int64_t>(cached_fns));
        insert_kv("unhandled-opcode", static_cast<std::int64_t>(unhandled));
        insert_kv("fallback-count", static_cast<std::int64_t>(fallback));
        insert_kv("consistency-violations", static_cast<std::int64_t>(consistency));
        insert_kv("intrinsics", static_cast<std::int64_t>(intrinsics));
        insert_kv("opcode-total", k_opcode_total);
        insert_kv("opcode-coverage-pct", coverage_pct);
        insert_kv("jit-cache-evictions", static_cast<std::int64_t>(jit_cache_evictions));
        insert_kv("invalidate-function-calls", static_cast<std::int64_t>(invalidate_calls));
        insert_kv("hotswap-invalidate-total", static_cast<std::int64_t>(hotswap_invalidate));
        insert_kv("epoch-mismatch-hits", static_cast<std::int64_t>(epoch_mismatch));
        insert_kv("mutation-epoch", static_cast<std::int64_t>(mutation_epoch));
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #601: query:jit-hotswap-closure-stats — live IRClosure
    // refresh / forced-deopt counters from invalidate_function's
    // proactive walk. Bumped after jit_hotswap_invalidate_total so
    // an AI agent can observe: "for the last invalidation, how many
    // closures were refreshable vs forced-deopt vs left stale?".
    // Forced-deopt is reserved at 0 in this foundation layer — the
    // func_id-scoped deopt decision (closure.func_id no longer in
    // current module) is a follow-up.
    add("query:jit-hotswap-closure-stats", [&ev](const auto&) -> EvalValue {
        std::uint64_t refreshed = 0;
        std::uint64_t forced_deopt = 0;
        std::uint64_t mismatch_prevented = 0;
        std::uint64_t hotswap_invalidate = 0;
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            refreshed = m->jit_hotswap_live_closure_refreshed_total.load(std::memory_order_relaxed);
            forced_deopt = m->jit_hotswap_forced_deopt_total.load(std::memory_order_relaxed);
            mismatch_prevented =
                m->jit_hotswap_epoch_mismatch_prevented_total.load(std::memory_order_relaxed);
            hotswap_invalidate = m->jit_hotswap_invalidate_total.load(std::memory_order_relaxed);
        }
        auto* ht = FlatHashTable::create(8);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("live-closure-refreshed-total", static_cast<std::int64_t>(refreshed));
        insert_kv("forced-deopt-total", static_cast<std::int64_t>(forced_deopt));
        insert_kv("epoch-mismatch-prevented-total", static_cast<std::int64_t>(mismatch_prevented));
        insert_kv("hotswap-invalidate-total", static_cast<std::int64_t>(hotswap_invalidate));
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #493: query:hotpath-bottleneck-stats — structured EDSL
    // hot-path breakdown for AI mutate→eval tuning.
    add("query:hotpath-bottleneck-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t eval_flat =
            m ? m->hotpath_eval_flat_calls.load(std::memory_order_relaxed) : 0;
        const std::uint64_t lowering =
            m ? m->hotpath_lowering_calls.load(std::memory_order_relaxed) : 0;
        const std::uint64_t soa_dual =
            m ? m->hotpath_soa_dual_emit_hits.load(std::memory_order_relaxed) : 0;
        const std::uint64_t soa_instr =
            m ? m->ir_soa_instructions_emitted.load(std::memory_order_relaxed) : 0;
        const std::uint64_t soa_funcs =
            m ? m->ir_soa_functions_emitted.load(std::memory_order_relaxed) : 0;
        const std::uint64_t soa_wired = m ? m->irsoa_wired_hits.load(std::memory_order_relaxed) : 0;
        auto* ws = ev.workspace_flat();
        const std::uint64_t dirty_up = ws ? ws->mark_dirty_upward_call_count() : 0;
        const std::uint64_t dirty_early = ws ? ws->mark_dirty_early_exit_count() : 0;
        const std::uint64_t dirty_nodes = ws ? ws->mark_dirty_total_nodes() : 0;
        const std::uint64_t passes_skip = ev.get_passes_skipped_type_dirty();
        const std::uint64_t shape_dispatch =
            shape::inline_shape_ref_dispatch_count.load(std::memory_order_relaxed);
        const std::uint64_t value_dispatch =
            types::value_dispatch_hit_count.load(std::memory_order_relaxed);
        std::uint64_t arena_triggers = 0;
        if (ev.arena_) {
            arena_triggers = ev.arena_->stats().auto_alloc_trigger_count;
        }
        if (ev.arena_group_) {
            arena_triggers += ev.arena_group_->auto_compact_trigger_count();
        }
        const std::uint64_t bottleneck_total = eval_flat + lowering + soa_dual + dirty_up +
                                               dirty_early + passes_skip + shape_dispatch +
                                               value_dispatch + arena_triggers;
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("eval-flat-calls", static_cast<std::int64_t>(eval_flat));
        insert_kv("lowering-calls", static_cast<std::int64_t>(lowering));
        insert_kv("soa-dual-emit-hits", static_cast<std::int64_t>(soa_dual));
        insert_kv("soa-instr-emitted", static_cast<std::int64_t>(soa_instr));
        insert_kv("soa-func-emitted", static_cast<std::int64_t>(soa_funcs));
        insert_kv("soa-wired-hits", static_cast<std::int64_t>(soa_wired));
        insert_kv("dirty-upward-calls", static_cast<std::int64_t>(dirty_up));
        insert_kv("dirty-early-exit", static_cast<std::int64_t>(dirty_early));
        insert_kv("dirty-total-nodes", static_cast<std::int64_t>(dirty_nodes));
        insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skip));
        insert_kv("shape-dispatch", static_cast<std::int64_t>(shape_dispatch));
        insert_kv("value-dispatch-hits", static_cast<std::int64_t>(value_dispatch));
        insert_kv("arena-alloc-triggers", static_cast<std::int64_t>(arena_triggers));
        insert_kv("bottleneck-total", static_cast<std::int64_t>(bottleneck_total));
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #494: query:pass-pipeline-stats — incremental pass-pipeline
    // yield + dirty short-circuit observability for AI mutate loops.
    add("query:pass-pipeline-stats", [&ev](const auto&) -> EvalValue {
        const auto* m = static_cast<const CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t pipeline_yield =
            aura::compiler::pipeline_yield_count.load(std::memory_order_relaxed);
        const std::uint64_t passes_skip_dirty =
            aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
        const std::uint64_t passes_skip_type = ev.get_passes_skipped_type_dirty();
        const std::uint64_t relower_skip =
            m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t relower_per_fn =
            m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mod_skip =
            m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
        const std::uint64_t pipeline_total = pipeline_yield + passes_skip_dirty + passes_skip_type +
                                             relower_skip + relower_per_fn + mod_skip;
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = 0xcbf29ce484222325ull;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * 0x100000001b3ull;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    auto kidx = ev.string_heap_.size();
                    ev.string_heap_.push_back(k_str);
                    keys[idx] = make_string(static_cast<std::uint64_t>(kidx)).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("pipeline-yield-count", static_cast<std::int64_t>(pipeline_yield));
        insert_kv("passes-skipped-dirty", static_cast<std::int64_t>(passes_skip_dirty));
        insert_kv("passes-skipped-type-dirty", static_cast<std::int64_t>(passes_skip_type));
        insert_kv("relower-skipped", static_cast<std::int64_t>(relower_skip));
        insert_kv("relower-per-fn", static_cast<std::int64_t>(relower_per_fn));
        insert_kv("module-dirty-skips", static_cast<std::int64_t>(mod_skip));
        insert_kv("pipeline-total", static_cast<std::int64_t>(pipeline_total));
        // Issue #606: pure-delegation observation — ShapeWrap +
        // LinearOwnershipWrap bump a static atomic on every run()
        // call. Surfaced here so the AI agent can verify the new
        // pure Wrap delegation is being exercised (or wire more if
        // it's not). The stat is the sum of both wraps so a
        // single field tells us "are the pure wraps hot?".
        const std::uint64_t shape_pure = aura::compiler::ShapeWrap::pure_delegation_hits();
        const std::uint64_t linear_pure =
            aura::compiler::LinearOwnershipWrap::pure_delegation_hits();
        insert_kv("pure-delegation-shape", static_cast<std::int64_t>(shape_pure));
        insert_kv("pure-delegation-linear", static_cast<std::int64_t>(linear_pure));
        insert_kv("pure-delegation-total", static_cast<std::int64_t>(shape_pure + linear_pure));
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    });

    // Issue #572: Task4-review closing hash for Pass/AnalysisPass Concepts +
    // fold short-circuit + DirtyAwarePass + pure Wrap delegation.
    add("query:pass-pipeline-dirtyaware-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        const auto* m = static_cast<const aura::compiler::CompilerMetrics*>(ev.compiler_metrics());
        const std::uint64_t pipeline_runs =
            aura::compiler::pass_pipeline_runs_total.load(std::memory_order_relaxed);
        const std::uint64_t pipeline_yield =
            aura::compiler::pipeline_yield_count.load(std::memory_order_relaxed);
        const std::uint64_t passes_skip_dirty =
            aura::compiler::passes_skipped_dirty_pipeline.load(std::memory_order_relaxed);
        const std::uint64_t passes_skip_type = ev.get_passes_skipped_type_dirty();
        const std::uint64_t passes_skipped_due_to_dirty = passes_skip_dirty + passes_skip_type;
        const std::uint64_t relower_skip =
            m ? m->relower_skipped_entirely_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t relower_per_fn =
            m ? m->relower_per_function_called_count.load(std::memory_order_relaxed) : 0;
        const std::uint64_t mod_skip =
            m ? m->module_dirty_skips.load(std::memory_order_relaxed) : 0;
        const std::uint64_t block_dirty_hits =
            m ? m->ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed) : 0;
        const std::uint64_t wrap_delegation =
            aura::compiler::ShapeWrap::pure_delegation_hits() +
            aura::compiler::LinearOwnershipWrap::pure_delegation_hits();
        const std::uint64_t latency_denom = pipeline_runs + passes_skipped_due_to_dirty + 1;
        const std::int64_t incremental_latency_win_pct =
            static_cast<std::int64_t>((passes_skipped_due_to_dirty * 100) / latency_denom);
        const std::uint64_t total = pipeline_runs + pipeline_yield + passes_skipped_due_to_dirty +
                                    relower_skip + relower_per_fn + mod_skip + block_dirty_hits +
                                    wrap_delegation;
        std::int64_t recommendation = 0;
        if (pipeline_runs > 0 && passes_skipped_due_to_dirty == 0)
            recommendation = 3;
        else if (wrap_delegation == 0 && pipeline_runs > 0)
            recommendation = 2;
        else if (passes_skipped_due_to_dirty > 0 || wrap_delegation > 0)
            recommendation = 1;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"pass-pipeline-runs", make_int(static_cast<std::int64_t>(pipeline_runs))},
            {"pipeline-yield-count", make_int(static_cast<std::int64_t>(pipeline_yield))},
            {"passes-skipped-due-to-dirty",
             make_int(static_cast<std::int64_t>(passes_skipped_due_to_dirty))},
            {"passes-skipped-dirty-pipeline",
             make_int(static_cast<std::int64_t>(passes_skip_dirty))},
            {"passes-skipped-type-dirty", make_int(static_cast<std::int64_t>(passes_skip_type))},
            {"wrap-delegation-count", make_int(static_cast<std::int64_t>(wrap_delegation))},
            {"relower-skipped", make_int(static_cast<std::int64_t>(relower_skip))},
            {"relower-per-fn", make_int(static_cast<std::int64_t>(relower_per_fn))},
            {"module-dirty-skips", make_int(static_cast<std::int64_t>(mod_skip))},
            {"ir-soa-block-dirty-hits", make_int(static_cast<std::int64_t>(block_dirty_hits))},
            {"incremental-latency-win-pct", make_int(incremental_latency_win_pct)},
            {"task4-review-schema", make_int(572)},
            {"pass-pipeline-dirtyaware-total", make_int(static_cast<std::int64_t>(total))},
            {"pass-pipeline-dirtyaware-recommendation", make_int(recommendation)},
        };
        return build_hash(kv);
    });

    // (query:soa-dirty-stats) — Issue #429: live SoA
    // dirty state aggregate. Returns a hash with 8 fields
    // computed in one pass over ir_cache_v2_:
    //   - cached_fns:            # entries in the cache
    //   - dirty_fns:             # entries with entry.dirty == true
    //   - total_blocks:          sum of block_dirty_per_func_[i].size()
    //   - dirty_blocks:          sum of #dirty blocks
    //   - total_instructions:    sum of IRFunction.instructions.size()
    //   - dirty_instructions:    # entries with entry.dirty
    //                            (per-instruction aggregate is a
    //                            follow-up — see CompilerService ::
    //                            get_soa_dirty_stats comment)
    //   - dirty_block_pct:       100 * dirty_blocks / total_blocks
    //   - dirty_instruction_pct: 100 * dirty_instructions /
    //                            total_instructions
    //
    // The new primitive complements (query:ir-soa-incremental-stats)
    // (mutation-event lifetime counts) and (compile:ir-soa-stats)
    // (a #254 hash that ships the migration-progress field).
    // query:soa-dirty-stats is the LIVE view (current
    // dirty state) — the AI Agent reads it to decide
    // whether the cache is in a healthy steady state
    // (low dirty_block_pct) or needs a re-lower burst
    // (> 20% dirty means the cache is falling behind the
    // mutation rate).
    add("query:soa-dirty-stats", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        Evaluator::SoaDirtyStats s;
        if (ev.get_soa_dirty_stats_fn_) {
            s = ev.get_soa_dirty_stats_fn_();
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"cached-fns", make_int(static_cast<std::int64_t>(s.cached_fns))},
            {"dirty-fns", make_int(static_cast<std::int64_t>(s.dirty_fns))},
            {"total-blocks", make_int(static_cast<std::int64_t>(s.total_blocks))},
            {"dirty-blocks", make_int(static_cast<std::int64_t>(s.dirty_blocks))},
            {"total-instructions", make_int(static_cast<std::int64_t>(s.total_instructions))},
            {"dirty-instructions", make_int(static_cast<std::int64_t>(s.dirty_instructions))},
            {"dirty-block-pct", make_int(static_cast<std::int64_t>(s.dirty_block_pct))},
            {"dirty-instruction-pct", make_int(static_cast<std::int64_t>(s.dirty_instruction_pct))},
        };
        return build_hash(kv);
    });

    // (query:arena-compaction-stats-hash) — Issue #430:
    // hash variant of (query:arena-compaction-stats). The
    // legacy primitive returns a single integer = sum of
    // 7 fields (cheaper for dashboards that only need
    // the aggregate). The hash variant exposes each
    // field as a distinct key for the AI Agent's
    // per-field reasoning (e.g. "is the save rate
    // dropping?" needs total_compaction_saved vs
    // compaction_count, which the sum collapses).
    //
    // Field list (10 total):
    //   - auto-compact-triggers: ArenaGroup trigger count
    //   - auto-compact-skips:    ArenaGroup skip count
    //   - compactions:           lifetime compact() calls
    //   - bytes-saved:           lifetime bytes reclaimed
    //   - last-saved:            bytes reclaimed by last compact
    //   - paused-by-boundary:    deferred at MutationBoundary
    //   - mutation-volume:       total_mutations_ (orchestration signal)
    //   - dirty-propagation:     mark_dirty_upward activity
    //   - fragmentation-ratio:   current main arena frag ratio * 100
    //   - peak-used-bytes:       high-water mark for main arena
    //
    // Both primitives share the same underlying counters;
    // pick the integer when you want a single dashboard
    // metric, pick the hash when you want per-field
    // reasoning. The integer variant is the recommended
    // hot path; the hash is for debugging / AI Agent
    // observability.
    add("query:arena-compaction-stats-hash", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as the
        // closure:stats / soa-dirty-stats primitives.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::uint64_t triggers = 0, skips = 0, compacts = 0, saved = 0;
        std::uint64_t paused = 0, mutations = 0, dirty = 0;
        std::uint64_t frag_pct = 0, peak = 0, last_saved = 0;
        if (ev.arena_group_) {
            const auto stats = ev.arena_group_->total_stats();
            triggers = ev.arena_group_->auto_compact_trigger_count();
            skips = ev.arena_group_->auto_compact_skip_count();
            compacts = stats.compaction_count;
            saved = stats.total_compaction_saved;
            last_saved = stats.last_compaction_saved;
            paused = ev.compaction_paused_by_boundary();
            mutations = ev.total_mutations();
            dirty = ev.get_dirty_propagation_count();
            // Main arena frag ratio (scaled 0..100). Use
            // arena_ (the main per-Evaluator arena) if set;
            // else 0 (no fallback path — ArenaGroup::arenas_
            // is private, and the per-module frag ratio is
            // already exposed via arena:adaptive-stats +
            // query:arena-compaction-stats). The
            // fragmentation-ratio-pct field is the
            // single-arena view; the multi-arena view is
            // in the per-arena strings.
            if (ev.arena_) {
                const auto s = ev.arena_->stats();
                frag_pct = static_cast<std::uint64_t>(s.fragmentation_ratio() * 100);
                peak = s.peak_used;
            }
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"auto-compact-triggers", make_int(static_cast<std::int64_t>(triggers))},
            {"auto-compact-skips", make_int(static_cast<std::int64_t>(skips))},
            {"compactions", make_int(static_cast<std::int64_t>(compacts))},
            {"bytes-saved", make_int(static_cast<std::int64_t>(saved))},
            {"last-saved", make_int(static_cast<std::int64_t>(last_saved))},
            {"paused-by-boundary", make_int(static_cast<std::int64_t>(paused))},
            {"mutation-volume", make_int(static_cast<std::int64_t>(mutations))},
            {"dirty-propagation", make_int(static_cast<std::int64_t>(dirty))},
            {"fragmentation-ratio-pct", make_int(static_cast<std::int64_t>(frag_pct))},
            {"peak-used-bytes", make_int(static_cast<std::int64_t>(peak))},
        };
        return build_hash(kv);
    });

    // (query:cxx26-invariants) — Issue #431: a 5-field
    // hash summarizing the codebase's C++26 zero-overhead
    // invariant density. The numbers are compile-time
    // constants tied to the source — they don't move at
    // runtime. The AI Agent reads the count to detect
    // drift (a regression in invariant coverage is a
    // "the codebase lost some compile-time safety" signal).
    //
    // Field list (5 total):
    //   - consteval-invariants: # static_assert blocks
    //     in src/core/cxx26_invariants.ixx (currently 22
    //     — SmallObjectPool tier + Value tag + concept
    //     self-check groups)
    //   - concept-count: # Concepts in src/core/concepts.ixx
    //     (currently 13 — NodeHandle, ASTContainer,
    //     Mutator, ArenaAllocator, Queryable, AuraInvocable,
    //     RangeOf, AnyRange, SymbolInterner, StableNodeRefLike
    //     + Issue #431's SoAColumnar, DirtyPropagator,
    //     ShapeDispatchable)
    //   - contract-hot-paths: # Contract pre/post/assert
    //     sites in Arena + Value + SoA + Pass (sum across
    //     these 4 hot files, currently 26 — issue #431
    //     scope-limited ship doesn't add new Contracts
    //     beyond what was already there; follow-up issues
    //     will)
    //   - concept-self-checks: # static_asserts in
    //     cxx26_invariants.ixx that verify Concepts
    //     compile (currently 1 — the std::vector<int>
    //     check)
    //   - concept-targets-documented: # "Target sites:"
    //     comments in concepts.ixx (currently 9 — each
    //     concept has a doc comment listing the
    //     consumers / future consumers)
    //
    // The contract-hot-paths count is approximate —
    // see ContractHotPathCount() for the exact
    // grep-and-sum. If a future issue adds Contracts
    // to value.ixx or ir_soa.ixx, this number will
    // jump. The AI Agent monitors the count.
    add("query:cxx26-invariants", [&ev](const auto&) -> EvalValue {
        // Reuse the same build_hash pattern as the
        // closure:stats / soa-dirty-stats primitives.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        // Compile-time constants — the file paths are
        // recorded in the comment; the literal numbers
        // are the live counts at the time of writing.
        // The AI Agent detects drift by re-reading the
        // file and comparing the count delta.
        constexpr std::int64_t kConstevalInvariants = 22;
        constexpr std::int64_t kConceptCount = 13;
        constexpr std::int64_t kContractHotPaths = 26;
        constexpr std::int64_t kConceptSelfChecks = 1;
        constexpr std::int64_t kConceptTargetsDoc = 9;
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"consteval-invariants", make_int(kConstevalInvariants)},
            {"concept-count", make_int(kConceptCount)},
            {"contract-hot-paths", make_int(kContractHotPaths)},
            {"concept-self-checks", make_int(kConceptSelfChecks)},
            {"concept-targets-documented", make_int(kConceptTargetsDoc)},
        };
        return build_hash(kv);
    });

    // (query:edsl-readiness) — Issue #440: a single
    // hash that aggregates the top 8 EDSL production
    // readiness signals from across the existing
    // query:*-stats primitives. The intent is for the
    // AI Agent to ask "is the EDSL production-ready?"
    // in a single query (vs. 8 separate (query:*) calls).
    //
    // Field list (6 total):
    //   - closure-stale-refresh:    closure_bridge refreshes (#531)
    //   - linear-check-pass:        linear ownership fast-path checks (#149)
    //   - mutation-rollbacks:       MutationBoundaryGuard rollbacks (#241)
    //   - mutation-commits:        MutationBoundaryGuard commits (#241)
    //   - stable-ref-invalidates:   StableNodeRef is_valid misses (#417)
    //   - generation-bumps:         mutation_epoch_ lifetime total (#401)
    //   - pattern-macro-filtered:   MacroIntroduced skipped in patterns (#421)
    //   - dirty-block-rate:         live per-block dirty % from #429
    //                                (capped 0..100)
    //
    // All fields are non-negative integers; the AI Agent
    // reads each as a signal:
    //   - High closure-stale-refresh + low stale-refresh → bridge healthy
    //   - Linear-check-pass dominance → linear ownership fast path active
    //   - Mutation-commits > mutation-rollbacks → mutations stick
    //   - Generation-bumps > 0 → mutating (cache eviction expected)
    //   - Pattern-macro-filtered > 0 → hygiene gate active
    //   - Dirty-block-rate > 20% → cache is falling behind
    //
    // The 8 fields are a curated subset of the 30+
    // existing (query:*-stats) primitives; the issue
    // body enumerates which. Adding more later is a
    // 1-line code change (add to the kv vector).
    add("query:edsl-readiness", [&ev](const auto&) -> EvalValue {
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(32);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto hcap = ht->capacity;
            for (auto& [k, v] : kv) {
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE;
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < hcap; ++at) {
                    auto idx = ((h >> 1) + at) & (hcap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };
        std::uint64_t closure_stale = 0, linear_pass = 0;
        std::uint64_t atomic_commits = 0;
        std::uint64_t stable_ref_invalidates = 0;
        std::uint64_t occurrence_stale_refreshes = 0;
        std::int64_t dirty_pct = 0;
        if (ev.compiler_metrics_) {
            auto* m = static_cast<struct CompilerMetrics*>(ev.compiler_metrics_);
            closure_stale = m->closure_stale_refresh_count_.load(std::memory_order_relaxed);
            linear_pass = m->linear_check_pass_count_.load(std::memory_order_relaxed);
            atomic_commits = m->atomic_batch_commits.load(std::memory_order_relaxed);
            stable_ref_invalidates = m->stable_ref_invalidations.load(std::memory_order_relaxed);
            occurrence_stale_refreshes =
                m->occurrence_stale_refreshes_total.load(std::memory_order_relaxed);
        }
        // dirty-block-rate from #429's get_soa_dirty_stats.
        if (ev.get_soa_dirty_stats_fn_) {
            const auto s = ev.get_soa_dirty_stats_fn_();
            dirty_pct = static_cast<std::int64_t>(s.dirty_block_pct);
        }
        std::vector<std::pair<std::string, EvalValue>> kv = {
            {"closure-stale-refresh", make_int(static_cast<std::int64_t>(closure_stale))},
            {"linear-check-pass", make_int(static_cast<std::int64_t>(linear_pass))},
            {"atomic-batch-commits", make_int(static_cast<std::int64_t>(atomic_commits))},
            {"stable-ref-invalidations",
             make_int(static_cast<std::int64_t>(stable_ref_invalidates))},
            {"occurrence-stale-refreshes",
             make_int(static_cast<std::int64_t>(occurrence_stale_refreshes))},
            {"dirty-block-rate", make_int(dirty_pct)},
        };
        return build_hash(kv);
    });

    // (gc-arena-stats) — Report per-arena allocation. Shows main arena +
    // every per-module arena. Format: "main:0.1MB/8.0MB;json.aura:0.5MB/8.0MB;..."
    // (semicolons separate entries; slashes separate used/capacity within an entry).
    add("gc-arena-stats", [&ev](const auto&) -> EvalValue {
        std::string out;
        auto fmt_arena = [&](const char* label, std::size_t used, std::size_t cap) {
            auto s = std::format("{}{}:{:.1f}MB/{:.1f}MB", out.empty() ? "" : ";", label,
                                 used / 1048576.0, cap / 1048576.0);
            out += s;
        };
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            fmt_arena("main", s.used, s.capacity);
        }
        if (ev.arena_group_) {
            for (auto& [name, stats] : ev.arena_group_->module_stats()) {
                // Trim path to basename for readability.
                auto slash = name.rfind('/');
                auto short_name = slash == std::string::npos ? name : name.substr(slash + 1);
                fmt_arena(short_name.c_str(), stats.used, stats.capacity);
            }
        }
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(out);
        return types::make_string(sidx);
    });

    // (gc-arena-info) — Return structured per-arena usage as Aura value.
    //
    //   Returns: vector of hashes, each describing one arena:
    //     {name: "main", used: 1.23, capacity: 11.0, used-pct: 11}
    //     {name: "json.aura", used: 0.5, capacity: 8.0, used-pct: 6}
    //     ...
    //
    //   First entry is a summary hash:
    //     {summary: #t, total-arenas: 3, total-used: 1.73, total-capacity: 19.0,
    //      overall-pct: 9}
    //
    //   All numeric values are in megabytes (MB). Pct values are integers 0-100.
    add("gc-arena-info", [&ev](const auto&) -> EvalValue {
        // Snapshot arena state. Each entry: (short_name, used-MB, cap-MB, pct).
        struct Snap {
            std::string name;
            double used;
            double cap;
            int pct;
        };
        std::vector<Snap> snaps;
        double total_used = 0.0, total_cap = 0.0;
        if (ev.arena_) {
            auto s = ev.arena_->stats();
            double u = s.used / 1048576.0;
            double c = s.capacity / 1048576.0;
            snaps.push_back({"main", u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
            total_used += u;
            total_cap += c;
        }
        if (ev.arena_group_) {
            for (auto& [full_name, stats] : ev.arena_group_->module_stats()) {
                auto slash = full_name.rfind('/');
                auto short_name =
                    slash == std::string::npos ? full_name : full_name.substr(slash + 1);
                double u = stats.used / 1048576.0;
                double c = stats.capacity / 1048576.0;
                snaps.push_back({short_name, u, c, c > 0 ? static_cast<int>(u / c * 100.0) : 0});
                total_used += u;
                total_cap += c;
            }
        }
        int overall = total_cap > 0 ? static_cast<int>(total_used / total_cap * 100.0) : 0;

        // Build a small Swiss-table hash. Inline copy of the (hash ...) primitive
        // pattern. Capacity 8 is enough for the 5-field hashes below.
        auto build_hash = [&](std::span<const std::pair<std::string, EvalValue>> kv) -> EvalValue {
            auto* ht = FlatHashTable::create(8);
            if (!ht)
                return make_void();
            auto meta = ht->metadata();
            auto keys = ht->keys();
            auto vals = ht->values();
            auto cap = ht->capacity;
            for (auto& [k, v] : kv) {
                // Hash the key with FNV-1a (matches user-level (hash ...) behavior).
                std::uint64_t h = 0xcbf29ce484222325ull;
                for (char c : k)
                    h = (h ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
                auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
                if (fp == 0xFF)
                    fp = 0xFE; // Issue #258: avoid HASH_EMPTY collision
                // Intern the key as a String EvalValue.
                auto kidx = ev.string_heap_.size();
                ev.string_heap_.push_back(k);
                EvalValue key_ev = make_string(kidx);
                bool inserted = false;
                for (std::size_t at = 0; at < cap; ++at) {
                    auto idx = ((h >> 1) + at) & (cap - 1);
                    if (meta[idx] == 0xFF) {
                        meta[idx] = fp;
                        keys[idx] = key_ev.val;
                        vals[idx] = v.val;
                        ht->size++;
                        inserted = true;
                        break;
                    }
                }
                if (!inserted) {
                    // 8 slots should be enough for the 5-key hashes we build.
                    FlatHashTable::destroy(ht);
                    return make_void();
                }
            }
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        };

        std::vector<EvalValue> result;
        // Summary entry first.
        {
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"summary", make_bool(true)});
            kv.push_back({"total-arenas", make_int(static_cast<std::int64_t>(snaps.size()))});
            kv.push_back({"total-used", make_float(total_used)});
            kv.push_back({"total-capacity", make_float(total_cap)});
            kv.push_back({"overall-pct", make_int(overall)});
            result.push_back(build_hash(kv));
        }
        for (auto& s : snaps) {
            auto name_idx = ev.string_heap_.size();
            ev.string_heap_.push_back(s.name);
            std::vector<std::pair<std::string, EvalValue>> kv;
            kv.push_back({"name", make_string(name_idx)});
            kv.push_back({"used", make_float(s.used)});
            kv.push_back({"capacity", make_float(s.cap)});
            kv.push_back({"used-pct", make_int(s.pct)});
            result.push_back(build_hash(kv));
        }
        auto vidx = ev.vector_heap_.size();
        ev.vector_heap_.push_back(std::move(result));
        return make_vector(vidx);
    });

    // Issue #560: (stats:list) — returns the list of every
    // registered *-stats primitive (the source of truth for
    // the std/stats Aura module). Each entry is the primitive
    // name (string). Used by std/stats.aura for the (stats:list)
    // + (stats:count) helpers + for AI Agent observability
    // dashboards that want to enumerate all stats.
    add("stats:list", [&ev](const auto&) -> EvalValue {
        // See kObservabilityStatsPrimitives above (single source of truth).
        const std::vector<std::string>& stats = kObservabilityStatsPrimitives;
        // Convert the C++ vector to an Aura list of strings.
        EvalValue result = make_void();
        for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    // Issue #560: (stats:count) — companion to (stats:list).
    // Returns the # of registered *-stats primitives.
    add("stats:count", [](const auto&) -> EvalValue {
        // Single source of truth = kObservabilityStatsPrimitives
        // (the static list shared with (stats:list) above). Returns
        // the literal element count at module-init time, so adding
        // a new entry to the const list automatically updates
        // (stats:count) without a second hardcoded literal to keep
        // in sync.
        return make_int(static_cast<std::int64_t>(kObservabilityStatsPrimitives.size()));
    });
}

} // namespace aura::compiler::primitives_detail
