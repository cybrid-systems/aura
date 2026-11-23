# M4 live-process fork, hot replace, and source export

This is the supported surface for `aura-agents` M4 (issues #5410-#5412).
It is deliberately separate from AOT `.so` reload (#5184).

## Snapshot contract (#5410)

`(mutate :snapshot tag)` checkpoints the **live workspace**, not the file or
`-e` top-level defines:

```aura
(mutate :snapshot "x")          ; -> -1
(query :snapshot-fail-reason)    ; -> :no-workspace
(set-code "(define x 10)")
(eval-current)
(mutate :snapshot "x")          ; -> >= 0
```

`ast:snapshot` is **not** a public binding; the public spelling is
`(mutate :snapshot ...)`. `mw:snapshot` is the internal implementation name
and must not be used by callers.

There is no `reachable` primitive to read after a fork. Static reachability
analysis is `(query :reaches name)` -> `(defs uses)`; a zero from an ad-hoc
`reachable` variable was a downstream probe name, not a snapshot result.

## Fork N isolated worldlines (#5410)

Use one COW overlay per fiber. Ordinary reads inside the body see the
overlay; writes stay private until commit. Commit is first-committer-wins,
and a rejected commit applies nothing.

```aura
(define f (fiber:spawn
  (lambda ()
    (mutate :overlay-begin)
    (mutate :overlay-set! "x" 11)   ; private to this fiber
    (mutate :overlay-commit))))      ; publish the winner
(fiber:join f)

(define g (fiber:spawn
  (lambda ()
    (mutate :overlay-begin)
    (mutate :overlay-set! "x" 99)
    (mutate :overlay-rollback))))    ; discard the loser
(fiber:join g)
```

APIs: `mutate :overlay-begin`, `:overlay-define`, `:overlay-set!`,
`:overlay-get`, `:overlay-has`, `:overlay-commit`, `:overlay-rollback`,
`:overlay-pending`, `:overlay-conflict`, `:overlay-report`. A conflict
returns the structured condition from `:overlay-conflict`. Scalar, string
and heap bindings are private; closure-valued rebinds still write through
(the documented #4715 limit).

## Hot replace without restarting (#5411)

Install the live program once with `set-code` + `eval-current`. After that,
`mutate :rebind` replaces one define and installs the new closure **without**
re-evaluating the whole workspace:

```aura
(set-code "(define counter 0)\n(define (tick) (set! counter (+ counter 1)))")
(eval-current)
(tick) (tick)                              ; counter = 2
(mutate :rebind "tick"
         "(lambda () (set! counter (+ counter 10)))"
         "hot")
(tick)                                     ; counter = 12, not 0
```

Do **not** call `eval-current` after a hot rebind: it re-evaluates every
define, resetting counters, RNG state, entities and open resources. A
rejected rebind returns an error and leaves the previous body and state
alive. The rebind path marks only the changed define and its caller
cone dirty; `eval-current` is the whole-workspace relower and is not part of
the hot path. `std/hot-strategy:swap!` and `:heal!` follow the same rule now; they
preserve runtime state and only rebind the named define.

## Source export for PRs (#5412)

After a rebind, use the source-preserving export to keep untouched comments
and whitespace byte-identical:

```aura
(current-source :workspace :preserve)
```

The plain `(current-source :workspace)` remains the compact unparse view.
`:preserve` falls back to unparse when the source stamp is stale, so it is
never a stale-text API. `mutate:rebind` patches the set-code buffer in
place; only the changed define body moves.

## Boundary with #5184

This page covers pure-Aura workspace hot replace (interpreter/JIT). Native
`.so` kernel hot reload and region masks remain `#5184` / `std/hot-update`.
