# Design History & Archives

This directory contains historical design documents, explorations, and issue closing notes.

**For current design and implementation status, see:**
- `design/core/` — the canonical on-ramp documents (all have `## 0. Implementation Status` tables)
- `design/compilation/`
- `design/runtime/`

## Contents

- `notes/` — Archived design explorations, speculative ideas, single-issue follow-ups, and older pipeline designs (~80+ files). These have reference value but are not the current truth. Use `git log -- docs/design/history/notes/<file>` to trace history.
- `closings/` — Issue closing summaries and post-mortems (moved from top-level `docs/issue-closings/` for better organization under design).

## Guidance for Readers

- New contributors and AI Agents should **skip** this directory initially and start with `core/` documents' Implementation Status sections.
- Many ideas here have been implemented, evolved, or superseded. Cross-reference with the corresponding `core/` document's §0 table.
- For iteration methodology history, see [roadmap.md](../roadmap.md) and `notes/projects_iteration.md`（`projects/` 目录已移除，见 commit `2882e37`）。

Key historical documents of note (with current pointers):
- `notes/projects_iteration.md` — Foundational project-driven iteration（当前见 `roadmap.md` + `tests/suite/`）。
- `notes/issue-*-*.md` and `closings/*-closing.md` — Deep dives and post-mortems. Cross-reference the corresponding `core/` §0 table for what was actually implemented.

If you are looking for the evolution of a particular feature (e.g. EDSL, workspace, concurrency), start in the relevant `core/` doc and use git blame/log on the historical files only for deep context.

## Guidance for Contributors (Living Docs Rule)

- **Do not create new files in `notes/`** unless the content is a genuinely unresolved speculative exploration or early research with no mapping to current core/.
- Most new work should:
  1. Update the relevant `design/core/*.md` §0 Implementation Status table (with date + code locations).
  2. Update `docs/api-reference.md` (Primitives Surface).
  3. If it's issue-resolution work, add/update a closing in `closings/`.
- Always add "Superseded by / Current status: see design/core/xxx.md §0" pointers when touching old notes.
- Follow the rules in `docs/README.md`（代码 + 测试为真相）。

This keeps the archive useful without polluting discoverability of the living design.
