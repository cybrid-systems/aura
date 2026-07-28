# lyapunov-fact

Controlled comparison of **Aura** (workspace `set-code` + native `ast:snapshot`
filter) vs **Python** (whole-function rewrite + external ΔV filter) on the same
code-quality task, the same Lyapunov-like proxy **V**, and the same target set
**S**.

Task: turn a recursive `fact` into a correct iterative implementation whose
AST size stays within 1.5× the seed size.

Two controllers:

1. **Rules** (zero dependency): three-rule controller that proposes GOOD-ITER
   whenever tests fail or recursion remains.
2. **LLM** (`AURA_LLM_API_KEY`): OpenAI-compatible chat model returns a new
   `def fact` / `(define (fact ...))`.

In both, a ΔV hard filter (`ΔV > 0.5` rejects) can be toggled on/off.

## Layout

```
demos/lyapunov-fact/
├── common.py                 # V, S, StepRecord, LLM client
├── aura_driver.py            # long-lived Aura Repl pipe driver
├── fact_core.aura            # Aura helpers (reference / optional load)
├── exp1_rules/
│   ├── run_aura_rules.py
│   └── run_python_rules.py
├── exp2_llm/
│   ├── run_aura_llm.py
│   └── run_python_llm.py
├── analyze.py                # CSV → comparison table
├── run_all_rules.sh          # full rules matrix
├── results/                  # per-trial trajectories
└── README.md
```

## V (identical both sides)

```
V = 10 * test_fail + 2 * recursive_residual + 0.1 * node_count + energy
energy = 0.05 * |new_node_count − old_node_count|
```

| term | meaning |
|------|---------|
| `test_fail` | 1 if `fact(10) ≠ 3628800`, else 0 |
| `recursive_residual` | remaining self-calls of `fact` |
| `node_count` | AST size (CPython `ast.walk` / Aura `query:children` walk) |
| `energy` | soft size-change penalty (scaled so a good rewrite can still drop V) |

## Target set S

```
S = { test_fail == 0  ∧  recursive_residual == 0  ∧  node_count ≤ 1.5 × initial }
```

(1.5× rather than 1.3× so Aura FlatAST and CPython AST both admit the
same iterative rewrite.)

## Rules (experiment 1)

```
if test_fail == 1:            → GOOD-ITER   (fix-correctness)
elif recursive_residual > 0:  → GOOD-ITER   (remove-recursion)
elif node_count > 25:         → GOOD-ITER   (simplify)
else:                         → stop
```

Filter threshold: reject if `ΔV > 0.5`.

## Build / run

```bash
./build.py build

# Rules matrix (recommended first)
export AURA_PIPELINE_STRICT=0
TRIALS=20 ./demos/lyapunov-fact/run_all_rules.sh

# Or one config at a time
python3 demos/lyapunov-fact/exp1_rules/run_python_rules.py --filter on  --trials 20
python3 demos/lyapunov-fact/exp1_rules/run_aura_rules.py   --filter on  --trials 20

# LLM (optional)
export AURA_LLM_API_KEY=...
export AURA_LLM_MODEL=grok-4          # or minimax-m3
export AURA_LLM_BASE_URL=             # optional
python3 demos/lyapunov-fact/exp2_llm/run_python_llm.py --filter on  --trials 10
python3 demos/lyapunov-fact/exp2_llm/run_aura_llm.py   --filter on  --trials 10

# Aggregate
python3 demos/lyapunov-fact/analyze.py
```

Key lookup order: `AURA_LLM_API_KEY` → `OPENAI_API_KEY` → `GROK_API_KEY` →
`DEEPSEEK_API_KEY`. Default base URL `https://api.minimax.chat/v1`.

## What to look for

| column | meaning |
|--------|---------|
| **succ%** | trials that reached S |
| **mean_k / med_k** | steps to first S |
| **rej%** | filter reject rate |
| **min_Δ** | worst single-step ΔV (drawdown) |
| **osc** | large \|ΔV\| sign reversals |
| **mean_V∞** | mean final V |

Rules controller is deterministic: both engines should hit **100% succ**
in one step with a smooth negative ΔV. LLM controller is noisier — native
snapshot + ΔV filter on Aura is expected to cut reject-able drawdowns more
cleanly than Python’s whole-function external filter.
