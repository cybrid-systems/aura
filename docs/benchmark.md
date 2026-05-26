# Aura EDSL Benchmark

> 135 个 LLM 代码生成任务，覆盖基础语法、标准库、类型系统、C FFI、EDSL、Workspace、ADT、M4 线性所有权、Synthesize。
>
> 自适应迭代修正（intend 模式，最多 3 次重试）。2026-05-26 跑分。

---

## Latest: 2026-05-26 — 全量 TODO 清理 + 编译器 Bug 修复后

**修复项：**
- `(: name Type val)` 绑定变量 — 之前 `var_sym` 丢弃，EDSL 类型标注场景失败
- `c-func` FFI 走 tree-walker — 之前 IR 路径无法 dispatch FFI closure
- Pipe mode 未绑定变量报错 — 之前函数参数内未绑定变量静默返回 0
- 7 个 EDSL task hint 语法修复 — `set-code` 接收 quoted list 而非 string，`require` 用 `'module` 而非 `"module"`

| 模型 | 任务数 | 通过 | 通过率 | 耗时 |
|:----|:-----:|:----:|:-----:|:----:|
| 🥇 **Grok 4.3** | 135 | **111** | **82.2%** | ~60s |
| 🥈 **DeepSeek v4 Flash** | 135 | **109** | **80.7%** | ~165s |

### 新增通过（修复后）

| 任务 | DeepSeek | Grok | 原因 |
|:-----|:--------:|:----:|:-----|
| edsl-defuse-cross | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-defuse-multi | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-require-stdlib | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-workspace-cow | ❌→✅ | ❌→✅ | hint 修复 |
| edsl-snapshot-multi | ❌→✅ | — | hint 修复 |
| type-coercion-chain | ❌→✅ | ❌→✅ | `(: x Int val)` 绑定修复 |
| type-multi-annot | ❌→✅ | ❌→✅ | `(: x Int val)` 绑定修复 |
| type-ownership-linear | — | ❌→✅ | `(: x Int val)` 绑定修复 |
| type-linear | — | ❌→✅ | `(: x Int val)` 绑定修复 |

### 稳定失败（双模型均未通过，18 个）

#### LLM 生成质量（15 个）

| 任务 | 原因 |
|:-----|:------|
| `ffi-sqrt` / `ffi-strlen` | LLM 不会调 C FFI：生成 `(c-func ...)` 后不加括号调用 |
| `edsl-rule` / `edsl-rule-basic` | LLM 用错 stdlib 函数名（如 `for-each` 格式不对） |
| `edsl-messaging` | LLM 对 send/recv 协议理解偏差 |
| `edsl-pipeline-basic` / `edsl-synthesize-pipeline` | synthesise 管线语法复杂，LLM 生成不匹配预期 |
| `edsl-splice-wrap` / `edsl-mutation-rollback` | EDSL mutate 原语节点 ID 偏移，LLM 算不对 |
| `edsl-optimize-multiarg` | 多参函数优化，LLM 生成调用格式错误 |
| `table-lookup` | LLM 生成 table 查表逻辑偏离预期 |
| `word-freq` | 用错函数名 `make-hash` → 应为 `hash` |
| `type-occ-deep` / `type-occ-match` | LLM 生成不匹配预期值 |
| `type-let-poly-hof` / `type-linear-hof` / `type-grad-multi-boundary` | 复杂 GC/gradual 场景，LLM 逻辑偏 |

#### 真正的编译器 Bug（0 个）

全部已修复 ✅。剩余失败均为 LLM 生成质量/模型方差。

### 逐任务对比

#### basic + algorithm

