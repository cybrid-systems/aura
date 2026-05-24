# Aura 路线图

**更新：2026-05-24**

---

## 当前工作

### P2.5 — --emit-binary 运行时改造（高优先级）

目标：让 `--emit-binary` 生成的独立二进制支持 M4 drop 语义。
采用 **Bump Allocator + Arena Reset**（已完成核心功能）。

| 项目 | 状态 | 说明 |
|:-----|:----:|:-----|
| runtime.c Bump Allocator | ✅ | `aura_bump_init/alloc/reset`，动态扩容 |
| runtime.c 闭包捕获 | ✅ | `aura_closure_capture` 真实实现 |
| runtime.c drop 函数族 | ✅ | `aura_drop_pair/cell/closure` + Free List |
| JIT OpDrop 指令处理 | ✅ | `OpDrop` → `aura_drop_*` 调用 |
| JIT 字符串支持 | ✅ | `aura_alloc_string` / `fn_string_ref` |
| `--emit-binary` 测试 | ✅ | 24 个测试（包含非数值 `#t`） |
| **runtime.c 单元测试** | **⏳** | 独立 C 级测试，验证 Bump/Drop/闭包/字符串 |
| 真实 AOT 编译器 | ⏳ | 替代当前 stub（`--ir` 结果硬编码到 C printf） |

### 近期计划

```
1. runtime.c 单元测试 — ✅ 已完成（23 个 C 级测试）

2. aura_emit_native_file 非数值输出修复 — ✅ 已完成
   ├─ #t/#f/字符串等非数值输出正确
   └─ 24 个 --emit-binary 测试通过

3. 真实 AOT 编译器 — ⏳ 后续
   ├─ LLVM .o → 链接 runtime.c → 独立 ELF
   └─ 替代当前 printf stub 实现
```
