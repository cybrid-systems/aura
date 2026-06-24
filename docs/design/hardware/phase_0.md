# AI-Native SystemVerilog/Verilog IR — Phase 0

> Issue #294. Scope-limited MVP that closes the issue body.
> The full design proposal is a multi-phase roadmap (Phase 1-4).

## Goal

Provide Aura users with the **3 most-requested query helpers** for
hardware verification workflows:

1. `eda:query:always-ff-with-clock` — find all flip-flop blocks
   driven by a specific clock
2. `eda:query:assertions-involving-signal` — find SVA assertions
   that mention a signal in their property expression
3. `eda:query:reset-condition-for-register` — find the reset
   signal in a multi-edge sensitivity list (typical SV pattern:
   `posedge clk, negedge rst_n`)

These are pure-Aura helpers that walk the existing EDA IR
(lib/std/eda.aura). No new C++ modules required.

## What ships in Phase 0

**lib/std/eda.aura** — 3 new helpers, exported in the public
interface list:

```scheme
(eda:query:always-ff-with-clock "clk_i" my-module)
;; → list of always_ff blocks whose single-edge sensitivity is clk_i

(eda:query:assertions-involving-signal "mr_err" my-module)
;; → list of SVA assertions whose property expression mentions mr_err

(eda:query:reset-condition-for-register "wr_state_q" my-module)
;; → reset signal name (e.g. "rst_ni") from a 2-edge sensitivity list,
;;   or #f if no always_ff has 2-edge sensitivity
```

**tests/test_issue_294.cpp** — 5 ACs covering the 3 helpers
(plus edge cases: no match, wrong clock, single-edge always_ff).

## What ships in subsequent phases (deferred)

The full issue body has 4 more phases:

| Phase | Scope | Status |
|---|---|---|
| **1** | Full SVA property/sequence representation, key mutation operations for implication direction / reset handling / condition normalization, transactional mutation with basic equivalence checking | OPEN |
| **2** | Export to formal verification tools (JasperGold / SymbiYosys), mutation testing support, integration with existing parsers (CIRCT/slang) | OPEN |
| **3** | Full agent loop: generate → query → mutate → verify → self-improve, handling of large designs and contamination (`ifdef`, generate) | OPEN |
| **4** | Round-tripping & pretty-printing, coverage-driven assertion generation, open-source release + benchmarks on OpenTitan / CVA6 etc. | OPEN |

These are queued as separate issues. Each phase is ~weeks of work.

## Design Decisions

1. **Pure Aura over C++** — the 3 query helpers are pure-Aura functions
   that walk the existing EDA IR. No new C++ modules, no new
   module interfaces, no new primitives. Keeps the surface small
   and the validation cycle fast.
2. **Heuristic-based reset detection** — `eda:query:reset-condition-for-register`
   returns the second-edge signal from a 2-edge sensitivity list.
   This is a heuristic; async/sync detection, polarity, and value
   semantics are Phase 1.
3. **No dependency on CIRCT/slang yet** — the helpers walk our own
   EDA IR (lib/std/eda.aura). Integration with CIRCT dialects is
   Phase 2 (architecture decision deferred).

## AI-Native Properties (Phase 0 subset)

- ✅ Reflective: helpers are Aura-callable, not C++-only
- ✅ Composable: each helper returns a list, callers chain with
  standard Aura `map` / `filter`
- ✅ Workspace-aware: works inside `cs.eval`, `set-code`, or
  `(load ...)` — the IR is the source of truth
- ✅ Pure: no mutation, no global state, deterministic

## Test Coverage

`tests/test_issue_294.cpp` covers:
- AC #1: clock matching returns the expected always_ff count
- AC #2: assertion matching by signal name
- AC #3: reset signal extraction from 2-edge sensitivity
- AC #4: single-edge always_ff returns `#f` for reset
- AC #5: non-existent clock returns 0 matches

Wired into `test_issues_jit` bundle (53/53, was 52).

## Future Work (per issue body)

- **Async vs sync reset detection** (Phase 1)
- **Polarity detection** (Phase 1)
- **Reset value detection** (Phase 1)
- **Agent loop integration** (Phase 3)
- **Mutation primitives** (Phase 1):
  - `mutate:convert-implication`
  - `mutate:normalize-long-condition`
  - `mutate:add-disable-iff-async-reset`
  - `mutate:lift-nested-conditions-to-assertion`
  - `mutate:fix-clock-cycle-alignment`
- **CIRCT / slang integration** (Phase 2)
- **Mutation testing** (Phase 2)
