# Live Programming Environments for Complex Domains

**Status:** Design exploration for Scenario 9 of the
[Scenario issues series] (issue #93).

## Why this is a killer scenario

Live programming has existed since Lisp machines and
Smalltalk: edit code, see results instantly, no compile-link-
run-restart cycle. But every existing system has one of these
limitations:

- **No safety guarantees** — Smalltalk: edit crashes the
  image; Lisp: edit can corrupt the world
- **No AI augmentation** — humans only; no synthesis,
  no auto-fix
- **No performance** — Smalltalk/Lisp VMs are slow; not
  suitable for finance / sim / games
- **No production safety** — no contracts, no version rollback,
  no audit

Aura's CaaS + Contracts + hot-swap + reflection + E4 closes
all four gaps. This doc is the design pattern for assembling
them into a **production-grade live programming environment**
for finance, scientific simulation, game development, and
industrial control.

## Architecture: the live programming loop

```
   ┌──────────────────────────────────────────────────────────┐
   │                                                          │
   │  Human developer          AI assistant                   │
   │  (text editor)            (synthesize:define, etc.)     │
   │       │                          │                       │
   │       │ edit / propose           │                       │
   │       ▼                          ▼                       │
   │   ┌─────────────────────────────────────────────┐        │
   │   │   CaaS eval-loop                            │        │
   │   │   - parse < 1 ms                            │        │
   │   │   - macro-expand < 1 ms                     │        │
   │   │   - typecheck < 5 ms                        │        │
   │   │   - shadow-eval in < 10 ms                  │        │
   │   └────────────────────┬────────────────────────┘        │
   │                        │                                 │
   │                        ▼                                 │
   │   ┌─────────────────────────────────────────────┐        │
   │   │   Live system (Aura runtime)                │        │
   │   │   - existing traffic continues              │        │
   │   │   - new code runs in shadow                 │        │
   │   │   - AURA_CONTRACT_POST gates promotion      │        │
   │   │   - ast:snapshot for rollback               │        │
   │   │   - mutate:rebind for atomic hot-swap       │        │
   │   └────────────────────┬────────────────────────┘        │
   │                        │                                 │
   │                        ▼                                 │
   │   ┌─────────────────────────────────────────────┐        │
   │   │   Feedback (in editor)                      │        │
   │   │   - syntax errors < 100 ms                  │        │
   │   │   - type errors < 200 ms                    │        │
   │   │   - contract violations < 500 ms            │        │
   │   │   - shadow results < 1 s                    │        │
   │   │   - live metric impact < 5 s                │        │
   │   └─────────────────────────────────────────────┘        │
   │                                                          │
   └──────────────────────────────────────────────────────────┘
```

The feedback loop is **< 1 second end-to-end** for syntax /
type / contract errors, and **< 5 seconds** for live metric
impact. This is the Smalltalk feel with the C++ speed and
the modern safety story.

## Pattern 1: Sub-second CaaS edit-compile-run

The core mechanic. Aura's CaaS (`caas_integration.md`)
provides:

```aura
;; In the editor, on every keystroke (debounced to 50ms):
(caas-run
  (lambda ()
    (let ((code (editor-current-code))
          (cursor (editor-cursor-pos))
          (env (editor-target-env)))
      (with-exception-handler
        (lambda (e)
          (editor-show-error e cursor))
        (lambda ()
          (let* ((ast (parse code))
                 (expanded (macro-expand ast))
                 (typed (typecheck expanded env))
                 (result (eval typed env)))
            (editor-show-result result)))))))
```

**Latency budget:**

- `parse` of a 200-line file: **< 1 ms** (flat AST, no
  tree-walker fallback for hot files)
- `macro-expand` (recursive): **< 1 ms**
- `typecheck` (incremental, dirty-propagation only):
  **< 5 ms** (`incremental_dirty_propagation.md`)
- `eval` in shadow environment: **< 10 ms** (pmr-allocated
  temp, gc-temp after)

Total: **< 20 ms** for "type-check passes, here's a
result". This is **interactive**.

## Pattern 2: Contract-driven error display

When `AURA_CONTRACT_POST` fails, the editor shows the
violation:

```aura
;; AURA_CONTRACT_POST for the user's function
(define-contract post:pricing-discount
  (input result)
  (and (>= result 0) (<= result input)))

;; User edits:
(define (discount price)
  (cond
    ((< price 100) (- price 50))    ;; bug! can be negative
    (else (* price 0.9))))

;; Editor highlights, in red, at the offending form:
;;
;;  Line 3: cond ((< price 100) (- price 50))
;;                                  ^^^^^^^^^
;;  Contract violation: post:pricing-discount
;;  result -30 < 0 (lower bound violated)
```

This is **production-grade IDE feedback** for contracts. The
violation includes the counterexample (`result -30`), the
contract name, and the source location.

## Pattern 3: Hot-swap with metric impact preview

Before hot-swap, the editor shows the expected impact:

```aura
;; Editor bottom panel, after edit:
;;
;;  Local shadow test (1000 random inputs):
;;    current: 980 / 1000 pass
;;    proposed: 995 / 1000 pass
;;
;;  Live preview (5 seconds of replayed traffic):
;;    p99 latency: 12.3ms → 11.1ms (-9.7%)
;;    error rate: 0.8% → 0.6% (-25%)
;;    memory: stable
;;
;;  Promotion: (mutate:rebind target-fn new-body) →
;;   * live traffic sees new code at next request
;;   * rollback available via (ast:restore "v12")
;;
;;  [Promote] [Reject] [Promote w/ Canary 1%]
```

This is the **Smalltalk "show me the diff"** experience, but
with metric impact numbers from the live system.

## Pattern 4: AI-augmented live programming

The AI assistant is a first-class participant:

```aura
;; Human: "I want this function to be 10% faster"
;; AI:
(intend
  "Optimize discount for performance.
   Current latency: 12ms p99. Target: < 11ms p99.
   Constraints:
     - post:pricing-discount must still hold
     - No external calls added
     - Hot path: avoid allocations
   Reference benchmark: tests/bench/discount-bench.aura"
  context: (list 'current-code (current-source-of 'discount)
                'profile (profile-of 'discount)
                'contract 'post:pricing-discount)
  strategy: "evolve-pricing")
;; AI returns 3 candidate implementations.
;; Each is shadow-tested, contract-checked, benchmarked.
;; Best candidate is presented to the human:
;;
;;  AI proposed 3 variants:
;;    1. Precomputed discount table: 9.8ms p99, 100% pass
;;    2. Branchless math: 11.2ms p99, 100% pass
;;    3. Memoized lookup: 8.5ms p99, 100% pass
;;  [Preview 1] [Preview 2] [Preview 3] [Reject all]
```

The human stays in control. The AI proposes. The contracts
gate. The metrics show impact.

## Pattern 5: Snapshot-and-replay

Every edit creates a snapshot. The user can scrub history:

```aura
;; Editor timeline (right panel):
;;
;;  v1   10:42:15  Initial implementation
;;  v2   10:45:22  Add tier-discount branch
;;  v3   10:48:03  AI: precomputed table        ← current
;;  v4   10:51:11  AI: branchless math          ← staged
;;
;;  Click v2 → editor shows the v2 code, with diff vs current.
;;  Click "Restore" → (ast:restore "v2") hot-swaps back.
```

This is the Smalltalk "version browser" experience, but
scaled to a production system with rollback.

## Pattern 6: Domain-specific live programming views

For different domains, the "result" view is different:

| Domain | Live result view |
|---|---|
| **Finance** | Order book, P&L curve, VaR chart, latency histogram |
| **Simulation** | Particle scatter, energy plot, simulation step counter |
| **Game dev** | Sprite preview, frame timing, FPS graph, audio waveform |
| **Industrial** | Sensor readings, actuator state, alarm log, trend chart |
| **ML** | Loss curve, accuracy, sample distribution, attention map |

Aura exposes these via the **query:* EDSL** (per #82) — the
editor can `(query:find 'order-book)` and visualize the
result. This is **structural editing** with **domain-aware
visualization**.

## Pattern 7: Multi-developer + AI collaboration

Multiple humans + multiple AI agents can edit the same live
system:

```aura
;; Per the agent-orchestration-evolution.md patterns:
;;
;;  - each participant has a "draft" workspace
;;  - drafts are merged via a CRDT-like protocol
;;  - conflicts are visible in the editor
;;  - promote-to-live requires AURA_CONTRACT_POST + quorum
```

This is **collaborative live programming** — Google Docs
meets Smalltalk meets AI pair programming.

## Pattern 8: Live programming as research

The same setup is a research platform (per
`research-self-improving-systems.md`):

- Every edit is an experiment
- AURA_CONTRACT_POST is the verifier
- Shadow test is the controlled environment
- Hot-swap is the promotion

A research-mode editor (vs production-mode) shows more
detail: shadow-test variance, contract failure modes, bandit
posterior updates.

## Pattern 9: Production safety rail

The editor enforces **safety rails** in production mode:

- **Read-only mode** for some functions (compliance, audit)
- **Mandatory human approval** for high-risk edits (e.g.,
  anything that touches payment processing)
- **Canary-only mode** for new edits (1% traffic for 10
  minutes, then auto-promote)
- **Rollback always available** via `ast:restore`

These are configured per `*evolution-mode*` (per
`production-live-evolution.md`):

```aura
(set! *evolution-mode*
      (hash 'read-only-functions '(charge-card transfer-funds)
            'require-human-approval #t
            'canary-pct 0.01
            'canary-duration 600
            'auto-rollback-threshold
            (hash 'p99-delta 0.5 'error-rate-delta 0.05)))
```

## Reference implementations

- `docs/design/caas_integration.md` — CaaS internals
- `docs/design/code_evolution_pipeline.md` — full pipeline
  with `set-code` / `eval-current` / `mutate:rebind` / etc.
- `docs/design/incremental_dirty_propagation.md` — typecheck
  speedup
- `docs/design/async_serve_design.md` — async eval pattern
- `projects/evo-kv/evo-kv-repl.aura` — REPL with edit
  history

## Latency budget (production live programming)

| Step | Target | Stretch |
|---|---|---|
| Parse (200 lines) | 1 ms | 0.5 ms |
| Macro expand | 1 ms | 0.5 ms |
| Typecheck (incremental) | 5 ms | 2 ms |
| Shadow eval | 10 ms | 5 ms |
| Contract check | 1 ms | 0.5 ms |
| Metric preview | 5 s | 1 s |
| **Total to first feedback** | **~20 ms** | **~10 ms** |

This is the **interactive budget**. Below 20 ms feels
instantaneous. Above 100 ms feels laggy.

## Industry comparison

| System | Latency | Safety | AI | Performance | Production |
|---|---|---|---|---|---|
| Smalltalk | < 50 ms | None (image corruption) | No | Slow (VM) | No |
| Lisp (SLIME) | < 100 ms | None | No | Slow (CMUCL) | No |
| Erlang (live upgrade) | Slow (release handler) | Best-effort | No | OK | Yes (telecom) |
| Excel | < 50 ms | Formulas | No | Fast | Yes (limited domain) |
| Bret Victor demos | Instant | None | No | N/A | No |
| **Aura live programming** | **< 20 ms** | **AURA_CONTRACT** | **E4 bandit** | **C++ speed** | **Yes (with rails)** |

Aura is the first system to combine **< 20 ms feedback**
with **production safety** with **AI augmentation** with
**C++ performance**. The closest is Erlang's hot code
upgrade, but Erlang has no AI and no contracts.

## Open follow-ups (not blocking this issue)

- **Editor plugin** — this doc is about the design; the
  actual editor (VSCode / Emacs / a custom web-based editor)
  is a separate project. Tracked separately.
- **Collaborative editing CRDT** — for multi-developer
  patterns. Aura's FlatAST has a tree structure that maps
  well to OT/CRDT; a follow-up design doc.
- **Domain-specific visualizations** — the visualization
  layer is per-domain (finance vs sim vs games). A
  visualization library (rendering Aura data structures as
  charts/maps) is a follow-up.
- **Live programming for non-Aura code** — the live
  programming experience for Aura code is the focus; for
  C++ / Python code, FFI can expose a subset, but full
  live editing of C++ is out of scope.
