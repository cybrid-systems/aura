# Aura 路线图

## 现有能力

Aura 是一个自修改 Lisp 运行时，核心能力包括：完整的 Lisp 求值器（树遍历 + IR 双路径）与 Sound Gradual Typing 类型系统；自修改 EDSL（`query:*` / `mutate:*` / `ast:*` / `workspace:*`）让 AI Agent 在运行时精确读写和版本化管理自身代码；Agent 编排层（`std/orchestrator`）提供 `orch:conduct` / `orch:pipeline` / `orch:parallel` 等高阶原语组织多 Agent 协作；LLVM ORC JIT 后端（38 opcode → native，7.55× 加速）与 AOT 二进制 emit 支持；标准库覆盖 40+ 模块；EDSL Benchmark 145 任务在 Grok 上达到 83% 通过率。

Functor 泛型模块（Parser/Evaluator 模板存储/实例化路径/类型系统 ModuleType/签名生成/测试）已完成。`--serve-async` fiber scheduler 多 session 支持已修复，15/15 测试通过。

TypeRegistry 已补全：VariantType / RecordType / 所有构造器的 occurs_check / substitute / free_vars / instantiate_forall（#41）。

TypeID 传播 + 类型注解 IR 层已完成：5 个 Type Propagation 测试全部通过，Coercion CastOp 正确携带 type_id。

## 路线图

### 开发方法论

从 P2.8 开始，采用 **Project-Driven Iteration**（详见 `docs/design/projects_iteration.md`）：

```
写真实项目 → 暴露 core gaps → 修复核心 → 写更难的项目
```

每写一个 `projects/` 下的 demo，至少暴露 3-5 个 Aura 核心短板。修复这些短板是 P 系列迭代的主要驱动力。

### 即将开始的 project 系列

| 项目 | 定位 | 预期暴露的 gap 方向 | 期望状态 |
|------|------|-------------------|---------|
| **P1 kv-store** | 最小键值存储 | hash API 完整性、时间 prim、类型检查接口 | 2 周内可跑 |
| **P2 cli-tool** | 命令行工具 + 文件处理 | JSON 序列化、string 处理、命令行参数 | P1 后 |
| **P3 chat-proto** | session 间消息协议 | mailbox prim 补全、serve 路由 | P2 后 |
| **P4 calc-plugin** | 运行时插件系统 | mutate:* 的正确使用模式、动态绑定 | P3 后 |

### P 系列核心规划

| 阶段 | 内容 | 关联 project |
|------|------|-------------|
| **P2.7b** | 原语作为闭包值传递 AOT 支持 | ✅ 已完成 |
| **P2.8** | Project-Driven 起步 + `projects/` 目录 | P1 kv-store |
| **P3** | 类型系统补全（#41 后续 + 类型级运算） | P4 calc-plugin |
| **P4** | 标准库补全（JSON、时间、字符串） | P2 cli-tool |
| **P5** | 运行时沙箱与环境隔离 | P4 calc-plugin |
| **P6** | Serve 协议升级 + 网络层 | P3 chat-proto |

## 每个 project 的预期产出

1. `projects/<name>/` 下的可运行代码
2. 修复的 core gap 列表（GAPS.md 中 tracking）
3. 新增/改进的 core API（commit to main）
4. 对应 core 功能的测试用例

## 如何参与

1. 选一个 project（建议从 P1 开始）
2. 写代码 → 跑 → 记录 gap
3. 修 core gap → 提 PR
4. 项目完成后更新状态
5. 选下一个 project
