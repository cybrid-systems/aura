# Aura 路线图

**更新：2026-05-24**

---

## 当前能力

| 维度 | 状态 |
|------|:----:|
| 编译器 | 0 crash fuzz (4800+ cases), 3 套 fuzz, ASan/UBSan 通过 |
| 类型系统 | Sound Gradual Typing, ADT + match 穷尽性, let-poly, blame |
| M4 线性所有权 | move/borrow/drop 编译期 + 运行时检测 |
| 标准库 | 29 模块 (~2.5k 行) |
| EDSL 自修改 | set-code → mutate → query → eval-current + colony:search |
| C FFI | dlopen/dlsym, Int/Float/String/Void |
| 增量编译 | 缓存 + 依赖跟踪 + hot-swap |
| 编译期反射 | P2996 auto_to_json/serialize/deserialize |
| AI Benchmark | 102 任务, Grok **92/102**, DeepSeek 87/102 |
| 执行后端 | 3 个: tree-walk / IR / JIT, 差分测试 0 diff |
| Serve 协议 | closure 检测, 多 session, timeout |

---

## P1 — 编译器核心加固

### M4 静态借用检查（Polonius-style）

当前 M4 依赖运行时检测（double-move/use-after-move/double-drop），缺乏编译期借用检查。

- **Goal**: 在 `OwnershipEnv` + `ConstraintSystem` 中加入贷款分析，捕获 `&mut + move` 组合的 use-after-move
- **Approach**: 类似 Rust borrow checker 的轻量版——不追求完整 lifetime，只追踪 loan → invalidate 路径
- **工作量**: ~5d
- **依赖**: 无

### Closure inline cache

OpMakeClosure + OpCapture + OpCall 三步在 hot path 开销大。

- **Goal**: 首次 OpCall 后缓存目标函数指针，后续直接 indirect call
- **Approach**: V8 风格 monomorphic inline cache——每个 call site 存一对 `(function_id, fn_ptr)`，命中直接调用，miss 走原 fallback
- **工作量**: 3d
- **依赖**: JIT bridge (aura_jit_bridge.cpp)

### Inline primitives to LLVM IR

高频原语（`+`, `car`, `cons`, `list-ref` 等）当前 fallback 到 `aura_prim_call`，缺 inline 优化机会。

- **Goal**: 将 top-N 最热原语直接生成 LLVM IR，消除 runtime call overhead
- **Approach**: 按使用频率逐步 inline。先做 `+` `-` `*` `car` `cdr` `cons`，再做 `list-ref` `string-ref` `hash-get`
- **工作量**: 3d（首批 6 个原语）→ 后续每个 0.5d
- **依赖**: aura_jit_bridge.cpp, lowering_impl.cpp

### IR SmallVector for operands

当前 `operands` 是 `std::array<uint32_t, 4>`，Call/Apply 用 pack_pair 打包参数基址+数量，可维护性差。

- **Goal**: 替换为 `llvm::SmallVector<uint32_t, 4>` 或同等 SSO 容器
- **Change scope**: `IRInstruction` 结构体 + 所有 operands 访问点 + pack_pair 调用者
- **工作量**: 1d
- **风险**: 改 IRInstruction 布局可能影响 JIT 中 IR → LLVM 的映射，需差分测试验证

### Occurrence typing 扩展到 match/cond

当前 `string?` 等 predicate 的 type narrowing 只在 `if` 分支生效。

- **Goal**: 在 `cond` 每个 clause guard 和 `match` 每个 clause pattern 中传播 predicate
- **Approach**: 复用 InferenceEngine 的窄化原语，走 TyCheckCond → CheckCase → narrow_env 路径
- **工作量**: 2d
- **依赖**: type_checker_impl.cpp

### Query/mutate 事务性

EDSL 场景下 `find`/`mutate` 失败时需要完整 rollback，当前可能产生部分修改。

- **Goal**: `query.ixx` 中确保每个 mutate 操作是原子事务：失败时自动 rollback 所有副作用
- **Approach**: 引入 TransactionScope RAII guard，记录 change list，commit only on success
- **工作量**: 1.5d
- **依赖**: FlatAST rollback 机制（已在）

### `mark_dirty_upward` 迭代化

当前是递归实现，深层 AST（>1000 节点）有栈溢出风险。

- **Goal**: 改成迭代 + `std::deque<uint32_t>`
- **Change scope**: `ast.ixx` 中 `mark_dirty_upward` 方法体替换，约 20 行
- **工作量**: <0.5d（quick win）

---

## P2 — 架构改善 + 语言扩展

### 语言扩展

