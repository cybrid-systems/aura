# Workspace source SSOT (Issue #2920)

## Rule

**After any successful `set-code` / `load` / `ast:restore`, and after any
structural mutate under `MutationBoundaryGuard`, the live `workspace_flat_`
(+ `workspace_pool_`) is the single source of truth for workspace program
text.**

| Reader | Correct path |
|--------|----------------|
| Agent / stdlib | `(current-source :workspace)` (#2918) |
| JIT `(eval-current :jit)` | `Evaluator::authoritative_workspace_source()` → live unparse hook |
| `serialize-workspace` | same authoritative helper |
| `ast:snapshot` source stamp | workspace unparse at snapshot time (#2918) |

## Text cache (`workspace_source_text_`)

- **Written** only by `set-code`, `load`, and post-restore restamp.
- **Invalidated** on Guard exit when the workspace mutation log grew (or
  lightweight field mutations committed) — so it never survives a structural
  mutate as a silent pre-mutate string.
- **Never** preferred over `get_workspace_source_fn_` (CompilerService
  `unparse_node` of live FlatAST).

Do **not** invent a third path that reads the text field without checking
validity / generation (see also #2918 for snapshot).

## Policy (implemented)

Policy **A** from #2920: cache stamp at set-code/load/restore; clear on
mutate boundary when the flat actually mutated; all post-mutate readers
go through live unparse (or empty → fail soft / tree-walk for JIT).

## Related

- [ast snapshot workspace](../stdlib/) dual-workspace `#2918`
- current-source unparse P0 tags `#2919`
