## Summary

<!-- What changed and why (1–3 sentences). -->

## Primitives / surface governance (#1451 / #1453)

- [ ] No new public `*-stats` / convenience prefixes (or intentional `--update-baseline` + justification)
- [ ] Ran `(primitive:validate-new "…")` for any proposed new public name
- [ ] **Tests updated** by appending a case to the theme home (`python3 tests/run.py home "<query>"`, [`tests/HOMES.md`](../tests/HOMES.md)) — required when touching `evaluator_primitives*` / evaluator core. Do not add a new issue-numbered filename.
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

## Test strategy (#1887)

- [ ] Hot-path / AI self-mod changes reference the matrix in [`tests/STRATEGY.md`](../tests/STRATEGY.md) (stamp `note_strategy_scenario` or extend a theme suite)
- [ ] Append to the theme home. Do not add a new issue-numbered test file ([`tests/HOMES.md`](../tests/HOMES.md))

## Test plan

- [ ] `./build.py gate`
- [ ] The home from `python3 tests/run.py home "<query>"`. Process-level checks go in the Python theme files. Do not add a new `test_issue_*` binary or a new `edsl_self_test.aura` case file.
- [ ] TUI: `./build.py test pets` when touching terminal / pets demos
- [ ] Linked issue(s): #

## Notes

<!-- Migration / demotion / follow-ups -->
