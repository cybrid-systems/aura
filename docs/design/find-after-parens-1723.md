# Paren-aware analytics field parse in evolve-strategy (#1723)

**Issue:** [#1723](https://github.com/cybrid-systems/aura/issues/1723)  
**Files:** `evaluator_primitives_agent.cpp`  
**Status:** P2 correctness — naive `find_after` truncated nested values.

## Problem

`evolve-strategy` parses `intend-analytics` s-expressions with a
`find_after` lambda that stopped at the first space or `)`. Nested
values (and a naive first-`)` match for `top-errors:(...)`) could
yield truncated tokens; `std::stod` then failed (see also #1724).

## Fix

1. `find_after`: track paren depth; end at top-level space or outer `)`.
2. `top-errors:(...)`: close the list by matching depth, not first `)`.

## Tests

`tests/test_find_after_parens_1723.cpp`
