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
- For project-driven iteration history, see also `projects/GAPS.md`, `projects/README.md`, and the main [roadmap.md](../roadmap.md).

Key historical documents of note (with current pointers where relevant):
- `notes/projects_iteration.md` — Original project-driven methodology. Current status in main roadmap and `projects/`.
- Various `notes/issue-*-*.md` and `closings/*-closing.md` — Follow-ups for specific issues. Many gaps have been closed in the core docs.

If you are looking for the evolution of a particular feature (e.g. EDSL, workspace, concurrency), start in the relevant `core/` doc and use git blame/log on the historical files only for deep context.
