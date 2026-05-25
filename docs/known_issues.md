# Aura 已知问题

**更新：2026-05-25 (AOT 56 emit 全绿)**

---

## 开放中的问题

| # | 问题 | 说明 |
|---|------|------|
| 45 | AOT 布尔值输出 raw int（`1` 而非 `#t`） | untagged runtime 无法区分 `#t` 和 integer 1 |
| 47 | struct 模块 AOT 不工作 | 使用 `define-type`(EDSL)，IR 路径不处理 AST 操作 |
| 48 | `display` 嵌套对/improper list 格式化不完美 | `((1 . 2) 3 . 4)` 显示 `((1 . 2) 3 . 4)`，和 eval 一致但待审查 |

## 已解决 (P2 AOT)

| # | 问题 | 解决方式 |
|---|------|----------|
| 36 | `--emit-binary` 是 stub | 真实 AOT: LLVM IR → llc → 链接 → ELF |
| 37 | runtime.c 无单元测试 | 23 个 C 级测试 |
| 38 | OpConstString 传空字符串 | 从 IRModule string pool 传递真实内容 |
| 39 | stdlib AOT 不可用 | 全部核心 stdlib 模块已验证 |
| 40 | 所有权模型 binary 路径未落地 | LinearWrap/MoveOp/BorrowOp/DropOp 正确处理 |
| 41 | `cons` AOT 不可用 | lowering 展开 + 负数 sentinel |
| 42 | 原语作为闭包值不可用 | AOT OpPrimitive → 负数 sentinel + 派发表 |
| 43 | `list` AOT 不可用 | lowering 展开为嵌套 OpMakePair |
| 44 | display 输出重复 | g_display_was_called 标志 |
| — | display 输出 raw sentinel | 格式化列表为 `(a b c)`，improper 为 `(a . b)` |
| 46 | 多文件 AOT | 支持 `./aura --emit-binary a.aura b.aura out` |
| — | named let 递归 | aura_closure_call 设置捕获 env |
| — | apply | runtime.c 遍历参数列表后调用 aura_closure_call |
