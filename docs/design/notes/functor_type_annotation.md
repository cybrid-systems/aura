# Functor Type Annotations (Issue #29, Gap #2)

**Status**: Design
**Author**: Ani

## Goal

Make `(Stack Int)` produce `Module{push: (List Int) Int -> (List Int)}` 
in typecheck-current instead of `Module{push: (__t0 __t1 -> Any)}`.

## Approach: Functional Type Annotations on Lambda Params

Syntax: `(lambda ((: x Int)) body)` — already parsed correctly by the parser.

The gap: parser creates the TypeAnnotation node but **loses it** — only the 
bare SymId is stored in `v.params`. The annotation node is orphaned.

## Implementation Plan

### Phase 1: Store param annotations in FlatAST

Add parallel vectors to store annotation NodeIds for each param:

- `param_annot_data_` — flattened NodeId array (annotation per param, NULL_NODE = none)
- `param_annot_begin_` / `param_annot_count_` — per-node indexing

Change `add_lambda` to accept optional annotations span.

### Phase 2: Type checker uses annotations

In `synthesize_flat_lambda`:
- Look up `flat.param_annotation(id, pi)` for each param
- If present, use the TypeAnnotation to determine the param type
- The annotation's `sym_id` gives the type name (e.g., "Int", "T")
- For compound types like `(List T)`, need recursive resolution

### Phase 3: Functor type param resolution

When the annotation references a functor type param (`T`):
- `T` is already bound in `env_` as a type var (from define-module handling)
- `reg_.lookup_type("T")` won't find it (T is not a registered type)
- Need to check `env_.lookup("T")` for the type var

### Phase 4: Test

- `typecheck-current` on `(Stack Int)` shows concrete types
- Functor tests still pass
- Param annotations in non-functor lambdas still work

## Risk

- Parser change may affect all lambda parsing
- Adding FlatAST vectors increases memory per node
- `(List T)` compound annotation requires recursive resolution
