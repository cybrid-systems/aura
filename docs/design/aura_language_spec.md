# Aura Language Specification

> Quick reference for Aura Lisp syntax, primitives, and idioms.
> Designed for LLM code generation — covers what models get wrong.
> Updated: 2026-05-23 — Sound Gradual Typing, ADT, M4 Linear Ownership.

---

## 1. Core Syntax

Aura is a Lisp-1: variables and functions share the same namespace.

```scheme
(+ 1 2 3)                          ; → 6   (variadic arithmetic)
(- 10 3 2)                         ; → 5   (subtract in order)
(* 2 3 4)                          ; → 24
(/ 10 2)                           ; → 5
(= 1 1 1)                          ; → #t  (chained comparison)
(< 1 2 3)                          ; → #t
(modulo 10 3)                      ; → 1   (NOT mod)
(not #f)                           ; → #t
(and #t #f)                        ; → #f
(or #t #f)                         ; → #t
```

### Comparison Operators

```scheme
(= a b)        ; numeric equality
(string=? a b) ; string equality (NOT string=)
(eq? a b)      ; pointer/reference equality (identity)
```

### Special Forms

```scheme
(define (fn-name args) body)     ; Function definition
(define name value)              ; Variable definition
(lambda (args) body)             ; Anonymous function
(if cond then-expr else)         ; Conditional — MUST have else
(cond (test expr) ...)           ; Multi-branch conditional
(begin expr1 expr2 ...)          ; Sequence (side effects)
(let ((var val) ...) body)       ; Local bindings (parallel)
(let* ((var val) ...) body)      ; Sequential local bindings
(letrec ((fn (lambda ...)) ...) body) ; Recursive local functions
(quote expr)   or  'expr         ; Quote (literal data)
(set! var val)                   ; Mutation
(display expr)                   ; Print + return void
(require "std/name" all:)        ; Import stdlib module
```

### Named Let (Looping)

```scheme
(let loop ((i 0) (acc 0))
  (if (= i 10) acc
    (loop (+ i 1) (+ acc i))))
```

### Type System (Sound Gradual Typing)

Aura uses **Sound Gradual Typing** with `Any` (untyped) as the top type.
Three-tier architecture: **lowering CastOp → type checker → blame tracking**.

```scheme
;; Expression-level annotation
(check 42 : Int)           ; Lowering emits CastOp at boundary
(check 3.14 : Float)       ; Float annotation

;; Variable type annotation (no-op at runtime)
(: x Int)                  ; Declares x has type Int (for type checker)
(: x Int 42)               ; 3-arg form: bind x:Int with value 42
(: f (-> Int String))      ; Function type annotation

;; Lambda parameter type annotation
((lambda ((: x Int)) (+ x 1)) 41)  ; Lambda with typed param → 42
(define (f (: x Int)) (+ x 1))     ; Define shorthand with typed param
(define (add (: a Int) (: b Int)) (+ a b)) ; Multiple typed params

;; Functor with type-annotated params
(define-module (Box :T) (export wrap)
  (define (wrap (: x :T)) x))
(Box Int)                          ; Instantiate: :T → Int

;; Runtime type predicates for occurrence typing
(string? x)                ; → #t if x is a string
(float? x)                 ; → #t if x is a float (3.14)
(pair? x)                  ; → #t if x is a cons pair
(symbol? x)                ; → #t if x is a symbol (InternalType)
```

**Occurrence typing** narrows types inside conditional branches:

```scheme
;; Type narrows to String in then-branch
(let ((x "hello"))
  (if (string? x)
    (string-append x "!")    ; x: String here
    0))

;; Float arithmetic after narrowing
(let ((x 3.14))
  (if (float? x) (+ x 1) 0))

;; and/or combine predicates
(if (and (pair? x) (string? (car x))) (string-append (car x) "!") 0)
```

**Coercion** happens at type boundaries automatically:

```scheme
(+ 1 2)                     ; Int + Int → Int
(+ 1.5 2)                   ; Float + Int → Float (coerce Int→Float)
(+ 1 "42")                  ; Int + String → Int (coerce String→Int: 42)
(display "count: " 42)      ; String + Int → String (coerce Int→String: "42")
(string-append "x" "y")     ; String + String → String
```

**Blame tracking** assigns responsibility to typed/untyped boundaries:

```scheme
;; Runtime type mismatch triggers blame error with position info
(+ 1 "hello")               ; Coercion warning on stderr
```

**Value Restriction**: only syntactic values (literals, lambdas, variables) are polymorphic; non-values (function calls, mutations) get monomorphic types.

```scheme
;; OK: syntactic value → can be polymorphic
(define id (lambda (x) x))

;; OK: let binding of syntactic value
(let ((x (lambda (y) y))) ...)

;; Value restricted: non-syntactic, gets monomorphic type
(let ((x (+ 1 2 3 4))) x)   ; x: Int, not polymorphic
```

**Gradual Guarantee**: changing a type annotation preserves all existing behavior.

---

## 2. Algebraic Data Types (ADT)

Define and pattern-match on tagged unions:

