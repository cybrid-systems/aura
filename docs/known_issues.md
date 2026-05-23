# Aura 已知问题

更新：2026-05-23

---

## ✅ 已修复（05-19 以来）

### ~~P0: 缓存函数字符串分支崩溃~~ ✅ `3e3e7a2`

**根因**：`Arg` 执行将负整数误判为 cell sentinel 索引。
**修复**：仅在 cell slot 在 `cell_heap_` 范围内时才视为 cell 引用。

### ~~P0: `set!` 闭包可变状态不持久~~ ✅ `3392d77`

**根因**：`let` 用 `bind(name, Int(0))` 直接存值，`set!` 的 `lookup_cell_ptr` 找不到 Cell。
**修复**：`let` 像 `letrec` 一样用 Cell 绑定，`set!` 正常工作。

### ~~缓存函数自引用（--ir 路径）~~ ✅

**根因**：`Define` handler lowering 时名字未预绑定到 scope。
**根治**：预绑定 Cell，自引用 Variable 找到 Cell 绑定。

### ~~`cadr`/`caddr` 已存在~~ ✅

内建原语，LLM 测试失败是其他原因。

### ~~缓存函数内嵌 lambda~~ ✅ `3cb9a33`

自递归跳过 IR 缓存。

### ~~`<closure[N]>` 显示~~ ✅

`<closure[281474976710656]>` → `#<procedure>`，闭包显示人类可读。

### ~~`unbound variable: ` 空名字~~ ✅

显示正确变量名 + 建议。根因：`with_suggestion` 自我赋值 bug。

### ~~extract_code 正则误删比较操作符~~ ✅

`<`, `>` 被 `re.sub(r'<[^>]+>', ...)` 当 XML 标签误删。修复为正则只匹配字母开头的标签名。

---

## P1 — 编译器缺陷

### 1. `eval_flat: unsupported node type`

`binary-search` 任务中 DeepSeek 生成了代码触发此错误（`evaluator_impl.cpp` default case）。
`eval_flat` switch 未处理的 NodeTag: **Coercion (0x10), Pair (0x12), Linear (0x16), Move (0x17), Borrow (0x18), MutBorrow (0x19), Drop (0x1A)**。
LLM 生成的内容触发未预料节点时会崩溃。需要排查 LLM 具体生成了什么并补全缺失分支。

### 2. TypeAnnotation 解释器路径信号缺失

树遍历 `eval_flat` 的 TypeAnnotation 分支评估 `v.child(0)`（变量引用），
对于变量未绑定的 `(: x Int)` 上下文，返回 0 而非报错。
建议：检查变量是否绑定，未绑定时报 `unbound variable` 错误。

### 3. `(: name Type val)` 三参数形式不存在

解析器 `parse_type_annot` 仅处理 `(: name Type)`（2 个后续 token），不会消费第 3 个参数。
`(: x Int 42)` 实际解析为 `(: x Int)` + 离开的 `42` 作为父表达式的额外参数。
**修复**：已更新所有任务 hint 改用 `(check expr : Type)` 形式。长期可考虑扩展 parser。

### 4. 类型不匹配应报错而非静默转换

`(display (+ 1 "hello"))` 输出 `1` 而非触发运行时类型错误（期望 blame 输出）。
解释器路径的 `+` 原语将 String 视为 0 做加法。需要 CoercionCastOp 在树遍历器也生效。

### 5. 树遍历器 CastOp 覆盖率

if 分支 CastOp 插入已实现（23b9362），但部分执行路径（Pair 构建、Set!、宏展开产物）
的 CastOp 覆盖率仍需加强。

## P2 — 功能缺口

### 6. 约束求解器 —— Union-Find 已实现但 `let` 多态未使用

Union-Find 约束求解器 + 多遍 worklist fixpoint 已实现（T2a），但 `let` 绑定仍不做泛化。
`TypeScheme::is_poly` 字段存在但始终为 false，多态函数类型退化为单一实例化。

### 7. ADT 类型推断已实现但缺少穷尽性检查

`define-type`/`match`/`Some`/`None` 的类型推断已完成（T2b，含 forall-wrapped 多态构造函数），
但 `match` 尚未检查模式的穷尽性。

### 8. 模块 import 类型检查

模块导入 (`require`/`import`) 的绑定大部分推断为 `Dynamic`，类型签名传播未实现。

### 9. M4 线性所有权运行时验证

M4 的编译期跟踪 (`OwnershipEnv`) 和 IR opcode 已实现，但运行时违规检测
（double move, use-after-move）尚未完全强化。

### 10. 运行时 blame 信息

JIT 路径有 blame 标签，解释器/IR 路径 blame 信息仍然薄弱。
BlameParty/BlameInfo 结构化框架已实现（T2c），但覆盖不全。

### 11. Coercion 全路径

Coercion 在 TypeAnnotation 边界、if 分支、call-site 参数处已插入 CastOp
（is_coercible + Float↔Int 转换），DeadCoercionEliminationPass 已实现（T2e），
但部分执行路径（Pair 构建等）的 CastOp 覆盖率仍需加强。

---

## P3 — 增强

### 7. 类型检查未完全增量

`typecheck-current` 当前全量遍历，无增量缓存。`FlatAST::dirty_` 字段存在但 TypeChecker 不读。

### 8. 桥接器 body_source fallback

`02681ac` — `body_source` 已加入 `ClosureBridgeData`，但 fallback 路径缺少测试覆盖。

### 9. 深递归边界

显式调用栈 (`9674eb0`) 已支持 `(deep 100000)` 无 segfault，但极端嵌套（>1M）可能触发 `std::bad_alloc`。

### 10. Serve 模式超时

`tcp-connect` 用非阻塞 connect() + poll() 8s 超时；serve 模式整体可用但缺乏超时熔断机制。

### 11. `--inspect` 扩展

- [ ] `--inspect typecheck` 类型推断状态 dump
- [ ] `--inspect evaluator` 树遍历器环境 dump
- [ ] `--inspect pretty` 格式化 JSON 输出
- [ ] 与 `--cache-open` 组合: 从缓存恢复 IRModule 然后 inspect

### 12. M3 P2996 反射

`std::meta` 是 C++26 实验特性，GCC 16 实现可能不完整。
`auto_serialize<T>()` 对嵌套 struct 和 enum 的支持有限（容器类型支持部分完成）。
已移除旧的 `src/compiler/reflect.ixx`。

---

## 最新测试状态

| 套件 | 数量 | 状态 |
|------|:----:|:----:|
| Integ (build.py) | 118 | ✅ |
| Bash regression | 106 | ✅ |
| Benchmark (LLM tasks) | 85 | ✅ (Grok 77/85 领先) |
| Smoke | 5 | ✅ |
| Regression | 6 | ✅ |
| Gradual guarantee | 10+ | ✅ |
| Fuzz | 46/47 | ✅ (1 known) |
| **suite/core.aura** | 多节 | ✅ |
| **suite/stdlib.aura** | 多节 | ✅ |
| **suite/typesystem.aura** | 11 test-suite 节 | ✅ |
| **suite/edsl.aura** | 多节 | ✅ |
| **suite/macros.aura** | 多节 | ✅ |
| **suite/module.aura** | 多节 | ✅ |
| **suite/errors.aura** | 多节 | ✅ |
| **suite/intent.aura** | 多节 | ✅ |
