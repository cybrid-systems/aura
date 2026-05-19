# Aura EDSL Benchmark

## Latest: 2026-05-19 — deepseek-v4-flash: 17/26 (65%)

| 等级 | 任务 | 状态 | 说明 |
|------|------|------|------|
| L0 | arith-basic | ✅ | `(+ 1 2 3 4 5)` → 15 |
| L0 | arith-chain | ✅ | `(square 5)` → 25 |
| L1 | lambda-simple | ✅ | `(double 10)` → 20 |
| L1 | letrec-fact | ✅ | `(fact 5)` → 120 |
| L1 | named-let | ✅ | sum 1..10 → 55 |
| L2 | list-range | ✅ | `(range 1 10)` → (1..10) |
| L2 | list-filter | ❌ | LLM output mismatch |
| L2 | list-map | ✅ | `(map double (list 1 2 3 4 5))` |
| L2 | list-foldl | ✅ | `(foldl + 0 (list 1 2 3 4 5))` → 15 |
| L2 | list-reverse | ✅ | `(reverse (list 1 2 3 4 5))` |
| L3 | prime-test | ❌ | LLM variable scoping error |
| L3 | primes-list | ✅ | `(primes 30)` → (2 3 5 7 11 13 17 19) |
| L3 | unique-hash | ✅ | `(unique (list 1 2 2 3 3 3))` |
| L3 | merge-sort | ❌ | `bad_variant_access` (LLM code quality) |
| L4 | hash-basic | ✅ | hash create + read |
| L4 | hash-stats | ❌ | LLM returns hash not value |
| L4 | word-freq | ❌ | string-split not required |
| L5 | type-check | ✅ | `(check 42 : Int)` |
| L5 | type-of | ✅ | `(type-of 42)` → Int |
| L5 | occurrence | ❌ | branch type narrowing |
| L6 | ffi-sqrt | ❌ | libm loading issue |
| L6 | ffi-strlen | ❌ | type error calling strlen |
| L7 | edsl-set-code | ✅ | `--serve` protocol |
| L7 | edsl-query | ✅ | `--serve` protocol |
| L7 | edsl-mutate | ✅ | `--serve` protocol |
| L8 | tcp-connect | ❌ | TCP connection timeout |

## 问题分布

| 类别 | 数量 | 说明 |
|------|------|------|
| 编译器 bug | 1 | `bad_variant_access` in merge-sort (LLM code, likely) |
| 标准库缺口 | 1 | `string-split` not auto-available (need require) |
| LLM 误解 | 7 | 未调用函数/调用方式错误/prompt 不明确 |

## 运行

```bash
# 需设置 LLM_API_KEY
LLM_API_KEY="..." LLM_MODEL="deepseek-v4-flash" python3 tests/edsl_benchmark.py
```
