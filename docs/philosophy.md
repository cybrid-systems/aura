# Aura 哲学

**让代码第一次拥有思考的能力。**

---

## 起源

大多数编程语言是为人类设计的（优化可读性、可预测性、开发者体验）。

Aura 问了一个不同的问题：**如果使用者是 AI，语言应该长什么样？**

答案不是“让 AI 写代码”，而是让语言本身成为 AI 可以理解、操作、进化的**活的系统**。

---

## 三个核心原则

### 1. 着力即差 — 不为 AI 设限

LLM 学过几十亿行代码。这些代码中有 Scheme、有 Common Lisp、有 Racket、有 Clojure。  
当 LLM 写出 `(first x)` 或 `(char=? c #\()` 时，这不是错误——这是模型在自然地做它最擅长的事。

**Aura 的做法：** 不是告诉 LLM"不要用 `first`"，而是让 `first` 在 serve 层就定义好。  
不是告诉 LLM"不要用 `char=?`"，而是确保 Aura 原生支持。

```
prompt 应该只做一件事: 告诉 LLM "你是 Aura"。
剩下的一切，语言自己处理。
```

这就是"着力即差"——越用力限制，效果越差。  
小满。自然生长。

### 2. 代码即记忆 — Auto-mutating AST

传统编译器中，源码是输入，AST 是中间产物，机器码是输出。  
AI 只能看到输入和输出，中间过程对它是黑盒。

Aura 把 AST 变成 AI 可以直接读、写、验证的记忆：

```
(set-code "(define (bad-fac n) (* n (bad-fac (- n 1))))")
(query:find "bad-fac")        ; AI 找到自己的函数定义 → (node-id)
(mutate:rebind "bad-fac"
  "(lambda (n) (if (= n 0) 1 (* n (bad-fac (- n 1)))))")
(eval-current)                ; AI 验证修改
```

这不是编译器——这是代码的**自我意识**。

### 3. 控制论闭环 — 蚁群搜索

AI 生成代码不需要完美。  
只需要方向正确，剩下的事情交给本地搜索：

```
LLM → 方向性代码 → 编译 → 测量距离 → fine/putt?
  → Aura 本地变异 (信息素排序, 批量 IPC)
  → 找到正确版本 → 返回
  → 都没找到 → LLM 重构
```

用控制论代替蛮力，  
用信息素代替随机猜测，  
用 PID 裁剪代替穷举。

---

## 设计仓库

完整的哲学讨论、架构决策、设计演变更详细的版本在：

**[github.com/cybrid-systems/ai-programming-language-design](https://github.com/cybrid-systems/ai-programming-language-design)**

---

## 延伸阅读

- 蚁群 / PID / 控制论相关：`design/notes/` 下的 ant_*、adaptive_intend_pid 等（历史探索，当前状态见 `design/core/agent_orchestration.md` §0 和 roadmap）。
- 语言规格：`design/notes/aura_language_spec.md`（历史），当前以 `design/core/` + 代码为准。
- 完整设计演进见 Git 历史和 `design/history/`。