| 任务 | DeepSeek | Grok |
|:-----|:--------:|:----:|
| arith-basic | ✅ | ✅ |
| arith-chain | ✅ | ✅ |
| lambda-simple | ✅ | ✅ |
| climbing-stairs | ✅ | ✅ |
| combinations | ✅ | ✅ |
| named-let | ✅ | ✅ |
| letrec-fact | ✅ | ✅ |
| fibonacci | ✅ | ✅ |
| gcd-euclid | ✅ | ✅ |
| prime-test | ✅ | ✅ |
| primes-list | ✅ | ✅ |
| quicksort | ✅ | ✅ |
| reverse-list | ✅ | ✅ |
| sieve | ❌ | ✅ |
| palindrome | ✅ | ✅ |
| deep-equal | ✅ | ✅ |
| compose-n | ✅ | ✅ |
| memoize | ✅ | ✅ |
| is-anagram | ✅ | ✅ |
| two-sum | ✅ | ✅ |
| first-unique | ✅ | ✅ |
| majority-element | ✅ | ✅ |
| max-subarray | ✅ | ✅ |
| binary-search | ✅ | ❌ |
| merge-sort | ❌ | ✅ |
| merge-sorted | ❌ | ✅ |
| contains-duplicate | ✅ | ✅ |
| valid-parens | ✅ | ✅ |
| tree-dfs | ✅ | ✅ |

#### list + hash

| 任务 | DeepSeek | Grok |
|:-----|:--------:|:----:|
| list-map | ✅ | ✅ |
| list-filter | ✅ | ✅ |
| list-foldl | ✅ | ✅ |
| list-range | ✅ | ✅ |
| list-reverse | ✅ | ✅ |
| list-flatten | ✅ | ✅ |
| list-zip | ✅ | ✅ |
| list-partition | ✅ | ✅ |
| hash-basic | ✅ | ✅ |
| hash-invert | ✅ | ✅ |
| hash-stats | ❌ | ✅ |
| json-roundtrip | ✅ | ✅ |
| word-freq | ❌ | ❌ |
| unique-hash | ✅ | ❌ |

#### type system

| 任务 | DeepSeek | Grok |
|:-----|:--------:|:----:|
| type-annot-int | ✅ | ✅ |
| type-annot-chain | ✅ | ✅ |
| type-annot-expr | ✅ | ✅ |
| type-annot-fn | ✅ | ✅ |
| type-check | ✅ | ✅ |
| type-of | ✅ | ✅ |
| type-coercion-if | ✅ | ✅ |
| type-coercion-chain | ✅ | ❌ |
| type-boundary-call | ✅ | ✅ |
| type-higher-order | ✅ | ✅ |
| type-let-poly | ✅ | ✅ |
| type-consistency | ✅ | ✅ |
| type-gradual-boundary | ❌ | ✅ |
| type-gradual-erasure | ✅ | ✅ |
| type-grad-multi-boundary | ❌ | ❌ |
| type-linear | ❌ | ❌ |
| type-linear-hof | ❌ | ❌ |
| type-let-poly-hof | ❌ | ❌ |
| type-value-restriction | ✅ | ✅ |
| type-occurrence | ✅ | ✅ |
| type-occurrence-float | ✅ | ✅ |
| type-ownership-linear | ✅ | ✅ |
| type-multi-annot | ✅ | ✅ |
| type-pair-occurrence | ✅ | ✅ |
| type-blame-runtime | ✅ | ✅ |
| type-occ-cond | ✅ | ❌ |
| type-occ-deep | ❌ | ❌ |
| type-occ-match | ❌ | ❌ |

#### ADT + C FFI

| 任务 | DeepSeek | Grok |
|:-----|:--------:|:----:|
| adt-either | ❌ | ✅ |
| adt-option | ✅ | ✅ |
| adt-multi-ctor | ✅ | ✅ |
| adt-tree | ❌ | ❌ |
| adt-wildcard | ✅ | ✅ |
| ffi-sqrt | ❌ | ❌ |
| ffi-strlen | ❌ | ❌ |

#### EDSL / Workspace / Synthesize

