# Aura EDSL Benchmark

> 57 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、TCP、递归算法、LeetCode 风格。
> 自适应用迭代修正 + 执行轨迹反馈（PID 控制理论）。

## Latest: 2026-05-21 — 多模型基准

| 模型 | `--max-attempts` | 通过率 | 总耗时 | 1次通过 | 多轮修复 | 失败 |
|------|:--------------:|:-----:|:-----:|:------:|:-------:|:----:|
| **DeepSeek v4 Flash** | 5 | **56/57 (98%)** | 11min | 45 | 11 | table-lookup |
| **MiniMax-M2.7** | 5 | **54/57 (94%)** | 14min | 44 | 10 | contains-duplicate, memoize, primes-list |

**所有失败均为 LLM 语法/语义偏差，非基础设施问题。**
零 serve 崩溃，零死锁，零 "no JSON"。

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

57 个任务，覆盖 9 个能力域：

- **基础 (12)**: arith-basic, arith-chain, lambda-simple, letrec-fact, named-let, string-reverse, string-split-join, type-check, type-of, occurrence, ffi-sqrt, ffi-strlen
- **列表 (11)**: list-range, list-filter, list-map, list-foldl, list-reverse, list-zip, list-partition, list-flatten, unique-hash, merge-sort, binary-search
- **哈希 (8)**: hash-basic, hash-stats, word-freq, palindrome, hash-invert, table-lookup, json-roundtrip, memoize
- **递归算法 (8)**: prime-test, primes-list, fibonacci, gcd-euclid, combinations, quicksort, sieve, tree-dfs
- **高阶/系统 (8)**: compose-n, deep-equal, macro-definer, tcp-connect, vector-ops, edsl-set-code, edsl-query, edsl-mutate
- **LeetCode (10)**: two-sum, reverse-list, valid-parens, max-subarray, contains-duplicate, merge-sorted, climbing-stairs, majority-element, first-unique, is-anagram

### 运行

```bash
# 全量
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5

# 指定模型
LLM_MODEL=minimax-m2.7 LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5

# 指定任务
LLM_API_KEY="***" python3 tests/edsl_benchmark.py --tasks is-anagram,hash-stats

# 多模型对比
LLM_MODEL=deepseek-v4-flash,minimax-m2.7 LLM_API_KEY="***" python3 tests/edsl_benchmark.py --max-attempts 5
```

### 修复详情

- **extract_code 正则**: `<[^>]+>` 吞噬了 Aura 比较操作符（`<`, `>`）→ `</?\w[^>]*>`
- **serve 非阻塞 IO**: fcntl + os.read 超时，无线程竞态
- **tcp-connect 超时**: 非阻塞 connect() + poll() 8s 超时
- **EDSL 单次 exec**: `(set-code ...) + (mutate:rebind ...) + (eval-current)` 一次调用
- **parser 报错增强**: 包含 `expected expression, got ')'` 等详细信息

## 运行模式

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

#### 任务列表

57 个任务，4 个难度等级 + 扩展：

##### 基础 (12)
`arith-basic`, `arith-chain`, `lambda-simple`, `letrec-fact`, `named-let`,
`string-reverse`, `string-split-join`, `type-check`, `type-of`,
`occurrence`, `ffi-sqrt`, `ffi-strlen`

##### 列表/集合 (11)
`list-range`, `list-filter`, `list-map`, `list-foldl`, `list-reverse`,
`list-zip`, `list-partition`, `list-flatten`, `unique-hash`,
`merge-sort`, `binary-search`

##### 哈希/字符串 (8)
`hash-basic`, `hash-stats`, `word-freq`, `palindrome`,
`hash-invert`, `table-lookup`, `json-roundtrip`, `memoize`

##### 递归/算法 (8)
`prime-test`, `primes-list`, `fibonacci`, `gcd-euclid`,
`combinations`, `quicksort`, `sieve`, `tree-dfs`

##### 高阶/系统 (8)
`compose-n`, `deep-equal`, `macro-definer`, `tcp-connect`,
`vector-ops`, `edsl-set-code`, `edsl-query`, `edsl-mutate`

##### LeetCode 风格 (10)
`two-sum`, `reverse-list`, `valid-parens`, `max-subarray`,
`contains-duplicate`, `merge-sorted`, `climbing-stairs`,
`majority-element`, `first-unique`, `is-anagram`

## 运行

```bash
# 单轮单次
LLM_API_KEY="..." python3 tests/edsl_benchmark.py

# 单轮 + 迭代修正 (推荐)
LLM_API_KEY="..." python3 tests/edsl_benchmark.py --fix --max-attempts 5

# 原生 intend 模式
LLM_API_KEY="..." python3 tests/edsl_benchmark.py --intend

# 只跑失败任务
LLM_API_KEY="..." python3 tests/edsl_benchmark.py --intend --failed

# 多模型对比
LLM_MODEL=deepseek-v4-flash,minimax-m2.7 LLM_API_KEY="..." python3 tests/edsl_benchmark.py --fix --max-attempts 5

# 多模型 + 多轮聚合
LLM_MODEL=deepseek-v4-flash,gpt-4o LLM_API_KEY="..." python3 tests/edsl_benchmark.py --rounds 3
```

## 全部通过 🎯

双模型（DeepSeek v4 Flash, MiniMax-M2.7）在 `--fix --max-attempts 5` 下均达成 57/57 (100%)。
无剩余不稳定任务。

### 修复详情

**根因**: `tests/edsl_benchmark.py` 的 `extract_code()` 函数中
`re.sub(r'<[^>]+>', ...)` 正则把 Aura 代码里的比较操作符（`<`, `>`）当 XML 标签误删。
修正循环喂给 LLM 的是残缺代码，LLM 无法据此修复。

**修复**: 改为 `re.sub(r'</?\\w[^>]*>', ...)`，只匹配标签名以字母开头的真正 XML/HTML 标签，
保留 `<`, `>` 比较操作符。

**副作用**: 同时改善了 parser 报错信息（`parse error at line 1:1: expected expression, got identifier '...'`）
和修正 prompt 按错误类型分诊。

## Fuzz 测试

| 指标 | 值 |
|------|-----|
| 编译器崩溃 | 0 |
| 内部错误 | 0 |
| 超时 | 0 |
| 通过 | 46/47 |
| 已知 bug 回归 | 4/4 通过 (CI 每次 push) |

详见 [tests/test_fuzz.py](../tests/test_fuzz.py) 和 [design/llm_fuzz_testing.md](design/llm_fuzz_testing.md)。
