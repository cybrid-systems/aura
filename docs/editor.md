# Editor support and the REPL

Doom Emacs is the supported editor. The module in
`editors/emacs/doom/lang/aura/` starts `aura lsp` and `aura repl`.

## Prerequisites

- `aura` on `PATH`: the nightly (`install.sh` links `~/.local/bin/aura`) or a
  binary built from this tree.
- Emacs 29.1 or newer.
- Doom Emacs.

Doom snapshots `PATH` with `doom env`. After installing Aura, run `doom env`
and restart Emacs.

## Install

Symlink or copy the module into Doom. Both layouts work (#5353).

```sh
mkdir -p "${DOOMDIR:-$HOME/.config/doom}/modules/lang"
ln -s /path/to/aura/editors/emacs/doom/lang/aura \
      "${DOOMDIR:-$HOME/.config/doom}/modules/lang/aura"
# or: cp -a /path/to/aura/editors/emacs/doom/lang/aura \
#           "${DOOMDIR:-$HOME/.config/doom}/modules/lang/aura"
```

The module ships this recipe in its own `packages.el`. Doom loads that file
with the module:

```elisp
(package! aura-lang
  :recipe (:host github
           :repo "cybrid-systems/aura"
           :files ("editors/emacs/*.el")))
```

In `init.el`, inside the existing `doom!` form:

```elisp
:tools
(lsp)                 ; eglot: (lsp +eglot)
:lang
(aura +lsp)
```

`+lsp` adds `lsp!` to Aura buffers. `:tools lsp` chooses the client: lsp-mode,
or eglot with `+eglot`. `+lsp` with no `:tools lsp` leaves `lsp!` undefined;
`doom doctor` reports that.

```sh
doom sync
```

## Keys

These are Doom's evil bindings (`:editor evil`). LSP keys need `:tools lsp`
and `(aura +lsp)`. `SPC c f` needs `:editor format`. `SPC c j` is a workspace
symbol with lsp-mode when ivy, helm, or vertico is enabled, and with
`(lsp +eglot)` when vertico is enabled (`consult-eglot-symbols`). `(lsp
+eglot)` without vertico binds `SPC c j` to `eglot-find-declaration`; this
server has no declaration provider, so that binding has no Aura target.

| key | action |
|---|---|
| `g d`, `SPC m g d` | definition. A primitive has no source definition; lookup stops with an error |
| `g D`, `SPC c D` | references |
| `K`, `SPC m h` | documentation: LSP hover, otherwise the REPL `:doc` |
| `SPC c r` | rename (`WorkspaceEdit`; the server does not write the file) |
| `SPC c f` | format the buffer or region with `aura --fmt`, once the formatter is registered |
| `SPC c a` | code action (quickfix for a did-you-mean hint) |
| `SPC c j` | workspace symbol (`consult-lsp-symbols`, `lsp-ivy-workspace-symbol`, `helm-lsp-workspace-symbol`, or `consult-eglot-symbols`) |
| `SPC s i` | imenu |
| `] e` / `[ e` | next / previous diagnostic |
| `SPC c x` | list diagnostics |
| `SPC m '` | open or switch to the REPL |
| `SPC m s d` / `s r` / `s b` | send the definition at point, the region, the buffer |
| `g r` / `g R` | send the region / the buffer and show the answer (`:tools eval`) |
| `SPC m r` | `:reload` the visited file |
| `SPC m w w` / `w s` / `w r` / `w k` | `:worldline` / `:snapshot` / `:restore` / `:ws` |

Rename refuses a primitive, a `std/` name, a name with no definition, an
illegal identifier, and a new name already bound in a file the edit touches.
With `(eval +overlay)` the REPL answer is shown at the end of the line. An
error line `- : unknown = ...` uses the error face.

## Configuration

| variable | default | meaning |
|---|---|---|
| `aura-lang-executable` | `nil` | binary. `nil` searches `PATH`, then `~/.local/bin/aura`, then `~/.aura/current/bin/aura` |
| `aura-lang-path` | `nil` | extra library roots for child processes. An existing `AURA_PATH` is left as it is |
| `aura-lang-lsp-args` | `("lsp")` | arguments that start the language server |
| `aura-lang-repl-args` | `("repl")` | arguments that start the REPL |
| `aura-lang-repl-buffer` | `"*aura*"` | REPL buffer name |
| `aura-lang-grammar-file` | `nil` | TextMate grammar. `nil` looks beside the library for `editors/grammar/aura.tmLanguage.json`, then the keyword lists in `aura-lang.el` |
| `+aura-trust-nightly-formatter` | `nil` | register `aura --fmt` for a nightly or `aura dev` build |
| `+aura-repl-timeout` | `10.0` | seconds to wait for one REPL answer |
| `+aura-result-duration` | `8` | seconds an error overlay stays (`nil` or `0` keeps it). Used with `:tools eval` `+overlay` |

```elisp
(setq aura-lang-executable "/opt/aura/bin/aura")
(setq +aura-trust-nightly-formatter t)
(setq +aura-repl-timeout 10.0)
```

The module registers `aura --fmt` when `aura --version` is a release of 0.2.7
or later, or when `+aura-trust-nightly-formatter` is non-nil. A nightly
(`aura nightly (<sha>)`) and a source-tree build (`aura dev (<sha>)`) report
a commit. `doom doctor` prints the binary it found and why no formatter was
registered. `(format +onsave)` formats on save through that same gate;
`SPC c f` does too.

`typeDiagnostics` is read from LSP `initializationOptions` and from
`workspace/didChangeConfiguration`. A `{"aura": {...}}` wrapper is accepted.
`"warning"` is the default (also `warn`, `true`, `on`); `"error"` raises the
severity; `"off"` stops publishing type diagnostics (also `none`, `false`,
`disabled`).

```elisp
;; lsp-mode
(after! lsp-mode
  (lsp-register-custom-settings
   '(("aura.typeDiagnostics" "off"))))

;; (lsp +eglot)
(setq-default eglot-workspace-configuration
              '(:aura (:typeDiagnostics "off")))
```

`initializationOptions` also accepts `tabSize` (integer 1–8, default 2, the
column width of a tab) and `insertSpaces` (default true). Body indent stays
two columns. A formatting request that carries its own options uses those.

## Troubleshooting

- `doom doctor` reports a missing binary, the formatter gate, and `+lsp`
  without `:tools lsp`.
- "Aura executable not found": Emacs does not see the binary. Run `doom env`
  after installing Aura, or set `aura-lang-executable`, then restart Emacs.
- No diagnostics: `(aura +lsp)` and `:tools lsp` are both on, then
  `doom sync`. The modeline names `aura-lsp` once a buffer is open.
- Saving does not format: the formatter gate has not passed. `doom doctor`
  prints the reason. `SPC c f` uses the same gate.
- LSP log and restart:
  - lsp-mode: `(setq lsp-log-io t)` before the server starts, then the
    `*lsp-log*` buffer. Restart with `M-x lsp-workspace-restart`
    (`SPC c l w r`).
  - eglot: Doom sets `eglot-events-buffer-config` to size 0.
    `M-x doom/toggle-debug-mode` turns the log back on, then
    `M-x eglot-events-buffer`. Restart with `M-x eglot-reconnect`.

## Other editors

`aura lsp` speaks standard LSP over stdio. The TextMate grammar is
`editors/grammar/aura.tmLanguage.json`.
