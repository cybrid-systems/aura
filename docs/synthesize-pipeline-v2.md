# Synthesize Pipeline v2 — 设计文档

**2026-05-26 | 状态: 设计中 | 优先级: P2**

---

## 1. 动机

当前 synthesize 管线只能操作单个表达式/define，依赖外部 Python 脚本跑 benchmark。目标是把 benchmark 的"意图 → 生成 → 测试 → 修复"循环内建到 Aura 本身，实现 **self-hosting code synthesis**。

### 现有局限

| 问题 | 表现 |
|:-----|:------|
| 无测试 | `synthesize:optimize` 只用 synthetic probes（0, 1, -1, 2），无法验证真实行为 |
| 一次性生成 | `synthesize:define` 调一次 LLM 就完事，错误不修复 |
| 单文件 | 所有生成的代码在单 workspace，无法引用跨文件定义 |
| 外部依赖 | benchmark 循环（intend）在 Python 里，Aura 自身没有代码验证循环 |

## 2. 核心概念

```
输入: 需求 + 测试用例
       ↓
    [生成器] ←── LLM (std/llm) ←── 迭代反馈
       ↓
    [验证器] —→ 测试通过？——→ ✅ 输出代码
       ↓ 否
    [诊断器] —→ pid:analyze → 反馈文本
       ↓
    返回生成器，用反馈改进
```

### 2.1 三元组

每个合成任务的核心：

```
(goal, generator, verifier)
```

- **goal** — 人类可读的描述（给 LLM 的 prompt）
- **generator** — 生成代码的策略（LLM / 模板 / 遗传 / 组合）
- **verifier** — 验证代码正确性的方式（测试用例 / typecheck / benchmark probes）

### 2.2 验证器类型

```
Verifier ::= TestSuite | TypeCheck | Benchmark
```

- **TestSuite** — 一组 `(input → expected-output)` 对
- **TypeCheck** — 编译通过即可
- **Benchmark** — `synthesize:optimize` 风格的 synthetic probes

### 2.3 循环

```
attempt n:
  1. generator(code[n-1], feedback[n-1])  →  code[n]
  2. verifier(code[n])                      →  pass/fail + 诊断
  3. if pass: return code[n]
  4. pid:analyze(output, expected)          →  feedback[n]
  5. if attempts < max: goto 1
  6. return best attempt
```

## 3. API 设计

### 3.1 `synthesize:test-driven` — 核心入口

```scheme
(synthesize:test-driven "factorial"
  :goal "Implement factorial: n! = n * (n-1) * ... * 1, with 0! = 1"
  :tests '((5 120) (0 1) (3 6))
  :max-attempts 5
  :model "grok-4.3")
```

**参数：**

| 参数 | 类型 | 默认 | 说明 |
|:-----|:-----|:-----|:------|
| `name` | string | 必填 | 生成的函数名 |
| `:goal` | string | `""` | 人类可读的目标描述 |
| `:tests` | list | `()` | `((input ... expected) ...)` 测试用例 |
| `:code` | string | `""` | 初始代码（可选，默认空） |
| `:max-attempts` | int | `5` | 最大尝试次数 |
| `:model` | string | `"deepseek-chat"` | LLM 模型 |
| `:strategy` | symbol | `:llm` | 生成策略（:llm / :mutate / :template） |

**返回值：** `(success? code-string test-results)`

**实现流程：**

```
1. 检查 name 是否已有定义，有则作为 baseline
2. 构建测试评估函数:
   (lambda (code)
     (set-code code)
     (eval-current)
     (for-each test-case (eval test) vs expected))
3. 主循环:
   a. 如果首次或全部失败 → 调 LLM 生成新代码
   b. 如果部分通过 → 用 pid:analyze 诊断 → 调 LLM 修复
   c. 验证 → 统计通过/失败
   d. 全部通过 → 返回成功
   e. 达到 max-attempts → 返回最佳结果
```

### 3.2 `synthesize:project` — 跨文件合成

```scheme
(synthesize:project "csv-parser"
  :files '(("core.aura" "(define (parse-csv s) ...)")
           ("test.aura" "(define (test-csv) (display (parse-csv ...)))"))
  :main "test.aura"
  :goal "Parse comma-separated values using std/string functions"
  :tests '("(display (parse-csv \"a,b,c\"))" → "(a b c)")
  :max-attempts 5)
```

**参数：**

| 参数 | 类型 | 说明 |
|:-----|:-----|:------|
| `name` | string | 项目名称 |
| `:files` | list | `(filename initial-code) ...` 文件列表 |
| `:main` | string | 主入口文件名 |
| `:goal` | string | 项目描述 |
| `:tests` | list | 测试用例列表 |
| `:max-attempts` | int | 每文件最大重试 |

**实现流程：**

