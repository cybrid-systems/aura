# Aura AI Agent 协议

**为 LLM / AI Agent 设计的 Aura 操作协议**

> Aura 的核心差异化优势：模型不需要硬记语法，只需要学会怎么指挥 Aura 自己去变换代码。

---

## 1. 核心原则

### 1.1 工具优先于生成

**不要**让模型直接写完整代码。
**要**让模型通过工具调用操作 Aura 的查询/变换/修复引擎。

```
❌ 模型生成代码 → 人类手动运行 → 发现错误 → 修改
✅ 模型 → tool: aura --query → 理解结构 → tool: aura --fix → 验证结果
```

### 1.2 观察 → 诊断 → 操作 → 验证

每次交互遵循四步循环：

```
1. OBSERVE   — tool: aura --query / aura --typecheck / aura --serve
2. DIAGNOSE  — 分析返回的 AST 或诊断信息
3. ACT       — tool: aura --query-and-fix / 输出变换指令
4. VERIFY    — tool: aura --ir / aura --typecheck 验证结果
```

### 1.3 利用 Aura 的自省能力

Aura 是 Homoiconic — 代码即数据。类型、AST、查询结果都是一等 S-表达式。
模型需要利用这一点，而不是试图用自己的权重记忆所有语法。

---

## 2. 工具定义

Aura CLI 的每个标志都是一个工具。以下是标准工具接口：

### Tool: `aura_query`

```json
{
  "name": "aura_query",
  "description": "查询 Aura AST 节点，支持类型/调用/变量引用等过滤条件",
  "parameters": {
    "code": "string — 要查询的 Aura 代码",
    "query": "string — 查询表达式，如 (node-type Call), (has-type? Int)"
  },
  "returns": "匹配计数 + 匹配节点摘要"
}
```

示例调用：
```
> aura_query(code="(+ 1 2)", query="(node-type LiteralInt)")
→ "query: 2 matches"

> aura_query(code="(if (string? x) (string-append x \"!\") x)", query="(has-type? String)")
→ "query: 1 matches"
```

### Tool: `aura_query_and_fix`

```json
{
  "name": "aura_query_and_fix",
  "description": "查询 AST 并应用变换修复",
  "parameters": {
    "code": "string — 要变换的 Aura 代码",
    "match": "string — 匹配模式，如 (node-type LiteralInt)",
    "replace": "string — 替换模式，如 (LiteralInt 99)"
  },
  "returns": "applied=true/false + 变换后的代码"
}
```

示例调用：
```
> aura_query_and_fix(code="(+ 1 2)", match="(node-type LiteralInt)", replace="(LiteralInt 99)")
→ "applied=true, result: (+ 99 2)"
```

### Tool: `aura_typecheck`

```json
{
  "name": "aura_typecheck",
  "description": "对代码执行类型检查，返回每个表达式的类型",
  "parameters": {
    "code": "string — 要检查的 Aura 代码"
  },
  "returns": "类型诊断，每行格式: expression : type"
}
```

示例调用：
```
> aura_typecheck(code="(+ 1 2)")
→ "type: Int"

> aura_typecheck(code="(+ \"a\" 1)")
→ "error: argument 1: coercion from String to Int at 1:2"
```

### Tool: `aura_ir`

```json
{
  "name": "aura_ir",
  "description": "通过 IR 管线执行代码（支持闭包、递归、letrec）",
  "parameters": {
    "code": "string — 要执行的 Aura 代码",
    "inspect": "bool (可选) — 是否输出闭包/env 内省信息"
  },
  "returns": "求值结果（以及可选的闭包/env 内省）"
}
```

示例调用：
```
> aura_ir(code="((lambda (x) (* x 2)) 5)")
→ "10"

> aura_ir(code="(letrec ((fact (lambda (n) (if (= n 0) 1 (* n (fact (- n 1))))))) (fact 5))", inspect=true)
→ "result: 120"
```

### Tool: `aura_serve`

```json
{
  "name": "aura_serve",
  "description": "增量编译服务 — 定义/执行/热替换函数",
  "parameters": {
    "command": "string — 命令类型: define | exec | redefine",
    "code": "string — 命令负载"
  },
  "returns": "JSON 格式的编译结果"
}
```

示例调用：
```
> aura_serve(command="define", code="(define (add x y) (+ x y))")
→ "{\"status\":\"ok\"}"

> aura_serve(command="exec", code="(add 1 2)")
→ "{\"status\":\"ok\",\"result\":3}"

> aura_serve(command="redefine", code="(define (add x y) (+ (* x 2) y))")
→ "{\"status\":\"ok\"}"
```

### Tool: `aura_hot_swap`

```json
{
  "name": "aura_hot_swap",
  "description": "运行时热替换函数体（不重启运行时）",
  "parameters": {
    "code": "string — 新函数定义"
  },
  "returns": "替换后的函数调用测试结果"
}
```

