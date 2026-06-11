# Aura 文档

> **推荐阅读路径（按受众）**
> - **AI Agent / LLM 写手**：先读 [tutorial.md](tutorial.md) 快速上手 → **[api-reference.md](api-reference.md)**（中央 Primitives Surface，带设计/代码/std/测试交叉引用） → 各 `design/core/` 的 **## 0. Implementation Status** 表格（当前实装真相）。
> - **想理解自修改核心**：`design/core/query_edsl.md` + `mutate_api.md` + `workspace_layering.md` + `typed_mutation.md`。
> - **想做 Agent 编排**：`design/core/agent_orchestration.md` + `std/orchestrator.aura` + `projects/evo-kv/` 示例。
> - **想贡献核心**：`docs/developer/evaluator.md` + 对应 core 设计 + `projects/GAPS.md`。

**文档分离**：`tutorial.md` + `api-reference.md`（含 std/） 作为 User Guide/Surface；`design/core/` 等作为 Design & Extension 参考。减少重复，通过交叉链接（见 api-ref 的 EDSL Surface 表）。

## 概况

| 文档 | 说明 |
|------|------|
| [教程](tutorial.md) | 上手实践，从零开始（含当前实装状态标注） |
| [API 参考](api-reference.md) | 所有原语参考（附实现状态提示） |
| [路线图](roadmap.md) | 发展方向 + 当前项目驱动迭代现状 |
| [基准](benchmark.md) | EDSL benchmark 数据 |
| [哲学](philosophy.md) | Aura 的设计思想 |

**项目驱动迭代**：当前主要通过 `projects/evo-kv/`（自演化 KV）等真实项目暴露 gap、修复核心。详情见 [projects/README.md](../projects/README.md) 和 `projects/GAPS.md`。

## 设计文档 (`design/`)

设计文档分三层：

- **`design/core/`** — 高价值核心设计 (6 篇) ，作为新贡献者的 on-ramp
- **`design/compilation/`** — 编译相关 (IR / JIT) (2 篇)
- **`design/runtime/`** — 运行时相关 (async serve / FFI) (2 篇)
- **`design/history/`** — 归档与历史（`notes/` + `closings/`）。包含 issue follow-up、推测设计等。新人可跳过；详见 history/ 下的 READMEs。

### Living Documentation Practices（活文档维护实践）

文档必须与代码保持同步，尤其是 EDSL 自修改核心（query/mutate/workspace/ast）。以下是强制实践：

#### 1. §0 Implementation Status 必须存在
**所有 `design/core/`、`design/compilation/`、`design/runtime/` 下的核心设计文档必须包含 `## 0. Implementation Status` 章节**（按 Issue #156 引入）。

该章节作为 **AI Agent 读者与新贡献者的"实现状态权威来源"**，要求：

1. **C++ Core Layer 表**：列出文档描述的所有 C++ 组件（primitive / class / pass / op），用 `✓` 标记已实装、`✗` 标记未做、`🟡` 标记部分实装 / 设计中、`△` 标记仅 API stub。标注实装源码位置（`src/...` 文件 + 行号或 `~L` 近似行号）。
2. **Aura Layer 表**：列出文档涉及的 Aura 端 helper / form / 原语，同样用 `✓` / `✗` / `🟡` / `△` 标注。
3. **未来工作 / 当前缺口**：列出未实装项 + 阻碍实现的依赖。
4. **AI Agent 读者请注意**：明确告诉 AI 读者本文档作为设计意图保留，但实装代码状态以 §0 + 文档内其它 "实现状态" 章节为准。

§0 章节应在文档开头的"标题 + 状态块"之后、第一个 `## 1. ...` 章节之前插入。**新加 `design/core|compilation|runtime/` 文档时必须同时建 §0**；修改现有文档时若新增/删除/重命名原语，需同步更新 §0 的两个表。

参考实现：
- `design/core/query_edsl.md` §0（#154 引入，原型）
- `design/core/mutate_api.md` §0（Phase 1 补全）
- 8 篇 `design/{core,compilation,runtime}/` 下的 §0（#156 批量补全）

#### 2. 新增/修改原语时的强制更新
当添加、修改或移除 primitive（尤其是 EDSL 相关）时，必须同步更新：
- `docs/api-reference.md`（Primitives Surface 表 + 代码位置 + std 包装）。
- 对应 `design/core/` 文档的 §0 Implementation Status 表（更新日期、实装标记、源码位置）。
- 如果影响 Agent 用法，更新 `tutorial.md` 示例或状态提示。
- 在 `src/compiler/evaluator_impl.cpp`（或相关）附近添加简要注释，引用设计文档。

