# 自适应 Intend — 控制理论驱动的迭代修复

> 把迭代修复升级成一个自适应闭环控制器。
> LLM 可以大步输出，也可以小步前进。距离决定策略。

---

## 1. 动机

### 1.1 现在的 intend 循环

```
  ┌──────────┐    generator    ┌──────────┐    verifier    ┌──────────┐
  │  LLM     │ ──── code ────→ │  Aura    │ ──── "#t" ──→ │  done    │
  │ generator │                │ compiler │                │          │
  │          │ ←── error ──── │          │ ←── fixer ──── │          │
  └──────────┘                 └──────────┘                 └──────────┘
```

所有 attempt 同质：**每次都是完整重写**。

问题：
- code 大致对了但输出有小偏差 → 完整重写可能弄坏已经对的部分
- LLM 不知道自己是"快到了"还是"全错了"
- 反馈固定（error string），不随距离调整信息密度

### 1.2 控制理论视角

修复代码和控制系统的联系：

| 控制理论 | 代码修复 |
|----------|----------|
| 被控对象 | LLM 生成的代码 |
| 目标设定 | goal + expected output |
| 误差信号 | 编译结果 + 输出偏差 |
| 控制输入 | system prompt（含反馈） |
| 执行器 | LLM |
| 传感器 | `set-code` + `eval-current` + `check_success` |

误差大 → 控制增益高（完整重写）  
误差小 → 控制增益低（定点修改）

---

## 2. 高尔夫隐喻 — 三阶段策略

```
   开球 (tee shot)           果岭 (approach)           推杆 (putt)
  ┌─────────────────┐    ┌──────────────────────┐    ┌────────────────┐
  │ 误差: 编译失败   │    │ 误差: 输出不匹配     │    │ 误差: < 15%    │
  │ 策略: 完整重写   │    │ 策略: EDSL 定点修改  │    │ 策略: 表达式级 │
  │ 增益: 高         │    │ 增益: 中             │    │ 增益: 低       │
  │ 反馈: 编译错误   │    │ 反馈: 预期vs实际     │    │ 反馈: AST 坐标 │
  │ + 完整源码       │    │ + 源码 + AST 统计    │    │ + 精确 diff    │
  └─────────────────┘    └──────────────────────┘    └────────────────┘
       │                         │                         │
       │    ┌─────────────┐      │                         │
       └──→ │ 距离检测     │ ←────┘                         │
            │ (distance)   │ ←─────────────────────────────┘
            └──────┬──────┘
                   │ 根据距离切换策略
                   ▼
        ┌─────────────────────┐
        │ 策略选择器           │
        │ coarse | fine | putt │
        └─────────────────────┘
```

### 2.1 阶段 1 — 开球 (coarse)

**目标**：最短时间让代码编译通过。

行为：
- `__gen__` 提示：写完整的实现，不拘泥细节
- `__fix__` 提示：修复编译错误，保持大框架不变
- 反馈：编译错误 + `current-source`（AST 格式化源码）
- Attempt 限制：最多 3 次

System prompt 风格（粗粒度）：
```
你是一位 Aura 程序员。任务：{goal}

要求：
- 先写出一个能编译通过并跑出大致正确结果的版本
- 不要过度优化，先跑通

只输出 Aura 代码。
```

### 2.2 阶段 2 — 果岭 (fine)

**目标**：修正输出精度，匹配预期。

行为：
- `__fix__` 提示：代码编译通过但输出不匹配，需要理解差距并修改
- 反馈：`current-source` + 预期 vs 实际输出 + 结构化诊断
- 额外反馈：显示 `(display ...)` 输出中缺少了哪些预期关键词
- 拒绝完整重写 — 提示 LLM 只修改产生输出的部分

System prompt 风格（细粒度）：
```
你的代码已经编译通过，但输出不匹配预期。

=== 当前源码（AST 格式化） ===
{current_source}

=== 实际输出 ===
{actual_output}

=== 预期包含 ===
{expected_keywords_list}

=== 诊断 ===
{structured_diagnosis}

请修改代码让输出匹配预期。不要重写整个函数，只修改显示/输出部分。
```

