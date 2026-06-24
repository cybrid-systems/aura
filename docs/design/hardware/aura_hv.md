# Aura-HV Self-Evolving Verification Loop

> Issue #295. Phase 0 MVP that closes the issue body.
> The full design is a multi-component architecture (intent, mutation,
> verification, feedback, strategy); Phase 0 ships the 2 simplest,
> most composable building blocks.

## Goal

Provide Aura users with the **2 simplest building blocks** for
hardware verification self-evolution:

1. `eda:query:coverage-holes` — find signals with no SVA assertions
2. `ws:try-mutation` — sandboxed eval with automatic rollback

These are the foundation for the full loop:
```
intent → query:coverage-holes → mutate → ws:try-mutation → commit/rollback
```

## What ships in Phase 0

**lib/std/eda.aura** — 1 new query helper:
```scheme
(eda:query:coverage-holes my-module)
;; → list of signal names NOT mentioned by any SVA assertion
;;   in the module body. Empty list if all signals are covered.
```

**evaluator_primitives_workspace.cpp** — 1 new C++ primitive:
```scheme
(ws:try-mutation expr-string)
;; → (result . snapshot-id) on success
;; → #f on parse failure or eval error (auto-rollback)
```

**tests/test_issue_295.cpp** — 6 ACs covering both helpers
(plus edge cases: full coverage, parse failure, bad args).

## The Self-Evolving Loop (Conceptual)

```scheme
;; 1. Identify gaps
(define holes (eda:query:coverage-holes my-module))

;; 2. Generate a mutation proposal (LLM or hand-coded)
(define proposal
  (string-append "(query:replace-signal-in-assertions \""
                 (car holes)
                 "\" \"err\")"))

;; 3. Sandbox-eval: success keeps a snapshot, failure auto-rolls back
(define result (ws:try-mutation proposal))

;; 4. Branch on success
(if (pair? result)
    (begin
      ;; Commit: keep the snapshot (or restore it from the pair)
      (display "Mutation committed!"))
    (display "Mutation failed; workspace rolled back"))
```

## What ships in subsequent phases (deferred)

| Phase | Scope | Status |
|---|---|---|
| **1** | `mutate:strengthen-assertion`, `mutate:inject-assertion`, `mutate:suggest-rtl-change`; error-signal-driven strategy selection; pheromone mechanism for mutation paths | OPEN |
| **2** | Integration with external formal tools (JasperGold, SymbiYosys, Verilator); structured result ingestion; coverage metrics from simulation | OPEN |
| **3** | Full agent loop scaffolding: intent → query → mutate → verify → adapt; PID-like coarse/fine granularity control; convergence criteria | OPEN |
| **4** | Large-design optimization, hierarchical verification, mutation testing safety analysis, false-positive prevention | OPEN |

## Design Decisions

1. **Pure-Aura for the query helper** — `eda:query:coverage-holes` walks
   the existing EDA IR. No new C++ modules. Matches the same pattern
   as the 3 query helpers from #294.
2. **Generic C++ primitive for the eval wrapper** — `ws:try-mutation`
   uses the existing `ast:snapshot` + `eval` + `ast:restore` building
   blocks. Composes well with the broader snapshot/restore infrastructure.
3. **Pair return value `(result . snap-id)`** — gives the caller
   everything needed for the commit path (use `ast:restore` to commit
   a different snapshot, or keep the current state for rollback).
4. **No coverage model — heuristic only** — Phase 0 uses "signal not
   mentioned by any assertion" as the gap definition. Full coverage
   model (toggle / statement / branch / expression) is Phase 2.

## AI-Native Properties (Phase 0 subset)

- ✅ Reflective: both helpers are EDSL-callable
- ✅ Composable: `coverage-holes` returns a list, callers chain with
  standard Aura `map` / `filter` / `string-append`
- ✅ Transactional: `ws:try-mutation` auto-rollback prevents agents
  from corrupting the workspace on bad mutations
- ✅ Workspace-aware: both helpers work in `set-code` / `eval` /
  `(load ...)` contexts

## Test Coverage

`tests/test_issue_295.cpp` covers:
- AC #1: coverage-holes returns un-asserted signal count
- AC #2: full coverage → empty holes
- AC #3: ws:try-mutation success returns (result . snap-id)
- AC #4: parse failure → #f (rollback verified)
- AC #5: non-string arg → #f

Wired into `test_issues_jit` bundle (54/54, was 53).

## Future Work (per issue body)

- **Mutation primitives** (Phase 1):
  - `mutate:strengthen-assertion assert-id :add-condition "..."`
  - `mutate:inject-assertion sub-spec "..."`
  - `mutate:suggest-rtl-change signal "req" :reason "..."`
- **Strategy selection** (Phase 1): error-signal-driven granularity
- **Pheromone mechanism** (Phase 1): prioritize historically successful mutations
- **Coverage model** (Phase 2): toggle / branch / expression coverage
- **Formal tool integration** (Phase 2): JasperGold / SymbiYosys / Verilator
- **Convergence criteria** (Phase 3): when to stop the loop
- **Hierarchical verification** (Phase 4)
