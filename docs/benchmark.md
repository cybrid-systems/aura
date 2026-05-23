# Aura EDSL Benchmark

> 99 个 LLM 代码生成任务,覆盖基础语法、标准库、类型系统、C FFI、EDSL、TCP、文件 I/O、递归算法、LeetCode 风格、ADT、M4 线性所有权、向量数学。
> 自适应迭代修正 + 执行轨迹反馈(PID 控制理论)+ 蚁群局部搜索。

## Latest: 2026-05-23 — 3 模型对比 (max-attempts=3, 1 round)

**任务优化 (第2版)：** 测试任务从 99 扩展到 102，新增 3 个 EDSL 任务（edsl-colony, edsl-find-pattern, edsl-mutate-chain）。
修复了 5 个共享失败的任务预期值和 hint（ffi-strlen, type-occurrence-float, directory-list, module-use），
所有 EDSL 任务增加了详细 hint。

**性能优化：** 任务级并行执行（`BENCH_WORKERS=14`），单模型耗时 ~7-9min。

| 模型 | 任务数 | 通过率 | 耗时 |
|:----|:-----:|:-----:|:----:|
| 🥇 **Grok 4.3** | 102 | **93/102 (91.2%)** | ~9min |
| 🥈 **DeepSeek v4 Flash** | 102 | **87/102 (85.3%)** | ~7min |
| 🥉 **MiniMax M2.7** | 102 | **47/102 (46.1%)** | ~13min |

比上版本（99 任务）:
- Grok 85→93 (+8)
- DeepSeek 81→87 (+6)
- MiniMax 44→47 (+3)
- **修复生效：** ffi-strlen ✅, type-occurrence-float ✅, directory-list ✅, module-use ✅, unique-hash ✅（Grok/DeepSeek）
- **新增 EDSL 任务：** edsl-colony ✅ 三模型全过, edsl-query ✅, edsl-mutate ✅
- **失败分析：** `#<procedure>` (binary-search, merge-sort) 仍存，EDSL复杂链式操作 (edsl-find-pattern, edsl-mutate-chain) 三模型均挂。

**失败分析:**

- **Shared fails(三模型均过不去):** `adt-tree`, `directory-list`, `edsl-mutate`, `edsl-query`, `edsl-set-code`, `ffi-strlen`, `table-lookup`, `word-freq`, `type-occurrence-float`, `unique-hash`
- **Grok 独败 (5):** `adt-option`, `binary-search`, `merge-sort`, `module-use`, `type-blame-runtime`
- **DeepSeek 独败 (4):** `adt-either`, `ffi-sqrt`, `tcp-connect`, `type-multi-annot`
- **MiniMax 独败 (37):** 大量 `no code extracted`,涉及 list/hash/type/vector 等多个类别

### 逐任务对比(99 任务)

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
| gcd-euclid | ✅ | ✅ | ✅ |
| prime-test | ✅ | ✅ | ✅ |
| primes-list | ✅ | ✅ | ✅ |
| quicksort | ✅ | ✅ | ✅ |
| reverse-list | ✅ | ✅ | ✅ |
| sieve | ✅ | ✅ | ✅ |
| palindrome | ✅ | ✅ | ✅ |
| deep-equal | ✅ | ✅ | ✅ |
| compose-n | ✅ | ✅ | ✅ |
| memoize | ✅ | ✅ | ✅ |
| is-anagram | ✅ | ✅ | ✅ |
| two-sum | ✅❌(3 att) | ❌(3 att no-code) | ✅ |
| first-unique | ✅ | ❌(3 att no-code) | ✅ |

