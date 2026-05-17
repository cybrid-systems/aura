# Known Issues

Found during real-world testing with DeepSeek AI agent.

## Language Issues

### 1. `when` / `unless` not available
- `when` is standard Scheme but not in Aura
- Workaround: use `(if cond (begin ...) '())`
- Fix: add as primitive or macro

### 2. `string->number` returns 0 for non-numeric strings
- `(string->number "abc")` → `0` (silent)
- Should return `#f` or error
- Currently used for CSV parsing, makes it hard to detect numeric vs string columns

### 3. `min`/`max` with floats
- Tested: `(min 10.5 25.0)` → `10.5` ✅
- Need more edge case testing with mixed int/float

### 4. Agent `max_tokens` truncation
- LLM output gets cut off mid-code when generating complex programs
- Agent now detects `finish_reason == "length"` and appends warning
- Need: increase default or split code across multiple rounds

### 5. LLM says DONE too early
- When code compiles but is truncated/incomplete, LLM may say DONE
- Agent should verify all requested functions exist

## Infrastructure

### 6. `eval_data_as_code` lambda closure arena
- Fixed: closures now clone body to arena-allocated FlatAST
- Tests pass 106/106

## Fixed (previously documented)

### Dotted pair parser infinite loop
- Fixed: added NodeTag::Pair

### letrec mutual recursion
- Fixed: desugars to define + set!

### parse_val missing Quote
- Fixed: parse_val now handles TokenKind::Quote

### parse_let multi-body expression
- Fixed: collects all body exprs into begin
