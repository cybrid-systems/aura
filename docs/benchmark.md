# Aura EDSL Benchmark

> 57 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、TCP、递归算法、LeetCode 风格。
> 自适应用迭代修正 + 执行轨迹反馈（PID 控制理论）。

## Latest: 2026-05-20 — deepseek-v4-flash, 57 任务, 55/57 (96%)

### 三模式对比

| 模式 | 命令 | ✅ 通过 | ❌ 失败 | 说明 |
|------|------|:---:|:---:|------|
| `--fix` (Python) | `--rounds 3 --fix --max-attempts 5` | 26/26 (100%) | 0 | 仅旧任务子集 (提示含完整代码) |
| `--intend` (C++) | `--intend` | **55/57 (96%)** | 2 | 自适应 PID + 结构化 fixer + trace 反馈 |
| `--intend --evolve` | `--intend --evolve` | 55/57 (96%) | 2 | E4 自进化 + hints 注入 |

### 改进轨迹

| 阶段 | 日期 | 通过率 | 关键变更 |
|------|------|--------|---------|
| 基线 | 05-19 | 17/26 (65%) | 无提示，单次生成 |
| 任务提示 | 05-20 | 21/26 (81%) | 加方向提示 + 迭代修正 |
| 验证器升级 | 05-20 | 44/57 (77%) | 校验输出匹配期望值 |
| `current-source` 捕获 | 05-20 | 51/57 (89%) | fixer 能看到 LLM 写的代码 |
| 自适应 PID 控制 | 05-20 | 52/57 (91%) | `measure-distance` + phase 切换 |
| API Reference 注入 | 05-20 | 54/57 (95%) | std/hash, list, socket 签名 |
| 执行轨迹反馈 | 05-20 | 55/57 (96%) | retry 时显示代码运行结果 |
| 修复 append/mod/tcp-recv | 05-20 | 55/57 (96%) | 3 个语言级 bug 修复 |
| Hint 清理 | 05-20 | 55/57 (96%) | 93 行完整代码删除→纯方向提示 |

### 新增控制论特性

#### 自适应 PID 循环 (`call_adaptive`)

```
measure-distance(rc, output, expected) → (phase, ratio, diagnosis)

  phase=coarse (rc!=0)        → 完整重写, temperature=0.3, tokens=4096
  phase=fine   (0<ratio<85%)  → 精修, temperature=0.2, tokens=2048
  phase=putt   (ratio>=85%)   → 微调, temperature=0.1, tokens=1024
```

#### 结构化诊断

- 检测 `<hash[N]>` 输出 → 告警使用 hash-keys/values
- 列出 output 中缺失的 expected 关键词
- 注入 API Reference（std/hash, std/list, std/socket...）
- 执行轨迹反馈（运行代码并输出中间结果）

#### 语言级 Bug 修复

| Bug | 影响的任务 | 修复 |
|-----|-----------|------|
| `append` 只处理 2 参数 | quicksort (只排3个元素) | 改为 variadic，循环拼接所有参数 |
| `mod` 未定义 | primes-list (prime? 错误) | 注册 `mod` 为 `modulo` 别名 |
| `tcp-recv` 需要 2 参数 | tcp-connect (返回空) | 第2参数可选，默认 4096 |

#### 运行模式

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
# 快速单轮
LLM_API_KEY="xxx" ./tests/edsl_benchmark.py

# 原生 intend 模式（推荐）
LLM_API_KEY="xxx" ./tests/edsl_benchmark.py --intend

# 只跑失败任务
LLM_API_KEY="xxx" ./tests/edsl_benchmark.py --intend --failed

# 多模型对比
LLM_MODEL=deepseek-v4-flash,gpt-4o LLM_API_KEY="xxx" ./tests/edsl_benchmark.py --rounds 3
```

## 剩余不稳定任务

| 任务 | 状态 | 问题 | 根本原因 |
|------|------|------|---------|
| **tcp-connect** | 🔄 50-75% | HTTP 连接网络抖动 | `tcp-recv` 有时返回空，LLM 代码本身正确 |
| **valid-parens** | 🔄 50-75% | 栈逻辑写不对 | 方向提示不精确，LLM 需要自己推理 char code 匹配 |

## Fuzz 测试

| 指标 | 值 |
|------|-----|
| 编译器崩溃 | 0 |
| 内部错误 | 0 |
| 超时 | 0 |
| 通过 | 46/47 |
| 已知 bug 回归 | 4/4 通过 (CI 每次 push) |

详见 [tests/test_fuzz.py](../tests/test_fuzz.py) 和 [design/llm_fuzz_testing.md](design/llm_fuzz_testing.md)。
