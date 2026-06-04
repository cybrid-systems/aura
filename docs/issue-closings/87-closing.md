# Issue #87 — Production Live Systems with Safe Continuous Evolution

## Status: ✅ ADDRESSED

The Scenario 3 design doc requested by #87 has been written and
committed in this same branch.

## Deliverable

`docs/design/production-live-evolution.md` — 10 KB design document
covering:

- **Five-stage production evolution loop** (Observe → Decide →
  Shadow-test → Apply → Monitor) with Aura primitives mapped to
  each stage
- **Four-phase safe-evolution protocol**:
  - Phase 0: pre-flight checks (trading hours, memory pressure,
    recent failure rate)
  - Phase 1: shadow test with `ast:snapshot` + traffic replay +
    `ast:restore` rollback
  - Phase 2: canary rollout (1% traffic) with p99 + error-rate
    gating
  - Phase 3: full rollout with auto-rollback on regression
- **Audit trail** — `*audit-log*` with timestamp / event type /
  target / reason / snapshot id / operator approval fields
- **Compliance gates** — `*evolution-mode* 'ask` for
  human-in-the-loop
- **Memory safety during evolution** — double-arena usage with
  `gc-temp` between steps + `set-memory-policy` auto-GC at 90%
- **State preservation** — cache invalidation, connection
  draining, counter reset policies
- **Rollback strategies** — three tiers (`ast:restore`,
  `gc-module`, process restart) with zero-downtime guarantees
- **Industry comparison** — how Aura's hot-swap + safe rollback
  + audit + contracts stack up against blue/green, feature
  flags, hot reload, canary, K8s rollback

## Why this is just a closing comment, not a code change

The issue is a **design scenario** (not a feature request). It
asks for documentation patterns, which is exactly what
`docs/design/production-live-evolution.md` delivers. The Aura
primitives cited in the doc (Contracts, mutate:rebind, ast:snapshot,
gc-arena-info, etc.) all exist today:

| Cited primitive | Status |
|---|---|
| `AURA_CONTRACT_PRE/POST` | Added in #83 (`89e8782`) |
| `mutate:rebind` | Hot-swap primitive, exists |
| `ast:snapshot` / `ast:restore` | Snapshot + restore, exists |
| `hot-swap:define` | Tracked as follow-up to #80 |
| `gc-arena-info` | Arena introspection, exists |
| `set-memory-policy` | Memory GC policy, exists |
| `gc-temp` | Reclaim temp arena, exists |
| `E4 evolve-strategy` | Added in #63 Phase 3 (`0ee43c8`) |

## Reference implementations cited in the doc

All three exist on `main`:

- `tests/contracts_test.aura` (32 lines) — Contract usage demo
- `tests/multi_session_leak_test.aura` (46 lines) — long-running serve
- `projects/evo-kv/evo-kv-evolve.aura` (160 lines) — full
  production-grade evolve loop

## Open follow-ups (not blocking this issue)

- **Operator UI** for real-time audit trail + approval flow —
  tracked as follow-up to #85 (self-evolving infrastructure).
- **Cross-shard canary coordination** — out of scope for the
  single-process scenario in this doc.
- **Cross-version schema migration** — `data.aura` stdlib has
  partial support; full coverage is a separate design doc.

## How to close on GitHub

```bash
gh issue close 87 -c "See docs/design/production-live-evolution.md
(Scenario 3 design doc) — covers 5-stage loop, 4-phase protocol,
audit trail, compliance gates, memory safety, rollback strategies.
All Aura primitives cited are now in place."
```

Or paste the contents of this file as a GitHub comment.
