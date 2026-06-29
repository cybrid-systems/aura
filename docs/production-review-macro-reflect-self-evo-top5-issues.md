# Production Review: Top 5 Test-Coverage Focused Open Issues for Macro + Static Reflection + Self-Evolution (EDSL/Query-Mutate/Guard/Hygiene/Marker/Dirty/Observability)

**Date**: 2026-06-29
**Theme**: Aura macro system (clone_macro_body, SyntaxMarker::MacroIntroduced, hygiene in query/inliner), static reflection (reflect.hh, auto_* for self-mod support), self-evolution EDSL (query:pattern/mutate:* + MutationBoundaryGuard + dirty/epoch/observability) for reliable AI Agent code modification.
**Principles**: Strictly non-duplicative of existing meta/issues; code-evidence based (specific files/functions); test-coverage heavy (new/extend tests + TSan/ASan/CI); commercial production readiness (long-running autonomous agents, zero inconsistent state, measurable metrics); refined actionable with edit_file-ready diffs.
**All issues already exist as P0 non-duplicative with [TestCoverage] labels; this consolidates the top 5 most aligned with review theme.**

## 1. [Production][Runtime-Review][P0][TestCoverage] MutationBoundaryGuard + per-fiber mutation_stack sync + precise is_at_mutation_boundary_safe + scheduler steal defer (#588)

**Relevance to Theme**: Core safety primitive for self-evolution mutate loops. Ensures hygiene/safety of nested Guards during fiber migration/steal (prevents partial mutation state that could corrupt macro-expanded or reflected code). Directly supports reliable EDSL query/mutate under concurrency.

**Refined Focus (additive to existing)**: Strengthen per-fiber sync in resume(), outermost depth==0 enforcement in steal, Guard dtor commit + notify, new sync_per_fiber_mutation_stack helper. Expose enhanced query:orchestration-metrics or boundary stats.

**Code Evidence**: evaluator.ixx (Guard RAII, thread_local depth, active_mutation_stack), fiber.h (mutation_stack_ptr, is_at_*_safe, YieldReason), scheduler/worker steal path, evaluator_impl.cpp (mutate:* use Guard + yield).

**Test Coverage Refinement**: Extend existing multi-fiber tests + new `tests/test_runtime_mutation_boundary_steal_safety.cpp`: 8+ fibers × nested Guard mutate + random steal + GC → assert only outermost steal, stack consistent post-resume, no depth mismatch, TSan clean, metrics (steal_deferred_mutation_boundary_count). Stress 10k+ attempts; integrate ./build.py check + ASAN/TSAN.

**Commercial Benefits**: Bulletproof concurrent self-mod in production AI agents; zero race/starvation on Guard boundaries; predictable orchestration for long-running autonomous evolution.

**Priority**: P0
**Next**: Phase 1: per-fiber sync + precise is_at_*_safe + steal defer (edit_file fiber.cpp + evaluator_fiber_mutation.cpp); Phase 2: full TSan + metrics. Pair with scheduler issue. Update with PR links/checkboxes.

---

## 2. [Production][Stdlib-Review][P0][TestCoverage] AI Native Primitives Development Support: Templates, Static Reflection, Agent-Friendly Generation & Extension (#587)

**Relevance to Theme**: Directly leverages static reflection (reflect.hh nonstatic_data_members_of, reflect_members<T>, auto_serialize/deserialize/validate, module_exports) for self-modifiable primitives. Enables AI Agents to autonomously generate/extend EDSL primitives using reflection metadata (schema, mutation safety). Complements macro/hygiene by making new primitives first-class in query/mutate.

**Refined Focus (additive)**: Lightweight DEFINE_PRIMITIVE macro/template in evaluator.ixx or new primitives_meta.h for auto-registration + arity/doc/schema gen. Use C++26 std::meta or reflect for runtime `query:primitive-meta`. Agent-generated primitive example + test matrix. Update contributing.md with "AI Agent Primitive Development Guide".

**Code Evidence**: evaluator_primitives_registry.cpp (manual add lambda), evaluator.ixx (PrimFn), reflect.hh (full container + struct support, auto_*), recent reflection/Contracts work.

**Test Coverage Refinement**: Matrix: define via template/macro → register → query meta → use in mutate/eval → assert schema match; fuzz Agent-style generation. New test in primitives test suite; TSan under fibers.

**Commercial Benefits**: Lowers barrier for AI to extend stdlib autonomously; accelerates commercial EDA/SV primitives; embodies "AI Native" at primitives layer for self-evolving EDSL.

**Priority**: P0
**Next**: Phase 1: macro/template + basic meta query (edit_file evaluator.ixx + registry); Phase 2: full reflection + guide. Update issue.

---

## 3. [Production][Stdlib-Review][P0][TestCoverage] Comprehensive Test Matrix + Consistency Audit for evaluator_primitives_registry.cpp + Core Primitives in AI Agent Hot Paths (#583)

**Relevance to Theme**: Ensures test coverage and consistency for EDSL primitives (list/math/core/query) that power self-evolution mutate/query/eval loops. Includes hot-path stability under mutation churn, error uniformity (AuraError), registry extension safety. Supports reflection-driven primitives and hygiene in query patterns.

**Refined Focus (additive)**: New dedicated `tests/test_primitives_registry_core_consistency_task_stdlib.cpp` with registry extension matrix, core consistency under churn + fibers, hot-path AI Agent 10k+ mutate-query-eval cycles. Add [[hot]]/contract annotations; expose query:primitives-stats. Audit unify error paths to centralized make_primitive_error.