| 项目 | 工作量 | 说明 |
|:-----|:------:|:----|
| Numerical arrays `#(1 2 3)` | 3d | 向量类型 + 数值运算原语 |
| 错误诊断（行列号/suggestion） | 2d | 给 Diagnostic 加 source location + fix-it suggestion |
| Serve 并发安全 | 2d | 多 session 间 state 隔离 + 锁保护 |
| FFI Opaque/Struct/回调 | 3d | 不透明指针传递、struct layout、C 回调 trampoline |

### 架构改善

#### NodeId generation 版本号

FlatAST rollback 时悬垂 NodeId 可能导致 use-after-free。

- **Goal**: `NodeId` 从 `uint32_t` 改为 `struct { uint32_t id; uint16_t gen; }`，每次回滚后 generation bump
- **Approach**: FlatAST 内部维护 `generation_` 计数器；所有 `get(NodeId)` 入口校验 gen 匹配
- **工作量**: 2d
- **风险**: 改 NodeId 类型会影响所有外部接口（Patch/customization/colony 序列化），需要全量回归

#### ADT 递归穷尽性 + 修复建议

当前 match 穷尽性检查只处理单层构造器。

- **Goal**: 支持递归 ADT（`(define-type (List a) (Cons a (List a)) (Nil))`）的模式穷尽检测
- **Sub-goal**: non-exhaustive match 报错时带修复建议（类似 Rust `match` `help:` 行）
- **Approach**: 递归构造器展开加 depth limit（防无限展开）；fix-it 建议在 Diagnostic 中附加
- **工作量**: 2d（穷尽）+ 1d（修复建议）
- **依赖**: type_checker_impl.cpp match 检查（已有 Step 1-4 基础）

#### All panics → Raise + runtime check

当前只有 OpDiv 有安全处理。

- **Goal**: `car` on `nil`、`list-ref` OOB、`string-ref` OOB 等所有可能 panic 的操作都加 runtime check + Raise
- **Approach**: 在 lower 层统一插桩——每个可能失败的 op 前插入 check，失败走 Raise 指令
- **工作量**: 1d

#### Parser fuzz corpus

从 stdlib + AI 生成的复杂 S-exp 中提取输入做 parser fuzz。

- **Goal**: 覆盖 quasiquote/unquote、深层嵌套 let、复杂 match、模块交叉引用
- **Approach**: 写 `tools/collect_fuzz_corpus.py`，从 `lib/` 和 `tests/` 提取表达式，构建 fuzz seed corpus
- **工作量**: 1d

#### 等价变异 fuzz

用 EDSL 做语义保持变换（变量重命名、交换加法操作数、内联函数）→ 验证三后端输出一致。

- **Goal**: 自动发现后端差异（tree-walk / IR / JIT）
- **Approach**: 复用现有差分测试框架，加入等价变异器组件
- **工作量**: 2d

---

## P3 — 远期优化 + 项目工具

### 编译器

| 项目 | 工作量 | 说明 |
|:-----|:------:|:----|
| NodeViewRef hot path | 1d | eval 高频路径用轻量引用避免 NodeView 拷贝 |
| StringPool rehash 阈值 | 0.5d | 加 reserve() 接口 + rehash 保护 |
| AOT 缓存 (--emit-binary) | 4d | JIT 结果缓存为 .aura_cache |
| 编译期反射可用性检测 | 0.5d | `#if __cpp_lib_reflection >= 2024` 降级标记 |
| 形式化类型健全性证明 | 长期 | type soundness 的形式化验证 |

### 项目工具

| 项目 | 工作量 | 说明 |
|:-----|:------:|:----|
| 二进制输出 | 3d | --emit-binary + PE/ELF minimal |
| LSP | 5d | document symbols / completions / hover / diagnostics |
| 包管理 | 5d | registry + version resolution + dep install |
| 自举 | 10d+ | Aura-in-Aura 编译器 |

### Benchmark

| 项目 | 工作量 | 说明 |
|:-----|:------:|:----|
| DeepSeek 回测 | 1d | 验证 task hint 修改对 score 影响 |
| Multi-model 交叉验证 | 2d | Grok + DeepSeek + MiniMax 同任务对比 |
| 性能 benchmark | 3d | Fib/JSON parse/sort 等微基准 |

---

## Priority velocity map

```
        Urgent
          │
          │  M4 borrow checker
          │  Closure inline cache
          │  Inline primitives
          ├─────────┬────────── Important
          │         │
          │  IR SmallVector
          │  Occurrence typing
          │  NodeId generation  ← 语言设计核心
          │
        Nice-to-have
```

## 回看

- **2026-05-24**: 代码审查建议整合入路线图，新增 P1 核心加固 7 项、P2 架构改善 4 项
- **2026-05-23 晚**: 差分测试 0 diff 🎉 — 198 用例, 三个后端完全一致（3341d27）
- **2026-05-23**: P0-P3 全关, 102 任务 benchmark, 4600+ fuzz 0 crash, ASan/UBSan 通过, 7 个编译器 bug 修复
