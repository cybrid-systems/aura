# synthesize:define JSON structure parse (#1715)

**Issue:** [#1715](https://github.com/cybrid-systems/aura/issues/1715)  
**Files:** `evaluator_primitives_agent.cpp`, `evaluator_primitives_json.cpp`  
**Status:** P1 correctness — hand-rolled LLM response scanner.

## Problem

`synthesize:define` scanned the HTTP body with `find("content")` and a
~30-line escape loop. Fragile to pretty-print, wrong `"content"` keys,
embedded quotes, `\uXXXX`, unbounded size, and `null`.

## Fix

1. **Structure walk** via existing `json-parse` + `hash-ref`:
   `choices → car → message → content`.
2. **Cap** extracted code at 256 KiB.
3. **`json-parse` `\uXXXX`** → UTF-8 (BMP) for non-ASCII identifiers.

No new dependency (repo has no nlohmann; Aura's `json-parse` is the
structured parser).

## Tests

`tests/test_synthesize_json_parse_1715.cpp`
