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
| C FFI | dlopen/dlsym, Opaque/Struct 内存操作, Int/Float/String/Void |
| 增量编译 | 缓存 + 依赖跟踪 + hot-swap |
| 编译期反射 | P2996 auto_to_json/serialize/deserialize |
| AI Benchmark | 102 任务, DeepSeek **90/102**, MiniMax/Grok 待出 |
| 执行后端 | 3 个: tree-walk / IR / JIT, 差分测试 0 diff |
| Serve 协议 | 多 session + 生命周期管理 + timeout, 原子 JIT |
| Closure JIT | 内联缓存 (64槽 + stack buffer) |
| Prim inline | display/newline/quotient/remainder 直出 LLVM IR |
| 事务变异 | MutationTransaction RAII 自动 rollback |
| 增量类型检查 | dirty 子树增量 re-check |
| 错误诊断 | 结构化 ParseError + 源行 caret + edit-distance suggestion |
| Fuzz 套件 | 518 seed corpus + 等价变异 transforms + structured fuzz |
| NodeId 安全 | generation 版本号 + is_valid/get_safe/validate 检测悬垂 ID |
| 数值数组 | `#(1 2 3)` 字面量脱糖 → `(vector 1 2 3)` |
| All panics → Raise | vector-ref/set!/list-ref OOB 统一 raise |

---

## P2.5 — --emit-binary 运行时改造（高优先级）

目标: 让 `--emit-binary` 生成的独立二进制真正支持 M4 linear ownership 的 drop 语义。
采用 **Bump Allocator + Arena Reset** 方案（替代当前 append-only 固定堆）。

| 优先级 | 项目 | 工作量 | 说明 |
|:------:|:-----|:------:|:-----|
| P0 | runtime.c Bump Allocator | 1d | `aura_bump_init/alloc/reset` 实现，替换所有 `aura_alloc_*` |
| P0 | runtime.c 闭包捕获 | 1d | `aura_closure_capture` 真实实现 |
| P0 | runtime.c drop 函数族 | 1d | `aura_drop_pair/cell/closure` — 幂等 + free list fallback |
| P1 | LLVM IR 入口插入 init/reset | 2d | `aura_jit.cpp` 中 `__top__` 入口 call init、出口 call reset |
| P1 | 字符串/向量分配函数 | 1d | `aura_alloc_string`, `aura_alloc_vector` |
| P2 | 迭代 Drop 替代递归 | 2d | 安全释放复合类型，避免栈溢出 |
| P2 | Bump overflow → 动态扩容 | 2d | 当前 exit(1) 改为 realloc + 重试 |

**合计**: ~10d 可让 --emit-binary 达到生产可用水平

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

- **2026-05-24**: P2 全部 9 项完成 🎉 — `#(1 2 3)` 字面量, 错误诊断(suggestion + caret), Serve 并发安全, FFI Opaque/Struct, All panics → Raise, Parser fuzz corpus(518 seeds), 等价变异 fuzz, NodeId generation, ADT 递归穷尽性 + fix-it. 累计 ~6h 交付。
- **2026-05-23 晚**: 差分测试 0 diff — 198 用例三后端一致
- **2026-05-23**: P0-P3 全关, 102 tasks benchmark, 4600+ fuzz 0 crash, ASan/UBSan 通过
