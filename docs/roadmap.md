# Aura 路线图

**更新：2026-05-26 — AOT 深入修复完成，emit 57/57 全绿**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心测试 | ✅ 7 suites / 57 emit / 全部通过 |
| 编译器 Bug | ✅ 0 个 open |
| EDSL Benchmark (Grok) | 113/135 (83.7%) |
| Benchmark 剩余失败 | 纯 LLM 生成质量，0 个编译器 bug |

## 🔴 待办

| 优先级 | 任务 | 说明 | 预估 |
|:------:|:------|:------|:----:|
| P3 | **Float 算术 AOT** — Add/Sub/Mul/Div 正确处理 float 操作数 | 当前 fixnum+float 混合运算退化为 fixnum | 1d |
| P4 | **Stdlib 递归 AOT** — permutations 等嵌套 closure 递归 segfault | 深层 closure calling + 递归交互问题 | 1-2d |
| P5 | **Benchmark 完全内建** — 用 synthesize-v2 替换 Python benchmark | `self_bench.aura` 原型已有，需完善 | 1-2d |
