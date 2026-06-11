# Aura

**AI-native Lisp — 代码自己进化。**

**让代码第一次拥有思考的能力。**

大多数编程语言是为人类设计的。Aura 问了一个不同的问题：

**如果使用者不是人类，而是 AI，语言应该长什么样？**

答案不是“让 AI 写代码”，而是让语言本身成为 AI 可以理解、操作、进化的**活的系统**。

---

## 30 秒 Quickstart

```bash
cmake -B build && cmake --build build --target aura -j
echo '(+ 1 2)' | ./build/aura   # → 3
```

```scheme
(set-code "...")
(query:calls "fact")
(mutate:rebind "fact" "..." "迭代化")
(eval-current)
```

完整示例见 [`docs/tutorial.md`](docs/tutorial.md)。

---

## 哲学

Aura 遵循三个原则：

1. **着力即差**  
   不为 AI 设限。prompt 只需告诉 LLM “你是 Aura”，剩下的一切，语言自己处理。

2. **代码即记忆**  
   AST 不再是编译器黑盒，而是 Agent 可直接读、写、验证的自我意识。  
   `set-code` → `query:*` 导航 → `mutate:*` 重构 → `eval-current` 验证。

3. **控制论闭环**  
   AI 只需给方向，Aura 负责本地精确搜索与变异。  
   用信息素排序和 PID 裁剪代替蛮力穷举，形成自演化闭环。

---

## 能力

- 精确自修改 EDSL（`query:*` / `mutate:*` / `ast:*`）
- 原生 Agent 编排与 fiber 并发
- 版本化工作区与事务安全
- 增量编译执行管线

更多当前状态见 [`docs/README.md`](docs/README.md)（core/ 为活文档）。

开发者文档：[`docs/developer/evaluator.md`](docs/developer/evaluator.md)。

## License

Apache 2.0
