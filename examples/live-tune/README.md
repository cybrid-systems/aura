# Live-tune example

A real Aura HTTP service de-duplicates sign-up email lists while it runs. [aura-perf](https://github.com/cybrid-systems/aura-perf) builds a call-context tree from live samples of those requests. MiniMax proposes a faster `dedupe`. `evo:trial` replays recent real responses in a child worldline; a passing candidate is swapped in with `mutate :rebind`. The process does not restart.

The README recording is `docs/assets/live-tune.gif` (and the asciinema file `docs/assets/live-tune.cast`). Everything on screen is from one real run. There is no hardcoded proposal and no scripted KEEP/ROLLBACK.

## What this take did

Recorded on Aura nightly `53e2ae6fbcb7313d49647b49a302f359e9255c40` (`ghcr.io/cybrid-systems/aura:53e2ae6fb`), with the **default sandbox** (`--preset dev --grant network`, `AURA_SANDBOX` unset, so the production type gate is on). aura-perf was [cybrid-systems/aura-perf](https://github.com/cybrid-systems/aura-perf) at `becc5be0100b71616e8229873894fafb3770afaf`. Model: `MiniMax-M3.1-Flash` at `https://api.minimaxi.com/v1`, `reasoning_effort=minimal`.

| | Before the swap | After the swap |
|---|---:|---:|
| HTTP requests/s | 1.6 | 69.6 |
| HTTP p50 | 462 ms | 14 ms |
| HTTP p99 | 2625 ms | 24 ms |
| aura-perf `dedupe` (avg of 3) | 425 ms | 5 ms |
| aura-perf whole request (avg of 3) | 430 ms | 12 ms |

- 894 completed HTTP requests; **0 failures, 0 server restarts**. The server PID and start ticks were the same before and after the swap. Every response was checked against an independent Python oracle.
- Round 1: MiniMax answered in 5.8 s with a `let*`-bound `loop` lambda. Aura refused to load it (`loop` is unbound inside that lambda). **ROLLBACK**, live code re-checked and unchanged.
- Round 2: MiniMax answered in 4.8 s with a named `let` and a hash table. Four recent real requests matched and were faster (1729 ms → 40 ms). **KEEP** via `mutate :rebind`.
- The before p99 of 2625 ms is a stall: the accept loop pauses during a synchronous trial, so in-flight requests queue. None failed. In the 16 s before that stall, p50 was 462 ms and p99 was 929 ms. After the swap, p99 was 24 ms (max 45 ms).
- Baseline times here are the cost of the original list-scan `dedupe`, not the post-trial slowdown tracked in Aura #5282 (a discarded trial no longer makes the live function ~3× slower on this build).

`perf:llm-propose` in aura-perf is still a stub. This example uses aura-perf for the call-context tree, mining, speed gate, and after-swap diff, and uses Aura's own `llm:chat`, `evo:trial`, and `mutate :rebind` for the live loop.

## Reproduce

You need a Linux x86_64 or arm64 host, tmux, zsh, Python 3, [asciinema](https://github.com/asciinema/asciinema) and [agg](https://github.com/asciinema/agg) if you want to record a GIF, an Aura nightly at or after `53e2ae6fbcb7313d49647b49a302f359e9255c40`, and a checkout of aura-perf.

```sh
# Pin Aura. Use the dist artifact or the image tag for this commit:
#   ghcr.io/cybrid-systems/aura:53e2ae6fb
export AURA_BIN=/path/to/aura   # the nightly `bin/aura` wrapper

# aura-perf is a separate repo; do not vendor it as a symlink in git.
git clone https://github.com/cybrid-systems/aura-perf
git -C aura-perf checkout becc5be0100b71616e8229873894fafb3770afaf
export AURA_PERF=$PWD/aura-perf

# The key must already be in the environment. Do not paste it into a command,
# a file, or the shell history.
# export MINIMAX_API_KEY=   # set this in your environment only

cd examples/live-tune
./scripts/record_demo.sh live-tune
```

`run_demo.sh` (called by the recorder) starts a tmux session with three zsh panes, types the real commands, and leaves `AURA_SANDBOX` unset. It copies `MINIMAX_API_KEY` into `LLM_API_KEY` for the Aura process and then unsets the original name in that process. `record_demo.sh` wraps `asciinema rec --cols 100 --rows 34` in `script(1)` so it can run without a desktop tty, trims only tmux's trailing teardown frame (`scripts/trim_teardown.py`), and refuses to write a cast that contains the API key.

Without recording, you can run the same session as `./run_demo.sh` from a 100×34 terminal.
