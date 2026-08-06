# Pure-Aura hot strategy (Issue #2582)

## Two surfaces

| Module | Granularity | Use when |
|--------|-------------|----------|
| **`std/hot-strategy`** | Workspace named defines via `mutate:rebind` + `ast:snapshot` | Agent denseness / closed-loop strategy swap (Aether Axis D) |
| **`std/hot-update`** | AOT `aot:reload` of `.so` plugins + region masks | Native code hot-reload, multi-agent region isolation |

`std/hot-update` is **intentionally AOT-oriented**. Do not use it for pure-Aura strategy rebind denseness probes.

## Recommended pure-Aura flow

```aura
(require "std/mutate" all:)
(require "std/hot-strategy" all:)

;; 1. Seed a named strategy in the workspace
(set-code "(define (strat x) (* x 2))")
(eval-current)
(hot-strategy:register! "strat" "(lambda (x) (* x 2))")

;; 2. Hot swap (snapshots first)
(hot-strategy:swap! "strat" "(lambda (x) (* x 3))" "aggressive")
(eval-current)
(strat 5)  ; → 15

;; 3. Last-good heal on failure
(hot-strategy:heal!)
(eval-current)
```

## Semantics

- **Version** — process-local counter (`hot-strategy:version`); not durable across process restarts.
- **Last-good body** — code string last successfully swapped/registered.
- **Last-good snap** — `ast:snapshot` id; `heal!` prefers `ast:restore`, else rebinds last-good body.
- **Freeze / coexistence** — none beyond workspace snapshot restore; concurrent fibers should use `mutate:atomic-batch-safe` if swapping multiple names.

## Rebind observability contract (Issue #2684 / denseness H7)

Pure-Aura `mutate:rebind` **does** mark incremental compile work. Host denseness probes often mis-read empty surfaces because of **wrong name**, **wrong arity**, or **reading after `eval-current` cleared dirty bits**.

### What rebind sets

| Surface | When | After `eval-current` re-lower |
|---------|------|-------------------------------|
| `(stats:get "compile:dirty-count")` | ≥1 if the name was already in `ir_cache_v2_` | often **0** (entry re-lowered clean) |
| `(compile:block-dirty-count "name")` | >0 body/impact blocks dirty | often **0** |
| `(compile:block-dirty-count)` | total dirty blocks (all defines) | often **0** |
| `(stats:get "compile:epoch")` | bumps (mutation epoch) | stays until next mutate |
| `query:jit-stats-hash` → `hotswap-invalidate-total`, `invalidate-function-calls`, `mutation-epoch` | **lifetime** counters bump on rebind | remain elevated |
| `query:jit-stats` / `compile:jit-stats` | AuraJIT metrics line (may be sparse until warm) | may grow `compiles` later |
| `hot-swap:fn` / `std/hot-update` | **AOT/plugin** path | not pure-Aura rebind |

### Recommended agent query pattern

```aura
(require "std/mutate" all:)
(require "std/stats" all:)

(set-code "(define sum-kernel (lambda (n) …))")
(eval-current)

(define epoch0 (stats:get "compile:epoch"))
(define inv0
  (let ((h (stats:get "query:jit-stats-hash")))
    (or (hash-ref h "hotswap-invalidate-total") 0)))

(mutate:rebind "sum-kernel" "(lambda (n) …closed-form…)" "spec")
;; Read *before* eval-current if you want sticky dirty bits:
(display (compile:block-dirty-count "sum-kernel"))  ; >0 when IR was cached
(display (compile:block-dirty-count))               ; total
(display (stats:get "compile:dirty-count"))         ; ≥1
(display (stats:get "compile:jit-stats"))           ; alias of query:jit-stats

(eval-current)
;; After re-lower, dirty counters drop — use lifetime deltas:
(define inv1
  (let ((h (stats:get "query:jit-stats-hash")))
    (or (hash-ref h "hotswap-invalidate-total") 0)))
;; assert: inv1 > inv0  and/or  (stats:get "compile:epoch") > epoch0
```

### Denseness Axis D claim

Correctness under load after algorithmic rebind does **not** require native JIT counter growth. Supported pure-Aura proof of “rebind caused recompile work” is:

1. **Pre-eval dirty** (`compile:dirty-count` / `compile:block-dirty-count`), and/or  
2. **Lifetime** `hotswap-invalidate-total` / `mutation-epoch` on `query:jit-stats-hash`.

Do **not** require `hot-swap:fn` or sticky `compile:*-dirty*` after `eval-current`.

## Aether / Hephaestus follow-up

Example 04 (hot-strategy-heal / jit-specialization) can call `std/hot-strategy` and the surfaces above instead of ad-hoc rebind+snapshot once this module is on the host path.

## Refs

- Issue #2582 / parent denseness tracker #2578
- Issue **#2684** (H7 compile/JIT dirty after pure-Aura rebind)
- Aether `examples/04-hot-strategy-heal`
- Hephaestus `examples/04-jit-specialization`, `notes/host-residuals.md` H7
- `lib/std/hot-strategy.aura`, `lib/std/hot-update.aura`
