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
    # Compact summary of each stdlib file's exports
    lib_summaries = {}
    for name, code in stdlib.items():
        funcs = [l.split(")")[0].split("(")[-1].split()[0] if "(define" in l else ""
                 for l in code.split("\n") if "(define" in l]
        funcs = [f for f in funcs if f]
        lib_summaries[name] = funcs

    return f"""Aura = Scheme-like Lisp. Keep code short.

BASIC:
(define (fn x) body), (lambda (x) body), (if c t e), (begin e...), (let ((x v)) body)
(+ 1 2 3), (- x y), (* x y), (/ x y), (= x y), (< x y), (not x)
(list 1 2 3), (cons 1 2), (car x), (cdr x), (map fn lst), (filter fn lst)
(foldl fn init lst), (null? x), (pair? x), (length lst), (append a b)
(modulo n m), (abs n), (gcd n m), (string-append a b), (string-length s)
(string-ref s i), (string->number s), (number->string n)
(string->list s), (list->string lst), (substring s start end)
(display val), (write val), (newline)
(hash), (hash-set! h k v), (hash-ref h k), (hash-keys h)

STDLIB (use: require std/name):
  std/list: {', '.join(lib_summaries.get('list',[]))}
  std/math: {', '.join(lib_summaries.get('math',[]))}
  std/string: {', '.join(lib_summaries.get('string',[]))}
  std/json: {', '.join(lib_summaries.get('json',[]))}
  std/struct: define-struct, make-<name>, <name>?, <name>-<field>

EDSL (增量变换, 用于精确修改运行中的程序):
  set-code / eval-current — 工作区生命周期
  query:find / children / node / calls / parent / siblings / pattern / node-type
  mutate:rebind / set-body / remove-node / insert-child / replace-value / replace-type
  typecheck-current — 增量类型检查
  详见 ai-agent-edsl 文档

EXAMPLE:
Compute sum to 100:  ```\n(require std/list)\n(foldl + 0 (range 1 101))\n```
Factorial:  ```\n(require std/math)\n(factorial 10)\n```

RULES:
- Code in ``` blocks only. Last expression = result.
- NO display/write for final value, NO displayln/println/apply/reduce
- Say DONE when correct."""

if __name__ == "__main__":
    prompt = build_system_prompt()
    print(f"System prompt: {len(prompt)} chars")
    # Show headers of stdlib
    import re
    for name, code in load_stdlib().items():
        first_line = code.split("\n")[0]
        print(f"  std/{name}.aura: {len(code)} chars, {code.count(chr(10))+1} lines")
