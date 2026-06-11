# Design: Issue #73 — Type info into IR / JIT pipeline

## Evidence (what's actually broken)

I read `src/compiler/lowering.ixx`, `src/compiler/ir.ixx`, `src/compiler/pass_manager.ixx`,
`src/compiler/ir_executor_impl.cpp`, `src/compiler/cache.ixx`, `src/compiler/cache_impl.cpp`,
`src/compiler/ir_reflect_serialize.cpp`, `src/compiler/aura_jit.cpp`,
`src/compiler/aura_jit_runtime.cpp`, `src/compiler/service.ixx`, `src/main.cpp`.
The pipeline intends to flow `TypeId` from the type-checker all the way
through to the JIT, but in practice the data is dropped at multiple points.

### Bug A — `cache_impl.cpp` never writes `type_ids_`

`src/compiler/cache.ixx:71` declares `const NodeId* type_ids_ = nullptr;` as a
column pointer. The reader layout (`src/compiler/cache_impl.cpp:248-249`)
allocates space for it. But the writer (`cache_impl.cpp:71-92`) only writes
`tags`, `int_vals`, `sym_ids`, `lines`, `cols`, `markers` — **no `type_ids`
column is written**.

Result: when a cached AST is loaded via `open_cache`, the `type_ids_`
pointer is never set (no place in the read path either, since the write
side is a no-op), so `flat.type_id(id)` returns 0 for cached nodes. The
`AuraType` cache format intends type info, but the runtime never
sees it.

### Bug B — `eval_ir()` does not run typecheck before lowering

`src/compiler/service.ixx:902-1010` is the `eval_ir` method. It does:
1. `parse_to_flat(...)` → builds the flat
2. `macro_expand_all(...)` → expand macros
3. `validate_ast(...)` → structural validation
4. `register_adt_from_define_types(...)` → register ADT ctors
5. `lower_to_ir_with_cache(...)` → **lower to IR**

There is **no `tc_pass.check_before_lowering(...)` call** between step 4
and step 5. So `flat.set_type(id, ...)` is never called for any node,
and `lower_to_ir` reads `flat.type_id(current_source_id)` which returns
0 for every node.

Compare to `eval()` at `src/compiler/service.ixx:661-678` which DOES call
`tc_pass.check_before_lowering(...)` before the IR pipeline. The two
methods diverge here.

**Verified empirically**: with a temporary `std::print` in
`LoweringState::emit`, every emit shows `tid=0` for the IR path. The
`--inspect typecheck` mode shows `Int (type_id=1)` for literal nodes, but
`--inspect ir` shows `type_id: 0` for the IR instructions that should
carry them.

### Bug C — `aura_jit.cpp` has no typecheck

1647 lines of JIT code (`src/compiler/aura_jit.cpp`) plus 695 lines of
runtime support (`aura_jit_runtime.cpp`) plus 365 lines of bridge
(`aura_jit_bridge.cpp`). None of them call `infer_flat` or
`check_before_lowering`. The JIT path is type-blind.

### Bug D — `TypeSpecializationWrap` early-exits on missing registry

`src/compiler/pass_manager.ixx:243-244`:
```cpp
void run(aura::ir::IRModule& module) {
    if (!type_reg_) return;
    ...
}
```

If the `TypeRegistry` is not passed in, the pass becomes a no-op. Five
call sites in `service.ixx` pass `&type_registry_`, but the default
construction path (and any future caller) silently loses all
type-specialization optimizations.

### Bug E — `IRInterpreter::check_runtime_type` is strict-mode only

`src/compiler/ir_executor_impl.cpp:928`:
```cpp
if (strict_mode_ && instr.type_id != 0) {
    auto rv = check_runtime_type(instr.type_id, ...);
}
```

Even when `instr.type_id` is correctly populated (post-Bug-B fix), the
runtime type check is gated on `strict_mode_` (default false). So a
`(+ 1 "hello")` that gets past the static typecheck (e.g. because
`+` is poly-typed `(Any, Any) -> Any`) won't be caught at runtime
unless strict mode is enabled.

### Bug F — `inspect_ir_json` outputs `type_id: 0` for every instruction

Downstream of Bug B. Confirmed:
```
{"opcode":1,"operands":[1,1,0,0],"source_ast_node_id":0,"type_id":0}
```

The serialization is correct (uses reflection over `IRInstruction`,
which has the `type_id` field); the data just isn't there to serialize.

### Bug G — `FlatAST::NodeView` doesn't expose `type_id`

`src/core/ast.ixx:257-275` defines `NodeView` without a `type_id` field.
The SoA storage has it (`type_id_`), and the accessor exists
(`flat.type_id(id)`), but `flat.get(id)` returns a `NodeView` that
omits it. This forces callers to remember to look it up separately.
Minor, but contributes to type info being silently dropped at API
boundaries (e.g. when caching or serializing a `NodeView`).

## The fix

### Phase 1: typecheck in `eval_ir` (Bug B + Bug F)

