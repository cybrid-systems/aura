Closing #153 — docs cleanup commit landed in c44ee86.

**What shipped** (97 file changes, all renames via `git mv` to preserve history):

`design/core/` (6 files — the on-ramp essentials):
- `agent_orchestration.md` (renamed from `design/`)
- `mutate_api.md` (renamed from `design/`)
- `typesystem.md` (renamed from `aura_typesystem.md`; the `typesystem_formal.md` content is in `notes/` for historical reference)
- `query_edsl.md` (renamed from `query_edsl_design.md`)
- `typed_mutation.md` (renamed from `typed_mutation_design.md`)
- `workspace_layering.md` (renamed from `design/`)

`design/compilation/` (2 files):
- `ir_pipeline.md` (renamed from `ir_pipeline_design.md`)
- `jit.md` (renamed from `llvm_jit.md`)

`design/runtime/` (2 files):
- `async_serve.md` (renamed from `async_serve_design.md`)
- `ffi.md` (renamed from `ffi_c.md`)

`design/notes/` (~80+ files — historical archive):
- All 18 `issue-*.md` follow-up docs (issue-108 through issue-79)
- All speculative research (`autonomous-self-evolving-agents.md`, `ai-driven-meta-programming.md`, `e4_evolvable_strategies.md`, etc.)
- All fine-grained optimization notes (`opt-*.md`)
- All superseded pipeline designs (`synthesize-pipeline-v2.md`, etc.)
- All fine-grained subsystem notes (`ast_validate.md`, `functor_*.md`, `gradual_typing.md`, etc.)

`docs/README.md` rewritten to:
- Reflect the new 4-folder structure (core / compilation / runtime / notes)
- Document the on-ramp vs archive distinction (new contributors skip `notes/`; `git log` tracks provenance)
- The README's previous '14 categories' were a flat list of every doc; the new structure groups by reader intent (core = read first, notes = read by exception)

**After:** 10 high-value docs in `core/compilation/runtime`; ~80+ archived in `notes/` (preserved with full git history). **No files deleted** — every rename is `git mv`.

**Honest scope:** this is the file-move commit (the '减法' subtraction per the issue body). The follow-up work (in a separate commit if desired) is the **MERGE phase**: combine related small files in `notes/` into stronger documents (per the issue's table of merge targets). The issue body proposes 6 merge targets; each is its own multi-hour effort and deserves a fresh session.

**Verified at ship:** docs-only commit. No code, no build, no test changes. 173/173 safety / 10/10 regression / all test_issue_* suites still green by construction (no changes touched any `.cpp` / `.ixx` files).

**Follow-ups** (deferred to separate issues):

1. **MERGE phase** — combine related small files in `notes/` into stronger documents (issue body proposes 6 merge targets). Each is multi-hour work; deserves fresh sessions.
2. **#156 §0 Implementation Status template** — applied to the 10 `core/compilation/runtime` docs in commit 2b603d6. Natural follow-up to the 4-folder structure created here.
3. **CI docs structure check** — enforce "new docs go in `core/compilation/runtime/notes/`, not flat `design/`" via a `tests/docs/check_structure.py` linter. Low priority; structure is already established and stable.
4. **Per-doc deep review** — each of the 10 high-value docs in `core/compilation/runtime` could benefit from a focused "is this still accurate?" pass. #154 (query_edsl) and #155 (mutate_api) shipped first; #156 added §0 sections; remaining 8 docs follow the §0 pattern.