```
1. 为每个文件创建子 workspace（workspace:create 或临时 flat）
2. 逐文件合成:
   a. 切换 workspace 到目标文件
   b. 用 test-driven 合成该文件内容
   c. 合并回主 workspace
3. 跨文件测试: 所有文件 set-code + eval-current 一次
```

### 3.3 `synthesize:debug` — 自动修复

```scheme
(synthesize:debug
  :code "(define (factorial n) (if (= n 0) 1 (* n (factiorial (- n 1)))))"
  :tests '((5 120) (0 1))
  :max-attempts 3)
```

**诊断→修复映射：**

| 错误类型 | 诊断 | 修复策略 |
|:---------|:------|:---------|
| unbound variable | 变量名拼写错误 | 找最近似的已定义变量名替换 |
| type error | 类型签名不匹配 | 添加类型转换或修正注释 |
| parse error | 语法错误 | 检查括号配对、引号转义 |
| output mismatch | 逻辑错误 | 用 pid:analyze 诊断 → LLM 修复 |

### 3.4 `synthesize:compose` — 策略组合

```scheme
(synthesize:compose "my-fn"
  :phases '(
    (:template "fib-template" :args '(n))
    (:llm :prompt "Optimize for tail recursion")
    (:mutate :strategy "swap-operators")
    (:test-driven :tests '((5 120) (0 1))))
  :max-attempts 3)
```

允许管线中混合多种生成策略。

## 4. 内部模块结构

```
lib/std/synthesize-v2.aura       # 主模块（public API）
lib/std/synthesize-core.aura     # 核心循环逻辑（内部）
lib/std/synthesize-test.aura     # 测试验证器（内部）
lib/std/synthesize-llm.aura      # LLM 生成器（内部）
lib/std/synthesize-mutate.aura   # 遗传变异生成器（内部）
```

### 4.1 主循环（synthesize-core.aura）

```
;; 核心迭代循环
(define (synthesize:iter goal-name code tests max-attempts model)
  (let loop ((attempt 1) (best-code code) (best-score -1))
    (if (> attempt max-attempts)
      (list #f best-code best-score)
      (let* ((result (synthesize:run-all-tests code tests))
             (score (car result))
             (details (cadr result)))
        (if (= score 100)
          (list #t code score)
          (let* ((output (synthesize:capture-output code))
                 (diagnosis (pid:analyze output (synthesize:expected-outputs tests)))
                 (feedback (caddr diagnosis))
                 (new-code (synthesize:llm-generate goal-name code feedback model)))
            (loop (+ attempt 1) new-code score)))))))
```

### 4.2 测试验证器（synthesize-test.aura）

```
;; 运行所有测试用例
(define (run-all-tests code tests)
  (set-code code)
  (eval-current)
  (let ((passed 0) (total (length tests)))
    (for-each
      (lambda (test)
        (let* ((input (car test))
               (expected (cadr test))
               (result (eval-input input)))
          (if (equal? result expected)
            (set! passed (+ passed 1))
            (display "  FAIL: " input " → " result ", expected " expected "\n"))))
      tests)
    (list (/ passed total 100) ....)))
```

### 4.3 LLM 生成器（synthesize-llm.aura）

```
;; 调用 LLM 生成/修复代码
(define (llm-generate name code feedback model)
  (let* ((prompt (build-prompt name code feedback))
         (response (aura-llm-call (build-json-body prompt model))))
    (extract-code response)))
```

## 5. 与现有系统的关系

### 向后兼容

- `synthesize:pipeline` 保持现有 API，新 API 作为 `synthesize-v2.aura` 添加
- `synthesize:optimize` 保留，底层 fitness 函数可复用 `synthesize:test-driven` 的测试验证器
- `eval-current-output` 和 `pid:analyze` 已存在，直接使用

### 依赖链

```
synthesize-v2
  ├── std/llm (aura-llm-call)
  ├── std/adaptive (pid:analyze, structured-diagnosis)
  ├── eval-current, eval-current-output, set-code (core primitives)
  └── json-encode (std/json) — 构建 LLM 请求体
```

## 6. 进度

| Phase | 状态 | 内容 |
|:------|:----:|:------|
| 1 | ✅ | `synthesize:test-driven` 基础 + `eval-test` / `run-tests` |
| 2 | ✅ | LLM 循环 + JSON 解析 + `call-llm` / `extract-code` |
| 3 | 🔧 | 跨文件合成 + workspace 管理 + 策略组合 |
| 4 | ⬜ | 替换外部 benchmark |

---

## 7. Phase 3 详细设计 — 跨文件合成

### 7.1 场景分析

**场景 A：多文件项目**

```
workspace/
  core.aura     →  (define (parse-csv s) (string-split s ","))
  test.aura     →  (require "core" all:) (display (parse-csv "a,b,c"))
```

