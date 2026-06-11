# Issue Closings Archive

This directory holds post-closing notes and summaries for resolved issues (originally at `docs/design/history/closings/`).

These are valuable for understanding the rationale and lessons from past work, but they are **historical**.

**Current implementation status and design for the topics discussed here should be consulted in:**
- `../core/` documents (especially their `## 0. Implementation Status` sections)
- `../notes/` for related explorations
- Main source code and tests

## How to Use

- Search by issue number (e.g. `112-closing.md`)
- Many closings reference the design docs that were updated at the time. Those design docs have since been moved/refined under `core/`.
- For active gaps, see `projects/GAPS.md`

Example: Issues around EDSL, workspace layering, concurrency, and mutation were heavily documented here and have largely been realized in the core specifications.

**For contributors**: When resolving issues, prefer:
1. Updating the matching `../core/*.md` §0 table (with current implementation status and code pointers).
2. Updating `docs/api-reference.md`.
3. Adding a concise closing note here.

See the full Living Documentation Practices in `docs/README.md` and `design/history/README.md`. Do not duplicate implemented content here.
