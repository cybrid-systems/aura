# Autonomous Self-Evolving AI Agents — Patterns for Aura

**Status:** Design exploration for Scenario 2 of the
[Scenario issues series] (issue #86).

## Why this is a killer scenario

Most agent frameworks (LangChain, AutoGPT, CrewAI) evolve
agents at the **prompt or tool-calling** level. Aura enables
evolution at the **code and strategy level**: agents can mutate
their own implementations, hot-swap reasoning strategies, and
discover new tool combinations at runtime.

This isn't a marginal improvement — it's the difference between
an agent that gets smarter over weeks and one that plateaus after
its initial prompt tuning.

## Capability ladder

Aura supports the full evolution ladder for autonomous agents:

| Level | Capability | Aura primitive |
|---|---|---|
| 1 | Reuse tools via JSON config | Stdlib primitives |
| 2 | Compose tools into workflows | `orch:conduct` / `orch:pipeline` |
| 3 | Generate tool calls via LLM | `intend` + strategy: name |
| 4 | Mutate own code based on metrics | `mutate:rebind` + `mutate:replace-pattern` |
| 5 | Hot-swap function implementations | `hot-swap:define` (in `#80` follow-up) |
| 6 | Evolve the **strategy** that decides **what to evolve** | E4 `evolve-strategy` + `strategy-inspect` |
| 7 | Self-discover new capabilities from primitive composition | `query:find` + `query:pattern` + `mutate:insert-child` |

Levels 1-3 are most existing frameworks. **Levels 4-7 are Aura-unique.**

## Pattern 1: Self-tuning tool selection

```aura
;; An agent that gets faster at picking the right tool over time

(define *tool-stats* (hash "tool" "uses" "successes" "avg-ms"))

(define (try-tool name args)
  (let ((start (current-time)))
    (define result (eval (string-append "(" name " " args ")")))
    (define ms (- (current-time) start))
    (hash-set! *tool-stats* name
               (hash "uses" (+ 1 (or (cdr (assq 'uses 
                                              (hash-ref *tool-stats* name))) 0))
                     "successes" (if (void? result) 
                                     (or (cdr (assq 'successes 
                                                  (hash-ref *tool-stats* name))) 0)
                                     (+ 1 (or (cdr (assq 'successes 
                                                       (hash-ref *tool-stats* name))) 0)))
                     "avg-ms" ms))
    result))

;; Periodic evolution: replace the dispatcher's heuristics
(define (evolve-dispatcher)
  (define analytics (intend-analytics "default-strategy"))
  (define new-strategy (evolve-strategy "default-strategy"))
  (define-strategy "default-strategy" new-strategy))
```

The agent tracks tool latency and success, periodically evolves
its dispatcher strategy based on `intend-analytics` (which the
E4 design already provides).

## Pattern 2: Self-repairing workflows

```aura
;; A pipeline that observes its own failure rate and swaps in
;; better implementations

(define (run-pipeline input)
  (let ((snap (ast:snapshot "pipeline-checkpoint")))
    (define result (orch:pipeline stage1 stage2 stage3 input))
    (cond
      ((void? result)
       ;; Failure — try the evolved version
       (define evolved (evolve-strategy "default-pipeline"))
       (define-strategy "default-pipeline" evolved)
       (ast:restore snap)
       (run-pipeline input))  ; retry with new strategy
      (else result))))
```

The agent rolls forward when successful, rolls back and re-evolves
when failed. The `ast:snapshot` + `evolve-strategy` combo makes
this safe.

## Pattern 3: Capability discovery

A self-evolving agent that finds gaps in its own toolkit:

```aura
(define (find-capability-gap)
  (let* ((current-prims (query:find-callable-primitive))
         ;; for each primitive, check if it has a wrapper
         (covered (map (lambda (p) (query:calls p)) current-prims))
         (gaps (filter (lambda (p) (null? (query:calls p))) current-prims)))
    ;; gaps are primitives that are called rarely or never — opportunities
    ;; for new tool compositions
    (when (pair? gaps)
      (define-strategy "discovery-strategy"
        (format "(define (suggest-~a-tool)\\n  ...)" (car gaps))))))
```

`query:find` + `query:calls` make this kind of self-analysis cheap.

## Pattern 4: Multi-agent evolution with safety

Multiple agents evolving concurrently requires:
- **Isolated memory** — each agent session has its own CompilerService
  (via `session:create` in serve mode)
- **Shared primitive registry** — agents can register new tools via
  `register-strategy!` for the others to discover
- **Contract verification** — every evolved function passes through
  `AURA_CONTRACT_PRE/POST` (issue #83) before being kept

The safety boundaries:
1. Each agent can only mutate **its own** workspace
2. Hot-swap is **opt-in** — the agent must explicitly call
   `hot-swap:define`
3. Rollback is **always available** via `ast:restore` with the
   snapshot id from `mutate:rebind`'s transaction

## Human-in-the-loop

Aura's default evolution is **aggressive** — agents will try to
evolve themselves without asking. For production use, gate this
with:

```aura
(define *evolution-mode* 'ask)  ; or 'autonomous' or 'dry-run'

(define (try-evolve target)
  (cond
    ((equal? *evolution-mode* 'autonomous)
     (hot-swap:define target (intend-suggest target)))
    ((equal? *evolution-mode* 'ask)
     ;; Queue a human-review task
     (timeline-record! "evolution-pending" target (current-time))
     (send 'operator (format "Agent proposes evolution of ~a. Approve?" target)))
    ((equal? *evolution-mode* 'dry-run)
     ;; Just record what WOULD be evolved
     (timeline-record! "evolution-dry-run" target (current-time)))))
```

A proper operator UI (`dashboard.aura`) is a follow-up.

## Comparison vs other agent frameworks

| Framework | Code-level evolve | Safe hot-swap | Memory observability | Type system |
|---|---|---|---|---|
| LangChain | ❌ prompt only | ❌ | ⚠️ custom | ❌ |
| AutoGPT | ❌ | ❌ | ⚠️ | ❌ |
| CrewAI | ❌ | ❌ | ⚠️ | ❌ |
| **Aura** | ✅ `mutate:rebind` | ✅ `hot-swap:define` | ✅ `gc-arena-info` | ✅ gradual + ownership |

## Reference implementations

- `projects/evo-kv/evo-kv-evolve.aura` — Evolve KV's own evolution loop
  (production example of pattern 2)
- `projects/evo-kv/evo-kv-auto.aura` — Self-repair driver
- `tests/contracts_test.aura` — AURA_CONTRACT usage (pattern 4 safety)
- `docs/design/e4_evolvable_strategies.md` — Underlying auto-tune framework

## Open questions

- **Meta-evolution policy**: when should an agent evolve the
  `*evolution-mode*` itself? E4's `parent` field tracks strategy
  lineage but the policy is not yet designed.
- **Cross-agent capability sharing**: when one agent discovers a
  new tool composition, should it auto-publish to siblings?
- **Evolution budget**: an agent that evolves too fast consumes
  memory and CPU; how do we bound the rate?
- **Operator UI**: how does a human see what's evolving in
  real-time and approve / reject?
