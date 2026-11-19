# aura-lang

Emacs Lisp for Aura: `aura-mode`, an inferior REPL, and client registration
for lsp-mode and eglot. It needs Emacs 29.1 or newer and does not load either
LSP client.

Doom Emacs is the supported editor. Install, keys, configuration, and
troubleshooting are in [docs/editor.md](../../docs/editor.md). The Doom module
is `editors/emacs/doom/lang/aura/`; its `packages.el` fetches this file.

Keyword and definition lists come from
`editors/grammar/aura.tmLanguage.json`.

## Tests

```sh
cd editors/emacs
emacs -Q --batch --eval '(setq byte-compile-error-on-warn t)' \
      -f batch-byte-compile aura-lang.el
emacs -Q --batch -L . -l test/aura-lang-test.el \
      -f ert-run-tests-batch-and-exit
emacs -Q --batch -L . -l doom/test/aura-doom-test.el \
      -f ert-run-tests-batch-and-exit
```

Tests that need the `aura` binary are skipped when it is missing.
