# Projects — 项目驱动的核心迭代

> **目的：** 写真实项目 → 暴露 core gaps → 修复核心 → 写更难的项目
>
> 详见 [`docs/design/projects_iteration.md`](../docs/design/projects_iteration.md)

## 项目列表

| 项目 | 状态 | 核心缺口 |
|------|------|---------|
| [kv/](kv/) — 最小键值存储 | ✅ works | 6 个 core gap 已修 |
| [chat/](chat/) — session 间消息 | ✅ works | send/recv/broadcast 内建 |
| cli/ — CLI 工具 | ⬜ planning | — |
| todo/ — TODO 应用 | ⬜ planning | — |
| chat/ — 聊天协议 | ⬜ planning | — |
| calc/ — 计算器插件系统 | ⬜ planning | — |

### 状态说明

- ⬜ **planning** — 设计阶段，未开始编码
- 🟡 **writing** — 正在编码，已发现 gap
- ✅ **works** — 项目可运行，所有已知 gap 已修复或已记录
- 🔒 **stable** — 项目稳定，core gap 已全部修复

## 发现的问题

所有 core gaps 在 `GAPS.md` 中跟踪。

## 规则

1. 每个项目必须能在 Aura REPL 中 `load` 并运行
2. 发现的 gap 必须记录到 `GAPS.md`
3. 🔴 阻塞型 gap 优先修复 🟠 重要型次之 🟡 改善型记录即可
4. gap 修复必须提交到 `src/` 并附带测试
