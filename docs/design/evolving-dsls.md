# Evolving Domain-Specific Languages

**Status:** Design exploration for Scenario 7 of the
[Scenario issues series] (issue #91).

## Why this is a killer scenario

Most DSLs ship as fixed parsers + interpreters. To change
how the DSL behaves, you edit the parser, recompile, ship a
new version. Examples:

- **Trading rules DSL** — "if VIX > 25, hedge" hard-coded
  into the parser; can't adapt to new market regimes
- **Simulation parameters DSL** — gravity constant in
  sim-config; can't tune at runtime
- **Business rules DSL** — "if customer.tier == 'gold' then
  discount 20%" — can't A/B test variants
- **Game AI DSL** — "if health < 30% then flee" — can't
  evolve harder

Aura's DSLs can **evolve their interpretation** at runtime:

1. The DSL surface is a thin parser (mostly `defmacro`)
2. The DSL semantics are Aura functions exposed via the
   macro system
3. Those functions can be E4-evolved like any other
4. Hot-swap the new semantics, keep the same surface
5. Contracts validate the new semantics are sound

The DSL user sees the **same syntax** they typed; the
runtime sees the **evolved interpretation**. They don't
even have to know.

## Two flavors of evolution

There are two distinct ways a DSL can evolve:

### Flavor A: Evolving DSL semantics (the "interpretation evolves")

The DSL's syntax stays the same. The functions that interpret
DSL forms are what evolves.

```aura
;; User wrote this DSL code 6 months ago:
(when-volatile vix-threshold: 25 action: hedge)
;; VIX threshold is 25. Hard-coded? No — it's a variable.

;; Today, the macro expanded to:
;; (when (> vix 25) (hedge))
;; But the binding `vix-threshold` could be E4-evolved.
;; New threshold: 22 (market is calmer now).
;; Hot-swap: (mutate:rebind vix-threshold 22)
;; Same DSL code, new behavior. No edit, no redeploy.
```

This is the most common case. The DSL author wrote
`vix-threshold` as a `define` and made it a tunable.

### Flavor B: Evolving DSL syntax (the "grammar evolves")

The DSL gains new forms over time. The parser/macro system
evolves to support them.

```aura
;; v1 DSL: simple if/else
(when condition action)

;; v2 DSL: adds for-each
(when condition action)
(for-each item in items do: action)

;; v3 DSL: adds guard clauses
(when condition action)
(when condition action else: alt-action)
```

This is rarer and riskier — old user code might use `else:`
as a variable name in the new grammar. Aura's macro
hygiene (see `hygienic_macros.md`) protects against this.

## The evolution taxonomy

```
                       Syntax stable?    Syntax evolves?
                       ──────────────    ──────────────
  Semantics stable:    (no evolution     (rare; only
                        needed)           when adding
                                           new forms)
  Semantics evolves:   (Flavor A —       (Flavor B +
                        common)           Flavor A;
                                           the "full
                                           adaptive DSL")
```

Most production DSLs land in the **Flavor A** quadrant. This
doc focuses there.

## Pattern 1: Tunable constants in DSLs

The simplest evolution. A `define` becomes a tunable.

```aura
;; Before: hard-coded
(define (hedge-rule vix price)
  (when (> vix 25) (sell-half position)))

;; After: tunable
(define-tunable vix-threshold
  candidates: (20 22 25 28 30)
  metric: (lambda (outcome) (cdr (assq 'pnl outcome))))

(define (hedge-rule vix price)
  (when (> vix vix-threshold) (sell-half position)))
```

The DSL user writes `(hedge-rule current-vix price)` and
gets the **current best threshold**, no matter when it's
called.

## Pattern 2: Evolving rule composition

A business-rules DSL where the rule order changes:

```aura
;; User code (unchanged for years):
(rule-set 'customer-pricing
  (rule 'gold-discount (when tier-is-gold then 20%))
  (rule 'volume-discount (when qty > 100 then 10%))
  (rule 'loyalty-discount (when tenure > 5y then 5%)))

;; Internally, each rule is a function.
;; The order/priority can evolve:
(define-tunable rule-priority
  candidates: (order-of-registration
               tier-based
               discount-magnitude
               ;; discovered by E4:
               dynamic-by-cohort))
```

The E4 bandit learns: for new customers, volume-discount
first (acquisition-friendly); for returning customers,
loyalty-discount first (retention).

## Pattern 3: Evolving strategy parameters

A game-AI DSL where enemy behavior adapts to player skill:

```aura
;; DSL surface
(ai-behavior 'goblin
  (when (sees-player?) (chase))
  (when (low-health?) (flee))
  (when (player-nearby-and-strong?) (call-reinforcements)))

;; Internally, parameters are tunables
(define-tunable goblin-cowardice
  candidates: (0.1 0.2 0.3 0.4 0.5)
  metric: (lambda (outcome) (cdr (assq 'player-engagement outcome))))
```

After 1000 player sessions, the bandit picks `0.3` —
goblins flee when below 30% health. Players find the game
more fun.

## Pattern 4: Evolving DSL interpretation (the meta case)

The DSL's **interpreter itself** evolves. This is the most
powerful pattern and the most dangerous.

```aura
;; DSL: "(if condition then else)" has a traditional
;; interpretation: lazy evaluation of branches.
;;
;; But sometimes, the user wants eager evaluation of `else`
;; (for side-effects). The DSL can evolve to support both:

(define (interpret-if condition then-branch else-branch style)
  (cond
    ((equal? style 'lazy)
     (if condition then-branch else-branch))
    ((equal? style 'eager)
     (let ((t (evaluate then-branch))
           (e (evaluate else-branch)))
       (if condition t e)))
    ((equal? style 'speculative)
     ;; evaluate both in parallel, pick the one we need
     (parallel (lambda () (evaluate then-branch))
               (lambda () (evaluate else-branch))
               condition))))

;; E4 tunes the default style per task class.
```

This is **Flavor B** territory — the DSL gains new
interpretations. The user keeps the same syntax; the
runtime decides how to interpret.

## Pattern 5: Evolving DSL *contracts*

The DSL has its own contract language. The contracts
themselves can evolve.

```aura
;; User writes
(rule 'my-rule
  (when condition action)
  ;; Contract: this rule fires between 1 and 100 times
  (contract 'fires-within '1..100))

;; Internally, the contract can be relaxed if the bandit
;; discovers the original bound was too tight.
```

This is research-grade — few systems have evolvable
contract specifications. Aura can support it via
`AURA_CONTRACT_POST` on the DSL's rule-evaluator.

## The role of macros

`defmacro` is the DSL embedding tool. The macro system
v2 (`macro_system_v2.md`) gives:

- Quasiquote (`` ` ``) for code templates
- Hygiene (gensym) for safe identifier binding
- Recursive expansion
- AI-friendly: macro expansion is **predictable and
  auditable**

For an evolving DSL, the macro is the **stability
boundary**. The user types a macro form; the expansion
is a call to an Aura function; that function is what
evolves. The macro itself rarely changes.

```aura
;; The macro defines the surface
(defmacro (rule name body ...) ...)

;; The expansion calls an Aura function
;; (define-rule name (lambda () body) contracts)
;; (define-rule 'my-rule (lambda () body) 'fires-within 1 100)

;; The Aura function is what E4 evolves
```

## The role of contracts

Every DSL action is wrapped in a contract:

```aura
;; AURA_CONTRACT_POST: the rule produced a finite result
(define-contract post:rule-fires-finite
  (rule result)
  (finite? result))

;; AURA_CONTRACT_POST: the rule respected the cap
(define-contract post:rule-respects-cap
  (rule-result rule)
  ;; look up the rule's 'fires-within contract
  (let ((cap (cdr (assq 'max-fires (rule-meta rule)))))
    (<= (rule-fire-count rule) cap)))
```

When E4 proposes a new rule implementation, the contracts
gate the hot-swap. If the new rule violates
`post:rule-respects-cap`, the swap is rejected. The DSL
user sees the **old behavior** until a better candidate
arrives.

## The role of hot-swap

Every DSL evolution is a hot-swap. The user doesn't
restart anything.

```aura
;; E4 decided vix-threshold should go from 25 to 22.
;; Steps:
;;   1. ast:snapshot "pre-vix-tune"
;;   2. (mutate:rebind 'vix-threshold 22)
;;   3. AURA_CONTRACT_POST verifies the new threshold is sane
;;   4. Live traffic continues; new hedge behavior applies
;;   5. If metric regresses, (ast:restore "pre-vix-tune")
```

The DSL author wrote zero deployment code. The platform
handles it.

## The role of CaaS

CaaS is the **fair-evaluation harness** for DSL candidate
strategies. For a trading DSL:

```aura
(caas-run
  (lambda ()
    (let* ((historical-data (replay-buffer '2024-Q1))
           (pnl-old (run-dsl-with-rule historical-data old-threshold))
           (pnl-new (run-dsl-with-rule historical-data new-threshold)))
      (list 'delta (- pnl-new pnl-old)
            'p-value (welch-t-test pnl-old pnl-new)
            'promote? (and (> (- pnl-new pnl-old) 0)
                           (< (welch-t-test pnl-old pnl-new) 0.05))))))
```

The decision to promote is data-driven, not hand-wavy.

## The role of E4 strategies

E4 is the **strategy-proposer** for the DSL. E4 reads the
DSL's failure history and proposes new candidate
implementations.

```aura
;; E4's role in an evolving DSL
(intend
  "The current vix-threshold strategy underperforms in
   low-volatility regimes. Propose 3 new candidates."
  context: *dsl-failure-history*
  strategy: "evolve-threshold")
```

The candidates are then shadow-tested via CaaS. The
winner is hot-swapped.

## Reference implementations

- **Trading rules**: `projects/evo-kv/evo-kv-aof.aura` —
  rewrite rules evolved by E4
- **Simulation params**: `tests/tasks/edsl/edsl-optimize-multiarg.aura`
  — multi-arg optimization demo
- **Business rules**: pattern in this doc, not yet a
  reference file
- **Game AI**: pattern in this doc, follow-up to extract

## Industry comparison

| DSL type | Examples | Evolution support |
|---|---|---|
| Trading | QuantConnect, Zipline | Config + restart |
| Simulation | AnyLogic, NetLogo | Config + restart |
| Business rules | Drools, OpenL Tablets | Manual rule update + redeploy |
| Game AI | Behavior trees (most engines) | Script reload, no live evolution |
| **Aura DSLs** | **This work** | **E4 + CaaS + hot-swap, no restart** |

The killer feature: **the DSL user doesn't know the DSL
evolves**. They write `(hedge-rule vix price)` once; the
threshold adapts under them. This is the gap the scenario
targets.

## Open follow-ups (not blocking this issue)

- **DSL version migration** — when a DSL adds new syntax
  (Flavor B), how to migrate old user code? Aura's macro
  system can have a `migrate-v1→v2` helper, but it's
  a real engineering task. Tracked as follow-up.
- **Cross-DSL composition** — can a trading DSL and a
  business-rules DSL share a tunable? E4 research.
- **DSL composition safety** — when two DSLs share a global
  (e.g., `*risk-budget*`), can they conflict at runtime?
  Contract-driven resolution is a follow-up.
- **Domain expert UI** — domain experts (traders, game
  designers) need a UI to see what the DSL is doing and
  approve / reject evolution. Tracked as follow-up to
  `#85` (operator UI).
