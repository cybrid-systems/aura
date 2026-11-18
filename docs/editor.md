# Editor support and the REPL

## `aura repl` — hot reload without regenerating data

```
aura repl backtest.aura        # REPL with backtest.aura already :load'ed (std prelude loaded)
```

| command | effect |
|---|---|
| `:load FILE` | evaluate every form of FILE and remember each `define` |
| `:reload [FILE]` | re-read FILE (default: the last one). Procedures whose text changed are swapped in place with `mutate:rebind`; changed value defines are re-evaluated; new defines are added; **unchanged defines and top-level expressions are not re-run**, so data such as generated prices stays bound |
| `:ws` | tracked definitions: name, kind, `file:line` (including the defines of files the loaded file `require`s) |
| `:worldline` / `:history` | this session's `:load` / `:reload` / `:snapshot` / `:restore` events, the `mutation-log:summary` totals, and the snapshots |
| `:snapshot [LABEL]` | remember the text of every tracked define; prints the snapshot id |
| `:restore ID` | put every define that changed since snapshot ID back (same rebind / re-evaluate path as `:reload`); defines added later stay |
| `:help` | list the commands |

Whitespace and comment changes do not count as a change. A define that came in
through `(require …)` is not a node of the live workspace AST, so `mutate:rebind`
cannot target it; `:reload` then re-evaluates just that define and says so
(`redefined in place: [...]`). Other bindings are still kept. A sandbox denial
is reported as an error, not worked around. `aura repl` also works
on a pipe (one response line per input line), which is what editor/agent
drivers use.

### `mutate:rebind` when the name is not in the workspace