### 2.3 阶段 3 — 推杆 (putt)

**目标**：误差 < 1 token。

行为：
- 使用 EDSL 反射（`query:find`、`query:children`）定位具体表达式
- 只修改定位到的 AST 节点
- 反馈中包含精确的 diff
- 适合接近正确但微小格式错误的场景

System prompt 风格（极细粒度）：
```
代码基本正确，只有微小格式偏差。

=== 需要修改的表达式 ===
{target_expression}

=== 当前值 ===
{current_value}

=== 期望值 ===
{expected_value}

用 EDSL 定点修改：
- (query:find ...) 定位节点
- (mutate:rebind node-id new-value) 修改
```

---

## 3. 距离度量 (Distance Function)

```python
def measure_distance(rc, output, expected, history):
    """返回 (phase, ratio, diagnosis)"""
    
    # 编译/运行时错误 → 开球
    if rc != 0:
        return ("coarse", 0.0, "compile-error")
    
    # 输出完全为空
    if not output or output.isspace():
        return ("coarse", 0.0, "empty-output")
    
    # 计算 expected 关键词在 output 中出现的比例
    norm_out = output.strip().lower()
    matches = sum(1 for kw in expected if kw.lower() in norm_out)
    ratio = matches / max(len(expected), 1)
    
    # 根据匹配比例判定阶段
    if ratio >= 0.85:
        return ("putt", ratio, "near-match")
    elif ratio > 0.0:
        return ("fine", ratio, "partial-match")
    else:
        # 有输出但完全不匹配：看输出是否类似结构
        if output.startswith('<hash') or output.startswith('(') or output.startswith('#'):
            return ("fine", 0.0, "structure-correct-content-wrong")
        return ("coarse", 0.0, "complete-mismatch")
```

### 3.1 结构化诊断生成

每个距离层级生成不同密度的诊断：

```
coarse → 无诊断（只给编译错误）
fine   → 包含：
         1. 实际输出中缺少了哪些 expected 关键词
         2. 输出格式分类（hash ref / list / string）
         3. 常见的 Aura 行为陷阱
putt   → 包含：
         1. 精确到 AST 节点的 diff
         2. 差值的位置和类型
```

诊断规则表：

| 输出特征 | 诊断 | 
|----------|------|
| `<hash[N]>` | "display hash shows reference; use hash-keys/hash-values to extract content" |
| `<list[N]>` | "display list shows reference; iterate or use (display-ln ...)" |
| 空输出 | "no display output; add (display ...) to show the result" |
| 输出有但缺关键词 | "missing expected {keyword} in output" |
| 结构对但值不对 | "function structure looks correct but values differ" |
| 多个 display 混在一起 | "concatenate display outputs correctly; use (newline) for separation" |

### 3.2 滞后保护 (Hysteresis)

防止在 coarse ↔ fine 之间来回震荡：

```python
MIN_COARSE_ATTEMPTS = 2     # 最少尝试 2 次 coarse
MIN_FINE_ATTEMPTS = 2       # 最少尝试 2 次 fine
STAY_COARSE_UNTIL = 0.15    # 至少 15% 匹配才 switch to fine
STAY_FINE_AFTER = 0.30      # 低于 30% 不回 coarse（直接 fine 继续）
```

---

## 4. 自适应增益调度 (Scheduled Gains)

类似 PID 控制器中增益随误差调整：

| 阶段 | 控制参数 | 值 | 含义 |
|------|----------|-----|------|
| coarse | `temperature` | 0.3→0.7 | 值越高 LLM 创造性越强，适合大改 |
| coarse | `max_tokens` | 4096 | 允许大段输出 |
| fine | `temperature` | 0.2→0.3 | 降温度，精密调整 |
| fine | `max_tokens` | 2048 | 限制输出长度 |
| putt | `temperature` | 0.1 | 最低温，确定性修改 |
| putt | `max_tokens` | 1024 | 最小输出量 |

### 4.1 随时间衰减 (Integral-like)

每次 attempt 后调整参数：

```
temperature = base_temp + 0.1 * attempt_count
max_tokens = base_tokens / attempt_count
```

