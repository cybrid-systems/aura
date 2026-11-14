#!/usr/bin/env bash
# The demo itself. Record it with:
#   asciinema rec --overwrite --cols 100 --rows 34 -c ./run_demo.sh casts/demo.cast
# Three tmux panes running zsh. Commands are typed into them for real:
#   service   : the Aura HTTP service (aura --preset dev --grant network server.aura)
#   aura-perf : the live profile the service writes (out/perf.log, shown by ./follow)
#   shell     : real HTTP load (python3 load.py) with live req/s, p50, p99
# Needs: AURA_BIN (Aura nightly for commit 53e2ae6fbcb7313d49647b49a302f359e9255c40
#        or later), MINIMAX_API_KEY already in the environment, tmux, zsh, python3,
#        and AURA_PERF pointing at a checkout of cybrid-systems/aura-perf
#        at becc5be0100b71616e8229873894fafb3770afaf (or compatible main).
set -euo pipefail
cd "$(dirname "$0")"
: "${AURA_BIN:?set AURA_BIN to an Aura nightly binary}"
: "${MINIMAX_API_KEY:?MINIMAX_API_KEY must already be in the environment}"
: "${AURA_PERF:?set AURA_PERF to a cybrid-systems/aura-perf checkout}"
if [ ! -d "$AURA_PERF/lib/perf" ]; then
  echo "AURA_PERF=$AURA_PERF has no lib/perf (clone https://github.com/cybrid-systems/aura-perf and pin becc5be0100b71616e8229873894fafb3770afaf)" >&2
  exit 1
fi
COLS="${COLS:-100}" ROWS="${ROWS:-34}"
mkdir -p out .demo/bin
ln -sfn "$AURA_BIN" .demo/bin/aura
# Runtime symlink only; not committed.
ln -sfn "$AURA_PERF/lib" lib
rm -f out/keep_at out/done out/server.pid out/load.pid out/evo.jsonl out/llm-round-*.json out/perf.log

# The key reaches the Aura process only as LLM_API_KEY in the environment.
export LLM_API_KEY="$MINIMAX_API_KEY"; unset MINIMAX_API_KEY
export LLM_BASE_URL="https://api.minimaxi.com/v1" LLM_MODEL="MiniMax-M3.1-Flash"
# Default sandbox (unset). --preset dev is restricted + mutate/read/write;
# --grant network is for the HTTP listener and MiniMax. Leave the sandbox
# at its default: #5279–#5281 are fixed, so rebind and evo:trial work
# with the production type gate on.
unset AURA_SANDBOX
export AURA_EVO_LOG="$PWD/out/evo.jsonl" DEMO_PORT="${DEMO_PORT:-8099}"
export ZDOTDIR="$PWD/.demo" PATH="$PWD/.demo/bin:$PATH" SHELL="$(command -v zsh)"
T=(tmux -L auradmo -f .demo/tmux.conf)
"${T[@]}" kill-server 2>/dev/null || true

"${T[@]}" new-session -d -s demo -x "$COLS" -y "$ROWS" zsh
"${T[@]}" split-window -v -l 11 -t demo:0.0 zsh
"${T[@]}" split-window -h -l 41 -t demo:0.0 zsh
"${T[@]}" select-pane -t demo:0.0 -T "service"
"${T[@]}" select-pane -t demo:0.1 -T "aura-perf"
"${T[@]}" select-pane -t demo:0.2 -T "shell"

type_in() {   # type a command into a pane, key by key, then Enter
  local pane=$1 text=$2 i
  for ((i = 0; i < ${#text}; i++)); do
    "${T[@]}" send-keys -t "demo:0.$pane" -l "${text:i:1}"; sleep 0.03
  done
  sleep 0.3; "${T[@]}" send-keys -t "demo:0.$pane" Enter
}

(
  sleep 1.2
  type_in 0 "aura --preset dev --grant network server.aura"
  until (exec 3<>/dev/tcp/127.0.0.1/"$DEMO_PORT") 2>/dev/null; do sleep 0.1; done
  SHELL_PID=$("${T[@]}" display-message -p -t demo:0.0 '#{pane_pid}')
  # Locate the actual aura interpreter child (not the wrapper / zsh PID).
  python3 - "$SHELL_PID" <<'FINDPID'
import os, sys, time
root = int(sys.argv[1])
def descendants(p):
    try:
        children = open(f"/proc/{p}/task/{p}/children").read().split()
    except OSError:
        return []
    out = []
    for c in children:
        out.append(int(c)); out.extend(descendants(int(c)))
    return out
for _ in range(100):
    hits = []
    for p in descendants(root):
        try:
            cmd = open(f"/proc/{p}/cmdline", "rb").read()
            if b"server.aura" in cmd and b"libexec/aura" in cmd:
                hits.append(p)
        except OSError:
            pass
    if hits:
        open("out/server.pid", "w").write(str(hits[-1]))
        break
    time.sleep(.1)
else:
    raise SystemExit("could not locate Aura server PID")
FINDPID
  type_in 1 "./follow out/perf.log"
  type_in 2 "python3 load.py --clients 1 --items 800 --redraw 6"
  for _ in $(seq 1 600); do [ -e out/done ] && break; sleep 0.5; done
  sleep 6
  "${T[@]}" send-keys -t demo:0.2 C-c          # load prints its before/after summary
  sleep 9
  "${T[@]}" kill-server                         # ends the attach below (and the panes)
  [ -e out/server.pid ] && kill -TERM "$(<out/server.pid)" 2>/dev/null || true
) > out/controller.log 2>&1 &

"${T[@]}" attach -t demo || true
wait