```scheme
;; Type definition
(define-type (Option a)
  (Some a)
  (None))

;; Constructors
(Some 42)                    ; Creates tagged pair
(None)                       ; Creates empty variant

;; Pattern matching via match
(match (Some 42)
  ((Some x) (display x))      ; Extracts x=42
  ((None) (display "none")))

;; Wildcard match
(match (Some 42)
  ((Some x) #t)
  ((None)   #t)
  ((_ _)    #f))              ; Wildcard catches everything

;; Multi-constructor type
(define-type (Season)
  (Spring)(Summer)(Fall)(Winter))
```

ADT constructors create tagged pairs: `(Some 42)` = `(#(tag=Some 0) . 42)`.
Use `match` to destructure — NOT `car`/`cdr` directly.

---

## 3. M4 Linear Ownership

Linear types enforce single-use ownership at compile time:

```scheme
;; Declare a linear variable
(define x : (Linear Int) 42)

;; Move transfers ownership
(let ((y (move x)))           ; x consumed, y owns the value
  (display y))

;; Borrow (read-only reference)
(let ((y (& x)))              ; x still valid after
  (display y))

;; Mutable borrow
(let ((y (&mut x)))           ; x still valid after
  (set! y 99))

;; Drop explicitly
(drop x)                      ; Release ownership
```

**Reader macros:**
- `&x` → `(borrow x)` — immutable borrow
- `&mut-x` → `(mut-borrow x)` — mutable borrow

**Rules:**
- `(Linear T)` typed variable can't be copied implicitly
- `(move x)` transfers ownership: `x` is no longer accessible
- `(& x)` borrows: `x` remains accessible
- At end of scope, linear variables must be consumed or dropped
- Violations are compile-time errors

---

## 4. Primitives

### Booleans
`#t` `#f` — NOT `true`/`false`/`#true`/`#false`

### Pairs and Lists
```scheme
(cons 1 (list))              ; → (1)
(list 1 2 3)                 ; → (1 2 3)
(car (list 1 2 3))           ; → 1
(cdr (list 1 2 3))           ; → (2 3)
(null? (list))               ; → #t
(pair? (cons 1 2))           ; → #t
```

### Vectors
```scheme
(vector 1 2 3)               ; → #(1 2 3)
(vector-ref v 1)             ; → 2
(vector-length v)            ; → 3
(vector-set! v 0 99)         ; Mutates in-place
(vector? v)                  ; → #t
(make-vector 5 42)           ; → #(42 42 42 42 42)
(list->vector (list 1 2))    ; → #(1 2)
(vector->list v)             ; → (1 2 3)
```

### Characters
```scheme
(char=? #\a #\b)             ; → #f
(char<? #\a #\b)             ; → #t
(char-alphabetic? #\a)       ; → #t
(char-numeric? #\1)          ; → #t
(char-whitespace? #\space)   ; → #t
(char-upcase #\a)            ; → #\A
(char-downcase #\A)          ; → #\a
(char->integer #\a)          ; → 97
(integer->char 97)           ; → #\a
```

### Strings
```scheme
(string-length "hello")       ; → 5
(string-append "a" "b")       ; → "ab"
(substring "hello" 1 3)       ; → "el"
(string=? "a" "b")            ; → #f
(number->string 42)            ; → "42"
(string->list "abc")           ; → (97 98 99)  ← INTEGER CHAR CODES!
(list->string (list 97 98))    ; → "ab"
(string-join (list "a" "b") ",") ; → "a,b"
(format "hello ~a!" "world")   ; → "hello world!"
```

**⚠️ CRITICAL:** `string->list` returns **integers** (ASCII codes), not characters.
Compare with `(= c 40)` NOT `(char=? c #\()`.
Char codes: `40=(` `41=)` `91=[` `93=]` `123={` `125=}`

### Hash Tables
```scheme
(define h (hash "a" 1 "b" 2))
(hash-ref h "a")              ; → 1
(hash-has-key? h "a")         ; → #t
(hash-set! h "c" 3)           ; ⚠️ MUTATES in-place, returns void!
(hash-keys h)                 ; → ("a" "b" "c")
(hash-values h)               ; → (1 2 3)
(hash-length h)               ; → 3
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
(display 42)                 ; prints 42, returns ()
(display h)                  ; ⚠️ prints <hash[N]>, NOT contents!
(display (hash-keys h))      ; shows ("a" "b" "c")
(display (hash-values h))    ; shows (1 2 3)
(display #<procedure>)       ; prints function reference
```

**CRITICAL:** `(define (f x) x)` followed by `(display f)` outputs `#<procedure>` NOT the function's result. Always call the function: `(display (f 42))`.

---

## 5. Standard Library

Load with `(require std/name all:)` (imports symbols into global scope):

### std/list
`filter` `map` `foldl` `foldr` `range` `sort` `take` `drop` `length` `reverse` `zip` `flatten` `partition` `member`

