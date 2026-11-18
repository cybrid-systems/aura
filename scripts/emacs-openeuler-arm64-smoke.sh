#!/bin/sh
# OpenEuler arm64 smoke for the aura-lang Emacs package (#5266 B3).
#
# This repository's CI has no arm64 runner, so this smoke is manual.  Run
# it from a checkout on an openEuler aarch64 host (22.03 LTS / 24.03 LTS):
#
#   sh scripts/emacs-openeuler-arm64-smoke.sh
#
# It installs Emacs and the Aura nightly (install.sh, arm64), installs a
# lsp-mode from MELPA into a scratch package dir, and runs the same ERT
# suites CI runs, through eglot and lsp-mode.
#
# Recording: paste the whole transcript (host, emacs --version,
# aura --version, aura doctor if present, and the ERT summary) into issue
# #5266 as an "OpenEuler arm64 smoke" comment.  Also state the openEuler
# Emacs version and whether it is >= 29.1 (eglot's minimum).  Do not mark
# the smoke as passing if any step was skipped.
#
# Env: AURA_LANG_TEST_BIN (use an existing binary, skip install.sh),
#      AURA_VERSION (release tag for install.sh, default nightly),
#      AURA_SMOKE_DIR (work dir, default ~/aura-emacs-smoke).
set -eu

case "$(uname -m)" in
  aarch64 | arm64) ;;
  *) echo "run this on aarch64 (got $(uname -m))" >&2; exit 2 ;;
esac

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=${AURA_SMOKE_DIR:-$HOME/aura-emacs-smoke}
mkdir -p "$work"

echo "== host =="
uname -a
if [ -r /etc/openEuler-release ]; then cat /etc/openEuler-release; fi

echo "== packages =="
sudo=
if [ "$(id -u)" -ne 0 ]; then
  sudo=sudo
fi
if ! command -v emacs >/dev/null 2>&1; then
  $sudo dnf install -y emacs curl tar gzip git
fi
emacs --version | head -1
version=$(emacs --version | head -1 | sed -e 's/.* //')
major=${version%%.*}
rest=${version#*.}
minor=${rest%%.*}
if [ "$major" -lt 29 ] || { [ "$major" -eq 29 ] && [ "$minor" -lt 1 ]; }; then
  echo "emacs $version is older than 29.1; eglot is built in from 29.1" >&2
  exit 1
fi
echo "emacs $version >= 29.1: eglot available (lsp-mode comes from MELPA below)"

echo "== aura =="
bin=${AURA_LANG_TEST_BIN:-$work/bin/aura}
if [ ! -x "$bin" ]; then
  AURA_VERSION=${AURA_VERSION:-nightly} AURA_PREFIX="$work/aura" AURA_BIN_DIR="$work/bin" \
    sh "$repo/install.sh"
fi
"$bin" --version
if "$bin" doctor; then
  echo "doctor: ok"
else
  echo "doctor: not available in this build (B2 not merged); continuing"
fi

echo "== lsp-mode from MELPA =="
pkgdir=$work/elpa
# No find(1): minimal openEuler images do not ship findutils (#4934).
if ! ls "$pkgdir" 2>/dev/null | grep -q '^lsp-mode-'; then
  emacs --batch --eval "(progn (require 'package) (setq package-user-dir \"$pkgdir\" package-check-signature nil package-archives '((\"melpa\" . \"https://melpa.org/packages/\"))) (package-initialize) (package-refresh-contents) (package-install 'lsp-mode))"
fi
ls "$pkgdir" | grep '^lsp-mode-' || true

echo "== ERT: eglot and lsp-mode =="
cd "$repo"
AURA_LANG_TEST_BIN="$bin" \
AURA_LANG_TEST_PACKAGE_DIR="$pkgdir" \
AURA_LANG_EMACS_ARGS=-Q \
  sh editors/emacs/test/run-ert.sh

echo
echo "Smoke finished.  Paste this transcript into issue #5266 and record:"
echo "  - openEuler release: $(cat /etc/openEuler-release 2>/dev/null || echo unknown)"
echo "  - emacs: $version (eglot built in, lsp-mode from MELPA)"
echo "  - aura: $("$bin" --version 2>&1 | head -1)"