早期 attempt：高温度 + 大输出 → explore  
后期 attempt：低温度 + 小输出 → exploit

---

## 5. 两套 fixer template

### 5.1 Coarse fixer（当前实现）

```scheme
(lambda (code err goal)
  (json-get-string (aura-llm-call (json-encode (hash
    "model" "deepseek-v4-flash"
    "messages" (list
      (hash "role" "system" "content" __sp__)
      (hash "role" "user" "content"
        (begin
          (set-code code)
          (let ((src (current-source)))
            (string-append
              "=== Source ===\n" src
              "\n=== Error ===\n" err
              "\n=== Goal ===\n" goal)))))
    "temperature" 0.3
    "max_tokens" 4096))) "content")))
```

### 5.2 Fine fixer（新增）

```scheme
(lambda (code err goal actual expected diag)
  (json-get-string (aura-llm-call (json-encode (hash
    "model" "deepseek-v4-flash"
    "messages" (list
      (hash "role" "system" "content" __sp__)
      (hash "role" "user" "content"
        (begin
          (set-code code)
          (let ((src (current-source))
                (n-calls (length (query:find "Call")))
                (n-vars (length (query:find "Variable"))))
            (string-append
              "=== Source ===\n" src
              "\n=== AST Stats ===\ncall-sites: " (number->string n-calls)
              " variables: " (number->string n-vars)
              "\n=== Actual Output ===\n" actual
              "\n=== Expected ===\n" expected
              "\n=== Diagnosis ===\n" diag
              "\n=== Goal ===\n" goal)))))
    "temperature" 0.2
    "max_tokens" 2048))) "content")))
```

### 5.3 Putt fixer（未来）

```scheme
(lambda (code err goal node-id current-value target-value)
  ;; 使用 EDSL 表达式级修改
  (begin
    (set-code code)
    (let ((expr (ast:node-source node-id)))
      (json-get-string (aura-llm-call (json-encode (hash
        "model" "deepseek-v4-flash"
        "messages" (list
          (hash "role" "system" "content" __sp__)
          (hash "role" "user" "content"
            (string-append
              "=== Target Expression ===\n" expr
              "\n=== Current Value ===\n" current-value
              "\n=== Expected Value ===\n" target-value
              "\nFix this expression only. Use (mutate:rebind " node-id " new-expr)")))
        "temperature" 0.1
        "max_tokens" 1024))) "content"))))
```

---

## 6. 系统 API Reference 注入

### 6.1 动态生成

按任务依赖的 std 模块，生成精简版 API 参考：

```python
API_REF = {
    "std/hash": """
=== std/hash ===
(hash key val ...)        → <hash[N]> 创建 hash
(hash-ref hash key)       → value 或 ()  (不抛出异常)
(hash-set! hash key val)  → void    修改 hash（副作用）
(hash-keys hash)          → list    返回 key 列表
(hash-values hash)        → list    返回 value 列表
(hash-size hash)          → int     entry 数量
(hash->alist hash)        → list    ((key . val) ...)
(display <hash>)          → <hash[N]> 不是内容！用 hash-keys/values
""",

    "std/list": """
=== std/list ===
(filter pred lst)         → list    (pred 是 -> bool 的函数)
(map fn lst)              → list
(foldl fn init lst)       → any
(for-each fn lst)         → void   副作用，不返回新 list
(range start end)         → list   [start, end)
(append a b)              → list
(reverse lst)             → list
(length lst)              → int
(display <list>)          → (1 2 3) 直接显示内容
""",

    "std/llm": """
=== std/llm ===
(aura-llm-call json-body) → string  LLM 调用，读 LLM_API_KEY 环境变量
(aura-verify code)        → "#t" | error-string
(compiler:set-code code)  → bool   设置并解析代码
(compiler:write code)     → void   设置代码并立即 eval
""",
}
```

### 6.2 注入时机

在 `build_sys_prompt` 末尾追加：

```python
def build_sys_prompt(stdlib, api_ref, task_name=None):
    sp = "... [base prompt] ..."
    # ... task-specific hints ...
    
    # Append API reference for loaded modules
    for mod in stdlib:
        if mod in API_REF:
            sp += API_REF[mod]
    
    return sp
```