示例调用：
```
> aura_hot_swap(code="(define (square x) (* x x))")
→ "result: 25"  (调用 (square 5) 验证)
```

### Tool: `aura_cache`

```json
{
  "name": "aura_cache",
  "description": "将代码编译为缓存文件（ABF v2 列格式）",
  "parameters": {
    "code": "string — 要缓存的 Aura 代码",
    "path": "string — 缓存文件路径"
  },
  "returns": "缓存写入确认 + 求值结果"
}
```

示例调用：
```
> aura_cache(code="(+ 1 2)", path="/tmp/add.cache")
→ "cache written to /tmp/add.cache\n3"
```

---

## 3. Zero-Shot Prompt 模板

### 3.1 通用 Agent Prompt

```
你是一个 Aura 编程专家。你的任务是通过工具调用操作 Aura 代码。

## 核心工作流
每次任务遵循四步循环：
1. OBSERVE — 使用 aura_query / aura_typecheck 理解代码结构
2. DIAGNOSE — 基于返回的分析结果诊断问题
3. ACT — 使用 aura_query_and_fix / aura_serve 修复或生成代码
4. VERIFY — 使用 aura_ir / aura_typecheck 验证结果

## 关键规则
- 不要自己生成完整代码。使用 Aura 的查询和修复引擎。
- 先查询，再修复。不要猜测结构。
- 每次操作后必须验证。
- 出现类型错误：先用 aura_typecheck 获取错误位置，再用 aura_query_and_fix 修复。

## 可用工具
{tool_descriptions}

## 任务
{task_description}
```

### 3.2 类型错误修复 Prompt

```
你正在修复一个 Aura 类型错误。

错误信息：{error}

步骤：
1. 使用 aura_typecheck 确认错误
2. 使用 aura_query 查询相关节点（查找错误位置附近的 AST）
3. 分析是类型标注缺失还是类型不匹配
4. 如果是标注缺失：使用 aura_query_and_fix 插入 :Type 标注
5. 如果是类型不匹配：插入 cast 或调整表达式
6. 使用 aura_typecheck 验证修复

示例修复流程：
输入: (+ "42" 1)
错误: coercion from String to Int at 1:2
修复: (cast (+ "42" 1) : Int)  或  使用 (string->number "42") 转换
```

### 3.3 代码生成 Prompt

```
你正在生成 Aura 代码实现以下功能：

{specification}

流程：
1. 设计核心函数签名（类型标注可选）
2. 用宏生成脚手架代码（如需要）
3. 使用 aura_ir 测试基本功能
4. 使用 aura_typecheck 验证类型
5. 使用 aura_query_and_fix 优化（如消除冗余 cast）

记住：优先利用 Aura 的内置能力（map/filter/list 操作），
不要手写循环。
```

---

## 4. 典型场景

### 4.1 类型错误自动修复

**问题**：AI 生成 `(+ "hello" 1)`，类型检查报错。

```
Step 1 [OBSERVE]:
  > aura_typecheck(code="(+ \"hello\" 1)")
  → "error: argument 1: coercion from String to Int at 1:2"

Step 2 [DIAGNOSE]:
  第一个参数是 String，+ 需要 Int。
  意图可能是字符串拼接，或者数字计算。

Step 3 [ACT]:
  > aura_query_and_fix(
      code="(+ \"hello\" 1)",
      match="(node-type Call (callee +) (arg (type String)) (arg (type Int)))",
      replace="(string-append \"hello\" (number->string 1))"
    )
  → "applied=true"

Step 4 [VERIFY]:
  > aura_ir(code="(string-append \"hello\" (number->string 1))")
  → "\"hello1\""
```

### 4.2 渐进标注

**问题**：AI 生成无类型代码，需要逐步添加类型标注。

```
Step 1 [OBSERVE]:
  > aura_query(
      code="(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))",
      query="(node-type Lambda) (no-annotation? #t)"
    )
  → "query: 1 matches (lambda at root)"

Step 2 [DIAGNOSE]:
  fact 的参数 n 没有类型标注。从使用模式看是整数。

Step 3 [ACT]:
  > aura_query_and_fix(
      match="(lambda (n) ...)",
      replace="(lambda ([n : Int]) ...)",
      ...
    )

Step 4 [VERIFY]:
  > aura_typecheck(code="(define (fact [n : Int]) ...)")
  → "type: (-> Int Int)"
```

### 4.3 运行时热替换

**问题**：AI 发现 fact 函数实现有 bug，需要不重启替换。

```
Step 1 [OBSERVE]:
  > aura_ir(code="(fact 5)")
  → "120"

Step 2 [DIAGNOSE]:
  对于 n=0 的情况，O(n) 递归效率低。优化尾递归版本。

Step 3 [ACT]:
  > aura_hot_swap(
      code="(define (fact-tail n acc) (if (= n 0) acc (fact-tail (- n 1) (* n acc))))"
    )
  → "result: 120"

Step 4 [VERIFY]:
  > aura_ir(code="(fact-tail 10 1)")
  → "3628800"
```