示例：添加 `foo:bar` primitive 后，立即更新 query_edsl 或 mutate_api 的 §0 表，并刷新 api-ref 中的 EDSL Surface 引用。

#### 3. 归档内容路由规则（notes/ & closings/）
- **仅在以下情况才新增到 `design/history/notes/`**：真正未解决的 speculative 设计探索、早期 pipeline 研究、或与当前 core/ 无直接映射的愿景文档。
- 大多数 issue 工作 → 走 closing（`design/history/closings/`）+ 更新对应 core/ 文档的 §0 表。
- 永远不要在 notes/ 中重复 core/ 已实装的内容；如果需要历史上下文，加 "Superseded by: design/core/xxx.md §0" 指针。
- 新 contributions 到 history/ 时，必须在该目录的 README 中更新索引或关键文档列表。

#### 4. 状态横幅与日期
- 所有核心文档顶部或 §0 使用明确的 Status / Implementation Status（含日期 + Issue #）。
- 每次实质性变更后更新日期（如 "2026-06-11, Issue #156"）。
- 使用现有模式：`> **Status (日期, Issue #):** ✅ ...` 或 `## 0. Implementation Status`。

#### 5. 其他实践
- **api-reference.md 是中央真相**：任何原语变化必须先反映在这里（带交叉链接）。
- **减少重复**：User Guide（tutorial + api-ref）聚焦“怎么用 + 当前状态”；Design（core/）聚焦“为什么 + 架构 + 实现细节”。用交叉链接连接两者。
- **历史文件**：新 notes/ 仅限真正探索性内容；否则优先更新 core/ 或添加 closing。
- **定期维护**：重大 PR 后，review 相关设计文档的 §0 是否过时。

这些规则确保文档对 AI Agent（清晰的“现在能用什么”）和人类开发者（架构 + 扩展点）都保持有用和准确。

详细背景见 Issue #156 关闭评论 + 相关 closing docs。


### 核心 (`design/core/`)

| 文档 | 内容 |
|------|------|
| [agent_orchestration](design/core/agent_orchestration.md) | Agent 编排模型、意图、inter-agent 通信 |
| [mutate_api](design/core/mutate_api.md) | 动态变更 / 自演化 API |
| [typesystem](design/core/typesystem.md) | 类型系统设计 + 形式化 |
| [query_edsl](design/core/query_edsl.md) | 查询 EDSL（只读 AST 访问） |
| [typed_mutation](design/core/typed_mutation.md) | 类型化变更的形式 |
| [workspace_layering](design/core/workspace_layering.md) | Workspace 分层 / COW / lock |

### 编译 (`design/compilation/`)

| 文档 | 内容 |
|------|------|
| [ir_pipeline](design/compilation/ir_pipeline.md) | IR 管线设计 |
| [jit](design/compilation/jit.md) | LLVM ORC JIT 设计 |

### 运行时 (`design/runtime/`)

| 文档 | 内容 |
|------|------|
| [async_serve](design/runtime/async_serve.md) | Fiber / scheduler / 多 session serve 模式 |
| [ffi](design/runtime/ffi.md) | C FFI 设计与绑定 |

### 归档与历史 (`design/history/`) — 按需查阅

- `design/history/notes/`（原 `design/notes/`）：归档的设计探索、推测性研究、单一议题 follow-up 和早期管线设计（~80+ 篇）。新贡献者可以**跳过**；主要供历史参考。详见该目录下的 README。
- `design/history/closings/`（原 `docs/issue-closings/`）：issue 关闭总结和事后分析。已迁移到 design/ 下统一管理。详见该目录下的 README。

新贡献者应优先阅读 `design/core/` 的 Implementation Status 章节。历史文档可通过 `git log` 追溯。许多概念已在 core/ 中实现或演进。

**注意**：所有 `design/core/`、`design/compilation/`、`design/runtime/` 下的文档**必须**包含 `## 0. Implementation Status` 章节（C++ Core Layer + Aura Layer 表格 + AI Agent 读者注意事项），详见上文“进程规则”。

## 构建

构建指南见[项目 README](../README.md#构建)。
