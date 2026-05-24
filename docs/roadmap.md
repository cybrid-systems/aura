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
| `--emit-binary` 测试 | ✅ | 14 个数值/对/闭包测试 |
| **runtime.c 单元测试** | **⏳** | 独立 C 级测试，验证 Bump/Drop/闭包/字符串 |
| 真实 AOT 编译器 | ⏳ | 替代当前 stub（`--ir` 结果硬编码到 C printf） |

### 近期计划

```
1. runtime.c 单元测试（C level, ~2d）
   ├─ 分配 + Drop + Free List 复用
   ├─ 闭包捕获 + 调用
   ├─ Bump 多次分配 + Reset
   └─ 字符串分配/引用

2. aura_emit_native_file 改为真正 LLVM 编译 + 链接 runtime.c（~4d）
   ├─ 替代当前 stub 实现
   ├─ 编译 .o + 链接 runtime.c → 独立 ELF
   └─ 启用完整的非数值输出测试
```
