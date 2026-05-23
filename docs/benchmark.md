# Aura EDSL Benchmark

> 85 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、TCP、递归算法、LeetCode 风格、ADT、线性所有权。
> 自适应用迭代修正 + 执行轨迹反馈（PID 控制理论）+ 蚁群局部搜索。

## Latest: 2026-05-23 — 3 模型对比 (max-attempts=3, 1 round)

**架构变更：** 测试任务从 57 扩展到 85，新增 28 个类型系统/ADT/线性所有权任务。
**任务重组：** `test-*.aura` → `suite/*.aura`， `edsl_tasks/` → `tasks/<category>/`。
**新增类型系统任务：** type-annot-chain, type-boundary-call, type-coercion-if, type-gradual-boundary, type-linear 等 15 个类型系统任务。
**新增 M4 线性所有权任务：** type-linear.aura。

| 模型 | 任务数 | 通过率 | 总耗时 | 失败任务 |
|------|:-----:|:-----:|:-----:|:--------|
| 🥇 **Grok 4.3** | 85 | **78/85 (91.8%)** | ~13min | binary-search, merge-sort, memoize, ffi-sqrt, type-annot-fn, type-blame-runtime, valid-parens |
| 🥇 **DeepSeek v4 Flash** | 85 | **77/85 (90.6%)** | ~46min | adt-option, binary-search, merge-sort, edsl-set-code, ffi-sqrt, ffi-strlen, type-annot-fn, type-blame-runtime |
| 🥈 **MiniMax M2.7** | 85 | **76/85 (89.4%)** | ~23min | adt-option, binary-search, merge-sort, ffi-sqrt, ffi-strlen, json-roundtrip, tcp-connect, type-boundary-call, type-gradual-boundary |

**环比提升：** Grok 57→78 (+21)，DeepSeek 54→77 (+23)，MiniMax 53→76 (+23)。
**本次提升（2026-05-23 PM）：** 编译器修复（parser、blame、eval_flat、TypeAnnotation、match 穷尽性）生效。
Grok +1, MiniMax +2, DeepSeek +5 —— 对弱模型收益最大。
**共享失败：** `binary-search`、`merge-sort` 仍三模型均未通过（`#<procedure>` 问题，已加入 task hint）。
**类型系统任务** 从平均 ~60% 提升至 ~80%。

### 逐任务对比（85 任务）

#### basic

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| arith-basic | ✅ | ✅ | ✅ |
| arith-chain | ✅ | ✅ | ✅ |
| lambda-simple | ✅ | ✅ | ✅ |
| vector-ops | ✅ | ✅ | ✅ |
| climbing-stairs | ✅ | ✅ | ✅ |
| combinations | ✅ | ✅ | ✅ |
| named-let | ✅ | ✅ | ✅ |
| letrec-fact | ✅ | ✅ | ✅ |
| fibonacci | ✅ | ✅ | ✅ |

#### list

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| list-map | ✅ | ✅ | ✅ |
| list-filter | ✅ | ✅ | ✅ |
| list-foldl | ✅ | ✅ | ✅ |
| list-range | ✅ | ✅ | ✅ |
| list-reverse | ✅ | ✅ | ✅ |
| list-zip | ✅ | ✅ | ✅ |
| list-partition | ✅ | ✅ | ✅ |
| list-flatten | ✅ | ✅ | ✅ |

#### recursion

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| gcd-euclid | ✅ | ✅ | ✅ |
| prime-test | ✅ | ✅ | ✅ |
| primes-list | ✅ | ✅ | ✅ |
| quicksort | ✅ | ✅ | ✅ |
| reverse-list | ✅ | ✅ | ✅ |
| sieve | ✅ | ✅ | ✅ |
| tree-dfs | ✅ | ✅ | ✅ |
| two-sum | ✅ | ✅ | ✅ |

#### algorithm

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| binary-search | ❌ | ❌ | ❌ |
| merge-sort | ❌ | ❌ | ❌ |
| merge-sorted | ✅ | ✅ | ✅ |
| deep-equal | ✅ | ✅ | ✅ |
| majority-element | ✅ | ✅ | ✅ |
| max-subarray | ✅ | ✅ | ✅ |
| palindrome | ✅ | ✅ | ✅ |
| table-lookup | ✅ | ✅ | ✅ |
| valid-parens | ❌ | ✅ | ❌ |
| compose-n | ✅ | ✅ | ✅ |

#### hash

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| hash-basic | ✅ | ✅ | ✅ |
| hash-invert | ✅ | ✅ | ✅ |
| hash-stats | ✅ | ✅ | ✅ |
| unique-hash | ✅ | ✅ | ✅ |
| word-freq | ✅ | ✅ | ✅ |

