# Aura 路线图

**更新：2026-05-26 — P2 全清理 + 编译器 Bug 修复 + API 签名生成**

---

## 项目状态

| 维度 | 状态 |
|:-----|:------|
| 核心编译器 | ✅ 稳定（7 suites / 124 integ / 82% EDSL benchmark） |
| 编译器 Bug | ✅ 0 个 open issue（GitHub #1 #2 均已关） |
| GitHub Issues | ✅ 0 个 open |
| 剩余 benchmark 失败 | 21 个（纯 LLM 生成质量，非编译 bug） |

## ✅ 已完成

| 内容 | 状态 |
|:-----|:----:|
| 核心求值器（tree-walker + IR 双路径 + TCO） | ✅ |
| IR 管线（lower → passes → IR interpreter → JIT → AOT） | ✅ |
| LLVM ORC JIT（38 opcode → native, 7.55× vs TW） | ✅ |
| 增量编译（ArenaGroup + 缓存 + 热替换 + IR import） | ✅ |
| AOT 56 emit 全部通过 | ✅ |
| EDSL 反射原语（query / mutate / ast 全套） | ✅ |
| EDSL synthesize（register-template / fill / define / pipeline / optimize） | ✅ |
| workspace 系统（create / switch / list / delete / lock / merge / COW） | ✅ |
| Rule 系统（define / apply / apply-all / save / load / 全功能） | ✅ |
| 类型系统（Gradual Typing / ADT / M4 Linear / occurrence / let-poly） | ✅ |
| 关键字 `:foo` 自求值 | ✅ |
| 标准库（32 个模块） | ✅ |
| C FFI（c-func / c-load / dlopen / 类型签名） | ✅ |
| Serve 协议（`--serve` JSON-line + `--serve-async` fiber/eventfd） | ✅ |
| `synthesize:optimize` benchmark-driven fitness | ✅ |
| Multi-session workspace 共享 | ✅ |
| 编译器 bug 修复（类型标注绑定 / FFI closure dispatch / pipe mode 报错） | ✅ |
| **API 签名生成** — 从 std/adaptive 自动提取全量 API 签名注入 LLM prompt | ✅ **新** |
| EDSL benchmark hint 修复（6 个 task 文件） | ✅ |

## 🔴 待办（按优先级）

| 优先级 | 任务 | 说明 | 预估 |
|:------:|:-----|:------|:----:|
| P2 | **合成管线升级** — 跨文件合成、测试驱动合成、synthesize:debug | 从单 expr 修 bug 升级到"写需求→LLM 生成→测试验证"全流程 | 3-5d |
| P3 | **Stdlib 扩张** — HTTP client, Regex, 日期, 排序算法 | 当前 benchmark 测试需手写，加模块会改善生成质量 | 2-3d |
| P3 | **AOT 落地** — 数字/字符串/closure 的 AOT 编译 | 当前只有布尔 AOT，扩展到更多类型 | 3-5d |

## 🔭 前瞻（已列入但不启动）

| 任务 | 说明 |
|:-----|:------|
| VS Code / Cursor 插件 | **暂时不做**（用户明确跳过） |
| LSP / 包管理 | **暂时不做** |
| 分布式 EDSL / 形式化证明 / Agent 框架集成 | 待定 |
| Hint 注入工程化 | **暂时不做**（已有 API 签名 + 已有 hints） |