---

## 7. 实现架构

### 7.1 整体流程

```
Python 层                           Aura 层
═══════════                        ════════

loop attempts:
  │
  │  如果阶段切换：
  │    - 更新 __gen__/__fix__ template
  │    - 调整 temperature/max_tokens
  │    - 注入对应 system prompt
  │
  ├── run intend (coarse fixer) ──→  Aura: generator → compile → fix
  │                                    │
  │                                    │ success / fail after n attempts
  │                                    ▼
  ├── parse output
  │
  ├── measure_distance(rc, out, expected)
  │     │
  │     ├── "coarse" → retry (same phase)
  │     │
  │     ├── "fine"   → switch to fine fixer
  │     │               enriched system prompt
  │     │               structured diagnosis
  │     │               ↓
  │     ├── run intend (fine fixer) ──→  Aura: generator → compile → fix
  │     │                                  ↑ 使用 fine 版 prompt
  │     │
  │     ├── "putt"   → switch to putt fixer
  │     │               ↓
  │     ├── run intend (putt fixer) ──→  Aura: EDSL 定点修改
  │     │
  │     └── success → return
  │
  └── max attempts → return failure
```

### 7.2 Python 层改动 (`edsl_benchmark.py`)

```python
def classify_error(rc, output, expected):
    if rc != 0:
        return ("coarse", 0.0, "compile-error")
    if not output or output.isspace():
        return ("coarse", 0.0, "empty-output")
    norm_out = output.lower()
    matches = sum(1 for kw in expected if kw.lower() in norm_out)
    ratio = matches / max(len(expected), 1)
    if ratio >= 0.85:
        return ("putt", ratio, "near-match")
    elif ratio > 0.0:
        return ("fine", ratio, "partial-match")
    # check if output structure suggests a framework
    if output.startswith('<hash') or output.startswith('#'):
        return ("fine", 0.0, "structure-correct-content-wrong")
    return ("coarse", 0.0, "complete-mismatch")


def structured_diagnosis(output, expected):
    diags = []
    missing = [kw for kw in expected if kw.lower() not in output.lower()]
    if missing:
        diags.append(f"Missing in output: {missing}")
    if '<hash' in output:
        diags.append("display hash shows <hash[N]> reference, not content. "
                     "Use hash-keys or hash-values to extract.")
    if output == '' or output == '()':
        diags.append("Output is empty. Did you forget (display ...)?")
    return "\\n".join(diags) if diags else "Output format mismatch."
```

### 7.3 策略切换逻辑

```python
def run_intend_adaptive(name, goal, expected, stdlib):
    phase = "coarse"
    attempts_in_phase = 0
    max_attempts_per_phase = 3
    
    while True:
        # Build phase-specific prompt and fixer
        if phase == "coarse":
            sys_prompt = build_sys_prompt(stdlib, "", coarse_template)
            temp, tokens = 0.3, 4096
        elif phase == "fine":
            diag = structured_diagnosis(last_output, expected)
            sys_prompt = build_sys_prompt(stdlib, diag, fine_template)
            sys_prompt += get_api_ref_for_task(name, stdlib)
            temp, tokens = 0.2, 2048
        else:  # putt
            sys_prompt = build_putt_prompt(last_output, expected)
            temp, tokens = 0.1, 1024
        
        # Run intend with these params
        success, output, attempts = run_intend(
            sys_prompt, temp, tokens, max_att=max_attempts_per_phase)
        
        if success:
            return True
        
        # Measure distance and switch phase if needed
        new_phase, ratio, _ = classify_error(None, output, expected)
        attempts_in_phase += 1
        
        if new_phase != phase and attempts_in_phase >= MIN_ATTEMPTS[phase]:
            phase = new_phase
            attempts_in_phase = 0
        elif attempts_in_phase >= max_attempts_per_phase * 2:
            break  # give up
    
    return False
```

---

## 8. 预期效果

