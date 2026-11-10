#!/usr/bin/env bash
# #5067: the commit recorded in BUILD-INFO and (via CMake) aura --version.
#
#   AURA_GIT_SHA          explicit sha (release workflow passes GITHUB_SHA)
#   git rev-parse HEAD    fallback when this tree is a git checkout
#   AURA_REQUIRE_GIT_SHA=1  exit 1 instead of printing "unknown"
#
# Prints the sha on stdout. "unknown" only when a sha is not required.
set -euo pipefail
ROOT=${1:-${AURA_ROOT:-.}}
if [ -n "${AURA_GIT_SHA:-}" ]; then
  printf '%s\n' "$AURA_GIT_SHA"
  exit 0
fi
# CI runs this with a HOME the checkout step replaced (its safe.directory
# entry is gone) and with GIT_DIR/GIT_WORK_TREE possibly left by an earlier
# step, so a bare rev-parse answered "unknown" on a real checkout. Pin the
# safe directory for this call and ignore a leaked GIT_DIR.
sha=$(env -u GIT_DIR -u GIT_WORK_TREE git -c safe.directory="$ROOT" -C "$ROOT" rev-parse HEAD 2>/dev/null || true)
if [ -z "$sha" ]; then
  if [ "${AURA_REQUIRE_GIT_SHA:-}" = 1 ]; then
    echo "resolve_git_sha: refusing commit unknown" >&2
    exit 1
  fi
  sha=unknown
fi
printf '%s\n' "$sha"