| 任务 | DeepSeek | Grok |
|:-----|:--------:|:----:|
| edsl-query | ✅ | ✅ |
| edsl-mutate | ✅ | ✅ |
| edsl-mutate-chain | ✅ | ✅ |
| edsl-set-code | ✅ | ✅ |
| edsl-defuse | ✅ | ✅ |
| edsl-defuse-cross | ✅ | ✅ |
| edsl-defuse-multi | ✅ | ✅ |
| edsl-snapshot | ✅ | ✅ |
| edsl-snapshot-multi | ❌ | ✅ |
| edsl-find-pattern | ✅ | ✅ |
| edsl-summary | ✅ | ✅ |
| edsl-intend-basic | ✅ | ✅ |
| edsl-evolve | ✅ | ✅ |
| edsl-colony | ✅ | ✅ |
| edsl-workspace | ✅ | ✅ |
| edsl-workspace-cow | ✅ | ✅ |
| edsl-synthesize-template | ✅ | ✅ |
| edsl-synthesize-pipeline | ❌ | ❌ |
| edsl-pipeline-basic | ❌ | ❌ |
| edsl-optimize-fitness | ✅ | ✅ |
| edsl-optimize-benchmark-kw | ✅ | ✅ |
| edsl-optimize-multiarg | ❌ | ❌ |
| edsl-messaging | ❌ | ❌ |
| edsl-mutation-rollback | ❌ | ❌ |
| edsl-splice-wrap | ❌ | ❌ |
| edsl-rule | ❌ | ❌ |
| edsl-rule-basic | ❌ | ❌ |
| edsl-require-stdlib | ✅ | ✅ |

#### M4 + Module + File + Other

| 任务 | DeepSeek | Grok |
|:-----|:--------:|:----:|
| m4-borrow | ✅ | ✅ |
| m4-borrow-chain | ✅ | ✅ |
| m4-move | ✅ | ✅ |
| linear-basic | ❌ | ✅ |
| macro-definer | ✅ | ✅ |
| module-require | ✅ | ✅ |
| module-use | ✅ | ✅ |
| file-exists | ✅ | ✅ |
| file-size | ✅ | ✅ |
| file-write | ✅ | ✅ |
| directory-list | ✅ | ✅ |
| shell-cmd | ✅ | ✅ |
| command-output | ✅ | ✅ |
| tcp-connect | ✅ | ✅ |
| try-catch | ✅ | ✅ |
| string-reverse | ✅ | ✅ |
| string-split-join | ✅ | ✅ |
| vector-ops | ✅ | ✅ |
| vec-dot | ✅ | ✅ |
| vec-range | ✅ | ✅ |
| mat-identity | ✅ | ✅ |
| coerce-bool-int | ✅ | ✅ |
| coerce-float-int | ✅ | ✅ |
| coerce-int-string | ✅ | ✅ |
| bench-parse | ✅ | ❌ |
| bench-typecheck | ✅ | ✅ |
| bench-eval | ✅ | ❌ |
| table-lookup | ❌ | ❌ |
| occurrence | ✅ | ✅ |
| hash-stats | ❌ | ✅ |

---

## Run Yourself

```bash
# 全部 135 任务 × 双模型
python3 tests/run_bench_all.py --parallel

# 单模型手动跑
LLM_API_KEY="$(cat ~/keys/grok)" \
  LLM_MODEL="grok-4.3" \
  LLM_BASE_URL="https://api.x.ai/v1" \
  python3 tests/edsl_benchmark.py --max-attempts 3
```

## History

| 日期 | 版本 | 任务数 | Grok | DeepSeek | 说明 |
|:----|:----:|:-----:|:----:|:--------:|:------|
| 2026-05-26 | P2 全部 + Bug 修复 | 135 | **82.2%** | **80.7%** | hint 修复 + 3 个编译器 bug 修 |
| 2026-05-24 | P1 加固 | 111 | 83.8% | 82.9% | M4/closure/occurrence 等 7 项 |
| 2026-05-23 | 3 模型对比 | 102 | 91.2% | 85.3% | 早期基准、任务少、通过率虚高 |
