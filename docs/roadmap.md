# Aura 路线图

**更新：2026-05-26 — Phase 1-4 全部完成，核心修复全清**

---

## 项目状态

| 维度 | 数值 |
|:-----|:-----|
| 核心编译器 | ✅ 7 suites / 124 integ / 13 suite / 9 regression |
| 编译器 Bug | ✅ 0 个 open（GitHub #1 #2 已关）|
| GitHub Issues | ✅ 0 个 open |
| EDSL Benchmark (Grok) | 113/135 (83.7%) |
| EDSL Benchmark (DeepSeek) | 98/135 (72.6%) — LLM 方差 ±7% |
| benchmark 剩余失败 | 纯 LLM 生成质量，0 个编译器 bug |

## ✅ 已完成（Phase 1-4 + 修复）

- 核心求值器 + IR 管线 + LLVM ORC JIT + AOT + 增量编译
- EDSL / Synthesize / Workspace / Rule / 类型系统 / Stdlib / C FFI / Serve
- 编译器 bug 修复：类型标注绑定、FFI dispatch、pipe mode 报错、if-no-else 条件求值、rest-arg 崩溃
- API 签名生成（`get-full-api-ref` → system prompt 注入）
- **Synthesize Pipeline v2**: `test-driven` / `project` / `debug` / `compose` + LLM 修复循环
- 自托管 benchmark 原型（`tests/self_bench.aura`）
- EDSL hint 修复（6 个 task 文件） + 26+ suite 测试

## 🔴 剩余待办（按优先级）

| 优先级 | 任务 | 说明 | 预估 |
|:------:|:------|:------|:----:|
| P3 | **Stdlib 扩张** — HTTP client, Regex, 日期, 排序算法 | 当前 benchmark 测试需手写，加模块可改善 LLM 生成质量 | 2-3d |
| P3 | **AOT 落地** — 数字/字符串/closure 的 AOT 编译 | 当前只有布尔 AOT，扩展到更多类型 | 3-5d |
| P4 | **Benchmark 完全内建** — 用 synthesize-v2 替换 Python benchmark | `self_bench.aura` 原型已有，需完善 task 发现和报告 | 1-2d |
