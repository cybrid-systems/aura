#!/bin/sh
# Re-record the README demo: docs/assets/sum-to.cast and docs/assets/sum-to.gif.
#
#   AURA=./build/aura scripts/record_demo.sh
#
# Needs asciinema and agg (https://github.com/asciinema/agg) on PATH.
# The cast is a real `aura --preset dev examples/sum-to.aura` run.
# Width is 104 columns so the live letrec source (101 columns) stays one line.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [ "${1:-}" = "--play" ]; then
  [ -n "${AURA:-}" ] || { echo "AURA is not set" >&2; exit 1; }
  printf '\033[H\033[2J'
  cmd='aura --preset dev examples/sum-to.aura'
  printf '$ '
  rest=$cmd
  while [ -n "$rest" ]; do
    printf '%s' "$(printf '%s' "$rest" | cut -c1)"
    rest=$(printf '%s' "$rest" | cut -c2-)
    sleep 0.04
  done
  printf '\n'
  sleep 0.45
  "$AURA" --preset dev examples/sum-to.aura
  exit $?
fi

if [ -z "${AURA:-}" ]; then
  if [ -x "$ROOT/build/aura" ]; then
    AURA=$ROOT/build/aura
  elif command -v aura >/dev/null 2>&1; then
    AURA=$(command -v aura)
  else
    echo "set AURA, or build ./build/aura, or put aura on PATH" >&2
    exit 1
  fi
fi
case $AURA in
  /*) ;;
  *) AURA=$ROOT/$AURA ;;
esac
export AURA
export TERM=xterm-256color

command -v asciinema >/dev/null 2>&1 || { echo "asciinema is required" >&2; exit 1; }
command -v agg >/dev/null 2>&1 || { echo "agg is required (https://github.com/asciinema/agg)" >&2; exit 1; }
[ -f "$ROOT/examples/sum-to.aura" ] || { echo "missing examples/sum-to.aura" >&2; exit 1; }

mkdir -p "$ROOT/docs/assets"
CAST=$ROOT/docs/assets/sum-to.cast
GIF=$ROOT/docs/assets/sum-to.gif

asciinema rec --overwrite -q -y \
  --cols 104 --rows 26 \
  -c "$ROOT/scripts/record_demo.sh --play" \
  "$CAST"

agg --font-family "DejaVu Sans Mono" \
  --font-size 15 \
  --line-height 1.3 \
  --fps-cap 12 \
  --speed 1 \
  --idle-time-limit 5 \
  --last-frame-duration 3 \
  --theme github-dark \
  "$CAST" "$GIF"

echo "cast $(wc -c < "$CAST") bytes  gif $(wc -c < "$GIF") bytes"
