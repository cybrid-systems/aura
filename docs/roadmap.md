# Aura — 路线图

**更新：2026-05-19** — D1 LLVM JIT + D2 Sound Gradual Typing 完成。106/106 测试全绿。

---

## 完成度评分

| 维度 | 分数 | 说明 |
|------|------|------|
| 语言核心求值 | 🟢 10/10 | TW + IR 双路径 + 显式调用栈 |
| **类型系统** | 🟢 10/10 | Sound Gradual + coercion + occurrence + let-poly + type query + blame |
| LLVM JIT | 🟢 10/10 | ORC JIT, 38 opcode native, -O2, 增量 cache, 闭包/Pair/PrimCall |
| 编译器基础设施 | 🟢 9/10 | ArenaGroup / 增量 / 磁盘缓存 / 热替换 / IR import |
| 测试覆盖 | 🟢 8/10 | integ 87 + unit 74 + smoke 5 + bash 117 + bench 44 |
| 标准库 | 🟢 8/10 | 19 文件 ~1k 行 |
| 错误处理 | 🟢 9/10 | try/catch IR + diag + AST validate |
| EDSL / AI Agent | 🟢 9/10 | set-code/query/mutate/typecheck + LLM pipeline |
| 文档 | 🟢 8/10 | README + tutorial + design repo |

---

## 已完成

### Phase A-D: 核心功能
| Phase | 主要内容 | 状态 |
|-------|---------|------|
| A | Tree-walker, 宏, 模块, eval-var, closure bridge | ✅ |
| B | 增量编译, 类型系统 L6, diagnostics, CI/CD | ✅ |
| C | 卫生宏, AST 验证, IR import, stdlib v3 | ✅ |
| **D1** | **LLVM JIT** — ORC 编译, 算术/闭包/Cell/Pair/CastOp, PrimCall bridge, -O2, 增量 cache | ✅ |
| **D2** | **Sound Gradual Typing** — Coercion, CastOp, bi-directional check, occurrence, type-of, blame, type query | ✅ |

### Phase D1: LLVM ORC JIT
```
fib-20: TW 48.6ms → IR 23.0ms → JIT 6.4ms (7.55x)
```
- P1 基础架构: AuraJIT, ORC LLJIT ✅
- P2 算术: 38 opcode, 控制流, 比较 ✅
- P3 闭包+Cell: 捕获修复, 递归闭包 ✅
- P4 运行时: PrimCall bridge, display, eval 集成 ✅
- P5 优化: LLVM -O2 PassBuilder, 增量 cache ✅

### Phase D2: Sound Gradual Typing
- P1 Coercion: CastOp JIT + IR, `(cast expr : Type)` ✅
- P2 Bidirectional: `(check expr : Type)`, TypeAnnotation ✅
- P3 Type Language: `(: name Type)`, type-of, blame labels, type query ✅
- P4 Occurrence: predicate narrowing (string? → String, number? → Int) ✅

---

## 待启动

### D3: 自举 (40h)
Aura 编译器用 Aura 写。等前面稳定后再启。

### 已完成 (最新)
- **C FFI**: `c-load`/`c-func` — dlopen/dlsym, Int/Float/String/Opaque marshalling, JIT symbol API

### 短期改善 (1-3h/each)
- JIT EvalValue 兼容: Bool/Pair/String 正确编码 → auto-JIT 覆盖全量
- stdlib 补全: json/validate/struct 生产级
- `--serve` AI agent 优化
- FFI: JIT 符号表集成 → 零开销 C 调用
