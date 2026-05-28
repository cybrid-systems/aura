# Aura 路线图

## 现有能力

Aura 是一个自修改 Lisp 运行时，核心能力包括：完整的 Lisp 求值器（树遍历 + IR 双路径）与 Sound Gradual Typing 类型系统；自修改 EDSL（`query:*` / `mutate:*` / `ast:*` / `workspace:*`）让 AI Agent 在运行时精确读写和版本化管理自身代码；Agent 编排层（`std/orchestrator`）提供 `orch:conduct` / `orch:pipeline` / `orch:parallel` 等高阶原语组织多 Agent 协作；LLVM ORC JIT 后端（38 opcode → native，7.55× 加速）与 AOT 二进制 emit 支持；标准库覆盖 40+ 模块；EDSL Benchmark 135 任务在 Grok 上达到 100% 通过率。

## 下一步

- **Functor 泛型模块 (P3-P5)** — TypeTag::MODULE + ModuleType + register_module 类型系统接入，实例化缓存，`.aura-type` 签名自动生成，回归测试
- **--serve-async epoll 修复** — fiber scheduler epoll 交互问题，使单进程 fiber 并行可用
- **已知小问题清理** — `string-index` 内置化、`require` 多模块 `all:` 语义修复、`<kwd>` 垃圾值消除
