# Design Notes Archive

This directory contains archived design explorations, research notes, speculative ideas, older pipeline designs, and single-issue follow-ups (approximately 86 files).

**Important:** These are **not** the current design. For the authoritative, up-to-date design and implementation status, refer to:

- `../core/` (6 documents — start here)
- `../compilation/`
- `../runtime/`

All current core documents include a `## 0. Implementation Status` section with C++ vs Aura layer tables.

## Navigation Tips

- Use `git log -- docs/design/notes/<filename>` to understand when and why a document was written.
- Many notes have been partially or fully superseded by implementations in the core documents.
- Cross-reference with the main [roadmap](../roadmap.md) and `projects/` for how ideas evolved through real project work (e.g. evo-kv).

Notable files:
- `projects_iteration.md` — Foundational project-driven iteration methodology (see current roadmap and projects/README.md for evolution)
- Various `issue-*-*.md` files — Deep dives into specific problems that were later resolved

New readers (human or AI) are encouraged to start with the core documents rather than browsing this archive.

**For contributors**: Follow the rules in `docs/README.md` "Living Documentation Practices" and the parent `design/history/README.md`. Only add new files here for truly speculative/unresolved explorations. Prefer updating core/ §0 tables + api-reference.md + (if needed) a closing note.
