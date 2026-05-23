# P0: Colony Search 下沉 — 实施计划

## 现状

| 组件 | 状态 | 说明 |
|------|:----:|------|
| `lib/std/ant.aura` | ✅ | 信息素系统 (init/update/rank/export) |
| `_gen_edsl_variants()` | ✅ | Python 侧 EDSL 变体生成 |
| `internal_colony_search()` | ✅ | Python 侧蚁群搜索循环 |
| `colony:search` (ant.aura) | ❌ | 存根，未实现 |
| serve.exec_batch() | ✅ | 批量 exec 支持 |
| PID phase 集成 | ✅ | fine/putt 自动调用 |

## 差距

1. **`colony:search` 未实现** — 纯 Aura 版搜索循环是空的
2. **搜索只在 attempt=0** — attempt ≥1 时即使 fine/putt 也不走 colony
3. **变体数量限制** — putt=5, fine=12, 远低于设计目标的 1000+

## Step 1: 实现 colony:search (ant.aura)

核心：纯 Aura 本地搜索，不依赖 Python。

```scheme
(colony:search expected-str max-variants)
```

代替 Python 的 `_gen_edsl_variants` + `serve.exec_batch` 组合，
把所有变体生成+测试放在一次 `serve.exec()` 内完成。

### 变异策略
1. display-ref — 把函数体包一层 `(display val)`
2. lit-tweak — 数值 +1/-1/+2/-2
3. op-swap — `<↔<=`, `=↔not=` 等
4. cond-flip — 交换 if then/else 分支

## Step 2: 放开 attempt 限制

Phase=fine/putt 时，所有 attempt 都先试 colony。

## Step 3: 增加变体数量

fine: 12 → 50, putt: 5 → 20。

## 预期收益

- 每轮搜索从 ~300ms 降到 ~20ms（减少 Python ↔ serve 交互次数）
- 更少的 LLM 调用 → 更快、更便宜、方差更低
