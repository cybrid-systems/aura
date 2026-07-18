# Evolve-strategy top-errors stoi observability (#1725)

**Issue:** [#1725](https://github.com/cybrid-systems/aura/issues/1725)  
**Sibling:** [#1724](https://github.com/cybrid-systems/aura/issues/1724), [#1723](https://github.com/cybrid-systems/aura/issues/1723)  
**Files:** `evaluator_primitives_agent.cpp`, `observability_metrics.h`  
**Status:** P2 correctness — silent `catch (...)` on top-errors `std::stoi`.

## Coverage

Landed with #1724:

1. Narrow catch to `const std::exception&` (no bare `catch (...)`).
2. Bump shared `agent_evolve_analytics_parse_failures`.
3. Parse top-errors list interior only (avoids label false-positives).

This issue locks the AC on the **top-errors `stoi` path** specifically
(malformed counts must bump the metric; valid counts still drive hints).

## Tests

`tests/test_top_errors_stoi_1725.cpp`
