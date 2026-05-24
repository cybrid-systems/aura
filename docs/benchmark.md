# Aura EDSL Benchmark

> 111 个 LLM 代码生成任务,覆盖基础语法、标准库、类型系统、C FFI、EDSL、TCP、文件 I/O、递归算法、LeetCode 风格、ADT、M4 线性所有权、向量数学、高阶函数、occurrence typing、let-polymorphism。
> 自适应迭代修正 + 执行轨迹反馈(PID 控制理论)+ 蚁群局部搜索。

## Latest: 2026-05-24 — P1 加固后对比

P1 完成 7 项编译器核心加固：M4 静态借用检查、closure inline cache、inline primitives、occurrence typing、事务 rollback、pack_pair 消除、mark_dirty_upward 迭代化。
新增 9 个有难度类型系统用例 (occ-cond/occ-deep/occ-match/let-poly-hof/coercion-chain/grad-multi-boundary/linear-hof/borrow-chain/mutation-rollback)。

| 模型 | 任务数 | 通过率 | 耗时 |
|:----|:-----:|:-----:|:----:|
| 🥇 Grok 4.3 | 111 | **93/111 (83.8%)** | ~50s |
| 🥈 DeepSeek v4 Flash | 111 | **92/111 (82.9%)** | ~173s |

### 新增通过 (P1 加固后)

| 任务 | DeepSeek | Grok | 说明 |
|:-----|:--------:|:----:|:-----|
| binary-search | ❌ | ✅ | 二分查找 |
| merge-sort | ✅ | ❌ | 归并排序 |
| two-sum | ✅ | ✅ | 两数之和 |
| word-freq | ✅ | ❌ | 词频统计 |
| ffi-strlen | ✅ | ❌ | C FFI strlen |
| type-check | ✅ | ✅ | 类型检查器 |
| type-multi-annot | ✅ | ❌ | 多重类型标注 |
| type-linear-hof | ❌ | ✅ | 线性+高阶 **新** |
| m4-borrow-chain | ❌ | ✅ | 借用跨域 **新** |

### 稳定失败 (两模型均 Fail)

ffi-sqrt, adt-option, tcp-connect, occurrence, type-blame-runtime, edsl-mutation-rollback, type-linear, type-occ-cond/occ-deep/occ-match, type-let-poly-hof, type-coercion-chain, type-grad-multi-boundary

---



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
比上版本（99 任务）:
- Grok 85→93 (+8)
- DeepSeek 81→87 (+6)
- **修复生效：** ffi-strlen ✅, type-occurrence-float ✅, directory-list ✅, module-use ✅, unique-hash ✅（Grok/DeepSeek）
- **新增 EDSL 任务：** edsl-colony ✅ 两模型全过, edsl-query ✅, edsl-mutate ✅, edsl-set-code ✅
- **失败分析：** `#<procedure>` (binary-search, merge-sort) 仍存，`edsl-find-pattern`、`edsl-mutate-chain` 均挂 (模型差异)。

**失败分析（102 任务）：**

- **Shared fails（两模型均未通过）：** `adt-tree`, `binary-search`, `edsl-find-pattern`, `edsl-mutate-chain`, `merge-sort`, `table-lookup`, `type-blame-runtime`
- **Grok 独败 (6):** `adt-either`, `merge-sort`, `table-lookup`, `type-blame-runtime`, `word-freq`
- **DeepSeek 独败 (14):** `adt-wildcard`, `binary-search`, `edsl-find-pattern`, `edsl-mutate-chain`, `edsl-set-code`, `ffi-sqrt`, `ffi-strlen`, `merge-sort`, `table-lookup`, `tcp-connect`, `type-blame-runtime`, `type-gradual-boundary`, `type-ownership-linear`, `word-freq`
- 

### 逐任务对比(99 任务)

#### basic

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
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

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
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

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| list-map | ✅ | ❌(3 att no-code) | ✅ |
| list-filter | ✅ | ❌(3 att no-code) | ✅ |
| list-foldl | ✅ | ❌(3 att no-code) | ✅ |
| list-range | ✅ | ❌(3 att no-code) | ✅ |
| list-reverse | ✅ | ❌(3 att no-code) | ✅ |
| list-flatten | ✅ | ❌(3 att no-code) | ✅ |
| list-zip | ✅ | ❌(3 att no-code) | ✅ |
| list-partition | ✅ | ❌(3 att no-code) | ✅ |

#### hash

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| hash-basic | ✅ | ❌(3 att no-code) | ✅ |
| hash-invert | ✅ | ❌(3 att no-code) | ✅ |
| hash-stats | ✅ | ❌(3 att no-code) | ✅ |
| json-roundtrip | ✅ | ❌(3 att no-code) | ✅ |
| word-freq | ❌(3 att) | ❌(3 att no-code) | ❌(3 att) |
| unique-hash | ❌(3 att) | ❌(3 att no-code) | ❌(3 att) |

#### type system

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
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

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| adt-either | ❌(3 att) | ❌(3 att) | ✅ |
| adt-option | ❌(3 att) | ✅ | ✅ |
| adt-multi-ctor | ✅ | ✅ | ✅ |
| adt-tree | ❌(unbound clojure) | ❌(unbound deftype) | ❌(Leaf) |
| adt-wildcard | ✅ | ✅ | ✅ |

#### C FFI

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| ffi-sqrt | ❌(3 att) | ❌(3 att no-code) | ✅ |
| ffi-strlen | ❌(3 att) | ❌(3 att) | ❌(3 att) |

#### EDSL / reflection

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| edsl-query | ❌(invalid JSON) | ❌(no code) | ❌(3 att) |
| edsl-mutate | ❌(invalid JSON) | ❌(no code) | ❌(3 att) |
| edsl-set-code | ❌(invalid JSON) | ❌(no code) | ❌(invalid JSON) |

#### M4 linear

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| m4-borrow | ✅ | ❌(3 att no-code) | ✅ |
| m4-move | ✅ | ❌(3 att no-code) | ✅ |
| linear-basic | ✅ | ✅ | ✅ |

#### file I/O

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| file-exists | ✅ | ❌(3 att no-code) | ✅ |
| file-size | ✅ | ❌(3 att no-code) | ✅ |
| file-write | ✅ | ❌(3 att no-code) | ✅ |
| directory-list | ❌(3 att) | ❌(3 att no-code) | ❌(3 att) |

#### process

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| shell-cmd | ✅ | ❌(3 att) | ✅ |
| command-output | ✅ | ✅ | ✅ |

#### module

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| module-require | ✅ | ❌(25) | ✅ |
| module-use | ❌(3 att) | ❌(loaded ok) | ❌(3 att) |

#### network

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| tcp-connect | ❌(serve hang) | ✅ | ✅ |

#### vector math

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| vec-dot | ✅ | ❌(3 att no-code) | ✅ |
| vec-range | ✅ | ❌(3 att no-code) | ✅ |
| mat-identity | ✅ | ❌(3 att no-code) | ✅ |

#### coerce

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| coerce-bool-int | ✅ | ✅ | ✅ |
| coerce-float-int | ✅ | ✅ | ✅ |
| coerce-int-string | ✅ | ✅ | ✅ |
| occurrence | ✅ | ✅ | ✅ |

#### macro

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
| macro-definer | ✅ | ❌(3 att no-code) | ✅ |
| table-lookup | ❌(3 att) | ❌(3 att) | ❌(3 att) |

#### other

| 任务 | DeepSeek | Grok |
|------|:--------:|:----:|
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