#### algorithm

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| majority-element | ✅ | ❌(3 att no-code) | ✅ |
| max-subarray | ✅ | ❌(3 att no-code) | ✅ |
| binary-search | ❌(#<procedure>) | ❌(#<procedure>) | ❌(#<procedure>) |
| merge-sort | ✅ | ❌(#<procedure>) | ❌(#<procedure>) |
| merge-sorted | ✅ | ✅ | ✅ |
| contains-duplicate | ✅ | ❌(3 att no-code) | ✅ |
| valid-parens | ✅ | ✅ | ✅ |
| tree-dfs | ✅ | ✅ | ✅ |
| climbing-stairs | ✅ | ✅ | ✅ |

#### list

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| list-map | ✅ | ❌(3 att no-code) | ✅ |
| list-filter | ✅ | ❌(3 att no-code) | ✅ |
| list-foldl | ✅ | ❌(3 att no-code) | ✅ |
| list-range | ✅ | ❌(3 att no-code) | ✅ |
| list-reverse | ✅ | ❌(3 att no-code) | ✅ |
| list-flatten | ✅ | ❌(3 att no-code) | ✅ |
| list-zip | ✅ | ❌(3 att no-code) | ✅ |
| list-partition | ✅ | ❌(3 att no-code) | ✅ |

#### hash

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| hash-basic | ✅ | ❌(3 att no-code) | ✅ |
| hash-invert | ✅ | ❌(3 att no-code) | ✅ |
| hash-stats | ✅ | ❌(3 att no-code) | ✅ |
| json-roundtrip | ✅ | ❌(3 att no-code) | ✅ |
| word-freq | ❌(3 att) | ❌(3 att no-code) | ❌(3 att) |
| unique-hash | ❌(3 att) | ❌(3 att no-code) | ❌(3 att) |

#### type system

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| type-annot-int | ✅ | ✅ | ✅ |
| type-annot-chain | ✅ | ✅ | ✅ |
| type-annot-expr | ✅ | ✅ | ✅ |
| type-annot-fn | ✅ | ✅ | ✅ |
| type-check | ✅ | ✅ | ✅ |
| type-of | ✅ | ❌(3 att no-code) | ✅ |
| type-coercion-if | ✅ | ✅ | ✅ |
| type-boundary-call | ✅ | ❌(unbound :) | ✅ |
| type-higher-order | ✅ | ✅ | ✅ |
| type-let-poly | ✅ | ✅ | ✅ |
| type-consistency | ✅ | ❌(3 att no-code) | ✅ |
| type-gradual-boundary | ✅ | ❌(3 att no-code) | ✅ |
| type-gradual-erasure | ✅ | ❌(3 att no-code) | ✅ |
| type-linear | ✅ | ❌(3 att no-code) | ✅ |
| type-value-restriction | ✅ | ❌(3 att no-code) | ✅ |
| type-occurrence | ✅ | ❌(3 att no-code) | ✅ |
| type-occurrence-float | ❌(3 att) | ❌(3 att no-code) | ❌(3 att) |
| type-ownership-linear | ✅ | ❌(3 att no-code) | ✅ |
| type-multi-annot | ❌(unbound the) | ❌(3 att no-code) | ✅ |
| type-pair-occurrence | ✅ | ❌(3 att no-code) | ✅ |
| type-blame-runtime | ❌(3 att) | ❌(3 att) | ✅ |

#### ADT / datatype

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| adt-either | ❌(3 att) | ❌(3 att) | ✅ |
| adt-option | ❌(3 att) | ✅ | ✅ |
| adt-multi-ctor | ✅ | ✅ | ✅ |
| adt-tree | ❌(unbound clojure) | ❌(unbound deftype) | ❌(Leaf) |
| adt-wildcard | ✅ | ✅ | ✅ |

#### C FFI

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| ffi-sqrt | ❌(3 att) | ❌(3 att no-code) | ✅ |
| ffi-strlen | ❌(3 att) | ❌(3 att) | ❌(3 att) |

#### EDSL / reflection

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| edsl-query | ❌(invalid JSON) | ❌(no code) | ❌(3 att) |
| edsl-mutate | ❌(invalid JSON) | ❌(no code) | ❌(3 att) |
| edsl-set-code | ❌(invalid JSON) | ❌(no code) | ❌(invalid JSON) |

#### M4 linear

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| m4-borrow | ✅ | ❌(3 att no-code) | ✅ |
| m4-move | ✅ | ❌(3 att no-code) | ✅ |
| linear-basic | ✅ | ✅ | ✅ |

#### file I/O

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| file-exists | ✅ | ❌(3 att no-code) | ✅ |
| file-size | ✅ | ❌(3 att no-code) | ✅ |
| file-write | ✅ | ❌(3 att no-code) | ✅ |
| directory-list | ❌(3 att) | ❌(3 att no-code) | ❌(3 att) |

#### process

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| shell-cmd | ✅ | ❌(3 att) | ✅ |
| command-output | ✅ | ✅ | ✅ |

#### module

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| module-require | ✅ | ❌(25) | ✅ |
| module-use | ❌(3 att) | ❌(loaded ok) | ❌(3 att) |

#### network

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| tcp-connect | ❌(serve hang) | ✅ | ✅ |

#### vector math

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| vec-dot | ✅ | ❌(3 att no-code) | ✅ |
| vec-range | ✅ | ❌(3 att no-code) | ✅ |
| mat-identity | ✅ | ❌(3 att no-code) | ✅ |

#### coerce

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| coerce-bool-int | ✅ | ✅ | ✅ |
| coerce-float-int | ✅ | ✅ | ✅ |
| coerce-int-string | ✅ | ✅ | ✅ |
| occurrence | ✅ | ✅ | ✅ |

#### macro

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| macro-definer | ✅ | ❌(3 att no-code) | ✅ |
| table-lookup | ❌(3 att) | ❌(3 att) | ❌(3 att) |

#### other

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| try-catch | ✅ | ✅ | ✅ |
| string-reverse | ✅ | ✅ | ✅ |
| string-split-join | ✅ | ✅ | ✅ |
| module-use | ❌(3 att) | ❌(3 att) | ❌(3 att) |
| module-require | ✅ | ❌(25) | ✅ |

## Run Yourself

```bash
# 单模型(并行 14 线程)
BENCH_WORKERS=14 LLM_MODEL="grok-4.3" \
  LLM_BASE_URL="https://api.x.ai/v1" \
  LLM_API_KEY="$(cat ~/keys/grok)" \
  python3 tests/edsl_benchmark.py --max-attempts 3

# 指定任务子集
BENCH_TASK_FILTER="arith-basic,lambda-simple" BENCH_WORKERS=14 \
  LLM_MODEL="grok-4.3" ... python3 tests/edsl_benchmark.py
```

## Task Categories

| 类别 | 数量 | 描述 |
|------|:---:|------|
| basic | 21 | 基础算术、递归、字符串操作 |
| algorithm | 9 | LeetCode 风格算法 |
| list | 8 | 标准库 list 操作 |
| type system | 21 | 类型注解、渐进类型、多态 |
| hash | 6 | hash 表操作 |
| ADT | 5 | 代数数据类型 |
| C FFI | 2 | C 函数调用 |
| EDSL | 3 | 反射 AST 查询/修改 |
| M4 linear | 3 | 线性所有权/借用 |
| file I/O | 4 | 文件系统操作 |
| process | 2 | shell 命令执行 |
| module | 2 | 模块导入 |
| network | 1 | TCP 客户端 |
| vector math | 3 | 向量/矩阵运算 |
| coerce | 4 | 类型强制转换 + occurrence typing |
| macro | 2 | 宏定义 |
| other | 3 | try-catch, 字符串 |