### 4.4 代码结构变换

**问题**：将 `(if cond e1 e2)` 转换为 `(cond (cond e1) (else e2))`。

```
Step 1 [OBSERVE]:
  > aura_query(code="(if (> x 0) (display \"+\") (display \"-\"))",
               query="(node-type If)")
  → "query: 1 matches"

Step 2 [ACT]:
  > aura_query_and_fix(
      match="(node-type If)",
      replace="(cond (($cond) ($then)) (else ($else)))"
    )

Step 3 [VERIFY]:
  > aura_ir(code="(cond ((> x 0) (display \"+\")) (else (display \"-\")))")
  → "验证通过"
```

---

## 5. 工作流编排

### 5.1 简单任务

```
OBSERVE ──→ DIAGNOSE ──→ ACT ──→ VERIFY
                                      │
                                      ↓
                                     ✅ 完成
```

### 5.2 复杂任务（迭代）

```
OBSERVE ──→ DIAGNOSE ──→ ACT ──→ VERIFY
                                      │
                                      ↓ (失败)
                                  重试循环
                                      │
                                      ↓ (达到上限)
                                 回退 + 报告
```

### 5.3 多文件/模块任务

```
OBSERVE ALL ──→ DIAGNOSE EACH ──→ 优先级排序
                │
                ↓
           fix file 1 ─── verify ───→ fix file 2 ─── verify ──→ ...
                │
                ↓
           集成验证 ──→ 最终报告
```

---

## 6. 与 Fine-tuning 的配合策略

| 阶段 | 主要策略 | 模型要求 | Aura 版本 |
|------|---------|---------|-----------|
| 当前 | Zero-Shot + Tool Use | 任何 SOTA LLM | M3d+ |
| M4 完成 | Zero-Shot + 轻量 FT | 1-2 万条标注 | M4 |
| 生产 | 混合策略 | 双模型 | M5 |

### Zero-Shot 的成功关键

1. **工具定义质量** — 每个工具的 description 要精确到参数语义
2. **错误恢复** — 工具返回错误时，模型要有重试策略
3. **上下文窗口** — Aura 代码通常是短的（几十到几百行），适合 Zero-Shot

### 何时需要 Fine-tuning

1. 模型频繁生成不存在的 Aura API
2. 模型不理解宏展开的 lazy evaluation 语义
3. 模型对类型错误诊断模式识别不准

---

## 7. 实现参考

### 7.1 工具包装（Python 示例）

```python
import subprocess
import json

class AuraTools:
    def __init__(self, binary="./build/aura"):
        self.binary = binary

    def query(self, code: str, query_expr: str) -> str:
        result = subprocess.run(
            [self.binary, "--query", query_expr],
            input=code, capture_output=True, text=True, timeout=10
        )
        return result.stdout.strip()

    def query_and_fix(self, code: str, match: str, replace: str):
        result = subprocess.run(
            [self.binary, "--query-and-fix", match, replace],
            input=code, capture_output=True, text=True, timeout=10
        )
        return result.stdout.strip()

    def typecheck(self, code: str) -> str:
        result = subprocess.run(
            [self.binary, "--typecheck"],
            input=code, capture_output=True, text=True, timeout=10
        )
        return (result.stdout + result.stderr).strip()

    def ir(self, code: str) -> str:
        result = subprocess.run(
            [self.binary, "--ir"],
            input=code, capture_output=True, text=True, timeout=10
        )
        return result.stdout.strip()

    def serve(self, command: str, code: str) -> dict:
        payload = json.dumps({"type": command, "code": code})
        result = subprocess.run(
            [self.binary, "--serve"],
            input=payload, capture_output=True, text=True, timeout=10
        )
        return json.loads(result.stdout)
```

### 7.2 LLM Agent 循环（伪代码）

```python
def agent_loop(aura: AuraTools, llm, task: str):
    messages = [
        {"role": "system", "content": AGENT_PROMPT},
        {"role": "user", "content": task}
    ]
    for _ in range(MAX_TURNS):
        # 模型决定下一步操作
        response = llm.chat(messages, tools=TOOL_DEFINITIONS)

        if response.has_tool_call():
            # 执行工具
            result = execute_tool(aura, response.tool_call)
            messages.append({"role": "tool", "content": result})
        elif response.has_final_answer():
            return response.text
        else:
            # 模型在思考，继续
            messages.append(response)
    return "timeout"
```

---

## 8. 设计参考

- AuraQuery eDSL: `docs/aura_query_engine.md`
- 类型系统: `docs/aura_typesystem.md`（L6.1-L6.7）
- 增量编译: `docs/incremental_caas.md`
- 卫生宏: `docs/hygienic_macros.md`
- 模块系统: `docs/module_system_abf_v2.md`
