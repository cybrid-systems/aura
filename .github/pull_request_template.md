## Summary

<!-- What changed and why (1–3 sentences). -->

## Primitives / surface governance (#1451 / #1453)

- [ ] No new public `*-stats` / convenience prefixes (or intentional `--update-baseline` + justification)
- [ ] Ran `(primitive:validate-new "…")` for any proposed new public name
- [ ] **Tests updated** under `tests/` (C++ issue test and/or `edsl_self_test.aura`) — required when touching `evaluator_primitives*` / evaluator core
- [ ] Observability via `(stats:get)` / `(engine:metrics)` only
- [ ] TUI / terminal paths marked **protected** if touched

## Test plan

- [ ] `./build.py gate`
- [ ] Relevant `./build/test_issue_*` or `./build/aura < tests/edsl_self_test.aura`
- [ ] Linked issue(s): #

## Notes

<!-- Migration / demotion / follow-ups -->
