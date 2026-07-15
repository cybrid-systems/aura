# Primitives Surface Refactor — Minimal Public API + Progressive Delivery

> **Status**: in progress — **P0b–P4 + P5a/#1439 public stats removal + #1448 SlimSurface infra landed**  
> **Supersedes / extends**: [#558–#566 demotion epic](primitives-demotion-batch1.md),  
> [primitive-vs-stdlib-decision-framework.md](primitive-vs-stdlib-decision-framework.md),  
> [query-namespace-decision.md](query-namespace-decision.md),  
> [stdlib-organization-spec.md](stdlib-organization-spec.md),  
> **[#1448 SlimSurface v2 governance](primitives-slim-surface-v2.md)**  
> **Goal**: 对外暴露尽可能少；其余用宏 / 标准库；C++ 只保留高性能 + AI-native 工作面 + 引擎/宿主红线。  
>  
> **Shipped**:  
> - **#1448 SlimSurface infra**: `--strict` budget (target 420 / interim ceiling 700),  
>   `PrimMeta.deprecated` dispatch counter, `docs/design/primitives-slim-surface-v2.md`,  
>   gate runs freeze+strict, `tests/test_primitives_surface_convergence.cpp`  
> - P0b/#1432: `scripts/check_primitive_surface.py` freezes stats **and** convenience (`string`/`json`/`math`/`vector`/`path`/`time`) + `ast:ref-*` vs baseline; gate + unit test  
> - P1a/#1433: `(engine:metrics)` schema 2 + nested groups + `:prefix` / `:group` / `:all`; `lib/std/engine-metrics.aura`  
> - P1b/#1434: top-20 stats callers migrated to facade; `PrimMeta.deprecated` + `api-reference *deprecated*`; `scripts/find_top_stats.py`  
> - P2a/#1435: `(query :op …)` dispatcher for node/children/parent/find/def-use/mutation-log; core `query:*` aliases deprecated  
> - P2b/#1436: `(mutate :op …)` dispatcher for rebind/replace/move/extract/validate/atomic; core `mutate:*` + SV aliases deprecated  
> - P2c/#1437: `(workspace :op …)` dispatcher for create/switch/merge/lock/unlock; core `workspace:*` aliases deprecated  
> - P3/#1438: tutorial + api-reference + agent-prompt-template use op-dispatch as canonical; gen_docs marks deprecated  
> - P2a: `AURA_PRIMITIVES=s0|full` / `AURA_FULL_PRIMITIVES=0` gates eda/security/verify/stdlib-review  
> - P2b: s0 skips bulk eval/jit observability; registers `register_metrics_facade` only  
> - P3: `lib/std/surface.aura` re-exports string/json/math convenience; `tests/suite/stdlib_surface.aura`  
> - P4: `./build.py suite-s0` + CI job `s0-smoke` (curated SUITE_S0_FILES under `AURA_PRIMITIVES=s0`)  
> - **P5a/#1439 (v2.0-prep)**: `query:`/`compile:*-stats` removed from **public** `Primitives` / `(api-reference)`; impls live in `ObservabilityPrims::register_stats_impl`; callers use `(engine:metrics "…")`. Migration: [migration-stats-to-metrics.md](../migration-stats-to-metrics.md)  
> - **P5b/#1440**: gen_docs taxonomy fixed (convenience 170→~8); hot-path string/vector/json/hash/math reclassified **core** (stdlib composes on them); `lib/std/json` stringify fixed. Full delete of `string-append` class **blocked** by red-line #3 — see [CHANGELOG-v2.0-prep.md](../CHANGELOG-v2.0-prep.md)  
> - baseline `docs/generated/stats-primitives-baseline.json`

---

## 0. TL;DR

| 现状 (约) | 目标态 (12–18 个月感) |
|-----------|----------------------|
| ~**1060** 个注册 primitive 名 | 默认 **`api-reference` / 默认能力集 &lt; 150** 稳定名 |
| `query:` ~516，其中大量 `*-stats` | **结构查询 ~20–30**；观测并入 **1–3 个 facade** |
| 75 个 `evaluator_primitives_*.cpp` | 物理 TU 可保留，**注册入口收敛** |
| Issue 测试 ~523 二进制 | **按能力门控 / 分层 suite**，PR 只跑相关层 |
| #558–#566 净引擎名 ≈ 0 减少 | **真删除或隐藏** 观测名，不只 stdlib 包装 |

**一句话**：把 “系统调用表” 缩回 “语言 + AI 工作面”；`/proc` 式观测走一个 metrics 门，不占一千个 public 名。

**CI 会更快**：更少默认原语 → 更少 issue 测试绑定、更少 docs 抖动、更短链接/注册面、PR 可只跑 core+affected 层。

---

## 1. 问题诊断

### 1.1 表面过大

- 扫描约 **1060+** 唯一 `primitives_.add("…")` 名。
- **`query:` 占约一半**；名字含 `stats` 的约 **450+**。
- `evaluator_primitives*.cpp` ~**69k LOC**，接近 compiler 半壁。
- 文档决策写 **Default=stdlib**，现实是 **Issue 验收默认加 C++ 入口**。

### 1.2 过去 demotion 的诚实结论（#566）

- 引擎名 **几乎没减**；主要加了 `lib/std/stats`、`query`、`ast`、`workspace` **包装层**。
- 原因：多数 `query:*-stats` 被红线 #2/#6 挡住——“必须读内部 metrics”，于是 **每个 counter 一个 primitive**。
- 包装改善了 Agent 发现性，但 **没有** 减少构建/链接/注册/破坏性变更成本。

### 1.3 真正该小的 vs 该厚的

| 该小（对外） | 该厚（对内/stdlib） |
|--------------|---------------------|
| 语言核 + AI 工作面名字 | 宏、配方、orch、string/json |
| 稳定契约 | CompilerMetrics 内部字段 |
| 默认 `api-reference` | debug / capability 门后的面板 |

---

## 2. 设计原则（冻结）

1. **Default = macro / stdlib**。新能力先写 `lib/std` 或宏；过红线再 primitive。  
2. **对外少暴露**。未进 “稳定面清单” 的名字不算 public API。  
3. **观测是数据，不是系统调用**。Counter 进结构体；对外 `engine:metrics`（或等价）一次取 hash。  
4. **AI-native ≠ API 个数**。需要的是：可查询 AST、可事务变异、可回滚 workspace、可恢复 panic——不是 200 个 stats 名。  
5. **高性能红线保留**。热路径谓词/算术/cons 等仍 C++。  
6. **兼容渐进**。先 dual-write / alias / capability，再 deprecate，最后删除。  
7. **CI 与表面绑定**。每一阶段要有可测的 “默认面缩小” 与 “测试分层” 收益。

### 2.1 何时才允许新 primitive

必须满足决策框架 **至少一条红线**，且通过评审：

| 红线 | 含义 | 例 |
|------|------|-----|
| 引擎启动 | 无则无法 boot/eval | `set-code`, `cons`, `+` |
| 内部状态 | 锁 / FlatAST / def-use / IR | 核心 `mutate:*` / 结构 `query:*` |
| 热路径 | 每节点/每指令 | 类型谓词 |
| 宿主边界 | FFI / TUI / net / fiber | `tui:*`, `fs:*` |
| 类型/IR 桥 | checker/JIT 必需 | 少量 `compile:*` **动作** |
| 观测契约 | wire/JSON **协议字段** | **一个** metrics dump，不是 N 个名 |
| 诊断恢复 | panic 路径 | checkpoint / restore |

**禁止**：为单个 Issue 计数器新增 `query:foo-stats`（改 metrics 结构 + 测试读 hash）。

---

## 3. 目标架构

```
                    ┌─────────────────────────────┐
   用户 / Agent     │  稳定面 S0（默认暴露）         │  目标 <150 名
                    │  语言核 + AI 工作面 + 宿主可选  │
                    └─────────────┬───────────────┘
                                  │ require / 宏
                    ┌─────────────▼───────────────┐
                    │  lib/std + 宏（厚）            │  组合、领域、策略
                    │  edsl / stats / query 包装    │
                    └─────────────┬───────────────┘
                                  │ 少量 engine hooks
                    ┌─────────────▼───────────────┐
                    │  C++ primitives（窄）         │
                    │  core · mutate · query-core   │
                    │  workspace · eval · host      │
                    │  engine:metrics (facade)      │
                    └─────────────┬───────────────┘
                                  │
                    ┌─────────────▼───────────────┐
                    │  Evaluator / FlatAST / JIT    │
                    │  CompilerMetrics（内部字段多） │
                    └─────────────────────────────┘
```

### 3.1 稳定面分层（S0 / S1 / S2）

| 层 | 默认 | 内容 | 注册 |
|----|------|------|------|
| **S0 默认面** | 始终加载 | 语言核 + AI 工作面核心 + 必要宿主 | `register_s0_primitives` |
| **S1 引擎控制** | 能力门或 `--full-engine` | metrics facade、compile 动作、debug | `register_s1_*` |
| **S2 垂直** | feature 关闭默认 | `eda:*` `seva:*` 大包 security | `AURA_FEATURE_EDA=1` 等 |

`api-reference` **默认只列 S0**（或 S0+已 require 的 stdlib 导出）。  
S1/S2 需显式能力或编译选项，避免 Agent 被噪声淹没。

### 3.2 S0 草图（数量级，非最终冻结表）

**语言核（~40–80）**  
算术/比较、类型谓词、`cons/car/cdr`、list 基础、`apply`/`eval`、bool、字符串热路径少量。

**AI 工作面（~30–50）**  
- 执行：`set-code` `eval-current` `current-source`（+ 必要诊断）  
- 查询：`query:root` `query:node` `query:children-stable` `query:parent-stable`  
  `query:find` `query:pattern` `query:where` `query:def-use` `query:effects` …  
- 变异：核心 `mutate:*`（rebind/replace/extract/atomic-batch/…）  
- 版本：`ast:snapshot` `ast:restore` rollback / mutation-history 核心  
- 工作区：`workspace:create|switch|merge|lock|unlock|…` 核心  
- 恢复：panic-checkpoint / restore 一小撮  

**宿主（可选进 S0 或按二进制）**  
`tui:*`、基础 fs；fiber/msg 可 S0-serve 变体。

**明确不进 S0**  
- 几乎所有 `query:*-stats` / `compile:*-stats`  
- convenience string/json/math/vector（stdlib）  
- synthesize/strategy 高层（stdlib；C++ 仅保留无法下沉的钩子）  
- 实验性 compile 细粒度 dirty 操作（S1）

---

## 4. 观测折叠（关键路径）

### 4.1 目标 API

```scheme
;; 一次取回结构化 metrics（hash / nested hash）
(engine:metrics)                 ; 全量或默认摘要
(engine:metrics :group 'jit)     ; 分组
(engine:metrics :prefix "query:mutation")  ; 兼容旧名索引

;; 过渡期 stdlib（已有方向，见 lib/std/stats.aura）
(stats:get "query:mutation-log-stats")
(stats:list)
```

### 4.2 实现要点

1. **单一 C++ 源**：`CompilerMetrics` / 各 atomic counter → 序列化为 hash。  
2. **旧 primitive**：  
   - Phase A：仍注册，实现改为 “读 metrics 的薄壳” 或标记 deprecated。  
   - Phase B：移出默认注册，仅 `AURA_LEGACY_STATS=1` 或 S1 加载。  
   - Phase C：删除 + changelog。  
3. **测试**：issue 测试改为 `engine:metrics` / C++ 直接读 metrics 字段，**禁止**再为每个 counter 加 Aura 名。  
4. **Wire**：`--serve-async` / evo-explain 已吃 hash 的，对齐同一序列化。

### 4.3 与 #560 关系

#560 的 `stats:list/get` 是正确方向，但 **未停止引擎名增殖**。  
本方案强制：**新 counter 零新 public 名**；旧名只减不增。

---

## 5. 渐进实施路线图

每阶段：**可合并、可回滚、有退出标准、有 CI 收益**。

### Phase 0 — 基线与闸门（1 周）

**做：**

- 生成 `docs/generated/primitive-inventory.json`（名、文件、前缀、category）。  
- 冻结策略合入 CONTRIBUTING / pre-commit 检查：  
  - PR 若新增 `add("query:…-stats")` 或 bare `*-stats` → gate 失败（allowlist 空）。  
- 定义 S0 草案清单 PR（评审用，先不删注册）。  
- `api-reference` 增加 `:tier` / 默认过滤实验（可选 flag）。

**退出：** 库存可 diff；新增 stats 名被 CI 挡住。

**CI 收益：** 防止继续恶化；零运行时风险。

---

### Phase 1 — Metrics facade + 停止出血（2–3 周）

**做：**

- 实现 `engine:metrics`（或扩展现有 dump 为唯一规范入口）。  
- 迁移 **高频 / 新建** issue 测试读 facade。  
- 文档：Agent 推荐路径 = stdlib stats + engine:metrics。  
- 可选：`AURA_PRIMITIVES=s0|full` 构建/运行时模式（full 默认保持兼容）。

**退出：**

- 新 Issue 测试 **0** 新增 stats primitive。  
- 至少 20 个旧 stats 测试改为 facade（证明路径）。

**CI 收益：** 新 issue 测试不再强制新 `add` + docs 再生连锁；后续删除名时测试不绑死旧名。

---

### Phase 2 — 默认面收缩（兼容全量）（3–5 周）

**做：**

- 注册拆分：`register_s0` / `register_legacy_stats` / `register_vertical_eda` …  
- 默认 binary：`s0 + host + metrics facade`；`AURA_FULL_PRIMITIVES=1` 保持今日行为。  
- CI matrix：  
  - PR：`s0` + core suite + 变更相关  
  - main/nightly：`full` 回归  

**退出：**

- 默认 `api-reference` 名数量 &lt; 200（或相对基线 −50%+）。  
- PR `build-test` 墙钟可测下降（目标 −15–30%，视链接/测试切分而定）。

**CI 收益：** PR 链接更少 TU 符号？若用运行时门控则链接不变；**应用运行时 S0 + 测试分层** 收益更大（见 Phase 4）。物理拆 shared lib 可作为后续优化。

---

### Phase 3 — Convenience 真下沉 stdlib（4–6 周，可并行）

**做：**

- 按 `primitive_categories.yaml` convenience：string / json / math / vector / path / time。  
- 模式：stdlib 实现 → 引擎 primitive 标 deprecated → 一周期后 full 模式移除。  
- 遵守 [stdlib-organization-spec.md](stdlib-organization-spec.md)。

**退出：** convenience 簇引擎名 −N（可量化）；stdlib 测试覆盖。

**CI 收益：** 更少 C++ 改动面；stdlib 测试可 Aura-only、更快。

---

### Phase 4 — 测试与 CI 分层（与 Phase 1–2 强绑定）（持续）

这是 “CI 会高效很多” 的主杠杆。

| 层 | 内容 | 何时跑 |
|----|------|--------|
| **L0 gate** | docs/lint/format/fixtures + **禁新增 stats 名** | 每个 PR |
| **L1 core** | unit + suite（S0 语义）+ smoke | 每个 PR |
| **L2 engine** | mutate/query/workspace/eval 深度 | PR 触及 `src/compiler/evaluator*` / query / mutate |
| **L3 metrics** | metrics facade + 抽样 legacy stats | PR 触及 metrics / observability |
| **L4 vertical** | eda/seva/security | PR 触及对应目录或 nightly |
| **L5 full** | 全 issue 二进制 + full primitives | main / nightly / release |

**Issue 测试策略：**

- 新建 issue 测试 **禁止** 依赖即将删除的 stats 名。  
- 旧 issue 测试：批量改为读 metrics hash，或标 `@full-only`。  
- 目标：PR 默认 issue 集从 “全量 ~500” 降到 “核心 + path-based 增量”（已有 issues-fast，继续收紧）。

**退出：** PR 中位时长下降；main 全量仍守门。

---

### Phase 5 — 真删除与文档收敛（持续小步）

**做：**

- 按前缀批次删除 legacy stats 注册（每批 20–50，changelog）。  
- `docs/generated/primitives.md` 默认生成 S0；full 附录可选。  
- 清理 `obs_eval_*` / `obs_jit_*` 中 “一函数一 add” 模式 → 集中 facade 注册。

**退出：** 默认注册表持续下降；破坏性变更有版本说明。

---

### Phase 6 — 垂直模块可选化（中期）

**做：**

- `eda` / `seva` / 重 security 编译开关或动态注册。  
- aura-pets / 游戏类只链 S0+tui。  

**CI 收益：** 非 EDA PR 不编不测 EDA 原语。

---

## 6. 与现有资产的映射

| 已有 | 本方案用法 |
|------|------------|
| 决策框架 #558 | 红线不变；**执行**靠 gate 与评审 |
| categories.yaml #559 | S0/S1 分类源；convenience → Phase 3 |
| stats.aura #560 | 用户层保留；引擎侧改为 facade 后端 |
| query.aura #562 | Agent 产品 API；引擎只留结构核心 |
| stdlib 组织 #565 | 下沉目录规范 |
| demotion batch1 #566 | 承认包装不够 → 本方案强调 **删/藏** |

---

## 7. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 破坏下游 Agent 脚本 | 长周期 alias；`AURA_FULL_PRIMITIVES`；changelog |
| metrics 序列化不一致 | 单一 serializer + golden JSON 测试 |
| 误删热路径 | 红线评审 + 性能基准门 |
| 大 PR 不可审 | 每阶段小 PR；禁止 “删 500 名” 单 PR |
| CI 变复杂 | 分层表写进 `build.py`；文档一张图 |

---

## 8. 成功指标（可汇报）

| 指标 | 基线（约） | 6 个月目标 | 12 个月目标 |
|------|------------|------------|-------------|
| 默认 `api-reference` 名数 | ~1000+ | &lt; 400 | &lt; 150 |
| 新增 stats primitive / 季度 | 持续正 | **0** | **0** |
| PR 中位 `build-test` 时长 | 现状 | −15% | −30% |
| PR 默认 issue 测试数 | ~全量/fast | path-based | 进一步收紧 |
| convenience 仍在 C++ 的比例 | 高 | 半 | 低 |
| suite 全绿 + fuzz | 维持 | 维持 | 维持 |

---

## 9. 建议的第一批实施 PR（可马上开工）

| PR | 内容 | 风险 |
|----|------|------|
| **P0a** | 本设计合入 + CONTRIBUTING 冻结规则 | 低 |
| **P0b** | inventory 脚本 + gate：禁新增 `*-stats` 名 | 低 |
| **P1a** | `engine:metrics` 最小实现 + 1 个 golden 测试 | 中 |
| **P1b** | 10–20 个 issue 测试改读 metrics | 中 |
| **P2a** | `AURA_FULL_PRIMITIVES` / 注册拆分骨架（默认 full 兼容） | 中 |
| **P4a** | `build.py` L1/L2 路径过滤加强 | 低 |

**不在第一批：** 大规模删除名字、EDA 拆包、mutate 语义重写。

---

## 10. 结论

- 整体重构 = **表面分层 + 观测折叠 + stdlib 真下沉 + CI 分层**，不是一次重写 Evaluator。  
- 渐进实施保证每步可测、默认可兼容 full。  
- CI 效率主要来自：**不再为每个 counter 长测试二进制与文档**，以及 **PR 只跑 S0/相关层**。  

下一步若同意本方案：从 **P0a + P0b** 开干（文档 + 冻结闸门），再进 **P1a metrics facade**。
