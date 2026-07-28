# lyapunov-fact

A controlled comparison of Aura (structured local actions + native
snapshot filter) vs Python (whole-function rewrites + external filter)
on the same code-quality task with the same Lyapunov-like V proxy
and the same target set S.

The task in both experiments: turn a recursive `fact` into a correct,
iterative implementation whose AST size stays within 1.3× the initial
size. Two controllers are tested:

1. **Rules** (zero-dependency): a hand-written three-rule controller
   that proposes GOOD-ITER whenever tests fail or recursion remains.
2. **LLM** (env `AURA_LLM_API_KEY`): an OpenAI-compatible chat model is
   asked for a new `def fact` / `(define (fact ...))` form.

In both, a ΔV-based hard filter can be toggled on or off, and a native
or external snapshot lets the runner roll back a rejected proposal.

## Directory layout

```
demos/lyapunov-fact/
├── common.py               # shared StepRecord, V, in_S, LLM client
├── fact_core.aura          # Aura primitives (controller, step, helpers)
├── exp1_rules/
│   ├── run_aura_rules.py   # rules + native snapshot
│   └── run_python_rules.py # rules + external filter
├── exp2_llm/
│   ├── run_aura_llm.py     # LLM + native snapshot
│   └── run_python_llm.py   # LLM + external filter
├── analyze.py              # aggregate CSVs → summary table
├── results/                # per-trial trajectories (one CSV per run)
└── README.md
```

## V function (identical on both sides)

```
V = 10 * test_fail + 2 * recursive_residual + 0.1 * node_count + energy
```

* `test_fail` — 1 if `fact(10) != 3628800`, else 0
* `recursive_residual` — number of remaining `fact(` self-calls
* `node_count` — AST node count
* `energy` — `|new_node_count − old_node_count|` (size delta from the
  last accepted step)

## Target set S

```
S = { test_fail == 0  AND  recursive_residual == 0  AND  node_count <= 1.3 * initial }
```

## Rules (experiment 1)

```python
if test_fail == 1:                 → propose GOOD-ITER   (fix-correctness)
elif recursive_residual > 0:       → propose GOOD-ITER   (remove-recursion)
elif node_count > 25:              → propose GOOD-ITER   (simplify)
else:                              → stop
```

Aura's `propose-action` mirrors this exactly (see `fact_core.aura`).
The filter threshold is `ΔV > 0.5`; either side rejects proposals
that would push V up by more than that amount.

## LLM (experiment 2)

The system prompt asks the model to return only the new `def fact`
or `(define (fact ...))` text. The user prompt bundles the current
code, the per-component V values, and the S description. Both runners
read the same env vars:

```
export AURA_LLM_API_KEY="sk-..."   # required
export AURA_LLM_MODEL="grok-4"      # default
export AURA_LLM_BASE_URL=""         # optional
```

Env-var lookup order for the key: `AURA_LLM_API_KEY` →
`OPENAI_API_KEY` → `GROK_API_KEY` → `DEEPSEEK_API_KEY`. Base URL
defaults to `https://api.minimax.chat/v1` (matches the
`minimax-m3` model used at `~/code/keys/minimax`).

## Build

```
./build.py build
```

## Run

```
# Experiment 1 — rules, zero LLM dependency
python3 demos/lyapunov-fact/exp1_rules/run_aura_rules.py --filter on  --trials 30
python3 demos/lyapunov-fact/exp1_rules/run_aura_rules.py --filter off --trials 30
python3 demos/lyapunov-fact/exp1_rules/run_python_rules.py --filter on  --trials 30
python3 demos/lyapunov-fact/exp1_rules/run_python_rules.py --filter off --trials 30

# Experiment 2 — LLM
export AURA_LLM_API_KEY="sk-..."
export AURA_LLM_MODEL="grok-4"   # or minimax-m3
python3 demos/lyapunov-fact/exp2_llm/run_aura_llm.py   --filter on  --trials 20
python3 demos/lyapunov-fact/exp2_llm/run_aura_llm.py   --filter off --trials 20
python3 demos/lyapunov-fact/exp2_llm/run_python_llm.py --filter on  --trials 20
python3 demos/lyapunov-fact/exp2_llm/run_python_llm.py --filter off --trials 20

# Aggregate
python3 demos/lyapunov-fact/analyze.py
```

## What to look for

* **succ%** — fraction of trials that reached S
* **mean_k / med_k** — number of accepted steps before S
* **rej%** — filter reject rate
* **min_Δ** — worst single-step ΔV (deepest drawdown)
* **osc** — count of large |ΔV| sign reversals

The expectation is that Aura's structured local actions + native
snapshot produce a smoother V trajectory than Python's whole-function
replacement, especially in the LLM experiment where noisy proposals
are common. With the rules controller the gap should be small (rules
are stable); with the LLM controller the gap should widen.
