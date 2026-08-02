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

## Aether follow-up

Example 04 (hot-strategy-heal) can call `std/hot-strategy` instead of ad-hoc rebind+snapshot once this module is on the host path.

## Refs

- Issue #2582 / parent denseness tracker #2578
- Aether `examples/04-hot-strategy-heal`
- `lib/std/hot-strategy.aura`, `lib/std/hot-update.aura`
