# Truthiness

Since #4491, Aura follows R7RS §6.3: **only `#f` is false.** Every other
value is true, including `0`, `0.0`, `'()`, `""` and void.

This applies to `if`, `cond`, `when`, `unless`, `and`, `or`, `not`,
`while`, and every predicate argument (`filter`, `any`, …), on all
execution paths: the tree-walker, the IR executor, the JIT, and AOT.

- `and` / `or` return an operand, not `#t`/`#f`: `(and 1 'y)` is `y`, and
  `(or #f 0)` is `0`. `(and)` is `#t` and `(or)` is `#f`.
- `member`, `memv` and `assv` return `#f` when nothing is found, so
  "index-or-`#f`" style helpers work as in Scheme.

Implementation: `kIntZeroIsFalse` / `raw_is_truthy` in
`src/compiler/value_tags.h` is the single source of truth. It is read by
`pure::is_truthy`, the JIT `jit_truthy` helper, and constant folding.

## Migrating code that relied on 0 being false

Before #4491, integer `0` was false. Run with `AURA_TRUTHY_WARN=1` to get
a one-time warning on stderr the first time a condition tests integer
`0`. Replace `(if n …)` with `(if (not (= n 0)) …)` where 0 meant "no".
