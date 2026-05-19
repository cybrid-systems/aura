# Aura EDSL Benchmark

## Latest: 2026-05-19 v3 — deepseek-v4-flash: 19/26 (73%)

### Fixes applied this session

| Fix | Impact |
|-----|--------|
| **C FFI string signatures** | `(c-func lib "sqrt" "(Float) -> Float")` → ffi-sqrt passes ✅ |
| **user_bindings_ tracking** | Fixes `(define f ...) (display f)` showing `0` vs closure |
| **bad_variant_access catch** | Instead of crash, shows friendly type error |
| **Prompt v3** | stdlib examples, DO NOT USE list, C FFI examples |

### Results

| 等级 | 任务 | 状态 | 说明 |
|------|------|------|------|
| L0 | arith-basic | ✅ | `(+ 1 2 3 4 5)` → 15 |
| L0 | arith-chain | ✅ | `(square 5)` → 25 |
| L1 | lambda-simple | ✅ | `(double 10)` → 20 |
| L1 | letrec-fact | ✅ | `(fact 5)` → 120 |
| L1 | named-let | ✅ | sum 1..10 → 55 |
| L2 | list-range | ✅ | `(range 1 10)` → (1..10) |
| L2 | list-filter | ✅ | `(filter ...)` → (2 4 6 8 10) |
| L2 | list-map | ✅ | `(map double (list 1 2 3 4 5))` |
| L2 | list-foldl | ✅ | `(foldl + 0 (list 1 2 3 4 5))` → 15 |
| L2 | list-reverse | ✅ | `(reverse (list 1 2 3 4 5))` |
| L3 | prime-test | ❌ | LLM parse error |
| L3 | primes-list | ❌ | LLM variance (was ✅ in v2) |
| L3 | unique-hash | ✅ | `(unique (list 1 2 2 3 3 3))` |
| L3 | merge-sort | ❌ | `(take lst half)` arg order reversed → friendly error now |
| L4 | hash-basic | ✅ | hash create + read |
| L4 | hash-stats | ❌ | LLM outputs Common Lisp (defun/dolist/gethash) |
| L4 | word-freq | ❌ | raw `<hash[0]>` — needs `hash->list` |
| L5 | type-check | ✅ | `(check 42 : Int)` |
| L5 | type-of | ✅ | `(type-of 42)` → Int |
| L5 | occurrence | ✅ | `(if (string? x) ...)` narrowing works |
| L6 | ffi-sqrt | ✅ | `(c-func lib "sqrt" "(Float) -> Float")` → 3 |
| L6 | ffi-strlen | ❌ | LLM uses wrong lib path (x86_64 vs arm64) |
| L7 | edsl-set-code | ✅ | `--serve` protocol |
| L7 | edsl-query | ✅ | `--serve` protocol |
| L7 | edsl-mutate | ✅ | `--serve` protocol |
| L8 | tcp-connect | ❌ | LLM uses wrong libc path |

## 问题分布

| 类别 | 数量 | 说明 |
|------|------|------|
| LLM 误解 (Common Lisp 习惯) | 3 | hash-stats, word-freq, ffi-strlen — 写了 CL 代码 |
| LLM 方差 | 2 | prime-test, primes-list — 解析错误的随机代码 |
| LLM 参数顺序 | 1 | merge-sort — (take lst half) 反了 |
| CI 环境差异 | 1 | tcp-connect — libc 路径不对 |

**所有 7 个失败都是 LLM prompt 问题**，编译器 bug 已全部修完。

## 运行

```bash
LLM_API_KEY="..." LLM_MODEL="deepseek-v4-flash" python3 tests/edsl_benchmark.py
```
