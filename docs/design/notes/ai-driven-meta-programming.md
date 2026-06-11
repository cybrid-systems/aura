# AI-Driven Meta-Programming and Tool Evolution

**Status:** Design exploration for Scenario 10 of the
[Scenario issues series] (issue #94).

## Why this is a killer scenario

Today's "AI + tools" systems (LangChain, AutoGen, MCP-based
agents) have a **fixed tool library**. The AI can call
`search`, `calculator`, `send_email` — but it can't *add a
new tool*. To add a tool, a human writes code, opens a PR,
and ships.

Aura flips this. The system itself can:

1. **Discover** that a new tool would help (e.g., "I keep
   parsing dates in 12 places — there's no `parse-iso-date`
   primitive")
2. **Synthesize** the new tool from a spec
3. **Verify** the tool with contracts and shadow tests
4. **Integrate** the tool into the live runtime via hot-swap
5. **Promote** the tool to the standard library if it's
   useful enough
6. **Roll back** the tool if it regresses

The toolset itself becomes a **first-class evolvable
entity**. This is the gap the scenario targets.

## What "meta-programming" means here

Meta-programming in Aura has three layers:

| Layer | What | Who edits | When |
|---|---|---|---|
| **L1: Application code** | Business logic, DSL programs, sim configs | Humans + AI | Runtime |
| **L2: Primitives** | `(query:find)`, `(mutate:rebind)`, `(intend)`, etc. | Aura stdlib maintainers | Build time |
| **L3: The compiler & runtime** | Aura's own IR, parser, evaluator, GC | Aura core devs | Build time |

The scenario targets **L2**: the primitives layer. The AI
can:

- Add new primitives
- Modify existing primitives
- Compose primitives into workflows
- Promote a frequently-used composition to a named primitive

L3 (the compiler) is **out of scope** for AI tool evolution
— too risky, too entangled with the type system. (Self-
hosting Aura in Aura is a research-grade follow-up, not in
this doc.)

## The tool-evolution loop

```
        ┌──────────────────────────────────────┐
        │  Observe: which primitives are       │
        │  composed often? (frequency count)   │
        └────────────────┬─────────────────────┘
                         │ "parse-iso-date" composed
                         │ in 23 places, no primitive
                         ▼
        ┌──────────────────────────────────────┐
        │  Propose: synthesize a new primitive │
        │  (synthesize:define)                 │
        └────────────────┬─────────────────────┘
                         │ candidate: parse-iso-date
                         ▼
        ┌──────────────────────────────────────┐
        │  Verify:                            │
        │  - AURA_CONTRACT_POST for input     │
        │    range + output format            │
        │  - Property-based test (1000 dates) │
        │  - Compare to the 23 inline impls   │
        └────────────────┬─────────────────────┘
                         │ all pass
                         ▼
        ┌──────────────────────────────────────┐
        │  Stage: candidate tool installed    │
        │  in a sandboxed tool-set            │
        │  (not yet in production stdlib)     │
        └────────────────┬─────────────────────┘
                         │
                         ▼
        ┌──────────────────────────────────────┐
        │  Pilot: replace one of the 23        │
        │  inline implementations with        │
        │  (parse-iso-date ...). Measure.     │
        └────────────────┬─────────────────────┘
                         │ p99 / correctness same or better
                         ▼
        ┌──────────────────────────────────────┐
        │  Promote:                           │
        │  - all 23 sites use the new prim    │
        │  - add to stdlib (if generally useful)│
        │  - mark old inline code obsolete    │
        │  - ast:snapshot before, ast:restore │
        │    on regression                    │
        └──────────────────────────────────────┘
```

## Pattern 1: Primitive usage mining

Which compositions repeat?

```aura
;; Mine *intend-history* for repeated patterns
(define (mine-repeated-compositions min-count)
  (let* ((op-calls (flatten (map intend-ops *intend-history*)))
         (n-grams (map (lambda (ops)
                         (take 5 ops))
                       (sliding-window op-calls 5)))
         (counts (frequencies n-grams)))
    (filter (lambda (entry)
              (>= (cdr entry) min-count))
            counts)))

;; Result:
;;   ((string->date % parse % raise % list % fold) . 23)
;;   ((hash-ref % assq % default) . 47)
;;   ((eval % mutate:rebind % snapshot) . 31)
;;   ...
```

The first entry says: a 5-op sequence involving date parsing
appears 23 times. That's a candidate primitive.

## Pattern 2: AI-proposed primitive synthesis

The AI is asked to design a primitive for the repeated
pattern.

```aura
;; The AI's job:
(intend
  "Design a primitive named 'parse-iso-date' that captures
   the repeated pattern in this corpus.
   Reference corpus: 23 examples, all with this signature:
     (parse-iso-date '2024-12-31T10:30:00Z)
   Must be:
     - Total (returns a value or raises)
     - Fast (< 1 µs for typical input)
     - Handle: full ISO 8601, date-only, date+time, with/without
       timezone, leap seconds
   Tests in tests/primitives/parse-iso-date/"
  context: (list 'corpus (sample-of-pattern 23)
                'existing-impls (list 23 inline impls))
  strategy: "synthesize-primitive")
```

The AI returns a **proposed implementation** plus a
**test suite**. Both are human-reviewable.

## Pattern 3: Property-based test generation

Aura can generate **property-based tests** from the spec:

```aura
;; For parse-iso-date, the spec is:
;;   ∀ valid-iso-8601-string s,
;;     parse-iso-date s ≡ reference-impl s
;;
;; Property generator:
(define (gen-iso-date)
  (let* ((year (+ 1900 (random 200)))
         (month (+ 1 (random 12)))
         (day (+ 1 (random 28)))
         (hour (random 24))
         (minute (random 60))
         (second (random 60))
         (tz (random-element '("Z" "+08:00" "-05:00" ""))))
    (format "~4d-~2,'0d-~2,'0dT~2,'0d:~2,'0d:~2,'0d~a"
            year month day hour minute second tz)))

;; Generate 1000 random dates, check parse-iso-date matches
;; the reference impl (one of the 23 inline ones).
```

This is **the test suite** for the new primitive. It runs in
CaaS, takes < 1 second for 1000 dates.

## Pattern 4: AURA_CONTRACT for primitives

Every new primitive has a contract:

```aura
(define-contract post:parse-iso-date
  (input result)
  (cond
    ((not (iso-8601-string? input))
     (and (eq? result 'parse-error)
          (string-contains? (cadr result) "invalid format")))
    (else
     ;; For valid input, result is a structured date
     (and (date-record? result)
          (= (date-year result) (string-year input))
          (= (date-month result) (string-month input))
          (= (date-day result) (string-day input))))))

(define-contract pre:parse-iso-date
  (input)
  (string? input))   ;; input must be a string
```

When the primitive is hot-swapped in, `AURA_CONTRACT_POST`
gates the swap. If the new primitive fails the contract on
any of the 23 historical call sites, the swap is rolled
back.

## Pattern 5: Sandbox the candidate primitive

Before promoting globally, the candidate lives in a
**sandboxed toolset**:

```aura
;; Production toolset (immutable)
(define *stdlib-v3*
  (list 'parse-string 'parse-number 'parse-symbol
        'query:find 'mutate:rebind 'intend ...))

;; Sandbox toolset (mutable, per-experiment)
(define *stdlib-sandbox*
  (append *stdlib-v3*
          (list 'parse-iso-date 'parse-rfc-2822 'date-add)))

;; Code in the sandbox can call sandboxed primitives.
;; Code in production can NOT call sandboxed primitives
;; (compile-time check via AURA_CONTRACT_PRE).
```

This is the **safety boundary**. The AI can experiment in
the sandbox; production code is untouched.

## Pattern 6: Pilot and measure

One of the 23 call sites is chosen as a pilot:

```aura
;; Before:
(define (process-event evt)
  (let* ((date-str (cdr (assq 'timestamp evt)))
         (date (cond
                 ((string-contains? date-str "T")
                  (parse-iso8601-with-tz date-str))
                 (else
                  (parse-iso8601-date-only date-str)))))
    ...))

;; After (using new primitive):
(define (process-event evt)
  (let* ((date-str (cdr (assq 'timestamp evt)))
         (date (parse-iso-date date-str)))
    ...))
```

The pilot runs in shadow for 1 hour. If correctness, p99
latency, and error rate are all within tolerance vs the
original, the pilot is approved. The other 22 sites can be
migrated.

## Pattern 7: Hot-swap integration

The primitive is hot-swapped into the live runtime:

```aura
;; Steps:
;;   1. ast:snapshot "pre-parse-iso-date"
;;   2. Add 'parse-iso-date to *stdlib-sandbox* AND
;;      raise it to *stdlib-v3* (the production toolset)
;;   3. mutate:rebind for any cached parser
;;   4. AURA_CONTRACT_POST gates: all 23 sites pass
;;   5. Replay last 1000 events in shadow; verify outcomes
;;      match historical
;;   6. If yes, the new primitive is live
;;   7. If no, ast:restore "pre-parse-iso-date"
```

This is the **same hot-swap pattern** from
`production-live-evolution.md`, applied to a primitive
instead of a function.

## Pattern 8: Promote to stdlib

If the primitive proves generally useful (used in > 5
projects, > 100 call sites), promote to stdlib:

```aura
;; The promotion process:
;;   1. Move from sandbox to stdlib/core/
;;   2. Generate stdlib documentation (function, contract,
;;      examples, perf budget)
;;   3. Generate stdlib tests (property-based + reference)
;;   4. ast:snapshot of stdlib, mutate:rebind to include
;;   5. AURA_CONTRACT_POST gates the stdlib change
;;   6. All projects on this Aura version now have the prim

;; Tracked in *stdlib-changelog*:
(push *stdlib-changelog*
      (list 'added 'parse-iso-date
            'version 'stdlib-v3.0.1
            'rationale "synthesized by AI from 23 sites"
            'test-coverage 1000-property-tests
            'human-reviewer "ani@local"))
```

## Pattern 9: Rollback and obsolescence

The primitive can be rolled back or obsoleted:

```aura
;; Rollback the promotion:
(ast:restore "pre-parse-iso-date")

;; Or mark obsolete (for next version):
(define *stdlib-deprecated*
  (cons '(parse-iso-date "use date:parse instead" v3.0.2)
        *stdlib-deprecated*))
```

The AI can also **un-synthesize**: if a primitive is rarely
used, remove it and inline back at the call sites.

## Pattern 10: AI for primitive composition

Beyond synthesizing new primitives, the AI can compose
existing ones into higher-level workflows.

```aura
;; AI composes a workflow:
(define (data-pipeline-step)
  (compose
    (parse-iso-date %1)            ;; parse timestamp
    (validate-range %1 30)         ;; within 30 days
    (hash-set 'timestamp %1)        ;; structured record
    (intend "extract entities" %1)  ;; LLM extracts entities
    (merge-into-canonical %1)))     ;; merge into canonical

;; This becomes a named primitive: data-pipeline
```

The composed primitive is a **macro-like** thing — it's
just a function, but it's elevated to a stdlib name because
it's used often.

## The role of compile-time reflection

`std::meta` (per `compile_time_reflection.md`) lets the AI
introspect the type signatures of existing primitives to
design new ones that compose:

```aura
;; AI reads the reflection info for parse-string:
(meta-info 'parse-string)
;; ⇒
;; (signature (input string?) (output (or number? symbol? list?))
;;  contract pre:parse-string
;;  contract post:parse-string
;;  complexity O(n)
;;  side-effects none)

;; AI then designs parse-iso-date with compatible style
```

This is **structural compatibility checking** for primitives.
Aura's `std::meta` makes it automatic.

## The role of macros

`defmacro` (per `macro_system_v2.md`) is the **higher-level
mechanism** for promoting a frequent composition to a
syntax:

```aura
;; Before: composition is a function call
(data-pipeline (parse-iso-date ts))

;; After: promoted to macro for shorter syntax
(pipeline ts)

;; Macro expansion:
;; (pipeline ts) → (data-pipeline (parse-iso-date ts))
```

Macros give the AI a way to **grow the language syntax** in
addition to the library. But macros are riskier than
primitives (they can break hygiene, introduce identifier
shadowing). Promote to macro only after the function
primitive is well-tested.

## Reference implementations

- `docs/design/compile_time_reflection.md` — std::meta for
  primitive introspection
- `docs/design/llm_stdlib.md` — std/llm module
- `docs/design/code_evolution_pipeline.md` — the underlying
  pipeline for set-code / eval / mutate
- `docs/design/synthesize-pipeline-v2.md` — the
  generate→validate→diagnose→fix loop
- `docs/design/llm_fuzz_testing.md` — AI as QA

## Safety rails

The system has hard guardrails:

- **No L3 modifications** — the compiler and runtime are
  off-limits to AI
- **Sandboxed primitives** — new primitives can't be called
  by production code until promoted
- **Mandatory contracts** — every new primitive has
  `AURA_CONTRACT_PRE` and `AURA_CONTRACT_POST`
- **Pilot required** — promotion requires a successful
  pilot on real traffic
- **Human review for stdlib promotion** — sandboxed
  primitives can be promoted by AI; stdlib promotion is
  always human-approved
- **Audit log** — every primitive birth, swap, rollback,
  obsolescence is in `*stdlib-changelog*`

## Open follow-ups (not blocking this issue)

- **Self-hosting L3** — meta-circular Aura in Aura. Open
  research; not in scope here.
- **Cross-project primitive sharing** — when Project A
  synthesizes a primitive, can Project B use it? A
  primitive-registry design is a follow-up.
- **Primitive marketplace** — humans can also publish
  primitives. A vetted marketplace (security, performance,
  contracts) is a follow-up.
- **AI safety review** — for stdlib promotion, the AI
  should be self-critical: "would I propose this primitive
  if I were reviewing it?" A meta-level contract is
  follow-up research.
