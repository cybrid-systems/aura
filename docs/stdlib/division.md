# Division on exact integers (Issue #4499)

`/` with two exact integer operands truncates toward zero, as in C:

```scheme
(/ 7 2)    ; => 3
(/ -7 2)   ; => -3
(/ 7.0 2)  ; => 3.5   any inexact operand gives a float
```

Aura has no exact rationals (`7/2` is not a literal; see #4498), so this
is a deliberate decision, not R7RS's `7/2`. Use `modulo`/`remainder` for
the remainder, and when a fractional result is wanted either make one
operand inexact or use `fdiv` from `std/math`:

```scheme
(require "std/math" all:)
(fdiv 7 2)                  ; => 3.5
(/ (exact->inexact 7) 2)    ; => 3.5
```
