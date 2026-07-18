# strategies_mtx_ for strategy primitives (#1722)

**Issue:** [#1722](https://github.com/cybrid-systems/aura/issues/1722)  
**Sibling:** [#1720](https://github.com/cybrid-systems/aura/issues/1720)  
**Files:** `evaluator.ixx`, `evaluator_primitives_agent.cpp`  
**Status:** P2 race — strategies_ R/W without lock (covered by #1720 ship).

## Coverage

`strategies_mtx_` (landed with #1720) guards:

| Primitive | Lock |
|-----------|------|
| define-strategy | unique |
| register-strategy! | unique |
| strategy-set-field! | unique |
| strategy-field | shared |
| strategy-inspect | shared |
| evolve-strategy | shared (copy) + unique (insert) |
| strategy:set-strategy / strategy:active | unique/shared on `active_strategy_` |

## Tests

`tests/test_strategies_mtx_1722.cpp`
