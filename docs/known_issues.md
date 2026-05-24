# Aura 已知问题

**更新：2026-05-24**

---

## 开放中的问题

| # | 问题 | 工作量 | 说明 |
|---|------|:------:|------|
| 36 | `--emit-binary` 是 stub 实现 | ✅ 已改善 | 修复非数值输出（`#t`/字符串），`aura_emit_native_file` 现在正确处理非数值结果。真实 AOT（LLVM .o → 链接 runtime.c）待后续 |
| 37 | runtime.c 无独立单元测试 | ✅ 已加 | 23 个 C 级测试用例验证 Bump/Drop/闭包/字符串 |
| 38 | JIT OpConstString 传空字符串 | 1d | IR 模块的 string pool 未传入 JIT，字符串常量显示为空 |
| 39 | stdlib 在 `--emit-binary` 中不可用 | 2d | 29 个 stdlib 模块需 AOT 路径支持 |
| 40 | 所有权模型在 binary 路径中未完全落地 | 3d | IR 层 `OpDrop` 指令已生成，但 LLVM lowering 自动插入 drop 还需完善 |

## 已解决

| # | 问题 | 解决方式 |
|---|------|----------|
| 1-30 | P0/P1/P2 各项 | 2026-05-23 前已完成 |
| 31 | `--emit-binary` 无运行时 | `lib/runtime.c` 已实现 Bump Allocator + Drop + 闭包 |
| 32 | runtime 无 drop/释放机制 | `aura_drop_*` 函数族 + Free List 复用 |
| 33 | runtime 闭包捕获为空 | `aura_closure_capture` 真实实现 |
| 34 | runtime 无字符串分配 | `aura_alloc_string` / `aura_string_ref` |
| 35 | match + ADT segfault | 修复 `if (!ctors) continue;` 缺失 + self-move-assignment |
