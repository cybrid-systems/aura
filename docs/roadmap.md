# Aura — 实现进度跟踪

---

## 里程碑状态

```
M1 求值器          ✅  纯 FlatAST 管线 (SoA)，无 Expr* 指针树
M2 查询引擎        ✅  ASTIndex + QueryEngine + TransformEngine
M3a 语言补全       ✅  布尔/序对/begin/set!/quote/cond/letrec/string/vector
M3b 宏系统         ✅  defmacro + SyntaxMarker (卫生宏 Phase1)
M3c 反射           ✅  P2996 auto_to_json + kNodeMeta + 结构布局验证
M3d 类型系统       ✅  L6.1-L6.7: 渐进类型 + Occurrence + 类型查询
M3e 工具链         ✅  Benchmark(44) + 增量编译 + --serve + CI
M3f AI 闭环         ✅  mutation_loop + LLM 驱动 + AI Agent 演示(6 场景)
M4a 缓存           ✅  ABF v4 列式 (O(1) resolve + SyntaxMarker)
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
| Benchmark | 44/44 |
| 集成测试 | 62/62 |
| 测试套件 | 8 (build.py test all) |
| IR opcodes | 21 |
| 运行时类型 | 8 (Void/Int/Bool/String/Pair/Closure/Cell/Vector) |
| 语言原语 | ~53 |
| CI | GitHub Actions |

---

## 下一步计划

### 🔴 P0 — String 原语补齐到 IR 管线

| 问题 | 现状 |
|------|------|
| IR executor 不支持 string | `--ir` 模式下 `string-append` 返回 0 |
| number->string 在 evaluator | 树遍历器正常，IR 路径无支持 |
| serve exec 缓存函数查找 bug | `exec_with_cache` 返回 0 |

### 🟡 P1 — IR 缓存序列化 (ABF v4 Phase 3)

编译后的 IR 函数写入 cache 文件，`--cache-open` 直接执行（跳过 parse+lowering）。

### 🟡 P1 — 卫生宏 Phase 2

利用 SyntaxMarker 列做宏展开器 MacroExpander：
- 预展开管线（parse → macro_expand → typecheck → lower）
- MacroIntroduced 节点自动重命名

### 🟢 P2 — 类型系统 L6.8+ 补全

| 特性 | 状态 |
|------|------|
| forall 多态 | ⚡ register_forall 存在但 0 测试 |
| Float/Symbol/Pair 类型注册 | ⚡ TypeTag 枚举已有但未注册 |
| Tuple/Variant/Record 类型语言 | ⬜ 设计阶段 |

### 🟢 P2 — LLVM 后端探索

M4 正式第一步。建议等 Agent demo 证明价值后再投入。