| 指标 | 当前值 | 预计值 |
|------|--------|--------|
| 过率 (1 round) | 52/57 (91%) | 55-57/57 (96-100%) |
| 平均 attempts | 1.4 | 2.1（前期多尝试但更多成功） |
| hash-invert 修复后 | ✅ | ✅ |
| is-anagram | ❌ (4 att) | → ✅（fine 阶段给 hash equal? 诊断） |
| list-zip | ❌ (4 att) | → ✅（fine 阶段给出三行 display 示例） |
| primes-list | ❌ (4 att) | → 可能 ✅（看 LLM 能否理解筛法） |
| quicksort | ❌ (4 att) | → 可能 ✅（fine 阶段显示部分排序结果） |
| tcp-connect | ❌ (4 att) | → 可能 ✅（fine 阶段带 HTTP 响应格式说明） |

### 8.1 失败率消减归因

| 任务 | 失败原因 | 控制策略 | 预期修复路径 |
|------|----------|----------|-------------|
| is-anagram | hash `equal?` 语义 | fine + API ref | 看到 `hash->alist` 比较 |
| list-zip | 多格式 display | fine + 诊断 | 诊断提示"需要 3 个 display" |
| primes-list | 算法 bug | fine | 看源码后修复循环 |
| quicksort | 部分排序 | fine | 看递归代码后修复 base case |
| tcp-connect | HTTP 解析 | fine + API ref | 带 HTTP 响应格式模板 |

---

## 9. 架构决策 — 控制模块写在 Aura 层

### 9.1 三层的职责边界

```
┌─────────────────────────────────────┐
│  Python 外层 (edsl_benchmark.py)     │  编排层（shell）
│                                     │
│  run_intend_adaptive()               │
│  ├── 按 phase 调 temperature/tokens  │
│  ├── 选 __gen__ / __fix__ template   │
│  └── 调用 adaptive.aura 作决策       │
└──────────────┬──────────────────────┘
               │ 调用
               ▼
┌─────────────────────────────────────┐
│  lib/std/adaptive.aura               │  策略层（Aura stdlib）
│  (measure-distance rc out exp)       │
│    → (phase ratio diagnosis)         │
│  (structured-diagnosis out exp)      │
│    → 诊断文本                        │
│  (get-api-ref mod ...)               │
│    → API 参考文本                     │
│                                     │
│  不依赖编译器内部，纯数据变换          │
└──────────────┬──────────────────────┘
               │ 通过现有原语调用
               ▼
┌─────────────────────────────────────┐
│  编译器 C++                           │  传感器层（不修改）
│                                     │
│  intend 原语                          │
│  aura-verify                          │
│  current-source / query:find          │
│  typecheck-current / eval-current     │
└─────────────────────────────────────┘
```

### 9.2 为什么放 Aura 而不是 Python 或 C++

| 维度 | C++ 编译器 | Python benchmark | Aura stdlib ✅ |
|------|-----------|----------------|---------------|
| 热迭代 | ❌ 每次 rebuild | ✅ | ✅ 不需编译 |
| 复用性 | ❌ 绑死编译器 | ❌ 绑死 benchmark | ✅ 所有工具共用 |
| 测试 | ❌ 编译测试慢 | ✅ Python test 快 | ✅ Aura test 快 |
| E4 兼容 | ❌ | ❌ | ✅ `define-strategy` 直接对接 |
| 解耦度 | ❌ 最差 | ❌ 中度 | ✅ 最优 |
| LLM 可读 | ❌ | ✅ | ✅ 自己能读自己 |
| 运行时成本 | ❌ 膨胀编译器 | ❌ 子进程调用 | ✅ 同进程函数调用 |

### 9.3 Python 层只做编排

Python 侧最小必要职责：

```python
def run_intend_adaptive(name, goal, exp_str, stdlib):
    phase, ratio, diag = call_adaptive_measure_distance(rc, out, exp_str)
    if phase == "coarse":
        temp, tokens = 0.3, 4096
        sys_prompt = build_coarse_prompt(goal)
    elif phase == "fine":
        temp, tokens = 0.2, 2048
        api_ref = call_adaptive_get_api_ref(stdlib)
        sys_prompt = build_fine_prompt(goal, diag, api_ref)
    else:  # putt
        temp, tokens = 0.1, 1024
        sys_prompt = build_putt_prompt(...)

    result = run_intend(sys_prompt, temp, tokens)
    return result
```

