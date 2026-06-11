# Synthesize Strategies — 多策略代码生成

**Status**: Design (2026-05-25)
**Design Author**: Ani
**Driver**: 受控代码生成（EDSL Roadmap W9-10）

## 1. Problem

当前 `intend` 提供生成→验证→修复循环，但 generator 是单个函数（通常是 LLM）：

```lisp
(intend goal generator verifier [fixer] [max-attempts])
```

局限性：
- **LLM 不是万能**：简单变换（rename、inline）调 LLM 太贵太慢
- **策略不可选**：不同任务需要不同策略
- **策略不可组合**：多步骤任务无法编排
- **策略不可进化**：不能根据历史表现自动切换

## 2. 策略分类

### 2.1 Template Strategy — 模板实例化

**适用**：已知结构的代码生成（boilerplate、API 包装、数据层）

```
(synthesize:register-template 'get-handler
  "(define (handle-{name} req)
     (let ((data (db/query \"{query}\")))
       (json-response data)))"
  :params '(name query))

(synthesize:fill 'get-handler
  '{name "users" query "SELECT * FROM users"})
→ (define (handle-users req)
    (let ((data (db/query "SELECT * FROM users")))
      (json-response data)))
```

**性能**：纳秒级，零外部依赖。

### 2.2 LLM Strategy — 大模型生成

**适用**：新功能开发、复杂算法、自然语言描述→代码

```
(synthesize:define 'quicksort
  '((Vec Int) -> Vec Int)
  :strategy 'llm
  :model "deepseek-chat"
  :examples '(([3 1 2] -> [1 2 3])))
```

当前 intend 已覆盖此场景。增强点：
- 多模型 fallback（deepseek → grok → minimax）
- 示例驱动（few-shot）
- 约束描述（`:constraints '((no-alloc) (stack-only))`）

### 2.3 Genetic Strategy — 遗传变异

**适用**：benchmark 驱动的优化（调参、循环变换、inline/outline）

```
synthesize:optimize 'fib :strategy 'genetic
  :population 50 :generations 100
  :fitness-fn (lambda (code) (benchmark code)))
```

不需要 LLM，纯 AST 变异 + fitness 函数。

### 2.4 Pipeline Strategy — 组合策略

**适用**：复杂任务需要多步骤、多策略合作

```
Task: "实现带缓存的 HTTP 客户端"

Step 1 (Template):      骨架生成 → (define (fetch url) ...)
Step 2 (LLM):           核心逻辑 → HTTP GET + parse
Step 3 (Genetic):       缓存优化 → memoization
Step 4 (LLM Fixer):     错误处理 → timeout + retry
```

### 2.5 Hybrid Strategy — 混合策略

**适用**：结构化输出 + 灵活性

```
Template 生成骨架 → LLM 填空 → Template 格式化
```

## 3. 策略选择引擎

### 3.1 规则引擎（P0）

```lisp
(rule:define 'strategy-select
  (match task
    ((:type :refactor :kind :rename)     → :template "rename-var")
    ((:type :optimize :target :speed)    → :genetic)
    ((:type :new-code :complexity :high) → :llm)
    ((:type :new-code :complexity :low)  → :template "boilerplate-fn")
    (_ → :llm)))
```

### 3.2 历史驱动（P1）

从 `intend-analytics` 读取各策略的历史成功率，选择同类任务中最优策略。

### 3.3 自适应进化（P2）

策略选择器本身也是 Aura 代码，可被 mutate 编辑。

## 4. EDSL API

| 原语 | 功能 | 策略 |
|:----|:------|:-----|
| `synthesize:define` | 代码生成 | 多策略 |
| `synthesize:fill` | 填空式合成 | template + LLM |
| `synthesize:optimize` | benchmark 优化 | genetic |
| `synthesize:pipeline` | 多步骤组合 | composition |
| `synthesize:register-template` | 注册模板 | template |
| `synthesize:history` | 策略历史分析 | 所有 |

## 5. 与现有系统集成

```
intend 循环: generator → verifier → fixer
                │
      synthesize: llm / template / genetic / hybrid

增量编译: synthesize:define → AST → typecheck → eval-current → feedback

workspace: synthesize:optimize → workspace:create-child
                               → mutate → eval → merge if fitness improves
```

## 6. 实现路标

### P0 — Template（1 天）

| 任务 | 说明 |
|:----|:------|
| `synthesize:register-template` | 注册命名模板（字符串 + 参数列表） |
| `synthesize:fill` | 匹配模板、替换参数、eval 到 workspace |
| 参数替换引擎 | `{name}` → 实际值，支持默认值 |

### P1 — LLM Strategy（1 天）

| 任务 | 说明 |
|:----|:------|
| `synthesize:define` 包装 intend | 自动构建 generator/verifier/fixer |
| 多模型支持 | `:model` 参数，fallback 链 |

### P2 — Pipeline（2 天）

| 任务 | 说明 |
|:----|:------|
| `synthesize:pipeline` | 步骤编排 + 中间结果传递 |
| 策略选择器 | 从规则引擎或历史选择策略 |

### P3 — Genetic（3 天）

| 任务 | 说明 |
|:----|:------|
| `synthesize:optimize` | 种群管理 + fitness 计算 |
| 变异算子 | 复用现有 mutate 原语 |
