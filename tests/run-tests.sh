#!/usr/bin/env bash
# Thin entrypoint → tests/python/run-tests.sh (Issue #1932 layout).
exec bash "$(cd "$(dirname "$0")" && pwd)/python/run-tests.sh" "$@"