`src/compiler/service.ixx:eval_ir()`:
- Add the same `tc_pass.check_before_lowering(...)` block that `eval()`
  has, between `register_adt_from_define_types(...)` and the
  `lower_to_ir_with_cache(...)` call.
- Wire `strict_mode_` through so the type-error bail-out triggers
  here too.

This is the biggest one-line fix — once the FlatAST has type_ids,
everything downstream works (lowering reads them, the IR carries
them, the runtime can check them, the JSON shows them).

### Phase 2: populate `type_ids_` column in cache (Bug A + Bug G)

`src/compiler/cache_impl.cpp`:
- Add `std::vector<std::uint32_t> type_ids(n, 0);` to the writer's
  column arrays.
- Populate `type_ids[id] = flat.type_id(id);` in the per-node loop.
- Write the column: `f.write((const char*)type_ids.data(), n * 4);`
- Add `[11]: type_ids (n * 4)` to the column-layout comment.
- In the reader: `cache.type_ids_ = reinterpret_cast<const std::uint32_t*>(nd + next_pad(n * 4));`

`src/core/ast.ixx`:
- Add `std::uint32_t type_id` field to `NodeView` (read-only view).
- `flat.get(id)` populates it from the SoA column.
- Callers that currently do `flat.type_id(id)` separately can read it
  from the view, but the accessor stays for callers that have only a
  `NodeId`.

### Phase 3: default type registry for `TypeSpecializationWrap` (Bug D)

`src/compiler/pass_manager.ixx:TypeSpecializationWrap`:
- Default `type_reg_` to a non-null sentinel (e.g. lazy-init to
  `aura::core::TypeRegistry{}` member) so the pass always runs.
- When no registry is provided, log a warning and continue with the
  pass as a no-op (better than silently losing specialization).

### Phase 4: make runtime type check a non-strict default (Bug E)

`src/compiler/ir_executor_impl.cpp:928`:
- Drop the `strict_mode_` gate. Always run the runtime type check
  when `instr.type_id != 0` and `type_registry_` is set.
- Keep `strict_mode_` as a way to turn OFF the check (escape hatch
  for the gradual path), not the default.

This catches real type mismatches like `(+ 1 "hello")` at runtime,
giving users a hard error instead of a silent coercion or a `0`.

### Phase 5: typecheck in JIT path (Bug C)

`src/compiler/aura_jit.cpp` and `aura_jit_runtime.cpp`:
- Before the JIT compiles a function, call `tc_pass.check_before_lowering`
  on the function's body.
- Plumb the resulting `FlatAST` with type_ids into the JIT
  (currently the JIT may re-lower; need to verify).
- If the JIT is type-blind, at minimum it should refuse to compile
  functions whose static typecheck found a TypeError.

### Phase 6: `inspect_ir_json` should not silently drop type info (Bug F)

Once Phase 1 lands, the JSON will naturally show `type_id: 1` etc.
No additional change needed beyond the lowering propagation.

## Backward compat

- `eval_ir` adding a typecheck is a behavior change: previously the
  IR-direct path didn't typecheck at all, so real type errors went
  silently to the IR which has no type info, then to the IR
  interpreter which has no type check. Now they get caught at
  the static level. Existing code that was working under that
  blindness will continue to work; code that was silently broken
  will now get a hard error.
- `TypeSpecializationWrap` default-registry change: if the pass
  was a no-op before, it's a no-op after (when no registry). The
  only behavior change is when a registry IS available — the pass
  now actually runs.
- `IRInterpreter::check_runtime_type` non-strict by default: any
  existing code that relied on silent type coercion (e.g. `(+ 1 "x")`
  producing `1` via gradual cast) will now error. The strict_mode
  escape hatch preserves the old behavior.

## Test plan

- `tests/test_ir.cpp`: add a "TypeId flow into IR" section that:
  1. Parses `(+ 1 2)`, runs `check_before_lowering`, lowers, and
     asserts every IR instruction has the correct `type_id`.
  2. Parses a polymorphic function call, asserts that type vars
     survive lowering.
  3. Parses a type-mismatched call, asserts that the static check
     fires (Bug E reverse — non-strict, no coercion, error reported).
- `tests/test_regression.py`: add cases that go through `eval_ir`
  (e.g. `--inspect ir`) and assert the JSON contains `type_id` set.
- Smoke: `./build/aura --inspect ir < '(+ 1 2)'` should now show
  `type_id: 1` (Int) for the const instructions, not 0.

## Affected files

- `src/compiler/service.ixx` — eval_ir typecheck (Phase 1)
- `src/compiler/cache_impl.cpp` — type_ids_ write (Phase 2)
- `src/core/ast.ixx` — NodeView.type_id (Phase 2)
- `src/compiler/pass_manager.ixx` — TypeSpecializationWrap default (Phase 3)
- `src/compiler/ir_executor_impl.cpp` — runtime type check default (Phase 4)
- `src/compiler/aura_jit.cpp` — JIT typecheck (Phase 5)
- `src/compiler/aura_jit_runtime.cpp` — JIT runtime type check (Phase 5)
- `tests/test_ir.cpp` — new test section
- `tests/test_regression.py` — new cases
