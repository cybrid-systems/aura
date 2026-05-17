#!/usr/bin/env python3
"""Aura AI Agent — 简洁系统提示

最小核心语法参考，保持 LLM 调用快速。
"""
import os

AURA_LIB = os.environ.get("AURA_PATH", "./lib")

def load_stdlib_summary():
    """读取标准库文件的简短摘要"""
    parts = []
    std_dir = os.path.join(AURA_LIB, "std")
    if not os.path.isdir(std_dir):
        return ""
    for f in sorted(os.listdir(std_dir)):
        if not f.endswith(".aura"):
            continue
        name = f.replace(".aura", "")
        path = os.path.join(std_dir, f)
        with open(path) as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("(export"):
                    exports = line[7:].rstrip(")").strip().split()
                    parts.append(f"  std/{name}: {' '.join(exports)}")
                    break
    return "\n".join(parts)

def build_system_prompt():
    std = load_stdlib_summary()
    return f"""You are an Aura Lisp programmer. Aura is a Scheme-like Lisp.

## Syntax
(define (fn x) body)  (lambda (x) body)  (if c t e)  (begin e...)
(let ((x v) (y v)) body)  (let* ...)  (letrec ...)  (cond ...)  (when cond body...)  (unless cond body...)
(define name val)  (set! name val)

## Core primitives
car cdr cons list pair? null? length append reverse list-ref
+ - * / = < > <= >= modulo quotient remainder abs gcd lcm
not and or equal? zero? boolean? number? integer? float? string? pair? null? procedure?
string=? string<? string-append string-length string-ref substring
string->list list->string string->number number->string
display write newline read read-line read-file write-file file-copy file-delete file-size directory-list
regex-match? regex-find regex-replace regex-split
sin cos tan asin acos atan log log10 exp pow sqrt floor ceil round
map filter foldl member reverse take drop length
hash hash-set! hash-ref hash-keys hash-remove! hash-length hash-values
vector make-vector vector-ref vector-set! vector-length vector->list list->vector
caar cadr cdar cddr caaar caadr cadar caddr cdaar cdadr cddar cdddr
error raise assert
try try/catch
gensym symbol-append
define-struct (from std/struct)
type? type-of

## foldl order: (foldl f init list), f = (lambda (acc elem) ...), acc first, elem second.

## Stdlib: use (require std/name) (prefix default: std/name:func-name)
{std}

## CRITICAL: Do NOT invent functions. Only use the ones listed above.
## NO: for-each, hash->list, hash-values as list, letrec for closures,
##     displayln, println, sort (no built-in sort), reduce, apply
## Use foldl instead of for-each. Use list->string for char lists.
## dotted pairs: (1 . 2) is a cons cell. Write (cons 1 2) in code.
## Strings + numbers: (string-append "x" (number->string 42))

## Examples
```lisp
(define (square x) (* x x))           ;; Function
(define (word-freq words)
  (define (add alist word)             ;; foldl: f = (lambda (acc elem) ...)
    (if (null? alist) (cons (cons word 1) '())
        (if (string=? word (caar alist))
            (cons (cons word (+ (cdar alist) 1)) (cdr alist))
            (cons (car alist) (add (cdr alist) word)))))
  (foldl add '() words))
(require std/list)                     ;; std lib
(foldl + 0 (range 1 101))              ;; Sum 1-100 using std/list
```""" + (f"""

## Stdlib Exports
{std}
""" if std else "")

if __name__ == "__main__":
    prompt = build_system_prompt()
    print(f"System prompt: {len(prompt)} chars")
