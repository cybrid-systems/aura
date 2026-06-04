# Issue #90 — Research Platforms for Self-Improving Code Systems

## Status: ✅ ADDRESSED

The Scenario 6 design doc requested by #90 has been written and
committed in this same branch.

## Deliverable

`docs/design/research-self-improving-systems.md` — 14.3 KB design
document covering:

- **The research loop in-language** — hypothesis → E4 strategy →
  experiment design → CaaS run → statistical analysis → publish /
  refine (replaces Python + JSONL pipeline)
- **Sub-area 1: Program synthesis research** — 4 concrete
  research questions (Q1-Q4) mapped to existing EDSL
  primitives; full experiment code in 30 lines
- **Sub-area 2: Automated theorem proving** — contracts as
  specs, CaaS as proof-search runtime, SMT integration
- **Sub-area 3: Code repair / self-repair** — auto-repair loop
  with `pid:analyze` + `synthesize:define` + contract verify +
  hot-swap, 3 research questions
- **Sub-area 4: Self-improving systems (meta-circular)** — the
  system proposes its own synthesis strategy, shadow-tests it,
  hot-swaps if better. 5 deep research questions on
  convergence, sample efficiency, safety, generalization,
  self-reference limits
- **Pattern: Experiment reproducibility** — `ast:snapshot` +
  seeded RNG + corpus version + env fingerprint; `rerun-experiment`
  one-call re-run
- **Pattern: Corpus management** — versioned, stratified,
  holdout-reserved, regression-tested
- **Pattern: Statistical analysis** — `std/stats` + FFI to
  scipy, Welch t-test + Cohen's d, JSON export for publication
- **Industry comparison** — Codex, AlphaCode, LEAN/Coq, AutoML,
  Haskell/Idris; only Aura has the full in-language research loop

## Why this is a closing comment, not a code change

The issue is a **design scenario** (not a feature request). It
asks for documentation patterns. All cited primitives are in
place:

| Cited primitive | Status / Doc |
|---|---|
| `synthesize:define` | EDSL primitive, exists |
| `synthesize:optimize` | EDSL primitive, exists |
| `intend` | `intent_orchestration.md` |
| `evolve-strategy` (E4) | Added in #63 Phase 3 (`0ee43c8`) |
| `pid:analyze` | Failure diagnosis, exists |
| `AURA_CONTRACT` | Added in #83 (`89e8782`) |
| `CaaS` | CaaS primitive, exists |
| `mutate:rebind` | Hot-swap, exists |
| `ast:snapshot` / `ast:restore` | Snapshot + restore, exists |
| 145-task EDSL benchmark | `tests/tasks/edsl/` |

## How to close on GitHub

```bash
gh issue close 90 -c "See docs/design/research-self-improving-systems.md
(Scenario 6 design doc) — 4 sub-areas (synthesis, theorem
proving, repair, meta-circular self-improvement), 15 research
questions, reproducibility patterns, industry comparison. All
cited primitives are in place on main."
```

Or paste this file as a GitHub comment.
