# Aura 路线图

**更新：2026-05-26 — 统一值表示完成，P3/P4 修复**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites / 57 emit / 全部通过 |
| 编译器 Bug | ✅ 0 个 open |
| EDSL Benchmark (Grok) | 113/135 (83.7%) |
| Benchmark 剩余失败 | 纯 LLM 生成质量，0 个编译器 bug |

## 已完成（今日）

| 任务 | 说明 |
|:-----|:------|
| AOT fixnum/bool 修复 | OpMul/OpDiv fixnum 编码, bool tagged (#t=7,#f=3), comparisons |
| AOT runtime pair 检查 | `IS_PAIR` 替换 `val < 0` — fix apply/reverse/range/unique |
| Float 显示 + 算术 | float pool + aura_alloc_float + AOT OpAdd/Sub/Mul/Div float 处理 |
| 统一值表示 | EvalValue 从 std::variant 改为 int64_t tagged, 与 AOT 完全兼容 |
| emit 测试修复 | 13 个 bool 测试 `1`→`#t` + remainder fix + nested-car 替换 permute |

## 🔴 待办

| 优先级 | 任务 | 说明 | 预估 |
|:------:|:------|:------|:----:|
| P5 | **Benchmark 完全内建** | 用 synthesize-v2 替换 Python benchmark | 1-2d |
