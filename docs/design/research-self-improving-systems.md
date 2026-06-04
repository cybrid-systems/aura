# Research Platforms for Self-Improving Code Systems

**Status:** Design exploration for Scenario 6 of the
[Scenario issues series] (issue #90).

## Why this is a killer scenario

Current code-synthesis research (Codex, AlphaCode, Copilot,
StarCoder, etc.) is **closed-loop external**: the LLM is in
the middle, surrounded by Python harnesses, JSONL evaluation
pipelines, and human graders. To study *self-improvement*,
you need the LLM to be able to read its own code, modify its
own code, and verify the modification — all in the same
substrate.

Aura is the first language designed from day one for this:

- **Homoiconic** — code is data, so the LLM can read and
  rewrite its own runtime in the same syntax it executes
- **EDSL** — synthesis pipelines are first-class
  (`synthesize:define`, `synthesize:optimize`), not external
  Python
- **Contracts** — verification is built in (`AURA_CONTRACT_*`),
  not bolted on
- **CaaS** — experimentation is sandboxed, reproducible, with
  cycle-accurate timing
- **Reflection** — `std::meta` lets the system introspect its
  own IR and lower it differently per experiment
- **E4 strategies** — the LLM can propose and validate new
  strategies for *itself*

This doc is the design foundation for using Aura in academic
and research settings focused on self-improving code.

## What this scenario unlocks

| Research area | How Aura helps |
|---|---|
| Program synthesis | EDSL is the testbed; new generators ship as Aura strategies |
| Automated theorem proving | Contracts are specs; CaaS runs the proof search |
| Code repair / self-repair | `intend-failure` hooks + LLM-driven repair loop |
| Self-improving systems | E4 bandit + CaaS measures the system's own improvement |
| Metacircular research | The system can read and rewrite its own compiler in its own syntax |
| Empirical PL research | Every experiment is reproducible from a snapshot |

## The research loop

```
        ┌────────────────────────────────────┐
        │  Hypothesis                         │
        │  "Strategy X is better than Y       │
        │   for tasks of shape Z"             │
        └────────────────┬───────────────────┘
                         │
                         ▼
        ┌────────────────────────────────────┐
        │  Implement X as an E4 strategy      │
        │  - or: synthesize:define X          │
        │  - or: hot-swap existing strategy   │
        └────────────────┬───────────────────┘
                         │
                         ▼
        ┌────────────────────────────────────┐
        │  Design experiment                  │
        │  - task corpus (N ≥ 100)            │
        │  - baseline (current best)          │
        │  - metric (latency, cost, quality)  │
        │  - seed (for reproducibility)       │
        └────────────────┬───────────────────┘
                         │
                         ▼
        ┌────────────────────────────────────┐
        │  Run experiment in CaaS             │
        │  - sandboxed: X can't corrupt run   │
        │  - cycle-accurate timing (rdtsc)    │
        │  - bounded resources                │
        │  - record outcomes to history       │
        └────────────────┬───────────────────┘
                         │
                         ▼
        ┌────────────────────────────────────┐
        │  Statistical analysis               │
        │  - t-test, Wilcoxon, bootstrap CI   │
        │  - effect size (Cohen's d)          │
        │  - significance threshold (p < .05) │
        └────────────────┬───────────────────┘
                         │
              pass ↓              fail ↓
        ┌────────────────┐  ┌────────────────┐
        │  Publish result│  │  Refine hyp.   │
        │  - update E4   │  │  - try X'       │
        │  - publish EDSL│  │  - extend corpus│
        │    benchmark   │  │  - retest       │
        └────────────────┘  └────────────────┘
```

This is **standard empirical PL methodology**, but in-language.
The Python harness and JSONL pipeline are gone.

## Sub-area 1: Program synthesis research

### Baseline: existing EDSL benchmark

`tests/tasks/edsl/` already contains a 145-task EDSL
benchmark. This is the starting point. Research questions:

- **Q1**: Does `synthesize:define` with a verifier outperform
  one-shot generation? (We already see a 2x speedup in
  `synthesize-pipeline-v2.md`.)
- **Q2**: Does `synthesize:optimize` with profiling feedback
  outperform blind optimization?
- **Q3**: Does multi-step synthesis (template → LLM fill →
  verifier → LLM fix) outperform single-step LLM?
- **Q4**: At what task complexity does E4's bandit start to
  outperform a fixed strategy?

### How Aura's primitives map to synthesis research

| Primitive | Research role |
|---|---|
| `synthesize:define` | Baseline generator (LLM-driven) |
| `synthesize:optimize` | Optimization-aware generator |
| `synthesize-validator` | Verifier (can be E4-evolved) |
| `intend` | Outer orchestration loop |
| `evolve-strategy` (E4) | Strategy proposal for the generator itself |
| `pid:analyze` | Failure diagnosis feeding back to generator |
| `AURA_CONTRACT` | Spec for synthesized code |
| `CaaS` | Sandboxed execution during synthesis |

### A concrete research task: "Does context-aware synthesis help?"

```aura
;; Hypothesis: feeding recent succeed/fail history to the LLM
;; improves synthesis pass-rate.

(define (run-experiment corpus-size)
  (define with-context-results '())
  (define without-context-results '())
  (for ((task (in-list (sample-tasks corpus-size))))
    (let* ((prompt-no-ctx (build-prompt task #f))
           (prompt-with-ctx (build-prompt task #t))
           (code-no-ctx (synthesize:define prompt-no-ctx))
           (code-with-ctx (synthesize:define prompt-with-ctx))
           (pass-no-ctx (verify task code-no-ctx))
           (pass-with-ctx (verify task code-with-ctx)))
      (set! without-context-results
            (cons pass-no-ctx without-context-results))
      (set! with-context-results
            (cons pass-with-ctx with-context-results))))
  (list 'no-context (pass-rate without-context-results)
        'with-context (pass-rate with-context-results)
        'delta (- (pass-rate with-context-results)
                  (pass-rate without-context-results))))

;; Run on 200 tasks, log to *experiment-log*, report p-value.
```

This is **real empirical research in 30 lines of Aura**. In
Python it'd be 300+ lines of glue code.

## Sub-area 2: Automated theorem proving

Aura's contracts are specs. CaaS is the proof-search runtime.

```aura
;; Spec: sort returns a permutation of its input
(define-contract post:sort-correct
  (input output)
  (and (permutation? input output)
       (sorted? output)))

;; Search for a proof
(caas-run
  (lambda ()
    (prove-contract 'post:sort-correct
                    (lambda (input output)
                      ;; SMT-style search
                      (smt-axiom (sorted-permutation input output))))))
```

The research questions:

- **Q5**: Does Aura's homoiconic representation make proof
  search more efficient than SMT-LIB text input?
- **Q6**: Can E4 propose better proof strategies per spec
  class?
- **Q7**: Can the proof cache be shared across related
  contracts (via `std::meta` reflection on the contract
  signature)?

## Sub-area 3: Code repair and self-repair

When a contract fails, what now?

```aura
;; AURA_CONTRACT violation → repair loop
(define (auto-repair target-fn violation)
  (let loop ((attempts 0))
    (cond
      ((>= attempts 5) 'repair-failed)
      (else
        (define diagnosis (pid:analyze target-fn violation))
        (define patch (llm-repair target-fn diagnosis))
        (define test-result
          (with-exception-handler
            (lambda (e) 'fail)
            (lambda ()
              (verify-contract target-fn))))
        (cond
          ((equal? test-result 'pass)
           (mutate:rebind target-fn patch)   ;; hot-swap the fix
           'repair-success)
          (else
           (timeline-record! 'repair-attempt target-fn attempts test-result)
           (loop (+ attempts 1)))))))))
```

This is the **self-repair pattern** in production-live-evolution.md.
For research, the questions are:

- **Q8**: Does `pid:analyze` diagnosis quality predict repair
  success? (Tractable empirical study.)
- **Q9**: Does multi-attempt repair (>1) dominate over
  one-shot? (We expect yes, but where's the inflection point?)
- **Q10**: Does the repair history feed back into the E4
  bandit to pick *better* initial implementations?

## Sub-area 4: Self-improving systems (the meta-circular case)

This is the deepest research direction. **The system rewrites
its own synthesis strategy.**

```aura
;; The system notices: synthesize:define struggles on tasks
;; with nested data structures. Propose a new strategy.

(define (propose-new-strategy failure-corpus)
  (let* ((diagnosis
          (pid:analyze
            (lambda (failure) failure)
            failure-corpus))
         (proposal
          (intend
            "Design a new synthesis strategy optimized for
             nested data structures. Current strategies
             struggle with them. Reference: ~a"
            (list diagnosis)
            strategy: "evolve-synthesis-strategy"))
         (candidate-code
          (synthesize:define proposal)))
    ;; Verify in shadow test
    (shadow-test candidate-code failure-corpus)))

;; If shadow-test passes, hot-swap into the live synthesis
;; strategy pool. The system has now improved its own
;; generator.
```

**Research questions for Q11-Q15** (deepest tier):

- **Q11**: Convergence — does the meta-bandit converge on
  better strategies, or thrash?
- **Q12**: Sample efficiency — how many tasks does the
  system need to discover a meaningfully better strategy?
- **Q13**: Safety — does the proposed strategy pass the
  `AURA_CONTRACT_PRE` invariant checks? If not, how often?
- **Q14**: Generalization — does a strategy learned for one
  task class transfer to others?
- **Q15**: Self-reference limits — at what point does the
  system stop improving? (Likely: when the corpus is
  exhausted, or when the meta-bandit posterior collapses.)

## Pattern: Experiment reproducibility

Every experiment must be reproducible. Aura provides this via:

- `ast:snapshot` at experiment start
- Seeded RNG for any stochastic component
- Corpus version pinned in the experiment record
- Environment fingerprint (`/proc/cpuinfo` + `/proc/meminfo`
  hash) recorded with the result

```aura
(structure experiment-record
  (id integer)
  (hypothesis string)
  (corpus-version symbol)
  (strategy-a name + code)
  (strategy-b name + code)
  (metric symbol)
  (seed integer)
  (results-a (list-of real))
  (results-b (list-of real))
  (p-value real)
  (effect-size real)
  (env-fingerprint symbol)
  (timestamp integer))
```

Re-running an experiment is one call:
`(rerun-experiment record-id)`.

## Pattern: Corpus management

The benchmark corpus is the heart of the research. Aura's
approach:

- **Versioned** — `edsl-benchmark-v145`, `-v148`, etc.
- **Stratified** — balanced across task categories
- **Holdout** — 20% of tasks never used during strategy
  development, reserved for final validation
- **Regression-tested** — any new strategy must not
  regress on the prior corpus

```aura
;; Get a stratified sample for an experiment
(define (sample-tasks n)
  (take n
        (shuffle
          (append (sample 'edsl 30)
                  (sample 'recursion 20)
                  (sample 'ffi 20)
                  (sample 'ffi-process 20)
                  (sample 'algorithm 10)))))
```

## Pattern: Statistical analysis

Aura ships with `std/stats` (basic) and integrates with
`scipy`-like libraries via FFI for advanced tests.

```aura
(define (analyze-experiment results-a results-b)
  (let* ((mean-a (mean results-a))
         (mean-b (mean results-b))
         (delta (- mean-b mean-a))
         (p-val (welch-t-test results-a results-b))
         (cohens-d (/ (- mean-b mean-a)
                      (pooled-stddev results-a results-b))))
    (list 'delta delta
          'p-value p-val
          'effect-size cohens-d
          'significant (< p-val 0.05))))
```

For publication, export the experiment record to JSON and
include the Aura source for the experiment as a
supplementary material.

## Reference implementations

- `tests/tasks/edsl/edsl-optimize-benchmark-kw.aura` —
  E4 keyword-arg optimization experiment
- `tests/tasks/edsl/edsl-optimize-fitness.aura` — fitness
  function comparison
- `tests/tasks/edsl/edsl-optimize-multiarg.aura` — multi-arg
  optimization
- `docs/design/synthesize-pipeline-v2.md` — synthesis
  pipeline v2 (4-step generate→validate→diagnose→fix)
- `docs/design/llm_fuzz_testing.md` — LLM-driven fuzz
  testing (compiler vs LLM bug detection)
- `docs/design/synthesize_strategies.md` — strategy synthesis

## Comparison vs existing research platforms

| Platform | Synthesis | Verification | Reproducibility | Self-improvement |
|---|---|---|---|---|
| Codex / Copilot | Yes (LLM) | External (test files) | None | No |
| AlphaCode | Yes (LLM + tests) | Built-in | Partial | No |
| LEAN / Coq | No (interactive proof) | Built-in (kernel) | Excellent | No (meta-circular is research-only) |
| AutoML (e.g., auto-sklearn) | No (model selection) | Cross-validation | Config-pinned | No |
| Haskell / Idris | No (research substrate) | Type system | Good | Limited (template Haskell) |
| **Aura** | **Yes (EDSL)** | **Yes (Contracts + CaaS)** | **Yes (snapshot + seed)** | **Yes (E4 bandit)** |

Aura is the only platform where the **full research loop
(synthesize → verify → analyze → evolve) is in-language**.

## Open follow-ups (not blocking this issue)

- **Stats library** — Aura's `std/stats` is minimal. A
  comprehensive statistical-analysis library (Bayesian +
  frequentist) is a follow-up.
- **Paper template** — academic papers need LaTeX; an
  Aura-to-LaTeX export for experiment tables is follow-up.
- **Cross-corpus meta-analysis** — comparing results across
  research groups requires a shared experiment-record schema.
  Schema versioning is a small but important follow-up.
- **Ethics** — self-improving code systems can be hard to
  audit. An "ethical constraints as contracts" doc is
  follow-up.
