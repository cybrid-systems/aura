## Status — pattern catalog 落地, 设计 docs 互链

Issue #86 asked for design patterns for AI agents that evolve
their own logic at the code level. Done:

### ✅ Added

- **`docs/design/autonomous-self-evolving-agents.md`** (new, 177 lines):
  - **Capability ladder (levels 1-7)**: prompt → tool-calling → strategy
    swap → strategy evolution → self-discovered capabilities
  - **4 patterns** with Aura-level code skeletons:
    1. Self-tuning tool selection (`try-tool` + `evolve-dispatcher`)
    2. Self-repairing workflows (pipeline + `ast:snapshot` + `evolve-strategy`)
    3. Capability discovery (`query:find` + `query:calls` to find gaps)
    4. Multi-agent evolution with safety (per-session isolation + contracts)
  - **Human-in-the-loop** modes: `'autonomous` / `'ask` / `'dry-run`
  - **Comparison table** vs LangChain / AutoGPT / CrewAI (all ❌ code-level)
  - **4 open questions** (meta-evolution policy, cross-agent sharing,
    evolution budget, operator UI)

- **`docs/design/e4_evolvable_strategies.md`**: appended a "Related"
  section linking to the new agents doc.

- **`docs/design/agent_orchestration.md`**: appended a "Related"
  section linking to the new agents doc.

### ❌ Still open (per the issue's broader spirit)

- **Meta-evolution policy**: when should an agent evolve its own
  `*evolution-mode*`? Tracked in the design doc's open questions.
- **Cross-agent capability sharing** mechanism.
- **Evolution budget** enforcement (rate-limit mutations per agent).
- **Operator UI** for real-time evolution approval.

Closing this issue as **design documentation complete**;
implementation-level follow-ups are tracked in the design doc's
"Open questions" section.
