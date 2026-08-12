# Agent recovery closed loop (Issue #2917)

Production Agents self-evolve under multi-fiber load. When a primitive or
mutate path fails, the recommended pattern is **error-as-data → diagnose →
safe fix under boundary → re-verify → poll recovery stats**.

## Recommended pattern

```scheme
;; 1. Observe failure (error value or last mutate error)
(define err "unbound variable: map")
(define code "(map add1 (list 1 2 3))")

;; 2. Closed-loop recovery under MutationBoundaryGuard (automatic)
(define r (agent:recover-from-error err code))
;; r = (ok status-code cause fix-type fixed-code-or-void)

(define ok (car r))
(define status (car (cdr r)))          ; 1 = success
(define fixed (car (cdr (cdr (cdr (cdr r))))))

;; 3. On success, install fixed code and re-check readiness
(if ok
    (begin
      (set-code fixed)
      (engine:metrics "typecheck-status"))   ; "ok" when green
    #f)

;; 4. Poll aggregate recovery health
(engine:metrics "query:agent-recovery-stats")
;; fields: attempts-total, success-total, fail-total, last-status,
;;         success-rate-bp, hold-cleared-total, checkpoint-total,
;;         strict-hold, schema=2917
```

### Single-arg form (mutate residual)

```scheme
;; After a failed mutate that set last_mutate_error_ / strict_mutate_hold:
(agent:recover-from-error workspace-source-string)
```

## Status codes (`last-status` / result status-code)

| Code | Meaning |
|------|---------|
| 0 | idle (no recovery yet) |
| 1 | success (fix applied, hold cleared when present) |
| 2 | apply-fix failed |
| 3 | no diagnosis for error text |
| 4 | MutationBoundaryGuard acquire failed |
| 5 | bad args / no last error |

## Building blocks

| Primitive | Role |
|-----------|------|
| `diagnose` | error-string → (cause target fix-type fix-data explanation) |
| `apply-fix` | code + diagnosis → fixed code (under Guard) |
| `agent:recover-from-error` | full closed loop (#2917) |
| `check-preconditions` | node-level pre-mutate gate |
| `panic-checkpoint` / `panic-restore` | used internally / available for Agents |
| `(engine:metrics "typecheck-status")` | last mutate error or `"ok"` |

## Failure classes covered by `diagnose`

- unbound variable → `add-require` / `define-or-require`
- type error cannot call → `define-function`
- parse / procedure display
- **prim-heap-quota** / resource quota → `raise-quota` (recovery may bump soft limit)
- type mismatch / deny → `clear-hold-or-rewrite` (recovery clears strict hold)

## Safety

- `agent:recover-from-error` is an **agent:** name → `kEffectMutate` via #2152 dispatch.
- Always acquires `MutationBoundaryGuard::try_acquire` and bumps verify-tool counters
  (`query:verify-tool-guard-stats`).
- Saves a panic checkpoint when possible; on success clears `last_mutate_error_` /
  `strict_mutate_hold` so Agents do not leave half-green residual hold.

## Related

- Soft heap quotas: [prim-heap-quota.md](prim-heap-quota.md) (#2916)
- Error-return convention: [primitive-error-convention.md](primitive-error-convention.md)
- Authoring scaffold: [primitive-authoring-contract.md](primitive-authoring-contract.md)
