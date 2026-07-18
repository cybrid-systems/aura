# Evolve-strategy analytics parse observability (#1724)

**Issue:** [#1724](https://github.com/cybrid-systems/aura/issues/1724)  
**Sibling:** [#1723](https://github.com/cybrid-systems/aura/issues/1723), [#1669](https://github.com/cybrid-systems/aura/issues/1669)  
**Files:** `evaluator_primitives_agent.cpp`, `observability_metrics.h`  
**Status:** P2 correctness — silent `catch (...)` on `std::stod`/`std::stoi`.

## Problem

`evolve-strategy` parsed `intend-analytics` s-expressions with
`std::stod` / `std::stoi` under bare `catch (...)`, so malformed
fields (or truncated tokens from pre-#1723 nested-paren bugs) left
`success_rate` / `avg_attempts` at defaults with no metric or log.

## Fix (Option A)

1. Narrow catch to `const std::exception&`.
2. Bump `agent_evolve_analytics_parse_failures` on failure.
3. Optional `AURA_DEBUG_INTEND` stderr log; keep silent defaults in prod.
4. Same metric for top-errors `stoi` failures.

## Tests

`tests/test_evolve_analytics_parse_1724.cpp`