#### string

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| string-reverse | ✅ | ✅ | ✅ |
| string-split-join | ✅ | ✅ | ✅ |
| is-anagram | ✅ | ✅ | ✅ |
| contains-duplicate | ✅ | ✅ | ✅ |
| first-unique | ✅ | ✅ | ✅ |

#### adt

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| adt-either | ✅ | ✅ | ✅ |
| adt-multi-ctor | ✅ | ✅ | ✅ |
| adt-option | ❌ | ❌ | ✅ |
| adt-tree | ✅ | ✅ | ✅ |
| adt-wildcard | ✅ | ✅ | ✅ |

#### type

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| type-of | ✅ | ✅ | ✅ |
| type-check | ✅ | ✅ | ✅ |
| type-occurrence | ✅ | ✅ | ✅ |
| type-occurrence-float | ✅ | ✅ | ✅ |
| type-pair-occurrence | ✅ | ✅ | ✅ |
| type-value-restriction | ✅ | ✅ | ✅ |
| type-annot-chain | ✅ | ✅ | ✅ |
| type-annot-fn | ❌ | ✅ | ❌ |
| type-blame-runtime | ❌ | ✅ | ❌ |
| type-gradual-boundary | ✅ | ❌ | ✅ |
| type-gradual-erasure | ✅ | ✅ | ✅ |
| type-linear | ✅ | ✅ | ✅ |
| type-multi-annot | ✅ | ✅ | ✅ |
| type-ownership-linear | ✅ | ✅ | ✅ |
| type-coercion-if | ✅ | ✅ | ✅ |

#### coercion

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| coerce-bool-int | ✅ | ✅ | ✅ |
| coerce-float-int | ✅ | ✅ | ✅ |
| coerce-int-string | ✅ | ✅ | ✅ |

#### edsl

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| edsl-mutate | ✅ | ✅ | ✅ |
| edsl-query | ✅ | ✅ | ✅ |
| edsl-set-code | ❌ | ✅ | ✅ |

#### json + ffi

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| json-roundtrip | ✅ | ❌ | ✅ |
| ffi-sqrt | ❌ | ❌ | ❌ |
| ffi-strlen | ❌ | ❌ | ✅ |

#### other

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| linear-basic | ✅ | ✅ | ✅ |
| macro-definer | ✅ | ✅ | ✅ |
| memoize | ✅ | ✅ | ❌ |
| occurrence | ✅ | ✅ | ✅ |
| tcp-connect | ✅ | ❌ | ✅ |
| type-annot-expr | ✅ | ✅ | ✅ |
| type-annot-int | ✅ | ✅ | ✅ |
| type-boundary-call | ✅ | ❌ | ✅ |
| type-consistency | ✅ | ✅ | ✅ |
| type-higher-order | ✅ | ✅ | ✅ |
| type-let-poly | ✅ | ✅ | ✅ |

### 架构

```
run_single_task()
  │
  ├── coarse: LLM 输出完整代码 → serve.exec(full_code)
  │            （距离远：编译错误或完全不匹配）
  │
  ├── fine:   LLM 输出完整代码或 EDSL 突变
  │            （距离中：部分匹配，missing keywords）
  │
  └── putt:   LLM 输出微调代码
               （距离近：>= 85% 匹配）
                
build_adaptive_feedback()
  ├── measure-distance(rc, output, expected) → (phase, ratio, diag)
  ├── structured-diagnosis → missing keywords + hash 警告
  ├── get-execution-trace → 算法任务中间结果
  └── call-api-ref → std/hash, list 等模块参考注入

serve client (CaaS)
  ├── 每个任务独立 ./aura --serve 进程
  ├── 非阻塞 IO (fcntl + os.read) 读超时
  ├── EDSL 检测: 输出以 (set-code 开头 → 自动追加 (eval-current)
  ├── tcp-connect 非阻塞 connect() + 8s poll() 超时
  └── 崩溃/死锁恢复: 检测 → kill → 重启
```

### 任务列表

85 个任务，来自 13 个分类子目录 `tests/tasks/<category>/`：

| 分类 | 子目录 | 说明 |
|------|--------|------|
| **类型系统** | `type/` | type-annot, type-blame, type-coercion, type-gradual, type-linear 等 |
| **ADT** | `adt/` | define-type, match, variant 类型推断 |
| **基础** | `basic/` | arith, lambda, vector |
| **列表/集合** | `list/` | filter, map, foldl, range, sort 等 |
| **哈希** | `hash/` | hash-basic, word-freq, hash-invert 等 |
| **哈希算法** | `hash-algo/` | 哈希算法任务 |
| **递归算法** | `recursion/` | fib, gcd, quicksort, sieve 等 |
| **算法** | `algorithm/` | merge-sort, binary-search, deep-equal 等 |
| **字符串** | `string/` | string-reverse, string-split-join 等 |
| **C FFI** | `ffi/` | sqrt, strlen, 外部函数调用 |
| **EDSL** | `edsl/` | query, mutate, set-code |
| **JSON** | `json/` | json-roundtrip 等 |

