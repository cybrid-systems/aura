# Aura EDSL Benchmark

> 57 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、TCP、递归算法、LeetCode 风格。
> 自适应用迭代修正 + 执行轨迹反馈（PID 控制理论）。

## Latest: 2026-05-22 — 4 模型对比 (max-attempts=3, 1 round)

| 模型 | 通过率 | 总耗时 | 失败任务 |
|------|:-----:|:-----:|:--------|
| **Grok 4.3** | **54/57 (94.7%)** | ~11min | deep-equal, merge-sorted, primes-list |
| **DeepSeek v4 Flash** | **51/57 (89.5%)** | ~16min | binary-search, deep-equal, ffi-sqrt, ffi-strlen, is-anagram, primes-list |
| **MiniMax-M2.7** | **45/57 (78.9%)** | ~15min | contains-duplicate, deep-equal, edsl-set-code, ffi-sqrt, ffi-strlen, first-unique, is-anagram, list-zip, merge-sorted, prime-test, primes-list, tcp-connect |
| **Kimi k2.6** | **N/A** | — | Moonshot API 响应极慢（单请求超 30s+），无法完成 benchmark |

**共享失败 (3 模型均不能):** `deep-equal`, `primes-list` — 可能是 Aura 编译器自身限制或 task 设计问题。
**独家失败:** `binary-search` 仅 DeepSeek 不能；MiniMax 独有 7 个额外失败。
**Grok 为当前最优模型**，且速度最快（~66% 时间即完成）。

### 逐任务对比

| 任务 | DeepSeek | MiniMax | Grok |
|------|:--------:|:-------:|:----:|
| arith-basic | ✅ | ✅ | ✅ |
| arith-chain | ✅ | ✅ | ✅ |
| binary-search | ❌ | ✅ | ✅ |
| climbing-stairs | ✅ | ✅ | ✅ |
| combinations | ✅ | ✅ | ✅ |
| compose-n | ✅ | ✅ | ✅ |
| contains-duplicate | ✅ | ❌ | ✅ |
| deep-equal | ❌ | ❌ | ❌ |
| edsl-mutate | ✅ | ✅ | ✅ |
| edsl-query | ✅ | ✅ | ✅ |
| edsl-set-code | ✅ | ❌ | ✅ |
| ffi-sqrt | ❌ | ❌ | ✅ |
| ffi-strlen | ❌ | ❌ | ✅ |
| fibonacci | ✅ | ✅ | ✅ |
| first-unique | ✅ | ❌ | ✅ |
| gcd-euclid | ✅ | ✅ | ✅ |
| hash-basic | ✅ | ✅ | ✅ |
| hash-invert | ✅ | ✅ | ✅ |
| hash-stats | ✅ | ✅ | ✅ |
| is-anagram | ❌ | ❌ | ✅ |
| json-roundtrip | ✅ | ✅ | ✅ |
| lambda-simple | ✅ | ✅ | ✅ |
| letrec-fact | ✅ | ✅ | ✅ |
| list-filter | ✅ | ✅ | ✅ |
| list-flatten | ✅ | ✅ | ✅ |
| list-foldl | ✅ | ✅ | ✅ |
| list-map | ✅ | ✅ | ✅ |
| list-partition | ✅ | ✅ | ✅ |
| list-range | ✅ | ✅ | ✅ |
| list-reverse | ✅ | ✅ | ✅ |
| list-zip | ✅ | ❌ | ✅ |
| macro-definer | ✅ | ✅ | ✅ |
| majority-element | ✅ | ✅ | ✅ |
| max-subarray | ✅ | ✅ | ✅ |
| memoize | ✅ | ✅ | ✅ |
| merge-sort | ✅ | ✅ | ✅ |
| merge-sorted | ✅ | ❌ | ❌ |
| named-let | ✅ | ✅ | ✅ |
| occurrence | ✅ | ✅ | ✅ |
| palindrome | ✅ | ✅ | ✅ |
| prime-test | ✅ | ❌ | ✅ |
| primes-list | ❌ | ❌ | ❌ |
| quicksort | ✅ | ✅ | ✅ |
| reverse-list | ✅ | ✅ | ✅ |
| sieve | ✅ | ✅ | ✅ |
| string-reverse | ✅ | ✅ | ✅ |
| string-split-join | ✅ | ✅ | ✅ |
| table-lookup | ✅ | ✅ | ✅ |
| tcp-connect | ✅ | ❌ | ✅ |
| tree-dfs | ✅ | ✅ | ✅ |
| two-sum | ✅ | ✅ | ✅ |
| type-check | ✅ | ✅ | ✅ |
| type-of | ✅ | ✅ | ✅ |
| unique-hash | ✅ | ✅ | ✅ |
| valid-parens | ✅ | ✅ | ✅ |
| vector-ops | ✅ | ✅ | ✅ |
| word-freq | ✅ | ✅ | ✅ |

## 2026-05-21 — 旧版结果 (max-attempts=5)

| 模型 | 通过率 | 总耗时 |
|------|:-----:|:-----:|
| **DeepSeek v4 Flash** | **52/57 (91%)** | ~15min |
| **MiniMax-M2.7** | **51/57 (89%)** | ~20min |

> *注: 旧版使用 `--max-attempts 5` 并有不同 prompt 策略，分数略高。新版统一为 max-attempts=3 更接近实际使用场景。*

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
