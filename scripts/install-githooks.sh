#!/usr/bin/env bash
# Install Aura git hooks (clang-format, ruff, docs regen, pre-push gate).
# Run once per clone: ./scripts/install-githooks.sh
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

for hook in pre-commit pre-push; do
    if [ ! -f ".githooks/$hook" ]; then
        echo "error: missing .githooks/$hook" >&2
        exit 1
    fi
    chmod +x ".githooks/$hook"
done

git config core.hooksPath .githooks

echo "Installed git hooks:"
echo "  core.hooksPath = $(git config --get core.hooksPath)"
echo "  pre-commit     = .githooks/pre-commit  (auto-fix staged + full-tree format check)"
echo "  pre-push       = .githooks/pre-push    (./build.py gate before push)"
echo ""
echo "pre-commit: staged C++/Python auto-fixed; src/ changes regen docs/generated/*.md"
echo "pre-push:   runs ./build.py gate (docs + lint + clang-format + fixtures)"
echo ""
echo "Bypass only when intentional: git commit --no-verify / git push --no-verify"