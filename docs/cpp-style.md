# Aura C++ style (C++26 / C++ Core Guidelines)

Scope: `src/**` except third-party `src/linenoise/**`. Tracking issue: #4936.

Every rule carries one marker:

- **[hard]**: `scripts/clang_tidy_changed_lines.py` fails the gate on any finding on a changed line.
- **[soft]**: the same script reports it as a note; it becomes **[hard]** when its #4936 remediation batch lands.
- **[review]**: no reliable clang-tidy check; enforced in review.

Toolchain: GCC 16.2 builds Aura (`-fcontracts -freflection`). Clang 23.1 only runs clang-tidy; it has no
contracts, so `cmake/clang_contract_shim.h` expands `pre` / `post` / `contract_assert` to nothing.

## 0. Principles

1. **Zero overhead first.** An abstraction may not be slower than the hand-written C. Any change on a hot path
   (eval / lower / IR / JIT / GC / AST column access) passes the strict bench.
2. **Standard library only.** No third-party dependencies. LLVM appears only at the JIT boundary (`aura_jit*.cpp`).
3. **New code complies.** Existing code is fixed in the #4936 batches, not in drive-by rewrites.

## 1. Resources and ownership (R.*)

- R1 **[hard]** No bare `new` / `delete` / `malloc` / `free`. Ownership is a `std::unique_ptr`, a container or an
  arena (`cppcoreguidelines-owning-memory`, `cppcoreguidelines-no-malloc`). The vocabulary is in
  `src/core/ownership.hh`: `unique_file` / `open_file` / `close_file` for `FILE*`, `unique_c_ptr<T>` for memory a C
  API hands out (`realpath`, `getcwd`), `opaque_ptr` / `adopt_opaque<T>` for a pimpl member whose type the module
  interface cannot name, and `c_alloc` / `c_free` for the C heap shared with JIT code and C ABI callers (the only
  malloc-family calls in the tree). A pointer handed across a C ABI leaves a `unique_ptr` with `release()` and is
  adopted back into one on the other side. `gsl::owner<T*>` marks the rare raw owner (deleters).
- R2 **[review]** Process-lifetime singletons that fibers may touch after static destruction are
  `static aura::core::immortal<T> x;`, not a hand-written `static auto* p = new T`.
- R3 **[review]** A raw `T*` or `T&` never owns. Nullable non-owning parameters are `T*`, non-null ones `T&`.
- R4 **[soft]** Define all five special members or none; spell the rest `= default` / `= delete`
  (`cppcoreguidelines-special-member-functions`). An RAII guard is never copyable: delete its copy pair (the moves
  are then suppressed too), or, for an aggregate guard, inherit `aura::core::NoCopy` and brace-initialise the base
  first (`Guard g{{}, args...};`) so the guard stays an aggregate without a public data member. A move transfers
  the containers and load/stores atomics; it does not delegate to the copy constructor. The moved-from atomic keeps
  its value.
