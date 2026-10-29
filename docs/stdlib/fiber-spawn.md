# fiber:spawn denseness contract (Issues #2656 / #2685)

## Return values

| Result | Meaning |
|--------|---------|
| **positive int** | Success — fiber id for `(fiber:join fid)` |
| **`#f`** | Failure (bad args) |
| **error object** | Capability denial under sandbox (`fiber` / `*`) |

Success is **never** `0` or `-1`. Pre-#2656 CLI thread-fallback used
negative ids starting at `-1`, which denseness probes mis-read as failure
even though `(fiber:join -1)` worked.

## Backends

| Backend | When | Probe |
|---------|------|--------|
| **scheduler** | `g_fiber_spawn` set (`--serve-async`, `--serve-async-bench`, serve modes) | `(fiber:spawn-backend)` → `1` |
| **thread** | CLI stdin denseness (`AURA_PIPELINE_STRICT=0` file/stdin runs) | `(fiber:spawn-backend)` → `2` |

Thread fallback is a **supported concurrent denseness backend** for
Hephaestus axis C (multi-worker rebind under load). Real stackful
ucontext fibers require serve-async (epoll).

## Minimal denseness CLI

```bash
# Thread-fallback concurrent path (default stdin / file eval):
AURA_PIPELINE_STRICT=0 ./build/aura -e '(fiber:join (fiber:spawn (lambda () 1)))'
# → 1

# Real scheduler fibers (Linux epoll):
./build/aura --serve-async-bench path/to/script.aura
```

## Binding discipline (Issue #2685 / denseness H9 caveat)

Agents must treat fiber ids as **distinct storage**, not as values that
can be rebound through simultaneous `define` / `letrec` traps.

### Preferred denseness pattern — sequential `let*`

```scheme
(let* ((f1 (fiber:spawn (lambda () (+ 1 2))))
       (j1 (fiber:join f1))                 ; optional but recommended
       (f2 (fiber:spawn (lambda () (+ 10 20))))
       (j2 (fiber:join f2)))
  (and (> f1 0) (> f2 0) (not (eq? f1 f2))  ; distinct positive ids
       (= j1 3) (= j2 30)))
```

Hephaestus probes for concurrent-rebind / mutate-in-fiber use this style on purpose.

### Top-level / multi-`define` begin (product contract)

Aura evaluates multi-`define` `begin` with **letrec-like** cell pre-allocation
then **sequential RHS evaluation** into independent cells:

```scheme
(define f1 (fiber:spawn (lambda () (+ 1 2))))
(define f2 (fiber:spawn (lambda () (+ 10 20))))
;; Product contract (#2685): f1 ≠ f2, both > 0; joins return independent payloads.
```

Also holds for:

```scheme
(begin
  (define f1 (fiber:spawn …))
  (define f2 (fiber:spawn …))
  …)
```

**Do not** assume Scheme-style simultaneous RHS evaluation shares one
spawn result. Each RHS is a separate `fiber:spawn` call and must get its
own positive id (thread-fallback: `0x4000_0000|seq`).

### Historical confusion with #2656

Before positive ids, both names could print as `-1` (or look “failed”)
even when join worked — denseness hosts treated that as “same id alias”.
#2656 fixed the sentinel; #2685 locks the **binding** contract: two
spawns → two ids under sequential / multi-define evaluation.

### Agent anti-patterns

| Anti-pattern | Risk |
|--------------|------|
| Relying on `hot-swap:fn` for pure-Aura fiber ids | AOT surface only |
| Parallel `let` with two spawns **without** checking `eq?` | Harder to audit; prefer `let*` |
| Reusing one `define` name for a second spawn without join | Overwrites binding; first fiber may leak until join |

## Concurrent multi-name rebind (Issue #2686 / denseness H10)

Two CLI fibers calling `mutate:rebind` (+ `eval-current`) on **distinct**
names must not crash or leave names unbound.

### Product contract (#2686 / #2738)

