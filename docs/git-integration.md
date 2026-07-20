# Git Integration Domain (`git-*`) — Issue #1970

**Status: integration vertical (deferred from SlimSurface core).**

Parent: #1965 (Phase 3 commercial_readiness scope) · Decision: **KEEP**.

## Decision

The 7 `git-*` primitives are **kept** as an integration vertical, not
deleted. They are already excluded from the SlimSurface *core* budget
via `DOMAIN_STATUS["git-"] = "deferred"`.

| Option | Chosen | Why |
|---|---|---|
| Remove | No | Production sweep + issue suites call `git-status` / `git-diff` / etc.; Agent workflows use them for workspace introspection. |
| Keep + gate | **Yes** | Same pattern as #1967–#1969: optional surface + budget freeze. |

## Build flag: `AURA_ENABLE_GIT`

CMake option (default **ON**):

```bash
# Default full build (git-* registered)
cmake -B build -S .

# Slim / core-only: skip git-* registration
cmake -B build_slim -S . -DAURA_ENABLE_GIT=OFF
```

When `AURA_ENABLE_GIT=0`, `register_git_primitives` is a no-op.

**Not the same as `AURA_HAVE_LIBGIT2`:**

| Macro | Meaning |
|---|---|
| `AURA_ENABLE_GIT` | Whether the 7 `git-*` Aura primitives are registered (#1970) |
| `AURA_HAVE_LIBGIT2` | Whether the in-process libgit2 backend is linked (else popen fallback) |

You can enable git primitives with only the popen backend when libgit2 is
absent at build time.

Network / `tcp-*` / `sys-*` primitives that live in the same
`evaluator_primitives_io.cpp` TU are **not** gated by this flag
(tracked under #1975 and siblings).

## Commercial domain budget

```text
COMMERCIAL_DOMAIN_BUDGETS["git-"] = 7   # scripts/check_primitive_surface.py
```

`./build.py gate` → `check_primitive_surface.py --strict` fails if the
source-scanned `git-` count exceeds this budget.

## Surface (7 primitives)

All in `register_git_primitives` (`src/compiler/evaluator_primitives_io.cpp`):

| Primitive | Role |
|---|---|
| `git-status` | Short status |
| `git-diff` | Unified diff (`"staged"` optional) |
| `git-log` | Log slice |
| `git-commit` | Commit with message |
| `git-branch-current` | Current branch name |
| `git-stage` | Stage paths |
| `git-rev-parse` | Short HEAD |

Backend: `src/compiler/git_ctx.{h,cpp}` (libgit2 or popen). Related: #96.

## Related

- SlimSurface: #1448 / #1449
- Sibling commercial keep: #1967 (`tui:`), #1968 (`eda:`), #1969 (`auto-evolve-`)
- Sibling deferred: #1971–#1976
