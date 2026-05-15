# Aura — 实现进度跟踪

---

## 里程碑状态

```
M1 求值器          ✅  纯 FlatAST 管线 (SoA)，无 Expr* 指针树
M2 查询引擎        ✅  ASTIndex + QueryEngine + TransformEngine
M3a 语言补全       ✅  布尔/序对/begin/set!/quote/cond/letrec/string/vector/hash
M3b 宏系统         ✅  defmacro + 卫生宏 Phase 1-5
M3c 反射           ✅  P2996 auto_to_json + kNodeMeta + 结构布局验证
M3d 类型系统       ✅  L6.1-L6.8: 渐进类型 + Occurrence + forall 多态 + Float
M3e 工具链         ✅  Benchmark(44) + 增量编译 + --serve + CI
M3f AI 闭环         ✅  mutation_loop + LLM 驱动 + AI Agent 演示(6 场景)
M4a 缓存           ✅  ABF v4 列式 (O(1) resolve + SyntaxMarker)
M4b AI 协议         ✅  docs/ai_agent_protocol.md (7 工具定义)
M4c 模块系统       ✅  import + AURA_PATH + ABF v2 全链路
M4d 自进化         🚧  docs/typed_mutation_design.md (设计阶段)
M4e 生产           ⬜  LLVM JIT / AOT / 自举
```

---

## 代码库统计

| 指标 | 数值 |
|------|------|
| 源文件 | 33 (.ixx + .cpp) |
| 代码行数 | ~8800 |
| CTest | 52/52 |
| Benchmark | 44/44 |
| 集成测试 | 62/62 |
| 测试套件 | 8 (build.py test all) |
| IR opcodes | 23 (含 ConstF64) |
| 运行时类型 | 9 (Void/Int/Bool/String/Float/Pair/Closure/Cell/Vector/Hash) |
| 类型系统特性 | Gradual + Occurrence + forall + Float + lub promotion |
| 语言原语 | ~70 |
| CI | GitHub Actions |

---

## 已完成功能

### 2026.05.15 Session — Float + 类型加固 + 错误体验 + 闭包修复 + forall

| 特性 | 类型 | 文件 | 行数 |
|------|------|------|------|
| **Float 支持** | P0 | 14 | +403 |
| 词法/语法/值系统 | | lexer/parser/AST/EvalValue | 3.14 字面量、double variant |
| Float 运算 | | 求值器 + IR 执行器 | int/float 自动提升 |
| IR ConstF64 | | IR opcode + lowering + executor | 双精度常量加载 |
| Float 类型检查 | | TypeChecker | Float 类型注册 + lub 提升 |
| **类型推断加固** | P1 | TypeChecker | +80 |
| let-bound lambda 推断 | | synthesize_flat_call | 算术运算自定义返回类型 |
| 约束归一化 | | infer_flat | `cs_.normalize(result)` |
| **错误体验** | P1 | 3 文件 | +96 |
| 编辑距离建议 | | evaluator_impl.cpp | `(did you mean y?)` |
| 函数名+参数个数 | | type_checker_impl.cpp | `call 'map': expected 2, got 3` |
| 导入路径报告 | | evaluator_impl.cpp | `searched in: CWD + AURA_PATH` |
| **闭包生命周期修复** | P1 | main.cpp | -2 |
| 跨行 define+调用 | | 移除 cs.reset() | `(define add ...) (add 5 7)` → 12 |
| **forall 多态** | P2 | 4 文件 | +75 |
| register_forall 完善 | | type.ixx/type_impl.cpp | 存储 var+body, 实例化 |
| map/filter 类型推断 | | type_checker_impl.cpp | `∀a b. ((a→b), Any)→b` |
| **Typed Mutation 设计** | P3 | docs/typed_mutation_design.md | 三阶段实现路径 |

### 之前已完成

| 特性 | 类型 | 说明 |
|------|------|------|
| 哈希表 (Swiss table) | P0 | 8 原语 + string key 支持 |
| Float/Pair forall 启用 | P0 | TypeTag 注册 |
| IR 缓存序列化 | P1 | ABF v4 Phase3 |
| --cache-open 执行 | | 跳过 parse+lowering |
| import 原语 + 模块系统 | | ABF v2 全链路 |
| AURA_PATH | | 搜索路径 + .aura 自动扩展 |
| 卫生宏 Phase 1-5 | | SyntaxMarker → 展开 → 克隆 → 类型检查 |
| 向量类型 | | vector/vector-ref/vector-set!/vector-length/IR |
| IR string 原语 | | string-append/length/ref/substring/compare |
| 宏警告 | | 未使用参数 + 常量体 |

---

## 下一步计划

### 🔴 P3 — Typed Mutation（3 周）

类型安全变异算子，支持 AI Agent 安全自修改代码。

| Week | 内容 | 交付物 |
|------|------|--------|
| W1 | MutationRecord + MutationLog | FlatAST 存储、add_mutation、mutation_history |
| W2 | TypedMutationOp 原语 | check_preconditions、create_patches、apply、revert |
| W3 | Provenance 查询 + AI 协议集成 | mutation-log、rollback、--serve 扩展 |

详见 [`docs/typed_mutation_design.md`](docs/typed_mutation_design.md)

### 🟡 P4 — Capability Effects（2 周）

- `perform (MutateAST node code)` 通过 effect handler 调度
- `(: modify-module (Capability ModuleA -> Bool))` 类型签名
- 与现有 `primitives_.add` 机制集成

### 🟡 P4 — 增量热更新健全性（1 周）

- 子树级类型重检查（只检查受 mutation 影响的部分）
- 热更新时自动插入 coercion + blame 边界
- 与 ABF v2 + --serve 深度集成

### 🟢 P5 — LLVM 后端探索

M4e 正式第一步。建议等 Agent demo + Typed Mutation 验证价值后再投入。

### 🟢 P5 — 包管理器

在 AURA_PATH 基础上：
- 本地 `~/.aura/pkgs/` 仓库
- `(require "math")` 从 URL 下载并缓存
