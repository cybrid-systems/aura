# Hardware IR + Verilog Backend for Agentic EDA (Issue #182)

**Status:** Design + 4-cycle migration plan. 0 sub-items shipped.
**Date:** 2026-06-13
**Priority:** P1 (feature — unlocks Agentic EDA use case)

## Problem (per issue body)

Verilog is currently only usable as plain text or strings in Aura.
Hardware designers (and AI agents working on hardware) need:

- **Structured representation** of hardware logic, queryable
  like an AST
- **Semantic mutations**: insert clock gating, extract common
  sub-expressions, add protection logic
- **Safe experimentation**: try → evaluate → rollback
- **Self-evolution**: design strategies that improve over
  multiple iterations

Current text-based agents (Claude Code, Devin, etc.) lack
these capabilities because Verilog has weak type system and
no safe-mutation primitives.

## Building blocks already in Aura (no new C++ needed)

#182 is **almost entirely stdlib work** (`std/eda`). The C++
core already provides everything needed:

| Primitive | Source | Used by std/eda for |
|---|---|---|
| `query:find` / `query:node-type` / `query:children` / `query:parent` / `query:siblings` | C++ core | hardware structure queries |
| `query:where` / `query:filter` / `query:pattern` | C++ core + Aura helpers | structured hardware queries |
| `mutate:replace-value` / `mutate:rebind` / `mutate:splice` / `mutate:insert` / `mutate:remove` | C++ core | hardware mutations |
| `mutate:query-and-replace` (qar) | C++ core | clock-gating insertion, common-subexpr extraction |
| `ast:snapshot` / `ast:rollback` | C++ core | safe try → evaluate → rollback |
| `intend` | Aura | self-repair loop |
| `orch:*` | Aura | multi-agent EDA workflows |
| `define-hygienic-macro` (Issue #165) | Aura macro | EDSL syntax (eda:module, eda:port, etc.) |
| `define-struct` (std/struct) | Aura macro | IR data types (eda:module, eda:port, eda:wire) |

The proposal in the issue body is exactly right: "primarily
in the standard library (`std/eda`), with only minimal
language/runtime support if needed". No C++ work is required
for #182 — it's a 100% stdlib feature.

## 3 design options (per the proposal)

### Option A (Recommended): Pure EDSL via Aura macros
- Upper layer: Aura macros expand `eda:module` / `eda:port`
  / `eda:assign` / `eda:always` into a structured IR
- Lower layer: IR is regular Aura data (vectors of records)
- All transforms (query, mutate, Verilog emit) operate on
  the IR via existing `query:*` and `mutate:*` primitives
- **Cleanest separation, most reusable, plays to Aura's
  strengths**

### Option B: Verilog-as-data only
- Hardware = structured records of Verilog strings
- No EDSL — agents write Verilog text and use string-level
  queries
- Simpler initial implementation but loses Aura's strengths
  (no compile-time type checking, no macros)

### Option C: Pure Verilog parser/emitter (no EDSL)
- Aura exposes a Verilog parser and emitter
- IR is hidden inside the parser/emitter (opaque to Aura)
- Aura becomes a Verilog IDE, not a hardware programming
  environment

## Recommended approach

**Option A** (per the issue body + comment). It's the proper
fit for Aura:

- EDSL layer: ergonomic for both humans and LLM agents
  (LLMs are very good at writing structured S-expressions)
- IR layer: queryable, mutable, optimizable using existing
  Aura primitives
- Verilog layer: round-trip via parser/emitter (Cycle 3-4)

## Migration plan (4 cycles, each shippable)

### Cycle 1: EDSL + minimal IR (2-3d)
- `lib/std/eda.aura` skeleton with `(require ...)` /
  `(export ...)`
- IR data types via `define-struct`:
  - `eda:module` (name, ports, body)
  - `eda:port` (name, direction, bit-width)
  - `eda:wire` (name, bit-width, driver)
  - `eda:assign` (lhs, rhs expression)
  - `eda:always` (sensitivity list, body)
  - `eda:signal` (name, bit-width, kind)
  - `eda:expr` (op, args) — recursive expression node
- EDSL macros (using `define-hygienic-macro`):
  - `eda:module name [ports] body...` → `make-eda:module`
  - `eda:port name direction width` → `make-eda:port`
  - `eda:wire name width` → `make-eda:wire`
  - `eda:assign lhs rhs` → `make-eda:assign`
  - `eda:expr op args...` → `make-eda:expr`
- EDSL examples (`tests/examples/eda/counter.aura`)
- Tests: `tests/test_issue_182.cpp` — exercises the EDSL
  via `--eval-current` end-to-end (parse EDSL → IR → display)
- **No clocking, no always blocks yet** (Phase 1 of issue body)

### Cycle 2: Query + mutate helpers (3-5d)
- `eda:query:ports ir` → list of eda:port
- `eda:query:wires ir` → list of eda:wire
- `eda:query:assigns ir` → list of eda:assign
- `eda:query:dependencies signal ir` → signals that drive
  this signal
- `eda:query:dependents signal ir` → signals driven by
  this signal
- `eda:query:bit-width signal ir` → bit-width (with
  constant-folding for simple cases)
- `eda:mutate:rename-symbol old new ir` → IR with renamed
  signal (uses `mutate:query-and-replace`)
- `eda:mutate:insert-clock-gating ir clk-domain en-signal`
  → IR with clock gating inserted (Phase 1 mutate)
- `eda:mutate:extract-common-expr ir` → IR with common
  sub-expressions extracted (basic, no control flow)
- Tests: `tests/test_issue_182.cpp` — round-trip EDSL
  + query + mutate (no Verilog yet)
- **The "verifies the wiring" cycle**: confirms that
  Aura's existing query/mutate primitives are expressive
  enough for hardware operations

### Cycle 3: Verilog Emitter (3-5d)
- `eda:emit-verilog ir` → Verilog string
- Format:
  - module header
  - port declarations
  - wire/reg declarations
  - assign statements
  - always blocks (with sensitivity list)
  - endmodule
- Tests:
  - Emit a counter, parse it back via Verilator (or manual
    inspection if Verilator not available)
  - Multiple IRs, check emitted Verilog is well-formed
- **The "hardware-readable output" cycle**

### Cycle 4: Verilog Parser (5-7d, biggest)
- `eda:parse-verilog string` → IR
- Subset (practical for first ship):
  - module / endmodule
  - input / output / inout
  - wire / reg (1D, no packed arrays)
  - assign (arithmetic + bitwise + comparison)
  - always @(sensitivity) blocks
  - if / else (no case in v1)
  - function (declarations only, no calls yet)
- Tokenizer + recursive-descent parser
- Error recovery: skip bad statements, continue
- Tests:
  - Parse `tests/fixtures/eda/counter.v` → IR
  - Round-trip: parse → emit → parse → compare IRs
  - Parse a real Verilog file (e.g., a small RISC-V core
    subset)
- **The "import existing codebases" cycle**

## Total effort

- Cycle 1: 2-3 days
- Cycle 2: 3-5 days
- Cycle 3: 3-5 days
- Cycle 4: 5-7 days
- Total: 2-3 weeks focused work

## What this does NOT include (deferred)

- **#228 — Dependent type system for Hardware IR**:
  separate issue, separate cycle (could be Cycle 5 of #182
  or a standalone effort)
- **Clock domain crossing analysis**: requires the type
  system (#228)
- **Synthesis (Yosys) integration**: requires the parser
  + type system
- **Verification (Verilator) integration**: requires
  stable parser + type system
- **Optimization passes (PPA)**: requires stable IR + type
  system

The first 4 cycles establish the FOUNDATION: EDSL + IR +
query/mutate + Verilog in/out. Everything else builds on
this.

## Test scenarios (test_issue_182.cpp)

- **Cycle 1**: EDSL macros expand correctly to IR; display
  IR; round-trip EDSL → IR
- **Cycle 2**: EDSL → IR → query → mutate → new IR is
  structurally correct
- **Cycle 3**: IR → Verilog emit is well-formed; multiple
  IRs produce well-formed output
- **Cycle 4**: Verilog → IR round-trips through parser +
  emitter; real Verilog files parse correctly

## Why design + 4 cycles (not one big PR)

Same rationale as #181: each cycle is a shippable,
reviewable, bisectable unit. The full PR (1 PR) would be
~3000 lines across EDSL + parser + emitter + tests.

The Verilog parser (Cycle 4) is the riskiest piece — it's
where the C++ stdlib has the most hand-written code (token
recognizers + recursive descent). Splitting it into its
own cycle lets us catch parser bugs in isolation before
integrating.

## Composes with

- #144 (Contracts): the EDA IR can use `contract_assert` for
  invariants (e.g., port directions match, bit-widths
  compatible)
- #147 (post-mutation invariant check): `eda:mutate:*`
  operations automatically get invariant checking
- #165 (hygienic-macro): EDSL uses `define-hygienic-macro`
  for proper hygiene
- #176 (pure extract): the EDA IR is pure data, easy to
  reason about
- #228 (Hardware IR type system): follows this work
- `lib/std/struct.aura` (`define-struct`): IR data types
  use this

## Immediate workaround (already shipped)

Hardware design today uses plain Verilog files, edited
manually or by text-based agents. The proposal in this
design doc gives them Aura-native structured hardware
modification as a first-class capability.

## Why this is a design doc (not implementation)

The work is 2-3 weeks focused effort, spanning:
- EDSL macro design (ergonomic for LLM agents)
- IR schema design (must support Verilog round-trip)
- Parser design (recursive-descent, error recovery)
- Emitter design (formatting, comments, line width)

The first cycle is small enough to do as a TDD prototype.
The other cycles (parser especially) benefit from
incremental development + per-cycle review.
