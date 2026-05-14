# Aura — 实现进度跟踪

---

## 里程碑状态

```
M1 求值器          ✅  纯 FlatAST 管线 (SoA)，无 Expr* 指针树
M2 查询引擎        ✅  ASTIndex + QueryEngine + TransformEngine
M3a 语言补全       ✅  布尔/序对/begin/set!/quote/cond/letrec/string
M3b 宏系统         ✅  defmacro + gensym + 编译期验证
M3c 反射           ✅  P2996 auto_to_json + kNodeMeta + 结构布局验证
M3d 类型系统       ✅  L6.1-L6.7: 渐进类型 + Occurrence + 类型查询
M3e 工具链         ✅  Benchmark (42) + 增量编译 + --serve + HotSwap
M3f AI 闭环         ✅  mutation_loop + LLM 驱动 + AI Agent 演示
M4a 缓存           ✅  ABF v2 列式 (write/read/--cache/--cache-open/O(1) resolve)
M4b AI 协议         ✅  docs/ai_agent_protocol.md (7 工具定义)
M4c 生产           ⬜  LLVM JIT / AOT / 自举
```

---

## 代码库统计

| 指标 | 数值 |
|------|------|
| 源文件 | 28 (.ixx + .cpp) |
| 代码行数 | ~7500 |
| CTest | 49/49 |
| Benchmark | 42/42 |
| 集成测试 | 57/57 |
| 测试套件 | 8 (build.py test all) |
| IR opcodes | 21 |
| 内存池 tier | 4 (ASTArena + ArenaGroup) |

---

## 组件状态

### 核心 (core) ✅

| 模块 | 状态 | 说明 |
|------|------|------|
| arena.ixx | ✅ | ASTArena pmr bump allocator |
| ast.ixx | ✅ | FlatAST SoA + NodeView + StringPool |
| type.ixx | ✅ | TypeRegistry + TypeId (6 预定义类型) |

### 解析器 (parser) ✅

| 模块 | 说明 |
|------|------|
| lexer | Tokenizer |
| parser | S-表达式 → FlatAST (SoA) |

### 编译器 (compiler)

| 模块 | 状态 | 说明 |
|------|------|------|
| evaluator | ✅ | 树遍历求值器 (纯 FlatAST) |
| ir | ✅ | AuraIR 21 opcodes |
| lowering | ✅ | FlatAST → IR (cache-aware) |
| ir_executor | ✅ | IR 解释器 (闭包 + cells + coercion) |
| pass_manager | ✅ | concept-based fold pipeline |
| compute_kind | ✅ | Known/Unknown 分析 |
| arity | ✅ | 参数数量校验 |
| cache | ✅ | ABF v2 列式缓存 (v3 O(1) resolve) |
| diag | ✅ | 结构化诊断 + blame 位置 |
| service | ✅ | CompilerService |
| query | ✅ | ASTIndex / QueryEngine / TransformEngine |
| type_checker | ✅ | L6.1-L6.7 |

### 工具

| 工具 | 说明 |
|------|------|
| aura-reflect | P2996 IR opcode / schema 验证 |
| aura-schema | JSON Schema 生成 |
| main.cpp | CLI: 求值/查询/变换/类型检查/缓存/serve/hot-swap |

---

## 下一步

```
P0  向量类型          — 基础数据结构（变长数组）
P0  number->string    — 当前返回空字符串（字符串连接也无法正确处理）
P1  IR 缓存序列化     — 编译后 IR 函数写入 cache 文件（Phase 3）
P1  卫生宏 Phase 1    — FlatAST 标记 + 自动重命名
P2  CI + benchmark 回归 — GitHub Actions + --check 自动化
P3  LLVM 后端探索     — M4 正式第一步
```
