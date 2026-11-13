#!/bin/sh
# Re-record the README demo from a real aura run.
# Writes docs/assets/sum-evolve.cast and docs/assets/sum-evolve.gif.
# Requires: aura, asciinema, agg (https://github.com/asciinema/agg).
set -eu
cd "$(dirname "$0")/.."
if ! command -v aura >/dev/null 2>&1; then
  echo "record_demo: aura is not on PATH" >&2
  exit 1
fi
if ! command -v asciinema >/dev/null 2>&1; then
  echo "record_demo: asciinema is not on PATH" >&2
  exit 1
fi
if ! command -v agg >/dev/null 2>&1; then
  echo "record_demo: agg is not on PATH" >&2
  exit 1
fi
mkdir -p docs/assets
cols=96
rows=24
cast=docs/assets/sum-evolve.cast
gif=docs/assets/sum-evolve.gif
# The shell line is the command a reader runs. aura's own stdout/stderr follow.
asciinema rec --overwrite --quiet \
  --cols "$cols" --rows "$rows" \
  -c "sh -c 'printf \"%s\\n\" \"\$ aura --preset dev examples/sum-evolve.aura\"; exec aura --preset dev examples/sum-evolve.aura'" \
  "$cast"
# fps-cap keeps the GIF small: the run pauses on each line, so 8 fps is enough.
agg --font-size 14 --fps-cap 8 --theme github-dark --speed 1 "$cast" "$gif"
echo "cast $(wc -c < "$cast") bytes"
echo "gif  $(wc -c < "$gif") bytes"
