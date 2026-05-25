# Aura 路线图

**更新：2026-05-25**

---

## 当前状态：P2 完成

**106 核心测试 + 48 `--emit-binary` 测试全部通过。**

### 已完成的 AOT 能力

| 分类 | 表达式 | 方式 |
|:-----|:-------|:-----|
| 算术 | `+ - * /` 链式 | LLVM IR inlined |
| 比较 | `= < > <= >=` | LLVM ICmp |
| 逻辑 | `and or not` | lowering 展开为分支 |
| 类型 | `pair? null?` | PrimId → LLVM ICmp |
| 对 | `cons car cdr` | runtime.c 函数 (负数 sentinel) |
| 列表 | `list length list-ref reverse append member` | lowering 展开 + PrimId dispatch |
| 高阶 | `map filter foldl` | 原语派发表 + `aura_closure_call` |
| 字符串 | `string-append/length/ref/<?/=?` `number->string` `string->number` | PrimId dispatch |
| 条件 | `if let` | LLVM branch/inlined |
| 闭包 | lambda、闭包捕获、闭包作为值传递 | func_table + constructor 注册 |
| 原语传递 | `+ - * / = < > <= >= not` 作为闭包值 | AOT 模式 OpPrimitive 负数 sentinel |
| IO | `display` | 运行时函数 + 无重复打印 |
| 所有权 | `drop move borrow Linear` | IR opcodes → passthrough/drop |
| 多文件 | `--emit-binary a.aura b.aura out` | 文件拼接 → 统一编译 |

### 技术细节

**编译管线：** `源码 → FlatAST → IRModule → FlatFunction → LLVM IR (O2) → .ll → llc -filetype=obj → .o → 链接 runtime.c → 独立 ELF`

**运行时：** 单个 `lib/runtime.c` 提供 bump allocator、pair/cell/closure heap、string pool、drop 函数族、PrimId 派发表、func_table。

**原语派发表：** 编译器在 `--emit-binary` 时枚举 evaluator 的原语表，生成每 slot 对应的 C 包装函数 → 编译链接进二进制。`aura_closure_call` 检测负数 closure_id 并派发到该表。

## 剩余 TODO

| 优先级 | 任务 | 说明 |
|:------:|:-----|------|
| 🟢 | stdlib 模块化 AOT | lib/*.aura 预编译为 .o |
| 🟢 | LSP / 包管理 / 自举 | 长期项 |
| 🟢 | AOT 性能 (O3/LTO/内联) | 功能完整后优化 |

## 历史

- P0 (2026-05-23): 核心求值 + 类型系统 + ADT + EDSL
- P1 (2026-05-23): IR + Pass Manager + 增量编译
- P2 (2026-05-23~25): JIT 编译器 → 真实 AOT 编译器
