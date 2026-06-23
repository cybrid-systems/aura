# Schema-Driven Pre-Validation for Mutate (Issue #288, Cycle 3)

## Background

Static reflection (Phases 1-3) shipped `schema<T>()` generation via P2996
consteval JSON Schema synthesis. `query:schema` exposes the schema for a
type. **Cycle 3** (`#288`) adds the missing safety link: a pre-mutation
validation primitive, plus an optional `validate:` flag on `mutate:rebind`,
so an AI Agent can mutate workspace code without introducing obvious
shape violations (out-of-range integer literals, empty bodies, unbalanced
parens).

## What ships

### `mutate:validate-against-schema` primitive

`(mutate:validate-against-schema <code-string> <type-name>)`

Returns:
- `#t` on valid code (no detected shape violation, or no schema registered for type)
- `#f` on missing args / non-string args / unknown type
- `"(schema-violation \"<reason>\" \"<field>\")"` on shape violation (string repr)

### `validate_code_against_schema_simple` helper

Best-effort shape check covering the initial P0 scope:
- **Empty body** → `"empty-body"` violation
- **Unbalanced parens** → `"unbalanced-parens"` violation
- **Integer literal overflow** (`int64_t` range) → `"integer-literal-overflow"`
- **Malformed integer literal** → `"malformed-integer-literal"`

Out of scope (follow-up):
- Type compatibility (the actual P2996 type-level checks)
- Range constraints beyond int64
- Dynamic values / function bodies

### `mutate:rebind` optional 4th `validate:` arg

`(mutate:rebind name code-string [summary] [validate: type-name])`

When the 4th arg is a string, `mutate:rebind` runs `mutate:validate-against-schema`
on the new code string **before** any workspace mutation. A violation:
- Sets `ok = false` (so `MutationBoundaryGuard` dtor rolls back)
- Returns the schema-violation string repr (or a structured error if the
  validate primitive returns `#f`)

Back-compat: the 4-arg signature is optional; existing 2-arg / 3-arg
callers continue to work unchanged.

## Usage example

```scheme
;; Pre-check before mutating
(case (mutate:validate-against-schema "(+ x 1)" "int")
  ((schema-violation reason field)
   (display "rejected: ") (display reason) (newline))
  (#t
   (mutate:rebind "f" "(lambda (x) (+ x 1))" "add" "int")))

;; Or with the inline flag — schema check + rebind in one call
(mutate:rebind "f" "(lambda (x) (* x 2))" "double" "int")
;; → #t on success
;; → "(schema-violation ...)" on shape violation, source unchanged
```

## Tests

`tests/test_issue_178_cycle3.cpp` — 9 ACs / 9 CHECKs:
- AC1-2: unknown type returns `#f` (no schema registered)
- AC3-4: unbalanced parens / int overflow produce non-success
- AC5-6: malformed / missing args return `#f`
- AC7: `mutate:rebind` with `validate:` succeeds for valid code
- AC8: `mutate:rebind` with `validate:` + empty body rejected, source unchanged
- AC9: `mutate:rebind` without `validate:` preserves existing behavior

## Regression

Normal + asan, 17 binaries, 390 CHECKs all green. gate: docs + lint +
fixtures all green.

## Follow-ups (not shipped — out of scope for the P0 ship)

1. **Full schema validation**: route through `schema<T>()` for type-level
   checks (P2996 reflection of struct fields, type compatibility, range
   constraints). The current `validate_code_against_schema_simple` is a
   best-effort string-level check; the real one needs the type registry
   + codegen integration.
2. **typed_mutate integration**: wire `mutate:validate-against-schema`
   into the typed_mutate path so workspace-level typed mutations
   inherit schema validation automatically (vs. requiring explicit
   `validate:` flag on every call site).
3. **MutationBoundaryGuard integration**: the `MutationBoundaryGuard`
   dtor currently rolls back on `ok=false`; the follow-up is to
   surface a structured `MutationResult.invariant_status` field that
   distinguishes "schema violation" from other rollback reasons
   (related to the `#147` invariant check pattern).
4. **Other mutate primitives**: `mutate:query-and-replace`,
   `mutate:replace-type`, `mutate:replace-value`, `mutate:set-body`,
   `mutate:splice` — these don't yet have the `validate:` flag. Adding
   the flag is mechanical (same pattern as the rebind integration).

Refs: #178 (Static Reflection Phase 4 design), #216 (Cycle 2 schema<T>),
#288 (Cycle 3 pre-validation).
