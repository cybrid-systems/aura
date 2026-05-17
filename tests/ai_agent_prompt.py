#!/usr/bin/env python3
"""Aura AI Agent — 系统提示生成器

生成 Aura Lisp 语言的完整 API 参考，包含标准库导出信息。
"""
import os

AURA_LIB = os.environ.get("AURA_PATH", "./lib")


def load_stdlib_exports():
    """读取标准库文件，提取每个文件的导出绑定列表"""
    exports = {}
    std_dir = os.path.join(AURA_LIB, "std")
    if not os.path.isdir(std_dir):
        return exports
    for f in sorted(os.listdir(std_dir)):
        if not f.endswith(".aura"):
            continue
        path = os.path.join(std_dir, f)
        name = f.replace(".aura", "")
        with open(path) as fh:
            code = fh.read()
        # Extract export declarations
        exported = []
        for line in code.split("\n"):
            line = line.strip()
            if line.startswith("(export"):
                # (export sym sym ...)
                rest = line[7:].rstrip(")").strip()
                exported.extend(rest.split())
        exports[name] = exported
    return exports


def build_system_prompt():
    """Build the complete system prompt for the AI agent."""
    std_exports = load_stdlib_exports()

    return """You are an Aura Lisp programmer. Aura is a Scheme-like Lisp dialect.

## LANGUAGE QUICK REFERENCE

### Core Syntax (Keep code short, one function at a time)
```
(define (fn-name params...) body...)     ; Top-level function
(define name value)                       ; Top-level binding  
(lambda (params...) body...)             ; Anonymous function
(if cond then-expr else-expr)            ; Conditional
(begin expr1 expr2 ...)                  ; Block: execute all, return last
(let ((var val) ...) body...)            ; Local bindings (single body only)
```

### Core Data & Arithmetic (use s-expr only, no `defn`/`[brackets]`/`#()`)
```
(list a b c)      (cons a b)        ; Construct
(car pair)        (cdr pair)        ; Destruct  (car=first, cdr=rest)
(null? x)         (pair? x)         ; Type predicates
(length lst)      (reverse lst)     ; List operations
(append a b)      (map fn lst)      ; Append & map (map takes ONE list only)
(filter fn lst)   (foldl f acc lst) ; Filter & fold left

(+ a b ...)    (- a b)    (* a b ...)    (/ a b)
(= a b)        (< a b)    (> a b)
(modulo n m)   (quotient n m)   (remainder n m)
(abs n)        (gcd n m)        (not x)
(zero? x)      (number? x)      (integer? x)   (float? x)
```

### String Operations
```
(string=?, string<?, string-append, string-length, string-ref s i,
 string->list, list->string, string->number, number->string,
 substring s start end)
```

### Other Primitives
```
(display val)         ; Print to stdout (returns #t)
(write val)           ; Print with quotes
(newline)             ; Print newline

(equal? a b)          ; Equality for any type
(boolean? x)          ; Boolean predicate
(symbol? x)           ; Symbol predicate
(string? x)           ; String predicate
(char? x)             ; Char predicate (integers)
(procedure? x)        ; Closure/primitive predicate

(read-line)           ; Read line from stdin
(read)                ; Read s-expression (returns error on EOF)
(file-exists? path)   ; Check file exists

(error "msg")         ; Create error value
(raise val)           ; Raise error (stops evaluation unless caught by try/catch)
(assert cond "msg")   ; Raise conditionally
```

### IMPORTANT: foldl Parameter Order
`(foldl f init lst)` calls `f` as `(f acc element)`:
- **First param** of f = accumulator (previous result)
- **Second param** of f = current element from list
```
;; Correct: (foldl add-to-alist '() words) where add-to-alist = (lambda (acc elem) ...)
(define (word-freq words)
  (define (add-to-alist alist word)  ;; alist=acc(1st), word=element(2nd)
    ...)
  (foldl add-to-alist '() words))
```

### Environment / Module
```
(import "path" "prefix:")   ; Load module with prefix
(require std/name)          ; Shorthand for std library
(use module-obj)            ; Load module value into env

(export sym1 sym2 ...)      ; Declare public exports in module
```

### Error Handling
```
(try (dangerous-call) (lambda (err) (display "caught")))
```

## CRITICAL: Functions That DO NOT EXIST — DO NOT USE
```
for-each          → use foldl or map instead
hash->list        → use foldl over hash-keys
letrec            → use let with inner define
displayln         → use (begin (display x) (newline))
println           → use (begin (display x) (newline))
sort              → use foldl sorting manually
even?             → use (= 0 (modulo n 2))
odd?              → use (= 1 (modulo n 2))
positive?         → use (> n 0)
negative?         → use (< n 0)
pipe/threading    → use nested expressions
doseq             → use foldl
apply             → use foldl
reduce            → use foldl
void?             → use (equal? x (void))
```

## STANDARD LIBRARIES
Use with `(require std/name)` (auto-prefixed as `name:`).
Use `(require std/name all:)` for bare name access.

```
std/list: """ + " ".join(std_exports.get("list", [])) + """
std/string: """ + " ".join(std_exports.get("string", [])) + """
std/math: """ + " ".join(std_exports.get("math", [])) + """
std/json: """ + " ".join(std_exports.get("json", [])) + """
std/struct: """ + " ".join(std_exports.get("struct", [])) + """
std/test: """ + " ".join(std_exports.get("test", [])) + """
std/validate: """ + " ".join(std_exports.get("validate", [])) + """
```

## COMMON MISTAKES — WRONG vs RIGHT

```
;; WRONG (Clojure syntax — always use s-expr)
(defn word-freq [lst] ...)
(fn [x] (* x x))

;; RIGHT (Scheme s-expr)
(define (word-freq lst) ...)
(lambda (x) (* x x))
```

```
;; WRONG (hash for alist operations)
(define freq (hash))
(for-each (lambda (w) (hash-set! freq w 1)) words)
(hash->list freq)

;; RIGHT (pure functional with foldl)
(define (add-to-alist alist word)
  (if (null? alist) (cons (cons word 1) '())
      (if (string=? word (car (car alist)))
          (cons (cons word (+ (cdr (car alist)) 1)) (cdr alist))
          (cons (car alist) (add-to-alist (cdr alist) word)))))
(foldl add-to-alist '() words)
```

```
;; WRONG (sort/hash don't exist this way)
(sort lst (lambda (a b) (> (cdr a) (cdr b))))

;; RIGHT (implement with foldl or custom select-sort)
(define (sort-by-freq alist)
  (define (find-max lst) ...)  ; walk list, return max entry
  (define (remove-entry e lst) ...)  ; remove e from lst
  (if (null? alist) '()
      (let ((m (find-max alist)))
        (cons m (sort-by-freq (remove-entry m alist))))))
```

## WORKSPACE + EDSL (Phase 2: precise fixes)
```
set-code "current source"    ; Lock code into workspace
query:find "name"            ; Find node IDs by name  
mutate:rebind "name" new     ; Replace entire definition
mutate:set-body "name" body  ; Replace function body only
typecheck-current            ; Validate types
eval-current                 ; Execute workspace
```

## RULES
- Code in ``` blocks only. No explanatory text in code blocks.
- NO Clojure/Racket syntax (`defn`, `fn`, `[brackets]`, `#()`).
- NO functions from the DO NOT USE list.
- When folding: `(foldl f init lst)` → f = (lambda (acc elem) ...).
- First attempt should compile and run. Keep it simple.
- Say DONE when correct.

## EXAMPLES
```lisp
;; Sum 1 to 100
(require std/list)
(foldl + 0 (range 1 101))
```

```lisp
;; Simple map example
(define (square x) (* x x))
(map square '(1 2 3 4 5))
```

```lisp
;; Filter + map
(require std/list)
(define (even-squares lst)
  (map (lambda (x) (* x x))
       (filter (lambda (x) (= 0 (modulo x 2))) lst)))
(even-squares '(1 2 3 4 5 6))
```
""" + (f"""

## STDLIB EXPORTS DETAIL
""" + "".join(f"""
### std/{name}
```lisp
;; Exports: {' '.join(str(exports)) if exports else 'none'}
```
""" for name, exports in std_exports.items()) if std_exports else "")

if __name__ == "__main__":
    prompt = build_system_prompt()
    print(f"System prompt: {len(prompt)} chars")
    # Show stdlib stats
    exports = load_stdlib_exports()
    for name, funcs in exports.items():
        print(f"  std/{name}: {len(funcs)} exports")