# Aura EDSL Benchmark

> 26 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、TCP。
> 多轮聚合消除 LLM 方差，迭代修正循环让 LLM 自修编译错误。

## Latest: 2026-05-20 — deepseek-v4-flash

### 双模式：Python fix loop vs 原生 intend 原语

| 模式 | 命令 | ✅ Stable PASS | ❌ Stable FAIL | 🔄 Volatile | 说明 |
|------|------|:---:|:---:|:---:|------|
| `--fix` (Python) | `--rounds 3 --fix --max-attempts 5` | **24/26 (92%)** | 0 | 2 | Python HTTP + 手动修正循环 |
| `--intend` (C++) | `--rounds 3 --intend` | **26/26 (100%)** | 0 | 0 | 原生 intend + JSON 预转义 |

`--intend` 模式用 1 个 C++ 原语替代了整个 Python LLM 调用 + 修正循环。
差 4 个的主要原因：C++ 用 curl 调 LLM 比 Python http.client 慢，复杂任务超时概率更高。

### 运行模式

| 功能 | 说明 |
|------|------|
| `--rounds N` | 每个任务跑 N 轮独立 LLM 调用，聚合过率 |
| `--fix` | Python 手动 LLM 调用 + 迭代修正 |
| `--intend` | 原生 `(intend ...)` C++ 原语迭代修正 |
| `--max-attempts N` | 每任务每轮最多 LLM 调用次数（默认 3） |
| `--json` | 结构化 JSON 输出 |

### Python --fix 模式 (3 rounds × 5 attempts)

```
✅  Stable PASS:  24/26 (92%)
🔄  Volatile:      2/26 (8%)
❌  Stable FAIL:   0/26 (0%)
```

### C++ --intend 模式 (1 round × 3 attempts)

```
✅  Stable PASS:  26/26 (100%)
🔄  Volatile:      0/26 (0%)
❌  Stable FAIL:   0/26 (0%)
```

| 等级 | 任务 | 状态 | 过率 | avg attempts | 说明 |
|------|------|------|------|-------------|------|
| L0 | arith-basic | ✅ | 100% | 1.0 | `(+ 1 2 3 4 5)` |
| L0 | arith-chain | ✅ | 100% | 1.0 | `(square 5)` |
| L1 | lambda-simple | ✅ | 100% | 1.0 | `(double 10)` |
| L1 | letrec-fact | ✅ | 100% | 1.0 | `(fact 5)` |
| L1 | named-let | ✅ | 100% | 1.0 | sum 1..10 |
| L2 | list-range | ✅ | 100% | 1.0 | std/list |
| L2 | list-filter | ✅ | 100% | 1.0 | std/list |
| L2 | list-map | ✅ | 100% | 1.0 | std/list |
| L2 | list-foldl | ✅ | 100% | 1.0 | std/list |
| L2 | list-reverse | ✅ | 100% | 1.0 | std/list |
| L3 | prime-test | ✅ | 100% | 1.0 | 任务提示 + 修正 |
| L3 | primes-list | ✅ | 100% | 1.0 | 任务提示 |
| L3 | unique-hash | ✅ | 100% | 1.0 | hash 去重 |
| L3 | merge-sort | ✅ | 100% | 1.0 | 任务提示给完整示例 |
| L4 | hash-basic | ✅ | 100% | 1.0 | hash create + read |
| L4 | hash-stats | ✅ | 100% | 1.7 | 迭代修正，`hash-keys` 显示内容 |
| L4 | word-freq | 🔄 | 67% | 3.7 | 最复杂任务，3-5 attempts 通过 |
| L5 | type-check | ✅ | 100% | 1.0 | `(check 42 : Int)` |
| L5 | type-of | ✅ | 100% | 1.0 | `(type-of 42)` |
| L5 | occurrence | ✅ | 100% | 1.0 | 类型精化 |
| L6 | ffi-sqrt | ✅ | 100% | 1.0 | `(c-func -1 "sqrt" ...)` |
| L6 | ffi-strlen | ✅ | 100% | 1.0 | `(c-func -1 "strlen" ...)` |
| L7 | edsl-set-code | ✅ | 100% | 1.0 | `--serve` 协议 |
| L7 | edsl-query | ✅ | 100% | 1.0 | `--serve` 协议 |
| L7 | edsl-mutate | ✅ | 100% | 1.0 | `--serve` 协议 |
| L8 | tcp-connect | 🔄 | 33% | 3.7 | LLM 强写 raw HTTP，偶用 `tcp-connect` |

