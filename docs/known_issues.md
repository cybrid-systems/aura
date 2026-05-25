# Aura 已知问题

**更新：2026-05-25 (EDSL + Agent 交互能力全部落地)**

---

## 开放中的问题

| # | 问题 | 说明 | 影响 |
|---|------|------|:----:|
| 45 | AOT 布尔值输出 raw int（`1` 而非 `#t`） | untagged runtime 无法区分 `#t` 和 integer 1 | AOT |
| 47 | struct 模块 AOT 不工作 | `define-type`(EDSL)，IR 路径不处理 AST 操作 | AOT |
| 48 | `display` 嵌套对/improper list 格式化不完美 | 和 eval 一致但可改进 | Display |
| — | messaging P0 缺少阻塞 recv | 单线程 serve 无法实现真正阻塞，当前使用超时轮询 | Serve |
| — | workspace tree 非全局 | 每个 serve session 有独立 workspace tree，无法跨 session 共享 | Ws |
| — | `synthesize:optimize` fitness 仅基于代码长度 | 需要 benchmark 驱动的真实 fitness | Opt |
| — | 规则持久化仅支持 JSON 文件 | 缺少内置 VCS 集成 | Rule |

## 已解决

| # | 问题 | 版本 |
|---|------|:----:|
| — | 核心 EDSL (query/mutate) | W1-2 ✅ |
| — | AST 快照/回退/diff | W1-2 ✅ |
| — | Workspace 分层 + COW + lock | W5-6 ✅ |
| — | Inter-agent messaging (send/recv/reply) | W7-8 ✅ |
| — | Template + LLM + Genetic 代码生成 | W9-10 ✅ |
| — | Pipeline 编排 | W9-10 ✅ |
| — | 代码规范系统 (rule:define/apply/save) | W9-10 ✅ |
| — | AOT 56 emit 全部通过 | P2 ✅ |
