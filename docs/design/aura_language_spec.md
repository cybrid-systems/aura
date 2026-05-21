# Aura Language Specification

> Quick reference for Aura Lisp syntax, primitives, and idioms.
> Designed for LLM code generation — covers what models get wrong.

---

## 1. Core Syntax

Aura is a Lisp-1: variables and functions share the same namespace.

```scheme
(+ 1 2 3)           ; → 6
(- 10 3 2)          ; → 5  (subtract in order)
(* 2 3 4)           ; → 24
(/ 10 2)            ; → 5
(= 1 1 1)           ; → #t  (chained comparison)
(< 1 2 3)           ; → #t
(not #f)            ; → #t
(and #t #f)         ; → #f
(or #t #f)          ; → #t
```

### Special Forms

```scheme
(define (fn-name args) body)    ; Function definition
(define name value)             ; Variable definition
(lambda (args) body)            ; Anonymous function
(if cond then-expr else-expr)   ; Conditional — MUST have else
(cond (test expr) ...)          ; Multi-branch conditional
(begin expr1 expr2 ...)         ; Sequence (side effects)
(let ((var val) ...) body)      ; Local bindings
(letrec ((fn (lambda ...))) body) ; Recursive local functions
(quote expr)   or  'expr        ; Quote
(set! var val)                   ; Mutation
```

### Named Let (Looping)

```scheme
(let loop ((i 0) (acc 0))
  (if (= i 10) acc
    (loop (+ i 1) (+ acc i))))
```

### Type System

```scheme
(check 42 : Int)           ; Compile-time type check
(type-of 42)               ; → Int (runtime)
(string? x)                ; Predicate for occurrence typing
(if (string? x)
    (string-append x "!")
    0)
```

---

## 2. Primitives

### Booleans
`#t` `#f` — NOT `true`/`false`/`#true`/`#false`

### Pairs and Lists
```scheme
(cons 1 (cons 2 (list)))   ; → (1 2)
(list 1 2 3)               ; → (1 2 3)
(car (list 1 2 3))         ; → 1
(cdr (list 1 2 3))         ; → (2 3)
(null? (list))             ; → #t
(pair? (cons 1 2))          ; → #t
```

### Strings
```scheme
(string->list "abc")       ; → (97 98 99)  ← INTEGER CHAR CODES, not chars!
(string-length "hello")    ; → 5
(string-append "a" "b")    ; → "ab"
(substring "hello" 1 3)    ; → "el"
(string=? "a" "b")         ; → #f
(number->string 42)        ; → "42"
```

**⚠️ CRITICAL:** `string->list` returns **integers** (ASCII codes), not characters.
Compare with `(= c 40)` NOT `(char=? c #\()`.
Char codes: `40=(` `41=)` `91=[` `93=]` `123={` `125=}`

### Hash Tables
```scheme
(define h (hash "a" 1 "b" 2))
(hash-ref h "a")            ; → 1
(hash-has-key? h "a")       ; → #t
(hash-set! h "c" 3)         ; ⚠️ MUTATES in-place, returns void!
(hash-keys h)               ; → ("a" "b" "c")
(hash-values h)             ; → (1 2 3)
(hash-length h)             ; → 3
```

**⚠️ hash-set! MUTATES in-place and returns void.** DO NOT use as:
```scheme
;; WRONG — loop receives void as new 'seen'
(let loop ((seen (hash)) (xs lst))
  (loop (hash-set! seen x #t) (cdr xs)))

;; CORRECT — mutate then recurse with same variable
(let loop ((seen (hash)) (xs lst))
  (hash-set! seen x #t)
  (loop seen (cdr xs)))
```

### Display Output
```scheme
(display 42)                ; prints 42, returns ()
(display h)                 ; ⚠️ prints <hash[N]>, NOT contents!
(display (hash-keys h))     ; shows ("a" "b" "c")
(display (hash-values h))   ; shows (1 2 3)
```

---

## 3. Standard Library

Load with `(require std/name all:)` (imports symbols into global scope):

### std/list
`filter` `map` `foldl` `range` `sort` `take` `drop` `length` `reverse` `zip`
```scheme
(require std/list all:)
(filter odd? (list 1 2 3 4 5))      ; → (1 3 5)
(map (lambda (x) (* x 2)) (list 1 2 3))  ; → (2 4 6)
(range 2 10)                          ; → (2 3 4 5 6 7 8 9)
(foldl + 0 (list 1 2 3))              ; → 6
(sort (list 3 1 2) (lambda (a b) (< a b)))  ; → (1 2 3)
```

### std/string
`string-split` `string-trim` `string-join`

### std/hash
`hash-keys` `hash-values` `hash-has-key?` `hash-ref` `hash-set!`

### std/iter
`for-each` `for`
```scheme
(require std/iter all:)
(for-each (lambda (x) (display x)) (list 1 2 3))  ; prints 1, 2, 3
```

### std/math
`square` `sqrt` `factorial`

---

## 4. C FFI

```scheme
(c-func lib-id "name" "(ArgTypes) -> ReturnType")
```
- `lib-id`: `-1` = RTLD_DEFAULT (libc/libm), or positive int for dlopen handle
- Name: C function name as string
- Signature: string like `"(Float) -> Float"` or `"(String) -> Int"`

```scheme
(define sqrt-fn (c-func -1 "sqrt" "(Float) -> Float"))
(display (sqrt-fn 9.0))     ; 3.0

(define strlen-fn (c-func -1 "strlen" "(String) -> Int"))
(display (strlen-fn "hello"))  ; 5
```

---

## 5. TCP

```scheme
(tcp-connect "host" port)       ; → socket fd (integer) | ()
(tcp-send fd data)              ; → void
(tcp-recv fd)                   ; → response string
(tcp-close fd)                  ; → void
```

---

## 6. EDSL (Code Mutation)

Used for iterative code refinement:

```scheme
(set-code "(define (f x) x)")                    ; Set workspace AST
(query:find "f")                                 ; Find node IDs by name
(mutate:rebind "f" "(lambda (x) (* x 2))")       ; Replace definition
(eval-current)                                   ; Evaluate modified workspace
```

---

## 7. Common Pitfalls

| Mistake | Wrong | Correct |
|---------|-------|---------|
| Display hash | `(display h)` → `<hash[N]>` | `(display (hash-keys h))` |
| String chars | `(char=? c #\()` | `(= c 40)` |
| hash-set! return | `(loop (hash-set! ...) ...)` | `(hash-set! ...)(loop ...)` |
| Modulo | `(mod n i)` wrong result | `(modulo n i)` |
| Division by zero | `(/ 1 0)` crash | guard with `(if (= x 0) ...)` |
| Missing else | `(if cond expr)` → returns `()` | `(if cond expr default)` |
| Circular list | `(define xs (cons 1 xs))` | allocate properly |
| Wrong lib-id | `(c-func 'int ...)` | `(c-func -1 ...)` |

---

## 8. API Reference

The full primitive list is available at runtime:
```scheme
(api-reference)   ; returns string of all primitives
```