- R5 **[review]** Nodes and slots are held by strong ids (`NodeId`, `SymId`, ...). Never hold a raw pointer or index
  across compaction / densify (the #4928 class of bug).
- R6 **[hard]** No use after `std::move` (`bugprone-use-after-move`); no dangling `string_view` / `span`
  (`bugprone-dangling-handle`).

## 2. Type safety (Type.* / ES.*)

- T1 **[hard]** No C-style casts (`cppcoreguidelines-pro-type-cstyle-cast`, `modernize-avoid-c-style-cast`).
- T2 **[review]** `reinterpret_cast` only at the JIT/FFI boundary, serialized byte views and tagged-value encoding,
  wrapped in a named `inline` helper. Other byte reinterpretation uses `std::bit_cast`.
  Audited homes, not a license to add more casts:
  - `src/core/address_bits.hh` — `std::bit_cast` for pointer/integer identity, and
    `moving_pointer_slot` for a raw pointer the moving collector rewrites. A direct
    harness that writes through one pointer does **not** prove a Moving densify
    window; that integrated run stays open.
  - `src/reflect/reflect.hh` `serialize_detail` — trivially copyable bytes via
    `memcpy`, and `member_at` only when the member's dynamic type is that `T`.
    A `vector<T>` or `span<T>` member is visited as that type. Its on-disk
    prefix stays the byte count (`size() * sizeof(element)`), which is what
    the old `vector<char>` pun produced. Top-level `auto_serialize(vector<T>)`
    still prefixes the element count. Do not collapse those two layouts.
  Spellings pinned by an existing test stay written out (`#4144` `&fr.pool_`,
  `#3647` closure slots, `#3180` cover slots). FFI function pointers, integer
  cells, and densify interior slots in `ffi_hot_path` and `ffi_primitives`
  go through `src/compiler/ffi_addr.hh`. The same shapes still written with
  a raw cast in `aura_jit.cpp`, `aura_jit_runtime.cpp`, and `aura_jit_bridge.cpp`
  are still open, not exempt.
- T3 **[hard]** No `const_cast` (`cppcoreguidelines-pro-type-const-cast`). Existing
  sites are not exempt because they are numerous. A cast that drops const and then
  writes is still a defect unless the object is already mutable or a test pins the
  spelling (the `#4144` pool slot). Do not wrap those in a helper that hides the write.
- T4 **[hard]** Narrowing conversions are explicit (`bugprone-narrowing-conversions`): `static_cast` at the
  conversion, `static_cast<std::ptrdiff_t>` for iterator offsets, a typed literal (`std::uint64_t{0}`) in a
  `?:` whose other arm is a proxy or unsigned type. Size products use a `size_t` operand (`2uz * 1024 * 1024`,
  `bugprone-implicit-widening-of-multiplication-result`) **[hard]**.
- T5 **[hard]** `nullptr` and `using` (`modernize-use-nullptr`, `modernize-use-using`); `override` on overrides
  (`modernize-use-override`). New enums are `enum class` **[review]**.
- T6 **[hard]** No unchecked `std::optional` / `std::expected` access (`bugprone-unchecked-optional-access`).
- T7 **[soft]** Initialize every local (`cppcoreguidelines-init-variables`).

## 3. Functions and value semantics (F.* / Con.*)

- F1 **[review]** Prefer pure functions: inputs are parameters, output is the return value, marked `[[nodiscard]]`.
- F2 **[soft]** Parameter passing (`performance-unnecessary-value-param`):
  - read-only strings: `std::string_view`;
  - read-only contiguous sequences: `std::span<const T>`;
  - trivially copyable types of at most 16 bytes: by value;
  - other types: `const T&`;
  - sink parameters: by value, then `std::move`.
  - Forwarding references are forwarded (`cppcoreguidelines-missing-std-forward`).
- F3 **[review]** Locals and member functions are `const` by default.
- F4 **[review]** No new mutable globals. Put state in a context object (`Evaluator`, `TenantScope`, ...) or use
  `thread_local` with a comment saying why.
- F5 Errors:
  - recoverable errors return `std::expected<T, E>` (`Result<T>` / `AuraResult<T>`) **[review]**;
  - exceptions only at the fiber bridge and around std / LLVM calls that throw, converted on the spot; no exception
    escapes a `noexcept` function, destructor or `main` (`bugprone-exception-escape`) **[hard]**. Diagnostics
    printed from `noexcept` code use `aura::core::eprintln_nothrow` (`src/core/diag_print.hh`). `.clang-tidy`
    ignores `std::format_error`: every format string is a compile-time checked `std::format_string`, so do not
    add `std::vformat` / `std::runtime_format`;
  - no empty `catch` (`bugprone-empty-catch`) **[hard]**: an intentional swallow (destructor, `noexcept`
    teardown, best-effort probe) carries the `[SILENCE-PRIM-#N]` marker and says why;
  - parse numbers with `std::from_chars` (`aura::core::parse_leading_int<T>`), not `std::stoi` in a
    `try` **[review]**;
  - hot-path functions are `noexcept` **[review]**.
- F6 **[soft]** Do not let results depend on the iteration order of a pointer-keyed unordered container
  (`bugprone-nondeterministic-pointer-iteration-order`). If the loop body does not depend on the order (a
  sum, a set build, mark-all, any-of), iterate `std::views::keys` / `std::views::values` (or `insert_range`)
  and say so in a comment.

## 4. Concepts and generics (T.*)

- C1 **[review]** New template parameters are constrained by a standard concept (`std::integral`, `std::invocable`,
  ...) or a project concept from `src/core/concept_constraints`. An unconstrained `typename T` needs a comment.
- C2 **[review]** Callbacks:
  - called synchronously, not stored: `template <std::invocable<Args...> F>` or `std::function_ref`
    (libstdc++ 16, `__cpp_lib_function_ref` 202603);
  - stored: `std::move_only_function`;
  - `std::function` only in cold registries.
  - `PrimFn` is the owning small-buffer callable (`evaluator_pure`). The stored
    callable is copy constructible and `const`-invocable, returning `EvalValue`.
    A heap box uses aligned `operator new` / `delete`. Move copies an SBO payload
    and is not `noexcept`. Assignment builds the new payload before destroying the
    old one. If that build throws, the old value stays. If the following SBO
    install throws, the target is left empty and destructible. Metadata is stored
    only after a payload exists. FFI and ADT registration take that type; they do not
    keep a `std::function` mirror.
  - Primitive registration follows that split: `Evaluator::prim_registrar()` returns an owning
    `std::function` (the lambda moves the name and `PrimFn` into the table). `register_*` parameters
    and `PrimRegistrar` / `RegisterFn` aliases are `std::function_ref` and are invoked only before
    the function returns. The name argument stays `std::string` by value because it is a sink.
    `register_ast_primitives` rebinds a local owning `std::function` around `ast:restore` (#4910);
    the incoming `function_ref` is not stored on `Evaluator`.
  - A recursive local lambda uses deducing `this`, not a `std::function` that captures itself.
- C3 **[review]** Pin layout and performance assumptions with `static_assert`, e.g.
  `std::is_trivially_copyable_v<EvalValue>` and `sizeof(EvalValue) == 8`.
- Landed (one cold helper, not sections 4–8): `dimension_name_at` accepts
  `signed_integral` or `unsigned_integral`, and rejects `bool`. On this
  libstdc++, `unsigned_integral<bool>` is true while `make_unsigned_t<bool>` is
  undefined, so the concept excludes `bool` by name.
  `kDimensionNames` is a `constexpr` array and `dim_name` is `constexpr`.
  `log_append` is a cold audit boundary with `post(r: r.empty() || r.find('#') != npos)`.
  Pin-registry and quota-tenant locks are `scoped_lock` (P6). Every plain
  single-mutex `lock_guard` in `src/` is `scoped_lock` too. The pin-registry
  lock-wait counter already existed; no new hot-path metric was added.

## 5. Contracts (C++26 P2900)

- K1 **[review]** Preconditions and postconditions on public APIs and cold boundaries (mutation, pass, compact,
  module loader, FFI entry) use language `pre(...)` / `post(r: ...)`.
- K2 **[review]** Invariants inside a body use `contract_assert`. **No new bare `assert`.**
- K3 **[review]** Hot paths keep `AURA_HOT_CONTRACT` / `AURA_HOT_CHECK` (`src/core/cpp26_contract_stats.h`): the
  language contract evaluation semantic is fixed per TU at compile time, while the production pack needs a
  runtime Harden switch that costs one load when off. Hot paths use the macros, cold paths the language.
- K4 **[review]** Contract predicates have no side effects and are O(1) or a cheap O(n). Clang never sees them, so
  clang-tidy cannot check this.
- K5 GCC 16.2 limits: no `pre` / `post` on virtual functions (check in a non-virtual entry, NVI); value parameters
  named in `post` must be `const`.

## 6. Compile-time computation (Con.5 / F.4)

- E1 **[review]** Lookup tables, name tables and opcode metadata are `constexpr std::array` (sorted, binary search)
  or `consteval`-generated; associative lookups use `std::flat_map` / `std::flat_set`; small fixed buffers use
  `std::inplace_vector`. No runtime-built `static const std::unordered_set<std::string>` constant tables.
- E2 **[review]** Small functions that can run at compile time are `constexpr`; must-run-at-compile-time ones are
  `consteval`.
- E3 **[review]** Invariants provable at compile time are `static_assert`, not runtime checks.

## 7. Performance (Per.*)

- P1 **[review]** Hot paths have no `std::function`, virtual dispatch, `std::shared_ptr` copies, hidden heap
  allocation, `std::string` temporaries or exceptions.
- P2 **[soft]** No needless copies (`performance-unnecessary-copy-initialization`, `performance-for-range-copy`,
  `performance-move-const-arg`, `performance-inefficient-string-concatenation`).
- P3 **[soft]** Move constructors and `swap` are `noexcept` (`performance-noexcept-move-constructor`,
  `performance-noexcept-swap`).
- P4 **[review]** Branch hints are `[[likely]]` / `[[unlikely]]`, not macros.
- P5 **[review]** Macros only for include guards, platform/compiler detection and contract macros that need `#expr`.
  Constants are `constexpr`; function-like macros become `inline` functions or templates.
- P6 **[hard]** A single mutex is `std::scoped_lock` (`modernize-use-scoped-lock`).
  `std::unique_lock` stays where the code defers, adopts, tries, or unlocks
  early: `scoped_lock` cannot express those. Multiple mutexes stay on the
  existing `std::lock` + `defer_lock` order, not a silent `scoped_lock`
  that would reorder them.

## 8. Other

- **[hard]** No `goto` (`cppcoreguidelines-avoid-goto`).
- **[review]** `printf`-family only on C ABI and signal-safe paths; elsewhere `std::format` / `std::print`.
  Signal alt stacks, crash printers, and C ABI `FILE*` callbacks that must not
  throw stay on `printf`. WAL magic and on-disk `char[N]` members stay C arrays
  because the layout is the format. Read and copy scratch in `chunk_io.hh`
  (`4096` / `8192`) is `std::array`. The tenant-jail `readlink` / `process_vm_readv`
  / stdout scratch and the JIT `/proc/self/exe` plus `fgets` scratch are
  `std::array` too; the C API still receives `data()`. Scheduler metrics dumps are
  ordinary I/O and use `std::print`.
- **[review]** Split functions longer than 300 lines.

## Gate

- `scripts/clang_tidy_gate.py` (#4829, #4837) is the per-(file, check) baseline ratchet: existing debt may not grow.
- `scripts/clang_tidy_changed_lines.py` (#4936) runs the checks above with `--line-filter` on the lines changed
  since `merge-base(HEAD, origin/main)` (CI: `HEAD~1`). Its `HARD` / `SOFT` tuples are the source of truth for the
  **[hard]** / **[soft]** markers here. A TU Clang cannot parse only produces notes.
- Run it locally after the Clang module build:
  `python3 scripts/clang_tidy_changed_lines.py --db build_clang/compile_commands.json --build-bmis`.
