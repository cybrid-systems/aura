#!/usr/bin/env bash
# Install Aura git hooks (clang-format, ruff, docs regen).
# Run once per clone: ./scripts/install-githooks.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [ ! -f .githooks/pre-commit ]; then
    echo "error: missing .githooks/pre-commit" >&2
    exit 1
fi

chmod +x .githooks/pre-commit
git config core.hooksPath .githooks

echo "Installed git hooks:"
echo "  core.hooksPath = $(git config --get core.hooksPath)"
echo "  pre-commit     = .githooks/pre-commit"
echo ""
echo "On each commit with staged src/ changes, docs/generated/*.md is"
echo "auto-regenerated and re-staged (matches CI ./build.py gate)."