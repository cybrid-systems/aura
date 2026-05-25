# Aura 已知问题

**更新：2026-05-25**

---

## 开放中的问题

| # | 问题 | 说明 |
|---|------|------|
| 45 | AOT 布尔值输出 raw int（`1` 而非 `#t`） | untagged runtime 无法区分 `#t` 和 integer `1`，保持现状 |

## 已解决

| # | 问题 | 解决方式 |
|---|------|----------|
| 36 | `--emit-binary` 是 stub | **P2.6 真实 AOT**: LLVM IR → llc → .o → 链接 → ELF |
| 37 | runtime.c 无单元测试 | 23 个 C 级测试 |
| 38 | OpConstString 传空字符串 | 从 IRModule 的 string pool 传递真实内容 |
| 39 | stdlib AOT 不可用 | string ops/list ops/map/filter/foldl 已支持 |
| 40 | 所有权模型 binary 路径未落地 | DropOp(43) 等 linear ops 已实现 passthrough/drop |
| 41 | `cons` AOT 不可用 | lowering 展开为 OpMakePair + 负数 sentinel |
| 42 | 原语作为闭包值不可用 | AOT OpPrimitive 存负数 sentinel + 派发表 + 编译器生成 dispatch |
| 43 | `list` AOT 不可用 | lowering 展开为嵌套 OpMakePair |
| 44 | display 输出重复 | `g_display_was_called` 标志 → `42` 而非 `4242` |
| 46 | 多文件 AOT | 支持 `./aura --emit-binary a.aura b.aura out` |