### 9.4 与 E2-E4 的关系

```
E2 (define-strategy / register-strategy!)
    │
    ├── adaptive.aura 可注册为一个正式 strategy
    │   (register-strategy! "adaptive-golf" adaptive-strategy)
    │
    ├── E3 analytics 用来评估 phase 切换是否有效
    │
    ├── E4 evolve-strategy 可在 benchmark 跑完后
    │   调整 measure-distance 的阈值
    │
    └── 自适应 intend 是策略调度层，不是新原语
```

---

## 10. 路线图

### Phase 1: lib/std/adaptive.aura（低工作量）

Aura 模块实现核心控制逻辑：

- [x] `(measure-distance rc output expected-list)` → `(phase ratio diag)`
- [x] 规则表：`<hash[N]>`、`<list[N]>`、空输出、编译错误、结构正确值错误
- [x] `(structured-diagnosis output expected-list)` → 结构化诊断文本
- [x] `(get-api-ref stdlib-list)` → 按模块生成 API 参考

```scheme
;; 使用示例
(let ((distance (measure-distance 0 "<hash[2]>" '("hello" "foo"))))
  ; → (phase: "fine" ratio: 0.0 diag: "display hash shows <hash[N]>...")
  (case (phase distance)
    ("coarse" ...)   ; 完整重写
    ("fine" ...)     ; 精修
    ("putt" ...)))   ; 定点修改
```

### Phase 2: Python 编排层（低工作量）

- [ ] `run_intend_adaptive()` 调用 adaptive.aura 的 measure-distance
- [ ] 按 phase 选择 fixer template / temperature / max_tokens
- [ ] 滞后保护：MIN_COARSE_ATTEMPTS=2, MIN_FINE_ATTEMPTS=2

### Phase 3: API Reference 注入（中工作量）

- [ ] adaptive.aura 的 `get-api-ref` 输出注入到 system prompt
- [ ] 按任务依赖裁剪

### Phase 4: putt / EDSL 定点修改（高工作量）

- [ ] 确保 EDSL `query:find` / `mutate:rebind` 稳定可用
- [ ] putt fixer template
- [ ] 表达式级 diff 生成

---

## 附录 A: 完整 classify_error 参考

```python
DIAGNOSIS_RULES = [
    # (pattern, phase, diagnosis)
    ("<hash", "fine", 
     "display hash shows <hash[N]> — not the content. "
     "Use (hash-keys ...) or (hash-values ...) to extract."),
    ("<list", "fine",
     "display list shows <list[N]> — summary not content. "
     "Use (for-each display lst) to show each element."),
    ("#(", "fine",
     "Output is a raw vector/struct. "
     "Use (display ...) on specific fields."),
    ("error: unbound", "coarse",
     "Compiler error: unbound variable. Check function names."),
    ("error: parse", "coarse",
     "Syntax error: unbalanced parentheses or bad syntax."),
]
```

## 附录 B: 高尔夫比喻映射表

```
真实高尔夫        intend 修复
──────────        ────────────
开球 (Tee)        第一次生成代码 (wide open)
球道 (Fairway)    代码编译通过 (on the right track)
果岭 (Green)      输出接近预期 (near the hole)
推杆 (Putt)       最后几个字符的调整 (fine-tuning)
打偏 (Slice)      LLM 生成方向完全错了 (wrong approach)
球道沙坑          编译错误但框架对 (minor compile error)
果岭沙坑          输出格式对但值不对 (format correct, value wrong)
3 推 (3-putt)     3 次 attempt 都没完全修好
OB (Out of Bounds) LLM 去生成完全无关的代码
标准杆 (Par)      n attempts 完成 = 正常发挥
小鸟 (Birdie)     1 attempt 就完美通过
柏忌 (Bogey)      用了很多 attempt 才过
双柏忌 (Double)   超过 max attempts 放弃
```
