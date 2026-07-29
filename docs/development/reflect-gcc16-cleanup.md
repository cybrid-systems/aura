# Reflection GCC 16.1 cleanup (Issue #2289)

## Background

`src/reflect/` (especially `opcode_reflect.hh` and parts of `reflect.hh`)
carried workarounds written against early GCC 16 development snapshots
(16.0.x / pre-16.1). With a real **GCC 16.1.0** toolchain, those constraints
were re-probed and the invasive workarounds removed where safe.

## Removed workarounds (no longer required on 16.1.0)

| Historical constraint | Status on GCC 16.1.0 |
| --- | --- |
| Prefer `.data()[i]` over `operator[]` on meta ranges | **Fixed** — `operator[]` works for `enumerators_of`, `nonstatic_data_members_of`, and `template_arguments_of` |
| Fixed-size 256 name tables | **Fixed** — use `enum_count<E>()` / exact `std::array` size |
| Avoid `extract<E>(enumerator)` (throws) | **Fixed** for sequential enums used by Aura (`IROpcode`-style) |
| Index-only name tables | Prefer dense-by-value tables via `extract` |

## Residual limitations (still real)

1. **No constexpr-stored `std::vector<meta::info>`** — operator new is not
   usable as a constexpr static. Keep local consteval ranges and copy into
   `std::array` when a durable compile-time view is needed.
2. **`template_of` / `is_same_type` for `std::array` / `std::vector` /
   `std::span`** — still unreliable; `display_string_of` remains the detection
   path in `classify_type` (`reflect.hh`).
3. **Prefer `string_view` over `std::string` in consteval tables** — SSO /
   allocation edge cases remain fragile; name tables return `string_view`.
4. **Module + reflection ICE** — importing some Aura modules in the same TU as
   `-freflection` can still ICE; split TUs (see `reflect.ixx` / issue #178
   bridge) until a later GCC fix.

## Files touched

- `src/reflect/opcode_reflect.hh` — natural P2996 access, exact-size tables
- `src/reflect/reflect.hh` — `args[i]` instead of `args.data()[i]`; comments
- `src/reflect/read_auto_validate.hh` — `members[i]` instead of `.data()[i]`
- `tests/reflect/test_opcode_reflect_2289.cpp` — regression coverage

## Verification

```bash
cmake -B build -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc
cmake --build build --target aura-reflect test_opcode_reflect_2289 -j
ctest --test-dir build -R 'reflect|opcode_reflect_2289' --output-on-failure
```

Probed with: `g++ (GCC) 16.1.0`, `-std=c++26 -freflection`.
