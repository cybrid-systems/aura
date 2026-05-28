# Aura 路线图

## 现有能力

Aura 是一个自修改 Lisp 运行时，核心能力包括：完整的 Lisp 求值器（树遍历 + IR 双路径）与 Sound Gradual Typing 类型系统；自修改 EDSL（`query:*` / `mutate:*` / `ast:*` / `workspace:*`）让 AI Agent 在运行时精确读写和版本化管理自身代码；Agent 编排层（`std/orchestrator`）提供 `orch:conduct` / `orch:pipeline` / `orch:parallel` 等高阶原语组织多 Agent 协作；LLVM ORC JIT 后端（38 opcode → native，7.55× 加速）与 AOT 二进制 emit 支持；标准库覆盖 40+ 模块（字符串/数学/网络/JSON/CSV/集合/合成/规则/验证等）；EDSL Benchmark 135 任务在 Grok 上达到 100% 通过率。

## 下一步

| 方向 | 优先级 | 说明 |
|------|--------|------|
| **Functor 泛型模块** | P0 | 设计文档已就绪（`docs/design/functor_modules.md`）。parser 定义 `define-module`、求值器模板存储、实例化路径、类型系统 module 类型、缓存、签名生成 |
| **多 session fiber 并行** | P1 | `--serve-async` fiber scheduler epoll 交互问题修复，实现单进程 fiber 并行 |
| **模块类型签名** | P2 | 自动生成 `.aura-type` 签名文件，跨模块接口检查，IDE 支持 |
| **stdlib 补全** | P3 | 字符串/算法/数据结构模块补齐，关键 API 性能优化 |
