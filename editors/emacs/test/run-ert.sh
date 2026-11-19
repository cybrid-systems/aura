#!/bin/sh
# Run the aura-lang ERT suites once per Emacs LSP client.
#
#   editors/emacs/test/run-ert.sh
#
# Environment:
#   AURA_LANG_TEST_BIN          aura executable (else aura on PATH)
#   AURA_LANG_EMACS             emacs binary (default: emacs)
#   AURA_LANG_EMACS_CLIENTS     clients, default "eglot lsp-mode"
#   AURA_LANG_EMACS_ARGS        extra emacs args (for example -Q locally)
#   AURA_LANG_TEST_LOAD_PATH    colon-separated -L dirs (lsp-mode packages)
#   AURA_LANG_TEST_PACKAGE_DIR  package-user-dir to initialize (local MELPA)
#   AURA_LSP_DEBOUNCE_MS        server debounce for the unsaved-edit test
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")

emacs=${AURA_LANG_EMACS:-emacs}
clients=${AURA_LANG_EMACS_CLIENTS:-eglot lsp-mode}
status=0

if [ -n "${AURA_LANG_TEST_BIN:-}" ]; then
  if [ ! -x "$AURA_LANG_TEST_BIN" ]; then
    echo "run-ert: AURA_LANG_TEST_BIN is not executable: $AURA_LANG_TEST_BIN" >&2
    exit 1
  fi
  PATH=$(dirname "$AURA_LANG_TEST_BIN"):$PATH
  export PATH
elif ! command -v aura >/dev/null 2>&1; then
  echo "run-ert: no aura binary; set AURA_LANG_TEST_BIN or put aura on PATH" >&2
  exit 1
fi

load_args=
if [ -n "${AURA_LANG_TEST_LOAD_PATH:-}" ]; then
  old_ifs=$IFS
  IFS=:
  for dir in $AURA_LANG_TEST_LOAD_PATH; do
    load_args="$load_args -L $dir"
  done
  IFS=$old_ifs
fi

init=
if [ -n "${AURA_LANG_TEST_PACKAGE_DIR:-}" ]; then
  init="(progn (setq package-user-dir \"$AURA_LANG_TEST_PACKAGE_DIR\") (package-initialize))"
fi

echo "emacs: $("$emacs" --version | head -1)"
if [ -n "${AURA_LANG_TEST_BIN:-}" ]; then
  echo "aura:  $("$AURA_LANG_TEST_BIN" --version 2>&1 | head -1)"
else
  echo "aura:  $(aura --version 2>&1 | head -1)"
fi

# The base suite asserts that loading aura-lang does not load lsp-mode,
# so it runs in its own Emacs process before any client is loaded.
echo "===== ERT: aura-lang ====="
# shellcheck disable=SC2086
"$emacs" ${AURA_LANG_EMACS_ARGS:-} --batch ${init:+--eval "$init"} $load_args \
  -L "$root" \
  -l "$root/test/aura-lang-test.el" \
  -f ert-run-tests-batch-and-exit || status=1

echo "===== ERT: doom module ====="
# shellcheck disable=SC2086
"$emacs" ${AURA_LANG_EMACS_ARGS:-} --batch ${init:+--eval "$init"} $load_args \
  -L "$root" \
  -l "$root/doom/test/aura-doom-test.el" \
  -f ert-run-tests-batch-and-exit || status=1

for client in $clients; do
  echo "===== ERT: LSP client=$client ====="
  # shellcheck disable=SC2086
  AURA_LANG_EMACS_CLIENT=$client \
  AURA_LSP_DEBOUNCE_MS=${AURA_LSP_DEBOUNCE_MS:-250} \
    "$emacs" ${AURA_LANG_EMACS_ARGS:-} --batch ${init:+--eval "$init"} $load_args \
      -L "$root" \
      -l "$root/test/aura-lang-lsp-test.el" \
      --eval "(ert-run-tests-batch-and-exit \"^aura-lang-lsp-\")" || status=1
done

# #5367: Doom/straight byte-compiles aura-lang.el without an LSP client
# loaded, so a macro called as a function only breaks in the .elc — the
# source passes above cannot catch that.  Compile the client here in a
# clean Emacs (no lsp-mode on the compile load path, exactly the straight
# build condition) and run the lsp-mode suite against the .elc, loaded
# before the test file so its `require' is a no-op.
case " $clients " in
  *" lsp-mode "*)
    echo "===== ERT: LSP client=lsp-mode (byte-compiled aura-lang) ====="
    elc_dir=$(mktemp -d)
    if "$emacs" -Q --batch \
         --eval "(setq byte-compile-error-on-warn t)" \
         --eval "(setq byte-compile-dest-file-function
                       (lambda (_) (expand-file-name \"aura-lang.elc\" \"$elc_dir\")))" \
         --eval "(byte-compile-file \"$root/aura-lang.el\")"; then
      # shellcheck disable=SC2086
      AURA_LANG_EMACS_CLIENT=lsp-mode \
      AURA_LSP_DEBOUNCE_MS=${AURA_LSP_DEBOUNCE_MS:-250} \
        "$emacs" ${AURA_LANG_EMACS_ARGS:-} --batch ${init:+--eval "$init"} $load_args \
          -L "$root" \
          --eval "(load \"$elc_dir/aura-lang.elc\" nil t)" \
          -l "$root/test/aura-lang-lsp-test.el" \
          --eval "(ert-run-tests-batch-and-exit \"^aura-lang-lsp-\")" || status=1
    else
      echo "run-ert: byte-compiling aura-lang.el failed" >&2
      status=1
    fi
    rm -rf "$elc_dir"
    ;;
esac

exit $status
