# Aura 已知问题

**更新：2026-05-26 — EDSL benchmark 跑分后新增**

---

## 🔴 确认存在的编译器 Bug

| # | 问题 | 影响 | 优先级 |
|:-:|:-----|:----:|:------:|
| 1 | **`(: name Type val)` 不绑定变量名** — parse 把 `var_sym` 丢弃，只创建 TypeAnnotation 节点不 bind。`(: x Int 5)` 返回 5 但 `x` 没被定义 | 类型标注脚本、EDSLLM codegen | ✅ 已修 |
| 2 | **`c-func` FFI 调用返回 closure** — `((c-func -1 "strlen" ...) "hello")` 返回 `#<procedure>` 而非 `5` | C FFI 测试 | ✅ 已修 |
| 3 | **Pipe mode 下未绑定变量返回 0 而非 error** — `(display x)` 在 `x` 不存在时返回 0 而不是报 unbound variable | 调试体验 | P2 |
| 4 | **`edsl-snapshot-multi` 内联代码输出为空** — `ast:snapshot`/`ast:list-snapshots` 在 pipe mode 可能异常 | 快照测试 | P3 |

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

## 已关闭（不做）
| 问题 | 原因 |
|:-----|:------|
| 规则持久化 VCS 集成 | JSON save/load 够用 |
| 权限模型（module / symbol whitelist） | `workspace:lock` + COW 隔离够用 |
| eval 资源限制（CPU / memory / recursion depth） | 当前无多租户场景 |

## 最近修复
| 问题 | 修复 |
|:-----|:------|
| messaging 阻塞 recv | `pop_message` fiber yield + `g_fiber_block` 回调 ✅ |
| AOT 布尔值 raw int | pointer tagging + bool sentinel ✅ |
| struct AOT 不工作 | `define-type` 预注册构造函数 ✅ |
| workspace tree 跨 session 共享 | 共享 WorkspaceTree ✅ |
| `synthesize:optimize` fitness 基于代码长度 | benchmark 驱动探测 ✅ |
