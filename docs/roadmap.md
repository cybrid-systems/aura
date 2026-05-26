# Aura 路线图

**更新：2026-05-26 — Float 算术 AOT 完成**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites / 57 emit / 全部通过 |
| 编译器 Bug | ✅ 0 个 open (已知: if_false 预期已修) |
| EDSL Benchmark (Grok) | 113/135 (83.7%) |
| Benchmark 剩余失败 | 纯 LLM 生成质量，0 个编译器 bug |

## 🔴 待办

| 优先级 | 任务 | 说明 | 预估 |
|:------:|:------|:------|:----:|
| P4 | **Stdlib 递归 AOT** — permutations segfault | `(import "std/algorithm")` 加载的 closure + `apply` 内部调用 crash；letrec 内联版正常 | 1-2d |
| P5 | **Benchmark 完全内建** — 用 synthesize-v2 替换 Python benchmark | `self_bench.aura` 原型已有，需完善 task 发现和报告 | 1-2d |
