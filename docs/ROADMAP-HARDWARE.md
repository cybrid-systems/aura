# Aura → Agentic EDA Roadmap (Issue #304)

> **Traction direction**: Evolve Aura into the foundational AI-native
> self-evolving infrastructure for **Agentic EDA** and **autonomous
> hardware design**. This document is the **canonical** reference for
> the roadmap; see [Issue #304](https://github.com/cybrid/aura/issues/304)
> for the full discussion context.
>
> **Scope**: A long-term guiding document, NOT a shipping task. The
> concrete shipping work lives in child issues filed per Phase 0/1/2
> milestone. Treat this file as the index.

---

## Why this exists

Aura's core innovations — **auto-mutating ASTs**, **static reflection**,
**precise mutate primitives with snapshots/rollback**, **code-as-memory**,
**Hardware IR + Verilog backend**, and **intend-driven self-evolving
closed loops** — directly address the missing pieces for true
L4+ to L5 autonomy in chip design that commercial EDA vendors
(Cadence ChipStack, Synopsys AgentEngineer, Siemens Fuse) are
conservative on.

This roadmap is the **call-to-action** to turn Aura from an ambitious
research language into production-viable infrastructure for
agent-driven EDA workflows.

---

## Phased roadmap (summary)

### Phase 0: Foundation & Stabilization (Q3–Q4 2026) — **Immediate traction**

**Goal:** Make Aura reliable enough for serious experimentation in
hardware-adjacent tasks.

- Complete and harden **Hardware IR + Verilog Backend** (Cycle 2+)
- Stabilize core mutate/query/eval primitives
- Improve `intend` closed-loop reliability
- Add basic Verilog-aware query/mutate helpers
  (`query:module-instances`, `mutate:parameterize`)
- Documentation: "Agentic Hardware Design with Aura" tutorial

**Success metric**: A non-trivial RTL module (FIFO, simple ALU, or
small FSM) can be loaded → queried → mutated → verified in <10
iterations with >95% semantic preservation.

### Phase 1: EDA Integration Prototypes (Q1–Q2 2027)

**Goal:** Demonstrate clear value when combined with existing EDA tools.

- Reference integrations: open-source EDA (Yosys, Icarus, Verilator)
- Design-space exploration primitives: `synthesize:explore`
- Persistent memory layer on top of AST
- Safety & governance: mutation policies, approval hooks, diff
  visualization, formal property preservation checks

**Success metric**: End-to-end demo where an agent starts from spec,
generates initial RTL, runs 5–10 autonomous mutation+verification
iterations, produces measurably better result than baseline.

### Phase 2: Self-Evolving Workflows & Production Readiness (H2 2027 – 2028)

**Goal:** Move from prototypes to reusable, production-viable patterns.

- Advanced self-evolution patterns catalog
- Multi-agent collaboration for hardware (micro-architecture +
  verification strategy negotiation via Aura code)
- Performance & scale: 10k–100k+ line designs, agent loop latency
- Integration with governance runtimes
- Benchmark suite: Aura-driven evolution vs. pure LLM agent vs.
  traditional script-based flows
- Community & ecosystem: Starter kits for "Aura + [Open EDA /
  Cadence / Synopsys / Siemens]" hybrid agents

**Success metric**: At least one real (or realistic academic/industry)
use case where Aura-powered agents deliver >3–5x productivity or
quality improvement, with documented reproducibility.

---

## Key technical work items (prioritized)

1. Hardware IR / Verilog Backend completion & verification (highest)
2. Verilog/HDL-aware query & mutation library
3. `intend` / closed-loop robustness & strategy learning
4. Workspace + versioning enhancements for design exploration
5. Safety layer (policies, property preservation, rollback)
6. Documentation, examples, benchmark harness for EDA
7. Optional: Python/C++ bindings / MCP-style protocol for
   LangGraph / AutoGen / CrewAI integration

---

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| Reliability in safety-critical hardware | Strong snapshot/rollback, type system, property checking, human approval gates in early phases |
| Performance at scale | Incremental compilation + targeted mutation (not full re-analysis) |
| Adoption | Clear demos that show complementarity (not replacement) of existing EDA tools |
| Single-contributor risk | Use this roadmap to attract contributors; well-scoped issues with onboarding docs |

---

## Success metrics (overall)

- Verilog backend reaches production-usable quality for agent experiments
- ≥3 high-quality self-evolving EDA/hardware demos published
- Measurable community growth (stars, contributors, external forks)
- Clear integration stories / references from agentic EDA discussions

---

## Cross-references

- **Source issue**: [#304](https://github.com/cybrid/aura/issues/304) —
  Grok (xAI) Industry Insight Analyst proposal (2026-06-28)
- **Related work**:
  - [feat #182] Hardware IR + Verilog Backend (Cycle 1 partial)
  - [#85] Self-evolving infrastructure efforts
  - [projects/self-evolving-infrastructure/](https://github.com/cybrid/aura-projects)
  - Tutorial examples of `mutate` + `intend` closed loops
- **Related decision docs** (this repo):
  - [primitive-vs-stdlib-decision-framework.md](design/primitive-vs-stdlib-decision-framework.md)
  - [stdlib-organization-spec.md](design/stdlib-organization-spec.md)
  - [primitives-demotion-batch1.md](design/primitives-demotion-batch1.md)

---

## Status

| Phase | Status | Tracking issue |
|---|---|---|
| Phase 0 (Foundation) | TBD | child issue pending |
| Phase 1 (EDA Integration) | TBD | child issue pending |
| Phase 2 (Production) | TBD | child issue pending |

When each phase starts, file a child issue referencing this doc + the
relevant Phase X section, then update the Status row.

---

_Last updated: 2026-06-28 (Issue #304 acceptance). This is a
**living document** — please submit PRs to update the priorities
as the landscape evolves._
