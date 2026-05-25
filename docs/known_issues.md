# Aura 已知问题

**更新：2026-05-25**

---

## 开放中的问题

| # | 问题 | 工作量 | 说明 |
|---|------|:------:|------|
| 38 | JIT OpConstString 传空字符串 | 1d | IR 模块的 string pool 未传入 JIT，字符串常量显示为空 |
| 39 | stdlib 在 `--emit-binary` 中不可用 | 2d | 29 个 stdlib 模块需 AOT 路径支持 |
| 40 | 所有权模型在 binary 路径中未完全落地 | 3d | IR 层 `OpDrop` 指令已生成，但 LLVM lowering 自动插入 drop 还需完善 |
| 41 | `cons` 在 AOT 路径中不可用 | 1d | 当前通过 evaluator 原语派发（OpPrimitive+OpCall），AOT 缺 runtime 对应的 `aura_alloc_pair` 绑定 |
| 42 | evaluator 原语作为闭包值传递在 AOT 中不可用 | 2d | `(map + '(1 2 3))` 中 `+` 作为值通过 OpPrimitive 派发，AOT 缺原语表 |
| 43 | `list` 展开为 cons 链在 AOT 中不可用 | 2d | `(list 1 2 3)` 依赖 evaluator 的 `list` 原语 |
| 44 | `display` AOT 输出 side-effect + 返回值合并 | 0.5d | `(display 42)` 输出 `4242` 而非 `42` |
| 45 | AOT 布尔值输出 raw int（`1` 而非 `#t`） | 0.5d | 原生二进制约定了格式，与 eval 输出不一致 |
| 46 | 多文件 AOT 编译 | 3d | 当前只支持单表达式管道输入 |

## 已解决

| # | 问题 | 解决方式 |
|---|------|----------|
| 1-30 | P0/P1/P2 各项 | 2026-05-23 前已完成 |
| 31 | `--emit-binary` 无运行时 | `lib/runtime.c` 已实现 Bump Allocator + Drop + 闭包 |
| 32 | runtime 无 drop/释放机制 | `aura_drop_*` 函数族 + Free List 复用 |
| 33 | runtime 闭包捕获为空 | `aura_closure_capture` 真实实现 |
| 34 | runtime 无字符串分配 | `aura_alloc_string` / `aura_string_ref` |
| 35 | match + ADT segfault | 修复 `if (!ctors) continue;` 缺失 + self-move-assignment |
| 36 | `--emit-binary` 是 stub 实现 | **P2.6 真实 AOT 编译器完成**：LLVM IR → llc → .o → 链接 → ELF |
| 37 | runtime.c 无独立单元测试 | 23 个 C 级测试用例 |
