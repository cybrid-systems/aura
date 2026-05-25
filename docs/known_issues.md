# Aura 已知问题

**更新：2026-05-25 — 清除非真实 bug，只保留确认存在的遗留问题**

---

## 确认存在的遗留问题

| # | 问题 | 说明 | 影响 |
|---|------|------|:----:|
| 1 | messaging 缺少阻塞 recv | 单线程 serve 无法实现真正阻塞，当前使用超时轮询 | Serve |
| 2 | workspace tree 非全局 | ~~每个 serve session 有独立 workspace tree~~ ✅ 已修复（共享 tree） | Ws |
| 3 | `synthesize:optimize` fitness 仅基于代码长度 | 需要 benchmark 驱动的真实 fitness | Opt |

## 已完成（旧问题已修）

| 问题 | 修复 |
|:-----|:------|
| messaging 阻塞 recv | `pop_message` fiber yield + `g_fiber_block` 回调，`(recv)` 空时 yield fiber ✅ |

## 已确认不是 bug（LLM 误报）

以下 items 经手动验证均正常工作，原诊断误判为编译器 bug：

### 类型系统（曾列 P1）
| 任务 | 验证结果 |
|:-----|:---------|
| cond/match occurrence typing 收窄 | 返回 `"number"` ✅ |
| Blame 运行时输出 `<unknown>` | 输出 `"expected Int, got String 'hello'"`，信息完整 |
| `symbol->string` / `string->number` 缺失 | `string->number` 存在；Aura 没有 symbol 类型（quote 返回 int），`symbol->string` 不适用 |
| `:Int` 标注语法 | `(: x Int 42)` → `42` ✅ |
| 标注 erasure | 标注不改变语义 ✅ |
| let-poly HOF value restriction | compose → 42 ✅ |
| linear HOF move + closure | `(move (f x))` → 42 ✅ |
| Borrow 跨作用域运行时错误 | borrow+move → 4242 ✅ |

### EDSL（曾列 P2）
| 任务 | 验证结果 |
|:-----|:---------|
| `mutate:replace-value` + `eval-current` 不一致 | 修改后 eval 正确反映 mutation ✅ |
| `ast:list-snapshots` 返回空 | 返回 `((0 . "s1"))` ✅ |
| `workspace:switch` + COW 写泄露 | 子 workspace 写操作隔离正常 ✅ |
| `require` 语法解析 | `(require "std/list" all:)` 正常工作 ✅ |
| `synthesize:pipeline` invalid node id | 已修复 ✅ |
| `string-append` 多参数 | `(string-append "a" "b" "c")` → `"abc"` ✅ |
