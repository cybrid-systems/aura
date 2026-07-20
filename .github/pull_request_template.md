## Summary

<!-- What changed and why (1–3 sentences). -->

## Primitives / surface governance (#1451 / #1453)

- [ ] No new public `*-stats` / convenience prefixes (or intentional `--update-baseline` + justification)
- [ ] Ran `(primitive:validate-new "…")` for any proposed new public name
- [ ] **Tests updated** under `tests/` (C++ issue test and/or `edsl_self_test.aura`) — required when touching `evaluator_primitives*` / evaluator core
- [ ] Observability via `(stats:get)` / `(engine:metrics)` only
- [ ] TUI / terminal paths marked **protected** if touched
- [ ] If TUI / `examples/{cyber_cat,snake,tetris}` / `lib/std/tui/*` changed: `./build.py test pets` (#1454)

## Architecture / module boundaries (#1885)

- [ ] **Updated `src/core/module_boundary.ixx` if applicable** (new layer edge, bridge, or cross-layer contract)
- [ ] Dependency direction still Core ← Parser ← Compiler ← upper (no Core → Compiler)
- [ ] Cross-layer stable refs use provenance / `StableNodeRefLike` (not raw NodeId alone)
- [ ] `docs/architecture.md` still matches the DAG if rules changed

## Naming & comments (#1886)

- [ ] New public APIs follow [`docs/naming_convention.md`](../docs/naming_convention.md) (Purpose / Pre / Post / Safety Class / Issue / AI-Native Rationale where applicable)
- [ ] Primitive names respect freeze prefixes; PrimMeta filled for Agent-visible adds

## Test plan

- [ ] `./build.py gate`
- [ ] Relevant `./build/test_issue_*` or `./build/aura < tests/edsl_self_test.aura`
- [ ] TUI: `./build.py test pets` when touching terminal / pets demos
- [ ] Linked issue(s): #

## Notes

<!-- Migration / demotion / follow-ups -->
