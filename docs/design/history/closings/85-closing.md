## Status — broader infra design doc 落地,evo-kv 文档更新

Issue #85 asked for design documentation on self-evolving
infrastructure components. Done:

### ✅ Added

- **`docs/design/self-evolving-infrastructure.md`** (new, 163 lines):
  4 patterns for assembling Aura primitives into self-evolving
  infrastructure:
  - Pattern 1: Metrics → Trigger → Evolve → Verify
  - Pattern 2: Layered evolution (different timescales per layer)
  - Pattern 3: Safe hot-swap in flight (no half-state guarantee)
  - Pattern 4: Memory budget adaptation
  Plus a comparison table vs Redis / custom rewrite and a list
  of open questions (operator UI, multi-component contention, etc).

- **`projects/evo-kv/DESIGN.md`**: appended a "Related Documentation"
  section linking to the new design doc + double-arena.md +
  e4_evolvable_strategies.md + benchmark.md.

- **`projects/evo-kv/ROADMAP.md`**: appended a "Related: Broader
  Self-Evolving Infrastructure" note pointing readers to the new
  pattern catalog.

### ❌ Still open (per the issue's broader spirit)

- **Operator UI**: how does a human operator approve a candidate
  evolution? Currently: read `*evo-log*` and either let it
  proceed or restore. A `dashboard.aura` UI is a follow-up.

- **Multi-component evolution**: a KV + cache + queue all
  evolving concurrently — cross-component memory and CPU contention
  is an open problem.

- **Meta-evolution**: when the evolve-strategy itself needs to
  change. E4's `parent` field tracks lineage but the
  meta-strategy isn't designed.

Closing this issue as **documentation complete**; the
implementation-level follow-ups (operator UI, multi-component
contention, meta-evolution) are tracked in the design doc's
"Open questions" section.