`(mutate :rebind name code [summary])` replaces the value of a live top-level
define. If the workspace has no define with that name, the call still returns
`#t`: it parses `code`, adds a new top-level define, and links that define onto
the workspace root (the same success value as a replacement). The new define is
part of the live AST, so a later `ast:snapshot` / `ast:restore` keeps it and
`eval-current` runs it. This is the add path (issue #165), distinct from
`no-workspace`: defines that arrived only from a file, `-e`, or `--load` are
not in the live AST, and rebind reports `no-workspace` until they are installed
with `set-code` and `eval-current`. Over `--serve` that loop is
`(set-code "…")` then `(eval-current)`, and it needs `--preset dev` (mutate).
The session uses the same walker face as a file run unless
`AURA_PIPELINE_STRICT` pins Forbidden or force-soa.

Typical loop with the evolve-strategy kit: `KIT_STEPS=1 aura repl main.aura`
(runs one step; `train` / `val` prices stay bound), edit a function in
`backtest.aura`, `:reload backtest.aura`, then `(evolve! 1 2 train val)` runs
another step without regenerating the prices.

## `aura lsp` — diagnostics and goto-definition

`aura lsp` speaks the Language Server Protocol over stdio:

- **Diagnostics** on open, change and save. The file is type-checked in
  process (the same checker as `aura --typecheck`) together with every file
  it `(require …)`s, so names defined in required modules resolve, and an
  error inside a required module is reported on that module with its own
  `line:column`. Each diagnostic is the structured one the CLI and `--serve`
  JSON faces emit: `code` is the stable kind ident (`UnboundVariable`,
  `ArityMismatch`, `ParseError`, `TypeError`, ...), `message` is the
  producer's text with no kind prefix and no folded-in hint, and severity
  follows the kind (error, warning, information). The did-you-mean hint
  travels in `data.hint` / `data.suggestion`, so
  `textDocument/codeAction` offers a `quickfix` that replaces the name in a
  `WorkspaceEdit` the client applies; the server never calls `mutate:*`.
  A diagnostic inside a required file lists the `(require …)` site in
  `relatedInformation`, as do the producer's notes. Example: writing `memq`
  (not provided; use `member`) underlines it with code `UnboundVariable`,
  message `memq`, and a quickfix to `member`.
- **Type diagnostics** are warnings by default. `(define (g y) (+ y "s"))`
  reports `TypeError` with severity Warning and the same message
  `--typecheck` prints. Set `typeDiagnostics` in `initializationOptions` or
  in `workspace/didChangeConfiguration` settings to `"off"` to stop
  publishing them, or `"error"` to raise their severity; a
  `{"aura": {...}}` wrapper is accepted too.
- **Goto-definition** for top-level `define`s across `(require "…")`, resolved
  like the runtime: relative to the requiring file, then the project root
  (the directory with `aura.toml`), with or without `.aura`.
- **References and rename** (#5260). `textDocument/references` answers the
  declaration and every use that resolves to the same binding, with or without
  `includeDeclaration`, across `(require "…")` (project root,
  `AURA_PATH`, `std/`). `textDocument/prepareRename` and
  `textDocument/rename` answer from the same index; the rename result is a
  `WorkspaceEdit` (one `TextEdit` per file) that the editor applies.
  The server never writes a file and never calls a `mutate:*` primitive:
  `refactor:*` / `mutate:rename-symbol` are the live-workspace
  surface (#5252), and a rename here is text only. Text in a comment or a
  string literal is not a reference, and a `let`/lambda binding that
  shadows a top-level name is a different symbol. A primitive, a `std/`
  name, a name with no definition, an illegal identifier, and a new name
  already bound in a file the rename edits are all refused with a reason.
- **Highlighting** comes from the shared TextMate grammar in
  `editors/grammar/aura.tmLanguage.json`. Any editor that speaks LSP can
  run `aura lsp` over stdio; no editor-specific extension is maintained.
- **Nesting.** A JSON-RPC message nested deeper than 64 is answered with
  a parse error (`nesting too deep`, code -32700) and the server keeps
  reading. The same bound as `aura evo log`.

### Neovim (0.10+)

```lua
vim.filetype.add({ extension = { aura = "aura" } })
vim.api.nvim_create_autocmd("FileType", {
  pattern = "aura",
  callback = function(ev)
    vim.lsp.start({
      name = "aura",
      cmd = { "aura", "lsp" },
      root_dir = vim.fs.root(ev.buf, { "aura.toml", ".git" }),
    })
  end,
})
```

`gd` / `vim.lsp.buf.definition()` jumps across requires; diagnostics show on
`:w`.

### Emacs

`editors/emacs/` holds the `aura-lang` package: `aura-mode` (derived
from `lisp-data-mode`), an `inferior-aura` comint REPL, and registration
with both lsp-mode and eglot.  It needs Emacs 29.1+ and no Doom.  Install
it from a checkout with `use-package :load-path`, or with `package-vc-install`
and `:lisp-dir "editors/emacs"`.  See `editors/emacs/README.md` for the
three install recipes, the binary lookup order, and the REPL commands.

### Doom Emacs

`editors/emacs/doom/lang/aura/` is a Doom module built on the same package.
Copy or symlink it to `$DOOMDIR/modules/lang/aura/`, add `(aura +lsp)` to the
`:lang` group in `init.el`, then run `doom sync`:

    (doom! :lang
           (aura +lsp))

`packages.el` pins `aura-lang` from this repository
(`:files ("editors/emacs/*.el")`), so the Elisp client and the module come
from one tree; keep both on the same release tag.  `+lsp` adds `lsp!` to
`aura-mode-local-vars-hook`, and `:tools lsp` decides whether that starts
lsp-mode or eglot (`+eglot`), as for the other `:lang` modules.

Keys, on the localleader of `aura-mode`:

| key | command |
|---|---|
| `SPC m '` | open or switch to the REPL (`aura repl`) |
| `SPC m s d` / `s r` / `s b` | send the definition at point, the region, the buffer |
| `SPC m w w` / `w s` / `w r` | `:worldline` / `:snapshot` / `:restore` |
| `SPC m w k` | `:ws` (tracked definitions) |
| `SPC m r` | `:reload` the visited file |
| `SPC m g d`, `K` | definition (LSP, else imenu), documentation |

With `(eval +overlay)` the answer appears at the end of the line; the REPL's
error lines (`- : unknown = ...`) use the error face.

Save-time formatting is gated on the binary:

- `aura --fmt` kept comments only from #5263 wave 2 (commit `c04a44c3e`),
  whose first release is 0.2.7.  Before that it deleted every comment and
  still exited 0.
- The module registers `+aura-fmt` only when `aura --version` reports a
  release of 0.2.7 or later.  A nightly (`aura nightly (cf7353373)`) or a
  source-tree build (`aura dev (<sha>)`) reports a commit, not a version,
  so it is not trusted unless `+aura-trust-nightly-formatter` is set.
- `doom doctor` prints the version it found and why no formatter was
  registered; when the binary is missing from Emacs's PATH it points at
  `doom env`.

The module's README.org covers installation and troubleshooting.  Its ERT
tests run under plain Emacs (from `editors/emacs`):

    emacs -Q --batch -L . -l doom/test/aura-doom-test.el -f ert-run-tests-batch-and-exit

### Limits

- Diagnostics are static: parse errors, names that are unbound at runtime
  (not defined in the file or its requires, not a primitive or std prelude
  name), arity mismatches the runtime would reject, and the type checker's own
  type diagnostics as warnings (`typeDiagnostics`, above). One diagnostic per
  file is published: the first one the checker reports. Runtime-only errors
  still need a run. The code action covers did-you-mean suggestions only;
  extract/inline and other code actions are out of scope.
- Definitions are top-level `define` / `define-syntax` / `define-record-type`;
  local `let` bindings are not indexed.
- Completion (#5261) is a flat prefix list: primitives with their registry
  description, the document's and its requires' top-level defines, the
  bindings visible at the cursor, and `require` specs (std/ modules, the
  project root, AURA_PATH) with the path each one resolves to.
- Hover and signature help read the same primitive registry as the REPL's
  `:doc`; hover never shows a type until #5250 lands.
- References and rename (#5260) are per-name, not per-word: an unresolved atom
  is a reference only in a file whose require closure contains the declaring
  file, so text next to a same-spelled name that was never required stays out.
  Rename refuses a new name that is bound anywhere in a file it would edit
  (the conservative shadow check), refuses primitives, `std/` names, an
  illegal identifier and a name with no definition, and it produces text edits
  only: files change when the client applies the `WorkspaceEdit`. Nothing is
  written by the server and no `mutate:*` path runs.
