# 设计文档归档

## 已移除（PR2 + PR3）

以下目录已从工作区删除，内容保存在 git tag **`docs-archive-pre-2026-06`**（PR2 前快照）：

- `docs/design/notes/` — ~88 篇探索笔记
- `docs/design/history/closings/` — ~64 篇 issue 结案
- `docs/design/core/` — 8 篇核心设计（PR3）
- `docs/design/compilation/`、`docs/design/runtime/` — 4 篇（PR3）
- `docs/developer/evaluator.md` — 合并入 `docs/contributing.md`（PR3）

## 查阅历史

```bash
git show docs-archive-pre-2026-06:docs/design/core/query_edsl.md
git show docs-archive-pre-2026-06:docs/design/notes/issue-111-audit.md
```

## 当前文档入口

见 [docs/README.md](../../README.md)。实装以 `src/`、`tests/suite/`、`(api-reference)` 为准。