# aura-lang — Emacs support for Aura

An Emacs package for the [Aura](https://github.com/cybrid-systems/aura)
language.  It is plain Emacs Lisp: no C++ build and no Doom requirement.

- `aura-mode` for `.aura` files, derived from `lisp-data-mode`.
- Font-lock whose keyword and definition lists come from the TextMate
  grammar (`editors/grammar/aura.tmLanguage.json`, falling back to
  `editors/vscode/syntaxes/aura.tmLanguage.json` until #5273 lands).
- Indentation through `lisp-indent-function` properties, imenu, and
  `project.el` support for `aura.toml`.
- `run-aura` / `inferior-aura-mode`: a comint REPL over a PTY, including
  multi-line definitions and the `:ws` `:src` `:doc` `:type` `:worldline`
  `:snapshot` `:restore` commands.
- Optional registration with lsp-mode and eglot; neither is loaded by the
  package.

Requires Emacs 29.1 or newer (eglot is built in from 29).

## Install

### use-package with a checkout

```elisp
(use-package aura-lang
  :load-path "~/src/aura/editors/emacs"
  :mode "\\.aura\\'"
  :custom
  (aura-lang-executable "/usr/local/bin/aura"))
```

### package-vc-install (Emacs 30+)

```elisp
(package-vc-install
 '(aura-lang
   :url "https://github.com/cybrid-systems/aura"
   :lisp-dir "editors/emacs"))
```

### straight.el

```elisp
(straight-use-package
 '(aura-lang :type git :host github :repo "cybrid-systems/aura"
             :files ("editors/emacs/aura-lang.el")))
```

`aura-lang.el` is self-contained, so the `:files` list only needs that one
file.  This README and the ERT tests are not part of the installed package.

### Doom Emacs

`editors/emacs/doom/lang/aura/` is a Doom module that wraps this package
(`(doom! :lang (aura +lsp))`).  It lives outside the installed package on
purpose: copy or symlink it to `$DOOMDIR/modules/lang/aura/`.  Both
layouts work.  Doom's autoload scan skips a symlinked module directory
whose target is outside the modules tree (`doom-module-from-path` compares
`file-truename`s), so `config.el` and `doctor.el` load `autoload/aura.el`
themselves and do not depend on that scan.  See `docs/editor.md`, "Doom
Emacs", for the keys, the `:tools lsp` client switch and the `aura --fmt`
version gate.

## Finding the binary

`aura-lang-executable` is consulted first.  When it is nil the package
looks for `aura` in `PATH`, then `~/.local/bin/aura`, then
`~/.aura/current/bin/aura` (the paths `install.sh` creates).  A missing
binary signals a `user-error` that names all four options, and runs
`aura-lang-executable-not-found-functions` first so a doctor command can
add its own advice (for example “run `doom env`”).

`AURA_PATH` is passed through to child processes unchanged.  Set
`aura-lang-path` only to add extra library roots; it never overrides an
existing `AURA_PATH`.

`aura-lang-version` parses `aura --version` and returns a plist: release
builds such as `aura 0.2.6` give `(:kind release :version "0.2.6")`;
nightlies such as `aura nightly (cf7353373)` give
`(:kind nightly :commit "cf7353373")`.

## LSP

Both clients are only *declared*; loading `aura-lang` does not load
either one.

- **eglot** (built in): no setup beyond installing the package.  Eglot
  uses the `aura-mode` entry in `eglot-server-programs`, which runs
  `aura lsp` and reports `languageId` `aura`.
- **lsp-mode**: `(require 'lsp-mode)` (or let `lsp` autoload), then
  `M-x lsp` in an `.aura` buffer.  The package registers the client and
  `(aura-mode . "aura")` in `lsp-language-id-configuration` when lsp-mode
  loads.

Diagnostics come from the server’s static check: parse errors and names
that are unbound at runtime.  See `docs/editor.md` for the server’s
limits.

## REPL

`M-x run-aura` starts `aura repl` in `*aura*` over a PTY.  A PTY matters:
on a pipe the REPL evaluates multi-line forms line by line and reports a
parse error.

| command | effect |
|---|---|
| `C-c C-r` | `aura-lang-send-region` |
| `C-c C-c` | `aura-lang-send-defun` |
| `C-c C-b` | `aura-lang-send-buffer` |
| `C-c C-l` | `aura-lang-reload-file` (`:reload` the visited file) |
| `C-c C-w` | `aura-lang-repl-ws` |
| `C-c C-d` | `aura-lang-repl-doc` |
| `C-c C-t` | `aura-lang-repl-type` |
| `C-c C-s` | `aura-lang-repl-src` |
| `C-c C-o` | `aura-lang-repl-worldline` |
| `C-c C-n` | `aura-lang-repl-snapshot` |
| `C-c C-e` | `aura-lang-repl-restore` |

The key bindings are not installed by default; bind them in
`aura-mode-map` / `inferior-aura-mode-map` if you want them.

The REPL currently reports primitive errors as a value
(`- : unknown = car: not a pair`).  `inferior-aura-mode` font-locks that
shape as an error until #5234/#5241 make the result structured.

## Tests

```sh
cd editors/emacs
emacs -Q --batch --eval '(setq byte-compile-error-on-warn t)' \
      -f batch-byte-compile aura-lang.el
emacs -Q --batch -L . -l test/aura-lang-test.el \
      -f ert-run-tests-batch-and-exit
```

The ERT tests that need the Aura binary are skipped when it is missing.
They are outside `tests/`, so the Aura test-home rules and the issue-farm
do not apply.

## Known limits

- `[` and `]` are treated as matching brackets, not as list syntax.  The
  reader accepts `[1 2 3]` but `(display [1 2 3])` evaluates to `3` and
  `(car [1 2 3])` is a parse error, so the intended semantics are still
  open (see issue #5264).  The mode does not guess.
- Indentation follows the formatter’s body indent of two columns.  The A6
  formatter is not in this tree; `aura --fmt` normalises `let` / `when` /
  `cond` bodies to +2, which is what `aura-lang--indent-specs` produces.
- The lsp-mode client has not been exercised on this machine (lsp-mode is
  not installed); eglot is covered by an ERT test that speaks to a real
  `aura lsp`.
