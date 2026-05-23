# Aura 已知问题

更新：2026-05-23

---

## P1 — 功能缺口

### 1. `let` 泛化（synthesize 路径已启用，check 路径未启用）

`synthesize_flat_let` 已正确创建 `forall` 泛化类型，Call handler 的 `instantiate_all_direct`
正确处理调用时的实例化。**`let` 多态在 synthesize 路径正常工作**（`type-let-poly` 测试通过）。

但 `check_flat` 的 Let 处理器未做泛化，`check` 路径下 `let` 绑定不做多态。
`TypeEnv::Binding::is_poly` 字段存在但未被使用。

### 2. match 穷尽性检查

`define-type`/`match`/`Some`/`None` 的类型推断已完成，但 `match` 尚未检查模式的穷尽性。

### 3. 模块 import 类型检查

模块导入 (`require`/`import`) 的绑定大部分推断为 `Dynamic`，类型签名传播未实现。

### 4. M4 线性所有权运行时验证

编译期跟踪 (`OwnershipEnv`) 和 IR opcode 已实现，但运行时违规检测（double move, use-after-move）尚未强化。

### 5. 树遍历器 CastOp 覆盖率

Coercion 在 TypeAnnotation 边界、if 分支、call-site 参数处已插入 CastOp，
但部分 eval 路径（Pair 构建、Set!、宏展开产物）的 CastOp 覆盖率仍需加强。
Blame 诊断（IR 路径 CastOp + 算术 coercion + TypeAnnotation）已覆盖。

---

## P2 — 增强

### 8. `--inspect` 扩展

- - [x] `--inspect typecheck` 类型推断状态 dump
- - [x] `--inspect evaluator` 树遍历器环境 dump (IR path: empty, tree-walker: full)
- - [x] `--inspect pretty` 格式化 JSON 输出
- [x] 与 `--cache-open` 组合：`./aura --inspect cache-open <file.abc>` 从缓存恢复并 dump JSON

### 9. Serve 超时熔断

`tcp-connect` 用非阻塞 connect() + poll() 8s 超时；serve 模式 exec 命令支持 30s 超时（配置 `timeout` 字段）。

### 10. M3 P2996 反射

`auto_to_json`/`auto_serialize`/`auto_deserialize` 已重构，内部委托给
P1306 template-for 递归实现（`to_json_impl`/`bin_write`/`bin_read`），
现在完整支持嵌套 struct、泛型 `vector<T>`、`array<T,N>`、enum、string 等。
旧 `reflect_members<T>()` 运行时类型擦除路径被替换。

---

## 已修复（2026-05-23）

### ~~`(+ 1 "hello")` 静默返回 1~~ ✅ `052cb19`

IR executor + tree-walker 在 `coerce_to_int` 中输出 blame 到 stderr。

### ~~`eval_flat: unsupported node type`~~ ✅ `50208da`

Linear/Move/Borrow/MutBorrow/Drop 节点已添加到 eval_flat 内层 switch。

### ~~`(: x Int)` 无绑定返回 0~~ ✅ `82dfaf4`

TypeAnnotation→Variable 模式检测后走树遍历器，正确报 unbound variable。

### ~~`(: name Type val)` 三参数解析~~ ✅ `afe96fd`

Parser 支持 `(: name Type val)` 3 参数形式，task hints 恢复。

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
