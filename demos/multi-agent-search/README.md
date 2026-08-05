# multi-agent-search

**Concurrent multi-hypothesis mutation search** on Aura, with a
Lyapunov-style control filter — not “ask the model to rewrite fact”.

```
local catalog agents  ─┐
                       ├── dry-run rank (ΔV filter) ──► single commit
MiniMax M3 (N concurrent API calls) ─┘
```

## Why this shape

| Bad demo shape | multi-agent-search |
|----------------|-------------------|
| Single rewrite, tests model Scheme skill | **N proposals / wave**, tests host topology |
| LLM must emit full s-expr | LLM emits **one integer T** only |
| No concurrency story | ThreadPool MiniMax + optional `fiber:spawn` probe |
| Open-ended codegen | Aura wins on **snapshot dry-run + single mutator + V filter** |

## Domain: budget gate

```
(define (gate x) …)
Spec: deny 0,3  ·  admit 5,10   →  threshold T ∈ {4,5}
Seed: (define (gate x) #f)      →  fail_count = 4
```

### Control (Lyapunov proxy)

```
V = 10 · fail_count + 0.1 · node_count + energy
energy = 0.05 · |Δnodes|
S = fail_count == 0  ∧  node_count ≤ 1.5 · initial
```

Hard filter: reject candidate if **ΔV > 0.5** (restore snapshot).

## Topology (one wave)

1. **Local agents** — catalog templates (`ge-4`, `ge-5`, poison `lt-5`, …)
2. **LLM agents** (hybrid/live) — concurrent MiniMax M3 calls; each returns `T`
3. **Arbiter** — sequential dry-run under `ast:snapshot` / restore; rank by V
4. **Single mutator** — commit best only; never concurrent rebind of core

## Run

```bash
./build.py build
export AURA_PIPELINE_STRICT=0

# Offline — local mutation search only (CI-friendly, no API)
python3 demos/multi-agent-search/run_demo.py --mode offline --waves 5 --fiber-probe

# Hybrid — local + concurrent MiniMax M3
export AURA_LLM_API_KEY=...          # or MINIMAX_API_KEY
export AURA_LLM_MODEL=MiniMax-M3     # default
export AURA_LLM_BASE_URL=https://api.minimax.chat/v1
python3 demos/multi-agent-search/run_demo.py --mode hybrid --waves 5 --n-llm 4 --max-workers 4
```

## What to look for

| signal | healthy |
|--------|---------|
| `in_S=True` within 1–2 waves | offline always; hybrid if LLM/key ok |
| `V` monotone non-increasing on commits | Lyapunov filter working |
| `rej_filter` / install_fail in extra | poison proposals blocked |
| `n_llm` / concurrent workers | MiniMax fanout active |
| `fiber_sum=6` with `--fiber-probe` | host fiber:spawn path alive (#2656) |

CSV trajectories: `demos/multi-agent-search/results/*.csv`.

## Layout

```
demos/multi-agent-search/
  common.py        # V, S, MiniMax client, concurrent llm_chat_many
  aura_host.py     # Repl, install, snapshot, fail_count, fiber probe
  local_search.py  # catalog mutation search
  agents.py        # fanout local + llm proposers
  run_demo.py      # offline | hybrid | live
  README.md
  results/
```
