# Design History

`docs/design/notes/`（~88 篇）与 `docs/design/history/closings/`（~64 篇）已于 PR2 从工作区移除。

**查阅历史文档：**

```bash
git tag docs-archive-pre-2026-06   # 删除前的快照
git show docs-archive-pre-2026-06:docs/design/notes/<file>
git show docs-archive-pre-2026-06:docs/design/history/closings/<file>
```

**当前设计入口：**

- `design/core/` — 自修改、类型、workspace、编排
- `design/compilation/` — IR、JIT
- `design/runtime/` — serve、FFI
- 代码 + `tests/suite/` — 实装真相（见 [docs/README.md](../../README.md)）