# Aura 已知问题

**更新：2026-05-25 — 对齐当前实现，移除已解决项**

---

## 开放中的问题

### 类型系统（P1）
| # | 问题 | 说明 | 影响 |
|---|------|------|:----:|
| 1 | Occurrence typing cond/match 收窄不触发 | cond 通过 if 脱糖但不触发 occ 收窄；match 模式变量作用域与 occ 有 bug | type-occ-cond, type-occ-deep, type-occ-match, adt-either |
| 2 | Blame 运行时输出 `<unknown>` | `(+ 1 "hello")` 不显示谁 blame 谁 | type-blame-runtime |
| 3 | `symbol->string` / `string->number` 等转换原语缺失 | `(+ 42 "hello")` 的 coercion 需要这些中间函数 | type-coercion-chain, type-grad-multi-boundary |
| 4 | `:Int` 标注语法无法解析 | `(: x Int 42)` 被 parser 拒绝 | type-gradual-boundary |
| 5 | 标注 erasure 不彻底 | 带 / 不带 `: Int` 的执行路径不同 | type-gradual-erasure |
| 6 | Let-poly value restriction 过严格 | lambda 绑定不泛化 | type-let-poly-hof |
| 7 | Linear HOF move + closure 交互 bug | `(move (f x))` move 了不该回收的值 | type-linear-hof |
| 8 | Borrow 跨作用域运行时标记不清理 | borrow 结束后 move 报错 | m4-borrow-chain |

### 标准库 / EDSL（P2）
| # | 问题 | 说明 | 影响 |
|---|------|------|:----:|
| 9 | 运行时缺少关键字参数支持 | `:description`, `:workspace` 等无法作为原语使用 | std/rule, std/pipeline 不可用 |
| 10 | `mutate:replace-value` + `eval-current` 不一致 | 修改后 eval 不反映 mutation | edsl-mutation-rollback |
| 11 | `ast:snapshot` / `ast:list-snapshots` 只存内存 | `list-snapshots` 返回空 | edsl-snapshot-multi |
| 12 | `workspace:switch` + COW 写泄露 | 子 workspace 修改影响父 workspace | edsl-workspace-cow |
| 13 | `require` 语法解析不完整 | `(require 'std/... all:)` 形式解析不了 | edsl-require-stdlib |
| 14 | `synthesize:pipeline` invalid node id | 模板填充后 AST node id 不连续 | edsl-synthesize-pipeline |

### LLM 集成 / FFI（P3）
| # | 问题 | 说明 | 影响 |
|---|------|------|:----:|
| 15 | c-func hint 不直接 | LLM 持续用 0 而非 RTLD_DEFAULT(-1) | ffi-sqrt |
| 16 | c-func 调用缺少完整示例 | LLM 只写 `(c-func ...)` 不调用 | ffi-strlen |
| 17 | tcp-connect 依赖外部服务 | 无 timeout 或 mock 模式 | tcp-connect |
| 18 | 算法任务示例不全 | LLM 猜不对预期值方向 | palindrome, sieve, table-lookup, list-flatten, bench-parse |
| 19 | ADT match 语法示例不足 | LLM 不熟悉 match 语法细节 | adt-either, adt-option |

### 其他
| # | 问题 | 说明 | 影响 |
|---|------|------|:----:|
| 20 | AOT 布尔值输出 raw int | 无法区分 `#t` 和 integer 1 | AOT |
| 21 | struct 模块 AOT 不工作 | define-type 走 IR 路径不处理 | AOT |
| 22 | `display` 嵌套对/improper list 格式化 | 可改进 | Display |
| 23 | messaging 缺少阻塞 recv | 单线程 serve 超时轮询 | Serve |
| 24 | workspace tree 非全局 | 无法跨 serve session 共享 | Ws |
| 25 | `synthesize:optimize` fitness 仅基于代码长度 | 需 benchmark 驱动的真实 fitness | Opt |
| 26 | 规则持久化仅支持 JSON 文件 | 缺少内置 VCS 集成 | Rule |
