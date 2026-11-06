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

`(mutate:rebind name code [summary])` replaces the value of a live top-level
define. If the workspace has no define with that name, the call still returns
`#t`: it parses `code`, adds a new top-level define, and links that define onto
the workspace root (the same success value as a replacement). The new define is
part of the live AST, so a later `ast:snapshot` / `ast:restore` keeps it and
`eval-current` runs it. This is the add path (issue #165), distinct from
`no-workspace`: defines that arrived only from a file, `-e`, or `--load` are
not in the live AST, and rebind reports `no-workspace` until they are installed
with `set-code` and `eval-current`.

Typical loop with the evolve-strategy kit: `KIT_STEPS=1 aura repl main.aura`
(runs one step; `train` / `val` prices stay bound), edit a function in
`backtest.aura`, `:reload backtest.aura`, then `(evolve! 1 2 train val)` runs
another step without regenerating the prices.

## `aura lsp` — diagnostics and goto-definition

`aura lsp` speaks the Language Server Protocol over stdio:

- **Diagnostics** on open and save. The file is type-checked in process (the
  same checker as `aura --typecheck`) together with every file it
  `(require …)`s, so names defined in required modules resolve, and an error
  inside a required module is reported on that module with its own
  `line:column`. Example: writing `memq` (not provided; use `member`) in
  `backtest.aura` underlines it on its line with
  `unbound variable: memq`.
- **Goto-definition** for top-level `define`s across `(require "…")`, resolved
  like the runtime: relative to the requiring file, then the project root
  (the directory with `aura.toml`), with or without `.aura`.
- **Highlighting** comes from the TextMate grammar in
  `editors/vscode/syntaxes/aura.tmLanguage.json`.

### VS Code

```
cd editors/vscode
npm install                         # vscode-languageclient
ln -s "$PWD" ~/.vscode/extensions/aura-lang
```

Restart VS Code. Set `aura.path` if `aura` is not on `PATH` (for example a
wrapper script that runs the binary in the dev container).

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

### Limits

- Diagnostics are static: parse errors and names that are unbound at runtime
  (not defined in the file or its requires, not a primitive or std prelude
  name). The type checker's type errors are not published: on dynamically
  typed code most of them are false positives. Runtime-only errors still need
  a run.
- Definitions are top-level `define` / `define-syntax` / `define-record-type`;
  local `let` bindings are not indexed.
- No completion, hover, or rename yet.
