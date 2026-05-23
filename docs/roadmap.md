# Aura — 路线图

**更新：2026-05-23**

**当前定位**：语言核心可用（Sound Gradual Typing + ADT + M4 Linear Ownership + EDSL），
三模型 benchmark 89-92%。适合 LLM agent 生成 50-200 行代码片段的迭代修复场景。
下一步方向：补齐项目级编程的短板（文件 I/O、CLI、错误处理），而非加语言特性。

---

## 🔴 P0 — 短板，立刻提升可用性

| # | 缺口 | 具体任务 | 预估 | 状态 |
|---|------|---------|:----:|:----:|
| 25 | **文件系统原语** | `read-file`、`write-file`、`file-exists?`、`file-delete`、`directory-list` | — | ✅ 已有 |
| 26 | **CLI argv** | `(command-line)` 返回参数字符串列表。 | 0.5d | ✅ 已加 |
| 27 | **错误处理 try-catch** | `(try expr (catch (var) handler))` | — | ✅ 已有 |

## 🟡 P1 — 功能扩展

| # | 缺口 | 具体任务 | 预估 |
|---|------|---------|:----:|
| 28 | **stdlib: 文件系统** | `lib/std/fs.aura`：路径拼接、目录遍历、文件元数据。 | 1d |
| 29 | **进程原语** | `(shell cmd)`、`(command-output cmd)` | — | ✅ 已加 |
| 30 | **错误类型结构化** | 当前所有错误/诊断都是字符串。结构化 `(error type message context)` 便于 LLM 解析和 colony 搜索。 | 1-2d |

## 🟢 P2 — 中远期

| # | 缺口 | 说明 |
|---|------|------|
| 31 | 模块系统 + 编译单元 | `import`/`require` 已有基本实现，缺跨文件依赖解析和缓存。 |
| 32 | 数值计算 | 有 `Float` 和基本运算，缺数组/向量/矩阵。 |
| 33 | IDE / LSP 支持 | 等语言稳定。 |
| 34 | 包管理 | 远期。 |
| 35 | 自举 | 类型系统稳定后。 |

---

## Benchmark 基线（85 任务，max-attempts=3，1 轮，2026-05-23 PM）

| 模型 | 通过率 | 耗时 | 短板 |
|:----|:------:|:----:|:-----|
| 🥇 Grok 4.3 | **78/85 (91.8%)** | ~13min | algorithm (3), ffi-sqrt, type-annot-fn, type-blame-runtime |
| 🥇 DeepSeek v4 Flash | **77/85 (90.6%)** | ~46min | adt-option, algorithm (2), ffi (2), type-annot-fn, type-blame-runtime |
| 🥈 MiniMax M2.7 | **76/85 (89.4%)** | ~23min | adt-option, algorithm (2), ffi (2), json-roundtrip, tcp-connect, type (2) |

## 当前规划优先级

1. **P0: 文件系统原语** — 补齐最大短板，让 Aura 能读写文件
2. **P0: CLI argv** — standalone 脚本可用
3. **P0: try-catch** — 完善错误处理
4. **P1: stdlib 文件系统 + 进程** — 生态建设
5. **P1: 错误类型结构化** — 提升 LLM 诊断质量

## 已解决（2026-05-23 全天）

P0: blame / eval_flat / TypeAnnotation / parser / (#procedure)
P1: match 穷尽 / 模块类型 / M4 运行时 / parser 修复 / let 泛化 / 增量检查 / blame 覆盖
P2: --inspect 七子命令 / serve 超时 / P2996 递归序列化
Phase 2: pure Aura colony:search / eval-current-output fd 重定向
P3: PID analyzer (pid:analyze) / colony:search lit-tweak
编译器诊断: FFI 错误→stdout / ((: x Int)) lambda 参数 / closure warning→stdout / FFI 签名错误精确定位 / occurrence typing (integer? void? hash?)
