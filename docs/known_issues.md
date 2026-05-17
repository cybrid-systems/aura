# Known Issues

Found during real-world testing with DeepSeek AI agent + Aura.

## Current Issues

### 1. `string->number` returns #f for non-numeric
- ✅ Fixed: now returns `#f` instead of `0`

### 2. `when`/`unless` missing
- ✅ Fixed: added as special forms in eval_flat and eval_data_as_code

### 3. Agent: early DONE before verification
- ✅ Fixed: DONE check moved after auto-tests, TRUNCATED detection added

### 4. Agent: max_tokens truncation
- ✅ Fixed: detects `finish_reason == "length"`, adds warning to response

### 5. LLM generates Clojure syntax
- 🟡 Prompt improvement helps but doesn't eliminate (MiniMax worse than DeepSeek)

### 6. `quote` with `(a . b)` dotted pairs
- ✅ Fixed: added `NodeTag::Pair`

## Previously Fixed

| Issue | Fix |
|-------|-----|
| Dotted pair parser infinite loop | NodeTag::Pair |
| letrec mutual recursion | define + set! desugar |
| parse_val missing Quote | Added TokenKind::Quote case |
| parse_let multi-body hang | Collect body exprs into begin |
| eval_data_as_code closure arena | Clone body to arena FlatAST |
| `display`/`newline` return void | Changed from `make_int(1)` |
| chain_cmp returns bool | make_bool instead of make_int |
| Pipe mode prints only last | Added void skip + 2>&1 |
