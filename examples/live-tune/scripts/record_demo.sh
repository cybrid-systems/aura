#!/usr/bin/env bash
# Record run_demo.sh with asciinema, then render a GIF with agg.
#   scripts/record_demo.sh [name]   -> casts/<name>.cast, casts/<name>.gif
# Everything on screen is the real run: Aura service, aura-perf profile,
# MiniMax answers, evo:trial verdicts, load numbers.
set -euo pipefail
cd "$(dirname "$0")/.."
NAME="${1:-demo}"
export PATH="$HOME/.local/bin:$PATH" TERM=xterm-256color
COLS="${COLS:-100}" ROWS="${ROWS:-34}"
export COLS ROWS
mkdir -p casts
# asciinema wants a tty; script(1) provides one when this runs headless.
script -qfec "asciinema rec --overwrite --cols $COLS --rows $ROWS -c ./run_demo.sh casts/$NAME.raw.cast" /dev/null >/dev/null
# tmux's own "[exited]" teardown is dropped; every earlier event is unchanged.
python3 scripts/trim_teardown.py "casts/$NAME.raw.cast" "casts/$NAME.cast"
if grep -q -- "${LLM_API_KEY:-${MINIMAX_API_KEY:-__none__}}" "casts/$NAME.raw.cast"; then
  echo "secret found in cast" >&2; exit 1
fi
agg --theme dracula --font-size 15 --font-family "JetBrains Mono,DejaVu Sans Mono,Noto Emoji" \
    --font-dir "$HOME/.local/share/fonts/demo" --idle-time-limit 2 --fps-cap 8 --last-frame-duration 5 \
    "casts/$NAME.cast" "casts/$NAME.gif" >/dev/null
ls -l "casts/$NAME.cast" "casts/$NAME.gif"
