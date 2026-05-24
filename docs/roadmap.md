# Aura 路线图

**更新：2026-05-24**

---

## 当前能力

| 维度 | 状态 |
|------|:----:|
| 编译器 | 0 crash fuzz (4800+ cases), 3 套 fuzz, ASan/UBSan 通过 |
| 类型系统 | Sound Gradual Typing, ADT + match 穷尽性, let-poly, blame |
| M4 线性所有权 | move/borrow/drop 编译期(static borrow check) + 运行时检测 |
| 标准库 | 29 模块 (~2.5k 行) |
| EDSL 自修改 | set-code → mutate → query → eval-current + colony:search |
| C FFI | dlopen/dlsym, Int/Float/String/Void |
| 增量编译 | 缓存 + 依赖跟踪 + hot-swap |
| 编译期反射 | P2996 auto_to_json/serialize/deserialize |
| AI Benchmark | 102 任务, DeepSeek **90/102**, MiniMax/Grok 待出 |
| 执行后端 | 3 个: tree-walk / IR / JIT, 差分测试 0 diff |
| Serve 协议 | closure 检测, 多 session, timeout |
| Closure JIT | 内联缓存 (64槽 + stack buffer) |
| Prim inline | display/newline/quotient/remainder 直出 LLVM IR |
| 事务变异 | MutationTransaction RAII 自动 rollback |
| 增量类型检查 | dirty 子树增量 re-check |

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
- **风险**: 改 NodeId 类型会影响所有外部接口（Patch/customization/colony 序列化）

#### ADT 递归穷尽性 + 修复建议

当前 match 穷尽性检查只处理单层构造器。

- **Goal**: 支持递归 ADT 的模式穷尽检测 + non-exhaustive 带修复建议 (类似 Rust help:)
- **Approach**: 递归构造器展开加 depth limit；fix-it 建议在 Diagnostic 中附加
- **工作量**: 2d（穷尽）+ 1d（修复建议）
- **依赖**: type_checker_impl.cpp match 检查（已有 Step 1-4 基础）

#### All panics → Raise + runtime check

car on nil、list-ref OOB、string-ref OOB 等所有可能 panic 的操作加 runtime check + Raise。

- **工作量**: 1d

#### Parser fuzz corpus

从 stdlib + AI 生成复杂 S-exp 构建 fuzz seed corpus。

- **工作量**: 1d

#### 等价变异 fuzz

语义保持变换 → 验证三后端输出一致。

- **工作量**: 2d

---

## P3 — 远期优化 + 项目工具

### 编译器

| 项目 | 工作量 | 说明 |
|:-----|:------:|:----|
| NodeViewRef hot path | 1d | eval 高频路径用轻量引用避免 NodeView 拷贝 |
| StringPool rehash 阈值 | 0.5d | 加 reserve + rehash 保护 |
| AOT 缓存 (--emit-binary) | 4d | JIT 结果缓存为 .aura_cache |
| 编译期反射可用性检测 | 0.5d | `#if __cpp_lib_reflection >= 2024` 降级 |
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
| DeepSeek 回测 | 1d | task hint 修改对 score 影响 |
| Multi-model 交叉验证 | 2d | Grok + DeepSeek + MiniMax 对比 |
| 性能 benchmark | 3d | Fib/JSON parse/sort 等微基准 |

---

## 回看

- **2026-05-24**: P1 全部 7 项完成 🎉 — mark_dirty_upward 迭代化, M4 borrow check, closure cache, inline prim, pack_pair 消除, occurrence typing, 事务 rollback。DeepSeek 90/102 (+3↑)
- **2026-05-23 晚**: 差分测试 0 diff — 198 用例三后端一致
- **2026-05-23**: P0-P3 全关, 102 tasks benchmark, 4600+ fuzz 0 crash, ASan/UBSan 通过