**Code Evidence**: evaluator_primitives_registry.cpp (register_all_primitives orchestration), evaluator.ixx (Primitives/PrimFn), core primitives files (lambda + coercion + error via make_primitive_error + atomic).

**Test Coverage Refinement**: Above matrix + fuzz random arg mixes + error injection; metrics (primitive_call_overhead_us, error_path_cost); integrate ./build.py check + multi-fiber CI stress. TSan/ASan clean.

**Commercial Benefits**: Trustworthy consistent performant stdlib for every AI self-evolution loop; eliminates silent inconsistencies/perf cliffs; measurable reliability for commercial long-running agents.

**Priority**: P0
**Next**: Phase 1: test matrix + registry stress (edit_file new test + registry.cpp); Phase 2: hot-path metrics + consistency PR. Pair with #567 governance. Update with checkboxes.

---

## 4. [Production][Runtime-Review][P0][TestCoverage] Panic checkpoint + arena auto-rollback lifecycle with nested MutationBoundaryGuard + fiber yield/resume safety (#592)

**Relevance to Theme**: Hardens panic recovery in nested Guards during self-evo mutations (protects macro-expanded code, reflected structures, dirty/epoch state). Ensures arena snapshots consistent on fiber resume/steal, preventing partial state that breaks hygiene or reflection invariants.

**Refined Focus (additive to #548)**: Outermost Guard dtor success commit + clear; nested panic restore only if outermost; per-fiber checkpoint storage; post-restore arena size assert; new query:panic-checkpoint-lifecycle-stats.

**Code Evidence**: evaluator.ixx (save/restore/commit_panic_checkpoint, panic_safe_*_size_, auto_rollback_on_panic_), evaluator_impl.cpp (Guard in mutate:*), fiber.h (resume/yield at boundary).

**Test Coverage Refinement**: Extend test(353) + new `tests/test_panic_checkpoint_fiber_resume_safety.cpp`: Nested Guard mutate + panic injection + fiber yield/steal/resume → assert correct rollback, arena sizes consistent, no partial state, TSan clean. Matrix success vs panic vs migration; stress 5000+ scenarios.

**Commercial Benefits**: Ironclad panic recovery in every production AI self-modify loop under concurrency; zero inconsistent state; trustworthy long-running autonomous agents.

**Priority**: P0
**Next**: Phase 1: nested Guard lifecycle + fiber resume restore (edit_file evaluator.ixx + fiber.cpp); Phase 2: full concurrent stress. Update issue.

---

## 5. Hygiene + MacroIntroduced Propagation in query:pattern + IR InlinePass (refine existing #541 / #524 / related, or consolidate as top macro theme tracker)

**Relevance to Theme**: Core to macro system hygiene (clone_macro_body sets SyntaxMarker::MacroIntroduced + name_map; query:pattern / inliner must respect/filter to prevent capture in self-evo loops). Complements static reflection by ensuring macro-generated code is safely navigable/mutable.

**Refined Focus (additive)**: Dirty-aware incremental tag_arity_index in ast.ixx; core matcher early filter for MacroIntroduced (default skip with :respect-hygiene flag); InlinePass enforce source_marker hygiene; metrics query:pattern-hygiene-stats + ir-hygiene-stats. Wire to dirty/epoch for incremental self-evo.

**Code Evidence**: macro_expansion.ixx (clone_macro_body cloned_marker + name_map), ir.ixx (IRInstruction::source_marker, InlinePass respect_macro_hygiene_), query_matcher / evaluator_primitives_query (pattern + by-marker, hygiene-stats counter).

**Test Coverage Refinement**: New `tests/test_query_pattern_hygiene_macrointroduced.cpp`: macro expand (MacroIntroduced nodes) → query:pattern (filtered, no capture) → mutate safe → IR no violation; fuzz generate+macro SV + verification sequences; metrics accurate; TSan clean. >30% latency win on large AST post-mutate.

**Commercial Benefits**: Safe predictable macro usage in production AI self-evolution; completes AST→query→IR hygiene closed loop; enables trustworthy macro-heavy EDSL.

**Priority**: P0
**Next**: Phase 1: delta index hook + matcher filter (edit_file ast.ixx + query_matcher.cpp); Phase 2: metrics + SV-scale test. Update consolidated hygiene issue or #541.

---

**Overall Process & Commercial North Star**
- Every implementation PR must link relevant issue(s) and state "non-duplicative refinement / additive to macro+reflect+self-evo production review".
- Pair every change with specified test matrix + TSan/ASan stress + new/enhanced query:*-stats primitive for observability.
- Update this file and linked issues with checkboxes + PR links as progress.
- Run full stress in CI (./build.py check + ASAN/TSAN).
- These 5 form the highest-leverage test-coverage cluster for making macro hygiene, reflection-driven primitives, Guard safety, and EDSL observability production-deployable for reliable AI Agent self-modifying code.

**Evidence Base**: Strict reading of latest main (June 28-29 2026) src/compiler/{macro_expansion.ixx, evaluator.ixx, evaluator_primitives_*.cpp, ir.ixx, query_matcher.cpp, type_checker_impl.cpp, pass_manager.ixx}, reflect/reflect.hh, recent commits (#548 Guard, #543 SoA, primitives registry, etc.). All verifiable via raw GitHub or git show.

**Status**: Open. Actionable, focused, ready for immediate phased implementation. Ready for triage/assignment.

*This file written to repo as consolidated tracker for the Task 6 review theme.*