### 运行模式

```
  --rounds N       每个任务跑 N 轮独立 LLM 调用 (默认 1)
  --fix            Python 手动 LLM 调用 + 迭代修正
  --intend         原生 (intend ...) C++ 原语迭代修正
  --max-attempts N 每任务每轮最多 LLM 调用次数 (默认 3)
  --json           结构化 JSON 输出
  --trace          输出失败任务的详细诊断
  --tasks X,Y,Z    只运行指定的任务 (逗号分隔)
  --failed         只运行已知失败的任务
```

### 运行

```bash
# 全量 (单模型)
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --json

# 指定模型
LLM_MODEL=minimax-m2.7 LLM_API_KEY="***" python3 tests/edsl_benchmark.py --json

# 指定任务
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --tasks is-anagram,hash-stats

# 多模型自动对比 (run_bench_all.py)
python3 tests/run_bench_all.py

# 手动多模型
LLM_MODEL=deepseek-v4-flash,minimax-m2.7 LLM_API_KEY="***" python3 tests/edsl_benchmark.py --json

# 单轮 + 迭代修正 (推荐)
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --fix --max-attempts 5

# 多轮聚合
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --rounds 3

# 多模型 + 多轮
LLM_MODEL=deepseek-v4-flash,gpt-4o LLM_API_KEY="***" python3 tests/edsl_benchmark.py --rounds 3
```

### 修复详情

- **extract_code 正则**: `<[^>]+>` 吞噬了 Aura 比较操作符（`<`, `>`）→ `</?\w[^>]*>`
- **serve 非阻塞 IO**: fcntl + os.read 超时，无线程竞态
- **tcp-connect 超时**: 非阻塞 connect() + poll() 8s 超时
- **EDSL 单次 exec**: `(set-code ...) + (mutate:rebind ...) + (eval-current)` 一次调用
- **parser 报错增强**: 包含 `expected expression, got ')'` 等详细信息
- **编译器报错修复**: `<closure[N]>` → `#<procedure>`；`with_suggestion` 自我赋值 bug
- **Scheme 兼容层移除**: 着力即差 — 删除 serve 注册 + 字符串替换，模型被迫写纯正 Aura

## 测试套件

| 套件 | 数量 | 说明 |
|------|:----:|------|
| Integ (build.py) | 118 | 综合集成测试 |
| Bash regression | 106 | Shell 回归测试 |
| Benchmark (LLM) | 85 | EDSL 代码生成任务 |
| Smoke | 5 | 快速冒烟测试 |
| Regression | 6 | 已知 bug 回归 |
| Gradual guarantee | 10+ | 渐进保证测试 |
| Fuzz | 46/47 | LLM 驱动 fuzz |
| **suite/typesystem.aura** | **11 test-suite 节** | 类型系统单元测试 |
| **suite/stdlib.aura** | 多节 | 标准库测试 |
| **suite/edsl.aura** | 多节 | EDSL 测试 |
| **suite/macros.aura** | 多节 | 宏系统测试 |
| **suite/module.aura** | 多节 | 模块系统测试 |
| **suite/core.aura** | 多节 | 核心功能测试 |

### 测试架构

```
tests/
├── tasks/              ← EDSL benchmark 任务定义 (85个)
│   ├── basic/          基础
│   ├── adt/            ADT 类型推断
│   ├── type/           类型系统
│   ├── coercion/       Coercion 测试
│   ├── algorithm/      算法
│   ├── recursion/      递归
│   ├── hash/           哈希表
│   ├── hash-algo/      哈希算法
│   ├── list/           列表
│   ├── string/         字符串
│   ├── edsl/           EDSL 操作
│   ├── json/           JSON
│   └── ffi/            C FFI
├── suite/              ← 回归测试套件
│   ├── core.aura
│   ├── stdlib.aura
│   ├── typesystem.aura
│   ├── edsl.aura
│   ├── errors.aura
│   ├── macros.aura
│   ├── module.aura
│   ├── intent.aura
│   └── run-tests.aura
├── edsl_benchmark.py   ← EDSL benchmark runner
└── run_bench_all.py    ← 多模型对比
```

## Fuzz 测试

| 指标 | 值 |
|------|-----|
| 编译器崩溃 | 0 |
| 内部错误 | 0 |
| 超时 | 0 |
| 通过 | 46/47 |
| 已知 bug 回归 | 4/4 通过 (CI 每次 push) |

详见 [tests/test_fuzz.py](../tests/test_fuzz.py) 和 [design/llm_fuzz_testing.md](design/llm_fuzz_testing.md)。