1. **`eval-current` takes exclusive workspace lock** (`WorkspaceUniqueIfNeeded`)
   for the full tree walk so concurrent fiber `mutate:rebind` cannot race
   `FlatAST::get` / env binds mid-eval (pre-#2686 short pin unlocked too early).
2. **`mutate:rebind` uses GlobalExclusive** `MutationBoundaryGuard`
   (unique workspace lock) for the rebind body — concurrent rebinds
   serialize; the second waits rather than interleaving FlatAST writes.
3. **Same-thread nested mutate under `eval-current`** fails closed
   (`AdmissionRejected: nested-mutate-under-eval-current`) — avoids
   unique-under-unique EDEADLK.
4. **Lock adopt is TLS-only (#2738):** `WorkspaceUniqueIfNeeded` /
   `WorkspaceFlatPin` skip re-lock only when **this thread** holds
   `MutationBoundaryGuard` (`any_active_mutation_boundary()` / TLS depth
   slot). Process-wide `mutation_boundary_held_` and CLI shared
   `g_main_thread_stack` size must not skip locks on a concurrent fiber
   (that race tore SoA columns mid-`FlatAST::get` → intermittent SIGABRT).
5. Preferred denseness multi-name pattern remains **sequential**
   spawn→join per name when agents want simple success/failure; concurrent
   dual rebind is supported under the locks above (100× dual-name stress
   leaves both bindings present; no SIGABRT).
6. **`fiber:join` WARN** (`workspace mutated during join`) is intentional
   observability when another fiber mutates while the joiner waits — not
   a crash. Crash must not be reachable under dual-name rebind + join.
7. **CLI thread-fallback body serial (#2738):** concurrent `std::thread`
   fibers share one `Evaluator`. Bodies take `s_cli_thread_fiber_body_mtx`
   so `apply_closure` does not race `string_heap_` / env outside the
   workspace lock. Scheduler (`fiber:spawn-backend` = 1) does not use this
   mutex. Denseness dual-rebind still sees both names applied.

```scheme
;; Concurrent dual rebind (distinct names) — both should apply or one waits
(let* ((fa (fiber:spawn (lambda ()
              (and (mutate:rebind "ka" "(lambda (x) (* x 3))" "a")
                   (begin (eval-current) #t)))))
       (fb (fiber:spawn (lambda ()
              (and (mutate:rebind "kb" "(lambda (x) (* x 5))" "b")
                   (begin (eval-current) #t)))))
       (ra (fiber:join fa))
       (rb (fiber:join fb)))
  ;; After both joins: ka and kb bound; values 21 and 35 for arg 7
  …)
```

## Shutdown / lifetime (dtor drain)

Issue #3394: thread-fallback workers are **joinable**, not detached. The
spawn path registers each worker in a registry keyed by fiber id; a
successful `fiber:join` joins it immediately, and `~Evaluator` drains any
still-unjoined workers **before** arena/workspace teardown (bounded
best-effort — a body not ready after a 2s wait is detached with a
`[fiber:drain]` WARN so a stuck fiber cannot stall evaluator teardown;
same contract as orch agents, #2078). Before #3394 an abandoned spawn
could still be evaluating its body while the service tore down the
workspace — a use-after-free that surfaced as a flaky
`PersistentChildVector` out-of-bounds abort in `test_fiber_spawn_cli`.

Hosts should still prefer explicit `fiber:join` (or sequential `let*`
spawn+join per the binding discipline above); the dtor drain is a safety
net for abandoned fibers, not a substitute for joining.

## Sequential-yield surrogate

When spawn is capability-denied or a host deliberately avoids OS threads,
**cooperative sequential-yield fanout** (Aether examples 12 / 17 dual-mode)
remains a valid denseness PASS path for single-worker residual hunting.
It is not a substitute for multi-worker races once thread-fallback spawn
is available.

## Per-fiber COW overlay (Issue #4715)

A body can run against a private copy-on-write overlay instead of the shared
env top table:

```scheme
(fiber:overlay-begin)                 ; snapshot base epoch + commit sequence
(fiber:overlay-define "name" value)   ; private define
(fiber:overlay-set! "name" value)     ; private rebind of a committed binding
(fiber:overlay-get "name" default)    ; overlay value, else committed, else default
(fiber:overlay-has "name")
(fiber:overlay-commit)                ; merge; error on a write-write conflict
(fiber:overlay-rollback)              ; drop the overlay, apply nothing
(fiber:overlay-pending)               ; bindings held privately right now
(fiber:overlay-conflict)              ; condition object of our last rejection, or #f
(fiber:overlay-report)                ; deterministic stats string
```

While an overlay is open, **ordinary env reads inside that body see the
overlay** — not only `fiber:overlay-get`. The read path (`Env::lookup` /
`Env::lookup_cell_index`) tests one thread-local depth word and only then
consults the registry, so a program that never opens an overlay keeps
byte-identical semantics at the cost of a single thread-local load per env
lookup. Defines, `set!` and `(mutate:rebind …) + (eval-current)` inside the
body are captured into the overlay as a private cell: the shared cell is not
written, so a sibling body keeps reading the committed value until
`fiber:overlay-commit` publishes the new cell in one step (old-or-new, never
torn).

Commit is optimistic and **first-committer-wins**: the overlay scans every
pending binding in sorted name order *before* applying anything, and a binding
another fiber committed after this overlay began aborts the whole overlay and
applies nothing. There is no last-writer-wins merge. Two validations run in
that pre-apply pass, in this order:

1. **write-write** — a binding this overlay wrote that another fiber committed
   after it began;
2. **read-set / stale read** — a binding this overlay *read* (ordinary env
   reads are recorded while the overlay is open) that another fiber committed
   after it began. Everything the body computed from that read is stale, so
   committing it would be write skew; the commit is rejected instead.

A rejected commit still returns the historical `fiber:cow-conflict: <name>`
error string (the prefix existing callers match), with the condition appended
as trailing fields:

```text
fiber:cow-conflict: <name> binding=<name> winner-fiber=<id>
  our-epoch=<n> committed-epoch=<n> our-seq=<n> winner-seq=<n>
  kind=write-write|read-set
```

The same verdict is a condition object (a hash) from
`(fiber:overlay-conflict)`: `binding`, `winner-fiber`, `our-epoch`,
`committed-epoch`, `our-seq`, `winner-seq`, `kind`, `read-only`,
`schema`. `winner-fiber` is a decimal **string** — the overlay key is a
64-bit token (scheduler fiber id, or the CLI thread-key domain) that does not
fit the fixnum payload; the epoch / sequence fields are fixnums.

Every overlay commit appends one record to the workspace mutation log
(`query :mutation-log`): op `fiber:overlay-commit`, `target` = NULL_NODE,
summary `bindings=<sorted names> epoch=<n> fast-forward=<0|1>`. A rejected
commit appends a RolledBack record naming the winner:
`bindings=<names> conflict=<name> winner-fiber=… our-epoch=… committed-epoch=…
our-seq=… winner-seq=… kind=…`. The record needs a live workspace FlatAST
(a `set-code` / `eval-current` program, or a CompilerService with a
workspace); a pure-EDSL program has no workspace log to append to.
`fiber:overlay-report` keeps its historical fields and appends
`read-conflicts=<n>` (the stale-read subset of `conflicts`).

Limits (documented, tracked under #4715):

- A **closure-valued** rebind still writes through immediately. Rebinding a
  define must-deopts the closure the shared cell holds, so a private copy
  would leave sibling bodies calling an invalidated closure
  (`eval_flat: invalid closure`). Private capture applies to scalars, strings
  and heap values; closures join the overlay once the workspace-node slice
  (the other half of #4715) rebases the AST node together with its binding.
- `mutate:rebind` itself still mutates the workspace AST (set-code / AST
  snapshots / `function_sources_`); only the env binding is overlaid.
- A rolled-back overlay cannot restore a pre-rebind closure, for the same
  deopt reason — rollback is clean for value bindings.

## Related

- Hephaestus `notes/host-residuals.md` **H9** (spawn ids) + #2685 caveat
- Hephaestus concurrent-rebind + mutate-in-fiber (`let*`)
- Aether parallel-yield, dual-mode residuals
- Implementation: `src/compiler/evaluator_primitives_messaging.cpp`
  (spawn ids); multi-define begin in `evaluator_eval_flat.cpp`
