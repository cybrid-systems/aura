# Agentic Slim-Surface Rectification Plan

**Date**: 2026-07-15  
**Status**: Active synthesis of current open issues + concrete agent-facing implementation issues  
**Goal**: Focus on agentic self-evolution closed-loop + forced surface subtraction. Converge engine primitives to <=420 while hardening MutationBoundary / atomicity / type soundness / fiber safety so AI agents can reliably query -> decide -> mutate -> eval -> observe.

This document aggregates current open issues by theme and gives executable prioritization. It aligns fully with primitive-vs-stdlib-decision-framework.md, primitives-demotion-batch1.md and query-namespace-decision.md. It deliberately avoids repeating individual issue ACs or code snippets.

---

## 1. Current Open Issues Thematic Aggregation (2026-07-15 snapshot)

### A. SlimSurface / Primitives Governance (highest-priority subtraction)

Cluster covering:
- Engine primitives currently ~1064 (internal-observable / query:*-stats class ~504, convenience ~147).
- Epic target: converge to <=420.
- Infrastructure: registry gate, CI surface check, deprecation + alias, Observability Facade enforcement (stats:get / engine:metrics), refined governance policy (Code-as-Memory, Agent-Proof red lines).
- Test binding: every primitive change must ship with tests + CI hard gate.

Signal: Batch 1 demotion only delivered wrappers + candidate identification. Real engine deletion has not started. Without executing real subtraction, agent discovery cost and binary/JIT burden continue to grow.

### B. Mutation Safety and Atomicity (correctness base of agentic loop)

Cluster covering:
- Missing first-class (mutate:atomic-batch) EDSL surface (internal suppressed_bump_ / atomic_batch_depth_ / rollback_since already exist, but agents cannot cleanly express multi-step atomic transactions).
- Enforce every mutate:* path goes through outermost MutationBoundaryGuard (prevent AI-generated or new paths from bypassing).
- Long-mutation starvation prevention (hold-time already recorded; scheduler does not yet consume it for dynamic policy).
- Nested boundary + steal + GC compact full lifecycle re-pin for PanicCheckpoint / COW / StableNodeRef.
- Steal-path inner-boundary dynamic priority boost to prevent specific agent fibers from starving.

Signal: These are the production correctness and SLO critical paths for multi-fiber self-evolution. Subtraction must never compromise these safety contracts.

### C. Type Soundness under Mutation

Cluster covering:
- Occurrence narrowing staleness propagation after mutate.
- Incremental type-checking locality (affected_subtree).
- IR-level type propagation + CastOp zero-overhead.
- Linear Ownership hardening post-mutation.
- ADT / pattern matching / exhaustiveness (critical for self-modifying code safety).

Signal: After an agent mutates, the type system must remain trustworthy; otherwise the closed loop fails silently.

### D. Testing Governance Alignment

Cluster covering test-framework restructuring: layered (Unit + EDSL declarative + Property/Mutation Fuzz + Pets TUI), mandatory test binding for primitive changes, automated aura-pets regression, Self-Evolution closed-loop tests.

Signal: Without forced test binding, both subtraction and safety hardening will regress.

---

## 2. New Agent-Facing Implementation Issues (created 2026-07-15, non-overlapping)

These issues deliberately sit **above** the engine-level work already tracked. They turn the rectification plan into concrete agent-visible changes:

| Issue | Title | Focus |
|-------|-------|-------|
| #1460 | Rewrite std/agent auto-grow as true EDSL closed-loop | Force query → atomic-mutate → eval → real stats decide as the default path |
| #1461 | Define & enforce minimal Agent Decision Metrics contract | Stable, always-live metrics for back-off / escalate / commit |
| #1462 | Agent Migration Guide + temporary compatibility shims | Prevent breakage when Tier-1 query:* demotions land |
| #1463 | Self-Evolution Closed-Loop Reliability Suite | Multi-round reliability benchmark (complement to existing EDSL pass-rate) |

These four issues do **not** duplicate any current open engine/Guard/SlimSurface/type/testing-infrastructure issues.

---

## 3. Agentic-Focused Rectification Recommendations (subtraction + closed-loop hardening)

### Priority 0 (this week)

