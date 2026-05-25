# Aura 路线图

**更新：2026-05-25**

---

## 最新里程碑：P2.6 — 真实 AOT 编译器

**目标：** `--emit-binary` 从 shell wrapper 升级为真正的 LLVM AOT 编译管道。
生成原生 ELF 二进制，不依赖 aura 本体。

| 项目 | 状态 | 说明 |
|:-----|:----:|:-----|
| AOT LLVM IR 管道 | ✅ | `FlatFunction → LLVM IR → O2 → .ll → llc → .o` |
| 原生链接 | ✅ | 每函数 .o + runtime.c + function registration = 独立 ELF |
| 闭包 AOT | ✅ | func_table + `__attribute__((constructor))` 动态函数指针注册 |
| 算术/比较/if/let | ✅ | 直接内联为 LLVM IR 指令 |
| cons/car/cdr/display | ✅ | runtime.c 原生函数调用 |
| `and`/`or`/`not` | ✅ | Lowering pass 展开为条件分支（不进原语派发） |
| `pair?`/`null?` | ✅ | 通过 PrimId 走 OpPrimCall → LLVM ICmp |
| `quotient`/`remainder` | ✅ | 安全零除检查内联 |

### AOT 测试覆盖率

**26 个 `--emit-binary` 测试全部通过：**

```
add sub mul neg chain car cdr
pair? not-pair? null? not-null?
eq-lit lt bool
closure closure2
and or not and-chain
if-true if-false let
quotient remainder display
```

### 原生二进制格式

```bash
$ echo '(+ 1 2)' | ./build/aura --emit-binary /tmp/myapp
$ /tmp/myapp
3
$ file /tmp/myapp
ELF 64-bit LSB executable, ARM aarch64, dynamically linked, not stripped
$ readelf -h /tmp/myapp | grep Machine
Machine: ARM AARCH64
```

**输出规范：** 原生二进制输出原始 int64_t 值。
布尔值：`1` = `#t`, `0`（不输出，同 Scheme 空列表惯例）。
`display` 副作用 + 返回值合并输出（如 `(display 42)` → `4242`）。

---

## 近期计划

### P2.7 — AOT 原语补全

| 项目 | 工作量 | 说明 |
|:-----|:------:|:-----|
| `cons` 在 AOT 路径支持 | 1d | 当前通过 OpPrimitive + OpCall 派发，需改为 runtime.c 直接调用 |
| `list` 展开为 cons 链 | 2d | 降低对 eval 原语表的依赖 |
| `+ - * / = < >` 等传值调用 | 1d | 当前作为函数值传递时走闭包派发，不工作 |
| `display` 输出格式统一 | 0.5d | 分离 side-effect 与 return value 输出 |
| `string-append`/`string-length` 等 | 2d | 常用 stdlib 原语的 AOT 支持 |

### P3 — 工具链

| 项目 | 说明 |
|:-----|:-----|
| LSP 服务器 | 增量诊断、补全、类型预览 |
| 包管理 | 依赖解析、远程缓存 |
| 自举编译器 | Aura 写的 Aura 编译器 |

### P4 — 性能

| 项目 | 预期提升 | 说明 |
|:-----|:--------:|:-----|
| AOT -O3（当前 -O2） | ~5% | LLVM 默认 -O2，可升级 |
| LTO | ~10% | 链接时优化跨编译单元 |
| 内联运行时函数 | ~15% | 将 runtime.c 中的短函数标记 `__attribute__((always_inline))` |
| 多文件 AOT | — | 一次编译多个 .aura 文件 + 全局链接 |
| 模块化 AOT | — | stdlib 模块预编译为 .o，链接进应用 |

---

## 已完成里程碑

### P2.5 — Bump Allocator + Drop (2026-05-23 ~ 24)

| 项目 | 状态 |
|:-----|:----:|
| runtime.c Bump Allocator | ✅ |
| runtime.c 闭包捕获 | ✅ |
| runtime.c drop 函数族 | ✅ |
| JIT OpDrop 指令 | ✅ |
| JIT 字符串支持 | ✅ |
| `--emit-binary` 测试 | ✅ (24 → 26 tests) |
| runtime.c 单元测试 | ✅ (23 C 级测试) |
| AOT 编译器 | ✅ **P2.6 已接替** |

### P2 — JIT 编译器 (已完成)

- ORC JIT v2 后端
- 38 opcode → native code
- 7.55× 性能提升 vs tree-walker
- 字符串、闭包、drop 完整支持

### P1 — IR + Pass Manager (已完成)

- TypeSpecializationWrap
- ComputeKindWrap
- ArityWrap
- ConstantFoldingWrap

### P0 — 核心求值 (已完成)

- Tree-walker + IR 双路径
- Sound Gradual Typing
- ADT + match 穷尽性检查
- EDSL 自修改
- 模块系统
- 标准库 (29 模块)
- C FFI
- 错误处理
