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

Hephaestus probes `examples/09` / `10` use this style on purpose.

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

## Sequential-yield surrogate

When spawn is capability-denied or a host deliberately avoids OS threads,
**cooperative sequential-yield fanout** (Aether examples 12 / 17 dual-mode)
remains a valid denseness PASS path for single-worker residual hunting.
It is not a substitute for multi-worker races once thread-fallback spawn
is available.

## Related

- Hephaestus `notes/host-residuals.md` **H9** (spawn ids) + #2685 caveat
- Hephaestus `examples/09-concurrent-rebind`, `10-mutate-in-fiber` (`let*`)
- Aether `examples/12-parallel-yield`, dual-mode residuals
- Implementation: `src/compiler/evaluator_primitives_messaging.cpp`
  (spawn ids); multi-define begin in `evaluator_eval_flat.cpp`
