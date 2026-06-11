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

## Guidance for Contributors (Living Docs Rule)

- **Do not create new files in `notes/`** unless the content is a genuinely unresolved speculative exploration or early research with no mapping to current core/.
- Most new work should:
  1. Update the relevant `design/core/*.md` §0 Implementation Status table (with date + code locations).
  2. Update `docs/api-reference.md` (Primitives Surface).
  3. If it's issue-resolution work, add/update a closing in `closings/`.
- Always add "Superseded by / Current status: see design/core/xxx.md §0" pointers when touching old notes.
- Follow the full rules in `docs/README.md` → "Living Documentation Practices".

This keeps the archive useful without polluting discoverability of the living design.