LLM 需要看到 workspace 中已有定义才能跨文件引用。

**场景 B：用户提供既有代码修复**

```scheme
(synthesize:debug :code "(define (fact n) (if (= n 0) 1 (* n (factiorial (- n 1)))))"
  :tests '("(display (fact 5))" "120"))
```

优先走规则修复（typo 检测），不行再调 LLM。

**场景 C：多策略组合**

LLM 生成骨架 → 遗传优化常数 → 模板填空 → 测试验证

---

### 7.2 Workspace 管理层

`synthesize:project` 的核心是 workspace 切换：

```scheme
;; 内部流程：
1. workspace:create 为每个文件创建子 workspace
2. workspace:switch 切换到目标文件 → synthesize:test-driven → 切换回根
3. 跨文件测试：require 加载所有文件 → eval-current-output
4. 失败时 workspace:discard 回退
```

**已存在的 workspace 原语：**

| 原语 | 说明 |
|:-----|:------|
| `workspace:create name` | 创建子 workspace（COW，改造时克隆）|
| `workspace:switch id` | 切换到指定 workspace |
| `workspace:current` | 返回当前 workspace ID |
| `workspace:list` | 列出所有 workspace |
| `workspace:delete id` | 删除 workspace |
| `workspace:lock id` | 锁（只读/读写）|
| `workspace:discard id` | 丢弃子 workspace 更改 |
| `workspace:merge child-id` | 合并子 workspace 到父 |
| `workspace:sync-from src sym` | 从源 workspace 同步定义 |

---

### 7.3 `synthesize:project` — 跨文件项目合成

```scheme
(synthesize:project "csv-parser"
  :files '(("core.aura" :goal "parse CSV using std/string")
           ("test.aura" :goal "test parser" :main #t))
  :tests '(("(display (parse-csv \"a,b,c\"))" "(a b c)")))
```

**参数：**

| 参数 | 类型 | 说明 |
|:-----|:-----|:------|
| `name` | string | 项目名称 |
| `:files` | list | `(filename :goal "..." [:main #t])` 文件列表 |
| `:tests` | list | 集成测试用例，跑在所有文件上 |
| `:max-attempts` | int | 每文件最大重试数 |
| `:model` | string | LLM 模型 |

**实现步骤：**

```
1. workspace:create 为每个文件创建子 ws
2. 对每个文件：
   a. workspace:switch 到子 ws
   b. workspace:sync-from 导入已有文件定义
   c. synthesize:test-driven
   d. 失败 → workspace:discard 回退
3. 集成测试：切回根 ws，require 所有文件，跑跨文件测试
4. 成功 → workspace:merge 所有子 ws 到根
```

---

### 7.4 `synthesize:debug` — 自动修复（升级）

**诊断→修复规则：**

| 错误模式 | 检测 | 修复 |
|:---------|:------|:------|
| unbound variable (typo) | Levenshtein 距离 < 3 | 替换为最近似已定义名 |
| unbound variable (missing require) | 变量名在 stdlib 模块中 | 添加 require |
| parse error | 括号不匹配 | 重新解析 |
| type error | 函数签名 | 类型转换 |

先走规则，无匹配再调 LLM。

---

### 7.5 `synthesize:compose` — 策略组合

```scheme
(synthesize:compose "fib"
  :phases '(
    (:llm :prompt "generate fibonacci")
    (:mutate :strategy "numeric")
    (:test-driven :tests '("(display (fib 10))" "55"))))
```

每个 phase 的输出（代码）→ 下一个 phase 的输入。最终跑测试。

---

### 7.6 实现顺序

| 步骤 | 内容 | 依赖 |
|:----:|:------|:-----|
| 7.6.1 | workspace 管理层 — workspace 切换 + 同步 | 已有 workspace 原语 |
| 7.6.2 | `synthesize:project` 外壳 — 多文件编排 | 7.6.1 |
| 7.6.3 | `synthesize:debug` 规则修复 | test-driven 基本循环 |
| 7.6.4 | `synthesize:compose` — 策略组合 | 7.6.2 + 7.6.3 |

## 8. 风险与权衡

| 风险 | 缓解 |
|:-----|:------|
| LLM 调用在 Aura 端阻塞 serve 协议 | 已有时钟中断/超时机制；可加 fiber yield |
| stdout 捕获在 `eval-current-output` 已实现 | 直接使用 |
| Pipeline rollback 靠 `ast:snapshot` | 已实现且稳定 |
| LLM JSON body 在 std/llm 已封装 | 直接使用 json-encode |
| prompt 注入/代码提取稳定性 | 逐步从简单模板开始，验证后再复杂化 |
| 多个 workspace 之间代码同步 | `workspace:sync-from` 已实现 |
