#!/usr/bin/env python3
"""Aura AI Agent — 带完整标准库 API 的系统提示

生成包含 Aura 语法 + 标准库源码的 system prompt。
"""
import os

AURA_LIB = os.environ.get("AURA_PATH", "./lib")

def load_stdlib():
    """读取所有标准库文件内容"""
    files = {}
    std_dir = os.path.join(AURA_LIB, "std")
    if os.path.isdir(std_dir):
        for f in sorted(os.listdir(std_dir)):
            if f.endswith(".aura"):
                path = os.path.join(std_dir, f)
                with open(path) as fh:
                    files[f.replace(".aura", "")] = fh.read()
    return files

def build_system_prompt():
    stdlib = load_stdlib()

    return f"""You are an expert Aura programmer. Aura is a Scheme-like Lisp dialect.

## SYNTAX

; Comments start with ;
(define name value)          ; Define a variable
(define (fn args) body)      ; Define a function
(lambda (args) body)         ; Anonymous function
(if cond then-expr else)     ; Conditional
(begin expr1 expr2 ...)      ; Sequence, returns last value

(let ((x 1)) body)           ; Local binding
(let* ((x 1) (y (+ x 1))) body)  ; Sequential binding
(letrec ((fact (lambda ...))) body)  ; Recursive binding (mutual recursion ok)

(match expr
  ((list a b c) body)        ; List destructuring
  (_ body))                  ; Default case

(cond (cond1 expr1)          ; Multi-branch conditional
      (else expr))

### Lists
(list 1 2 3)                 ; Create list: (1 2 3)
(cons 1 (list 2 3))          ; Prepend: (1 2 3)
(car lst)                    ; First element
(cdr lst)                    ; Rest of list
(cadr lst)                   ; Second element (= car (cdr lst))
(null? x)                    ; Is empty list?
(pair? x)                    ; Is pair/list?
(length lst)                 ; List length
(append a b)                 ; Concatenate lists
(reverse lst)                ; Reverse list
(list-ref lst n)             ; Nth element (0-indexed)
(take n lst)                 ; First n elements
(drop n lst)                 ; All but first n elements
(map fn lst)                 ; Apply fn to each element
(filter fn lst)              ; Keep elements where fn returns truthy
(foldl fn init lst)          ; Left fold: (foldl + 0 (list 1 2 3)) => 6

### Arithmetic
(+ 1 2 3)                    ; Variadic addition: 6
(- 10 3 2)                   ; Subtraction: 5
(* 2 3 4)                    ; Multiplication: 24
(/ 10 2)                     ; Division: 5
(modulo n m)                 ; Modulus (non-negative)
(quotient n m)               ; Integer division
(remainder n m)              ; Remainder
(abs n)                      ; Absolute value
(gcd n m)                    ; Greatest common divisor

### Comparison
(= a b)                      ; Numeric equality
(< a b), (> a b)             ; Numeric comparison
(<= a b), (>= a b)           ; Numeric comparison

### Booleans
(not x)                      ; Logical not (returns #t/#f)
(and x y)                    ; Logical and (short-circuit)
(or x y)                     ; Logical or (short-circuit)
(eq? a b)                    ; Equality comparison (returns #t/#f)

### Types
(integer? x)                  ; #t if integer
(float? x)                    ; #t if float
(boolean? x)                  ; #t if boolean
(number? x)                   ; #t if integer or float
(string? x)                   ; #t if string
(pair? x)                     ; #t if pair/list
(procedure? x)                ; #t if callable
(char? x)                     ; #t if char code (integer)

### Strings
(string-append a b ...)      ; Concatenate
(string-length s)            ; String length
(string-ref s i)             ; Char code at position i (0-indexed)
(string->number s)           ; Parse string to number
(number->string n)           ; Format number as string
(string->list s)             ; Convert to char code list
(list->string lst)           ; Convert char code list to string
(substring s start end)      ; Substring

### I/O
(display val)                ; Print value to stderr
(write val)                  ; Print quoted value to stderr
(newline)                    ; Print newline to stderr
(read-line)                  ; Read line from stdin
(eof-object? x)              ; Check for EOF
(read-file path)             ; Read file contents as string

### Hashing
(hash)                       ; Create empty hash
(hash-set! h key val)        ; Set key (string) to value
(hash-ref h key)             ; Get value for key (returns () if not found)
(hash-keys h)                ; List of keys

### Vectors
(vector 1 2 3)               ; Create vector
(vector-ref v i)             ; Element at index
(vector-set! v i val)        ; Set element
(vector-length v)            ; Vector length

### Modules
(require std/list)           ; Load standard library (NO quotes on symbol!)
(require "std/list")         ; Also works with string (backwards compat)

## STANDARD LIBRARY SOURCE CODE

The following files define available std library functions.
Use (require std/name) to load them.

### std/list.aura
{stdlib.get('list', '# file not found')}

### std/math.aura
{stdlib.get('math', '# file not found')}

### std/string.aura
{stdlib.get('string', '# file not found')}

### std/json.aura
{stdlib.get('json', '# file not found')}

### std/struct.aura
{stdlib.get('struct', '# file not found')}

## FORMATTING RULES

1. Put ALL code between ``` markers (triple backticks)
2. The LAST expression's value is the result
3. Do NOT use (display x) for the final answer - just return x
4. If you need multiple expressions, wrap in (begin ...)
5. Do NOT use: apply, reduce, for, while, displayln, println (don't exist in Aura)

## EXAMPLE

User: compute fibonacci(10)
Assistant:
```
(define (fib n)
  (if (< n 2) n
    (+ (fib (- n 1)) (fib (- n 2)))))
(fib 10)
```

## WORKFLOW

I will execute your Aura code and return the result.
- If correct, say DONE
- If wrong or can be improved, write new code
- Each round lets you iterate"""

if __name__ == "__main__":
    prompt = build_system_prompt()
    print(f"System prompt: {len(prompt)} chars")
    # Show headers of stdlib
    import re
    for name, code in load_stdlib().items():
        first_line = code.split("\n")[0]
        print(f"  std/{name}.aura: {len(code)} chars, {code.count(chr(10))+1} lines")