1. **Land SlimSurface infrastructure** — **#1448 done**  
   Registry gate + `scripts/check_primitive_surface.py --strict` + `PrimMeta.deprecated` dispatch counter + `docs/design/primitives-slim-surface-v2.md` + gate CI + `test_primitives_surface_convergence`.  
2. **Observability facade Phase 1** — **#1450 done** (epic #1449)  
   Engine `(stats:get)` / `(stats:prefix)` / `(engine:surface)`; residual 10 public `*-stats` marked deprecated + catalogued.  
   Remaining: real demotion to public ≤420; hard-remove residual aliases.

2. **MutationBoundary contract + first-class atomic-batch**  
   - Every mutate:* entry must go through outermost Guard (runtime contract + metric).  
   - Expose (mutate:atomic-batch), reusing existing suppressed_bump_ / rollback_since infrastructure.  
   This directly serves the agentic loop: agents can cleanly write multi-step mutate as a single atomic transaction.

3. **Long-mutation + PanicCheckpoint lifecycle**  
   Guard dtor consumes hold-time and triggers scheduler policy; nested + steal + GC re-pin must be 100% reliable. This is the correctness floor for multi-agent concurrent self-evolution.

### Priority 1 (this iteration)

4. **Execute real Tier-1 demotion** + **#1462 Migration Guide & shims**  
   Land the previously identified query:siblings / find-by-name / nodes-with-marker / subtree etc. engine deletions (deprecation cycle first, then hard remove). Ship the agent migration guide and temporary shims in the same window.

5. **Type incremental + post-mutation soundness**  
   Prioritize occurrence-narrowing propagation + affected_subtree locality.

6. **Forced testing closed-loop** + start **#1460 / #1461**  
   Land declarative edsl_self_test + CI gate. In parallel, rewrite std/agent to the true EDSL loop and lock down the Decision Metrics contract.

### Priority 2 (follow-on)

7. Steal fairness + dynamic priority.  
8. Deeper type safety (Linear Ownership, ADT exhaustiveness).  
9. Hard-removal completion + final <=420 validation + **#1463** reliability suite.

---

## 4. Direct Mapping to Agentic Niche

| Agentic need | Corresponding rectification | Why subtraction helps |
|--------------|-----------------------------|-----------------------|
| Precise self-modification loop | atomic-batch + Guard enforcement + #1460 closed-loop rewrite | Clean surface + forced EDSL path makes correct usage the default |
| Multi-agent concurrent safety | fiber/steal/long-mutation/PanicCheckpoint | Smaller engine is easier to prove concurrent invariants on |
| Decision observability | stats facade + **#1461 Decision Metrics contract** | Removing fake/redundant stats + guaranteeing a live minimal set makes back-off / escalate trustworthy |
| Long-term maintainability + 1M context | surface <=420 + governance + **#1462 migration** | Smaller engine + clear migration sharply reduces discovery cost and breakage |
| Formal correctness + measurable reliability | type post-mutation soundness + test binding + **#1463 suite** | After subtraction, effort and measurement concentrate on the paths that actually affect correctness |

**Subtraction is not feature deletion.** It moves non-red-line capability into stdlib so the engine only retains the hooks the agentic closed-loop truly depends on.

---

## 5. Execution Discipline

- Every new primitive must first pass the red-line check in primitive-vs-stdlib-decision-framework + the governance policy.
- Every demotion / Guard / atomic-batch change must update edsl_self_test + corresponding C++ tests and link the relevant issues.
- Do not expand individual issue ACs or pseudo-code in this document (they already live in the issues). Keep the thematic aggregation style.
- When updating this file, also update the P-series status in docs/roadmap.md.

---

## 6. Immediate Next Actions

1. Merge / advance the SlimSurface infrastructure work (registry gate + surface-check script).  
2. Implement (mutate:atomic-batch) + Guard enforcement contract (can proceed in parallel).  
3. Start the first real engine demotion batch (query: Tier-1) together with #1462 Migration Guide.  
4. Kick off #1460 (std/agent closed-loop) and #1461 (Decision Metrics contract) as soon as atomic-batch is usable (or stubbed).  
5. Keep this document as the single unified rectification view; link it from the Epic and from #1460–#1463.

---

*Synthesized by Grok from the 2026-07-15 open-issue list + existing demotion framework + production review. New agent-facing implementation issues #1460–#1463 added the same day. Subsequent iterations should edit this file while preserving the thematic aggregation style.*