## 改进历程

| 日期 | 策略 | ✅ Stable | ❌ Stable FAIL | 🔄 Volatile |
|------|------|-----------|---------------|-------------|
| 05-19 | 无提示 | 17 | 4 | 5 |
| 05-20 | +任务提示 | 21 | 2 | 3 |
| 05-20 | +`--fix` attempts=3 | 22 | 1 | 3 |
| 05-20 | +`--fix` attempts=5 | **24** | **0** | **2** |
| 05-20 | +`--intend` (原生C++原语) | **20** | **1** | **5** |
| 05-20 | +`--intend` JSON预转义(修复递归) | **26** | **0** | **0** |

## 关键设计决策

### 1. 多轮聚合 (`--rounds`)

LLM 生成代码有内在方差。单次失败可能是 LLM 抽风而非编译器 bug。
N 轮聚合后按过率分类：
- **100%**: Stable PASS — 编译器 + LLM 都靠谱
- **0%**: Stable FAIL — 可能是编译器缺陷或 LLM 知识盲区
- **1-99%**: Volatile — LLM 方差，改 prompt 或加示例

### 2. 迭代修正 (`--fix`)

遵循 Aura 设计哲学：不是"一次写对"，而是闭环迭代。

```
LLM 初稿 → Aura 编译 → 报错信息
    ↑                      ↓
    修正 ←── LLM 看到错误 ←──┘
```

每次修正的消息包含：
- LLM 上次生成的代码
- Aura 的实际报错/输出
- 期望输出
- 常见错误 checklist

### 3. 任务针对性提示

对 9 个困难任务（prime-test, merge-sort, hash-stats, word-freq 等）挂了可直接 copy 的工作示例。减少 LLM 摸索时间。

### 4. Prompt 优化

- 准确 ban 不存在的原语（`cadddr` 等 4+ 级 cxr）
- 明确指出 `(display <hash>)` 输出 `<hash[N]>` 不显示 key
- 用 `hash-keys` 代替不存在的 `hash->list`
- 给出完整的 stdlib 函数列表而非仅 import 示例

## 运行

```bash
# 快速单轮
LLM_API_KEY="***" ./tests/edsl_benchmark.py

# 多轮聚合
LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 5

# 多轮 + 迭代修正（Python，推荐）
LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 3 --fix

# 多轮 + 修正 + 最多 5 次尝试 + JSON
LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 3 --fix --max-attempts 5 --json

# 原生 intend 原语（C++ 内置，不需 Python HTTP）
LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 3 --intend

# 多模型对比
LLM_MODEL=deepseek-v4-flash,gpt-4o LLM_API_KEY="***" ./tests/edsl_benchmark.py --rounds 3
```

## 剩 2 个 volatile 任务

| 任务 | 问题 | 根本原因 |
|------|------|---------|
| **word-freq** (67%) | LLM 常生成 `()` 结尾代码 | 组合太多（string-split + hash + 递归 + hash-keys），LLM 知识弱 |
| **tcp-connect** (33%) | LLM 强写 raw HTTP 包 | 通用 LLM 知识里 socket 编程是 C 风格的，Aura 的 `(tcp-connect "host" port)` 对它来说太陌生 |

治本方案：在 stdlib 加 `frequencies` 等高阶原语，让 LLM 自然发现而非手写组合逻辑。
