# Aura 路线图

**更新：2026-05-26 — Stdlib 扩张完成，仅剩 AOT + Benchmark**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心编译器 | ✅ 7 suites / 124 integ / 92 stdlib suite / 9 regression |
| 编译器 Bug | ✅ 0 个 open |
| EDSL Benchmark (Grok) | 113/135 (83.7%) |
| benchmark 剩余失败 | 纯 LLM 生成质量，0 个编译器 bug |

## 🔴 待办

| 优先级 | 任务 | 说明 | 预估 |
|:------:|:------|:------|:----:|
| P3 | **AOT 落地** — 数字/字符串/closure 的 AOT 编译 | 当前只有布尔 AOT，扩展到数字/字符串/closure | 3-5d |
| P4 | **Benchmark 完全内建** — 用 synthesize-v2 替换 Python benchmark | `self_bench.aura` 原型已有，需完善 task 发现和报告 | 1-2d |