```scheme
(require std/list all:)
(filter odd? (list 1 2 3 4 5))          ; → (1 3 5)
(map (lambda (x) (* x 2)) (list 1 2 3)) ; → (2 4 6)
(range 2 10)                            ; → (2 3 4 5 6 7 8 9)
(foldl + 0 (list 1 2 3))                ; → 6
(sort (list 3 1 2) (lambda (a b) (< a b))) ; → (1 2 3)
(take 3 (list 1 2 3 4 5))              ; → (1 2 3)
(drop 2 (list 1 2 3 4 5))              ; → (3 4 5)
(partition even? (list 1 2 3 4))        ; → ((2 4) (1 3))
```

### std/string
`string-split` `string-trim` `string-join` `str->list` `list->str`

### std/hash
`hash-keys` `hash-values` `hash-has-key?` `hash-ref` `hash-set!` `hash-count`

### std/iter
`for-each` `for`

```scheme
(require std/iter all:)
(for-each (lambda (x) (display x)) (list 1 2 3))  ; prints 1, 2, 3
```

### std/math
`square` `sqrt` `factorial` `pi` `abs` `min` `max` `sin` `cos` `tan` `floor` `ceil` `round` `exp` `log` `pow` `rand` `rand-int` `mean` `median` `stddev` `sum` `product`

### std/io
`read-file` `write-file` `file-exists?` `delete-file`

### std/data
`stack` `queue` — functional stack/queue implementations

### std/algorithm
`sort` `binary-search` `linear-search` — generic algorithm helpers

### std/adaptive
`measure-distance` `structured-diagnosis` — PID control theory feedback

### std/random
`rand` `rand-int` `rand-range` `shuffle`

### std/datetime
`now` `date` `time` `format-date`

---

## 6. C FFI

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

## 7. TCP

```scheme
(tcp-connect "host" port)        ; → socket fd (integer) | ()
(tcp-send fd data)               ; → void
(tcp-recv fd)                    ; → response string
(tcp-close fd)                   ; → void
```

---

## 8. EDSL (Code Mutation)

Used for iterative code refinement:

```scheme
(set-code "(define (f x) x)")                    ; Set workspace AST
(query:find "f")                                 ; Find node IDs by name
(mutate:rebind "f" "(lambda (x) (* x 2))")       ; Replace definition
(eval-current)                                   ; Evaluate modified workspace
```

---

## 9. Intend (Built-in LLM)

```scheme
(intend "Write a function to compute fibonacci" max-attempts: 5)
```
- Built-in special form that calls an LLM API (curl)
- Reads `LLM_API_KEY` / `LLM_MODEL` / `LLM_BASE_URL` env vars
- Iteratively generates, evaluates, and fixes Aura code
- Returns `#(status:... goal:... iterations:...)`

---

## 10. Compile-Time Introspection (--inspect)

```bash
./build/aura --inspect < file.aura
./build/aura --inspect ir
./build/aura --inspect closures
./build/aura --inspect cache
```

- Dumps internal IR, closure metadata, and cache structures as JSON
- Uses P2996 compile-time reflection for validated serialization

---

## 11. Common Pitfalls

| Mistake | Wrong | Correct |
|---------|-------|---------|
| Display hash | `(display h)` → `<hash[N]>` | `(display (hash-keys h))` |
| String chars | `(char=? c #\()` | `(= c 40)` |
| hash-set! return | `(loop (hash-set! ...) ...)` | `(hash-set! ...)(loop ...)` |
| Modulo | `(mod n i)` wrong result | `(modulo n i)` |
| Division by zero | `(/ 1 0)` crash | guard with `(if (= x 0) ...)` |
| Missing else | `(if cond expr)` → returns `()` | `(if cond expr default)` |
| Display procedure | `(display f)` → `#<procedure>` | `(display (f args))` |
| Wrong lib-id | `(c-func 'int ...)` | `(c-func -1 ...)` |
| cdddr (4-level cxr) | `(cddddr lst)` | `(cdr (cdr (cdr (cdr lst))))` or `(list-tail lst 4)` |
| Recursive let | `(let ((x (lambda ...))) ...)` | `(letrec ((x (lambda ...))) ...)` |

---

## 12. API Reference

The full primitive list is available at runtime:
```scheme
(api-reference)   ; returns string of all primitives
```

## 13. Benchmark Tasks

85 EDSL code generation tasks across 13 categories. Latest results (2026-05-23):

| Category | Description | Tasks |
|----------|-------------|-------|
| basic | Arithmetic, lambdas, vector ops | 9 |
| list | Map/filter/foldl/range/sort/reverse | 8 |
| recursion | GCD, primes, quicksort, tree DFS | 8 |
| algorithm | binary-search, merge-sort, DP | 10 |
| hash | Hash table operations, word-freq | 5 |
| string | Reverse, split/join, anagram | 5 |
| adt | define-type, match, variant inference | 5 |
| type | Gradual typing, coercion, occurrence | 15 |
| coercion | Bool/Int/Float/String coercion | 3 |
| edsl | set-code, query, mutate | 3 |
| json | JSON round-trip | 1 |
| ffi | C FFI sqrt, strlen | 2 |
| other | linear-basic, macro, tcp, memoize | 11 |

Run: `LLM_API_KEY="***" python3 tests/edsl_benchmark.py`
