# Aura 已知问题

更新：2026-05-23

---

## 已修复（2026-05-23）

### P0 — 编译器缺陷

| Issue | Fix |
|-------|:---:|
| `(+ 1 "hello")` 静默返回 1 | `052cb19` — IR + tree-walker 输出 stderr blame |
| `eval_flat: unsupported node type` | `50208da` — Linear/Move/Borrow/Drop 添加 |
| `(: x Int)` 无绑定返回 0 | `82dfaf4` — TypeAnnotation 走树遍历器报错 |
| `(: name Type val)` 三参数 | `afe96fd` — parser 正确消费 3 参数 |
| `#<procedure>` 丢分 | `f8166c7` — system prompt + task hint |

### P1 — 功能缺口

| Issue | Fix |
|-------|:---:|
| match 穷尽性检查 | `de2c59d` — 4 步完整实现（parser → 构造器表 → type checker → 测试） |
| 模块 import 类型 | `fab6a13` — 28 个 stdlib 模块 90+ 签名注册 |
| M4 线性所有权 | `6e3f78f` — double-move/use-after-move/double-drop 运行时检测 |
| Parser 修复 | `6e3f78f` — let/letrec 用 parse_expr、M4 关键字消费 RParen |
| `let` 泛化 | 已验证 synthesize 路径已正常工作（`register_forall` + `instantiate_all_direct`） |
| 增量类型检查 | 已验证已实现（`is_dirty` + `set_type`） |
| Blame 覆盖 | 已验证足够（CastOp + 算术 coercion + TypeAnnotation） |

### P2 — 增强

| Issue | Fix |
|-------|:---:|
| `--inspect` 扩展 | `03be5f1` — ir/closures/cache/typecheck/evaluator/pretty/cache-open 全覆盖 |
| Serve 超时熔断 | `e259288` — 30s async timeout |
| P2996 反射 | `b07b5c6` — auto_to_json/auto_serialize 委托 P1306 递归实现，支持嵌套 struct / 泛型 vector / array / enum |

---

## 未解问题

（当前无未解 issue）
