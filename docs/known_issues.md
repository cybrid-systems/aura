# Aura 已知问题

**更新：2026-05-25 (AOT 54 emit 全绿)**

---

## 开放中的问题

| # | 问题 | 说明 |
|---|------|------|
| 45 | AOT 布尔值输出 raw int（`1` 而非 `#t`） | untagged runtime 无法区分 `#t` 和 integer `1` |
| — | `display` 嵌套对/ improper list 格式化 | `((1 . 2) 3)` 输出 `((1) 3)` |
| — | `list:map`/`list:filter` 等模块内自引用 | list 模块内部函数使用自引用而非 named let |
| — | stdlib 缓存 | 当前用源码内联，未走预编译 .o |

## 已解决 (P2 AOT)

| # | 问题 | 解决方式 |
|---|------|----------|
| 36 | `--emit-binary` 是 stub | P2.6 真实 AOT: LLVM IR → llc → 链接 → ELF |
| 37 | runtime.c 无单元测试 | 23 个 C 级测试 |
| 38 | OpConstString 传空字符串 | 从 IRModule string pool 传递真实内容 |
| 39 | stdlib AOT 不可用 | string ops/list ops/map/filter/foldl/apply/permutations |
| 40 | 所有权模型 binary 路径未落地 | DropOp(43) 等 linear ops passthrough/drop |
| 41 | `cons` AOT 不可用 | lowering 展开为 OpMakePair + 负数 sentinel |
| 42 | 原语作为闭包值不可用 | AOT OpPrimitive 负数 sentinel + 派发表 |
| 43 | `list` AOT 不可用 | lowering 展开为嵌套 OpMakePair |
| 44 | display 输出重复 | g_display_was_called 标志 → `42` 而非 `4242` |
| — | display 输出 raw sentinel | 格式化列表为 `(a b c)` |
| 46 | 多文件 AOT | 支持 `./aura --emit-binary a.aura b.aura out` |
| — | named let 递归 | aura_closure_call 设置捕获 env |
| — | apply | runtime.c 遍历参数列表后调用 aura_closure_call |
