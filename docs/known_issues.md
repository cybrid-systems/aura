# Aura 已知问题

更新：2026-05-23

---

## P1 — 编译器缺陷

### 1. `eval_flat: unsupported node type`

`binary-search` 任务中 DeepSeek 生成了代码触发此错误（`evaluator_impl.cpp` 内层 switch default case）。
内层 switch 未处理的 NodeTag: **Linear (0x16), Move (0x17), Borrow (0x18), MutBorrow (0x19), Drop (0x1A)**。
（Coercion/Pair 已被外层 `ast_to_data` 路径处理。）

### 2. TypeAnnotation 解释器路径

树遍历 `eval_flat` 的 TypeAnnotation 分支评估 `v.child(0)`（变量引用），
对于变量未绑定的 `(: x Int)` 上下文，返回 0 而非报错。

### 3. 类型不匹配应报错而非静默转换

`(display (+ 1 "hello"))` 输出 `1` 而非触发运行时类型错误。
解释器路径的 `+` 原语将 String 视为 0 做加法。

### 4. 树遍历器 CastOp 覆盖率

if 分支 CastOp 插入已实现，但部分执行路径（Pair 构建、Set!、宏展开产物）的 CastOp 覆盖率仍需加强。

---

## P2 — 功能缺口

### 5. `let` 泛化未使用

Union-Find 约束求解器 + worklist fixpoint 已实现，但 `let` 绑定不做泛化。
`TypeScheme::is_poly` 字段存在但始终为 false。

### 6. match 穷尽性检查

`define-type`/`match`/`Some`/`None` 的类型推断已完成，但 `match` 尚未检查模式的穷尽性。

### 7. 模块 import 类型检查

模块导入 (`require`/`import`) 的绑定大部分推断为 `Dynamic`，类型签名传播未实现。

### 8. M4 线性所有权运行时验证

编译期跟踪 (`OwnershipEnv`) 和 IR opcode 已实现，但运行时违规检测（double move, use-after-move）尚未强化。

### 9. Blame 信息薄弱

JIT 路径有 blame 标签，解释器/IR 路径 blame 信息薄弱。
BlameParty/BlameInfo 结构化框架已实现但覆盖不全。

### 10. Coercion eval 路径覆盖

Coercion 在 TypeAnnotation 边界、if 分支、call-site 参数处已插入 CastOp，
但部分 eval 路径的 CastOp 覆盖率仍需加强。

---

## P3 — 增强

### 11. 增量类型检查

`typecheck-current` 全量遍历，无增量缓存。`FlatAST::dirty_` 字段存在但 TypeChecker 不读。

### 12. `--inspect` 扩展

- [ ] `--inspect typecheck` 类型推断状态 dump
- [ ] `--inspect evaluator` 树遍历器环境 dump
- [ ] `--inspect pretty` 格式化 JSON 输出
- [ ] 与 `--cache-open` 组合：从缓存恢复 IRModule 后 inspect

### 13. Serve 超时熔断

`tcp-connect` 用非阻塞 connect() + poll() 8s 超时；serve 模式整体缺乏超时熔断机制。

### 14. M3 P2996 反射

`auto_serialize<T>()` 对嵌套 struct 和 enum 支持有限（容器类型部分完成）。

---

## 测试状态

| 套件 | 数量 | 状态 |
|------|:----:|:----:|
| Integ (build.py) | 118 | ✅ |
| Bash regression | 106 | ✅ |
| Benchmark (LLM tasks) | 85 | ✅ (Grok 77/85) |
| Smoke | 5 | ✅ |
| Regression | 6 | ✅ |
| Gradual guarantee | 10+ | ✅ |
| Fuzz | 46/47 | ✅ |
| suite/core、stdlib、typesystem 等 | 15+ | ✅ |
