# Aura

**AI-native Lisp — 代码自己进化。**

**让代码第一次拥有思考的能力。**

大多数语言为人类设计。Aura 问：如果使用者是 AI，语言应该长什么样？

不是“让 AI 写代码”，而是让语言成为 AI 可理解、操作、进化的活系统。

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
   不为 AI 设限。prompt 只说“你是 Aura”，语言自己处理。

2. **代码即记忆**  
   AST 是 Agent 可直接读写验证的自我意识。  
   `set-code` → `query:*` → `mutate:*` → `eval-current`。

3. **控制论闭环**  
   AI 给方向，Aura 做本地精确变异。信息素排序 + PID 代替蛮力，形成自演化闭环。

---

## 能力

- 精确自修改 EDSL（query / mutate / ast）
- 原生 Agent 编排与并发
- 版本化工作区与事务安全
- 增量编译执行管线

文档索引：[`docs/README.md`](docs/README.md)（**代码 + 测试为真相**；运行 `(api-reference)` 查看全部原语）。

```bash
./build.py build    # 构建
./build.py check    # CI：构建 + 核心测试
```

贡献运行时：[`docs/developer/evaluator.md`](docs/developer/evaluator.md)。

## License

Apache 2.0
