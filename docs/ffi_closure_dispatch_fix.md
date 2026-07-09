# FFI-in-closure dispatch bug — root cause + fix

## Symptom

FFI closures created with `c-func` work when called from **top-level inline**
`begin`/`if`/`let` bodies, but **segfault (exit 139) when called from inside a
user closure** (lambda body, `while` body, named-let body, or any
`(define (f) ...)` body that invokes the FFI closure).

Reproduces on the stock `build-gcc16/aura`:

```scheme
(begin
  (define libgmp (c-load "/opt/homebrew/lib/libgmp.dylib"))
  (define mpz-init-set-ui (c-func libgmp "__gmpz_init_set_ui" 1 1 1))
  (define mpz-get-ui      (c-func libgmp "__gmpz_get_ui"      1 1))
  (define r (c-alloc 256))
  (define rp (c-opaque->int r))
  (mpz-init-set-ui rp 42)
  (display (mpz-get-ui rp))        ; ✓ top-level inline: prints 42
  (define (f) (mpz-get-ui rp))
  (f))                             ; ✗ segfault 139
```

This forced the `pidigits.aura` port to unroll its loop into top-level ops
(`scripts/gen_pidigits.py`).

## Root cause (confirmed part)

`c-func` tags the FFI closure id with a high bit, intending to mark "this is a
foreign function":

```cpp
// src/compiler/ffi_primitives_impl.cpp:108-111
auto fidx = funcs_.size();
funcs_.push_back({fn_ptr, name, ret_type, std::move(arg_types)});
auto closure_id = static_cast<std::uint64_t>(fidx) | (1ULL << 63);   // ← marker
return types::make_closure(closure_id);
```

`make_closure` → `make_ref(RefClosure, closure_id)`:

```cpp
// src/compiler/value_tags.h:203
inline constexpr std::int64_t make_ref(std::uint64_t type, std::uint64_t index) noexcept {
    return static_cast<std::int64_t>((index << 6) | (type << 2) | 1ULL);
}
```

The index is shifted left by 6. `closure_id = fidx | (1ULL<<63)` — the marker
is bit 63. `(1ULL<<63) << 6` = bit 69, which **overflows uint64 and is
truncated to 0**. `as_closure_id` = `ref_index` = `val >> 6` then recovers
**only `fidx`** — the high-bit marker is gone:

```
fidx=5: (5 | (1<<63)) << 6  (mod 2^64) = 0x140   ; bit 63 truncated
         0x140 >> 6                        = 5    ; marker lost, cid = fidx
```

So `cid < ffi_runtime_.func_count()` is TRUE at the top-level call site
(`evaluator_eval_flat.cpp:2498` and `apply_closure` at `:133`) purely because
`fidx` happens to be small — **not because the marker works**. The marker bit
is dead code. This is confirmed.

## Root cause (the segfault — high confidence, not yet built-verified)

The dispatch at `evaluator_eval_flat.cpp:2498` and `:133`:

```cpp
auto cid = as_closure_id(*fn);
if (cid < ffi_runtime_.func_count()) {        // "Check for foreign function"
    ... auto& ff = ffi_runtime_.func_at(cid); // ← no bounds check
```

The comment claims this checks "high bit set" but the code compares against
`func_count()`. At the top-level call site the cid arrives intact (`= fidx`),
the branch is taken, and `func_at(fidx)` is in bounds. When the **same FFI
closure value is read out of a callee closure's captured env** and the call is
driven through the TW-closure inline path (`:2519+`) / `materialize_call_env`,
the cid reaches `func_at` as a value that is `>= func_count()` (the marker bit,
having survived in some intermediate representation, or an env-frame
re-encoding, yields a large cid), `func_at` does `funcs_[i]` with **no bounds
check** (`ffi_primitives.ixx:144`), and the out-of-bounds read jumps to a
garbage `fn_ptr` → SIGSEGV.

The two code paths diverge because eval_flat has **two** FFI dispatch sites
(`:133` in `apply_closure`, `:2498` in the inline call) and the TW inline path
(`:2519+`) routes closure-body calls differently from top-level calls. Without
the marker bit surviving, the two paths disagree on whether a given closure
value is foreign.

**To confirm the segfault path definitively**, build with this patch and run the
reproducer above under a debugger; the crash should be at `func_at`/`funcs_[i]`
with a large `cid`. (Could not build-verify here — the macOS + GCC-16 build is
currently broken by `import std;` not exporting `<cstring>`/`<atomic>`/
`<unordered_set>` in `src/serve/*.cpp` and `src/core/type.ixx`; that is a
separate, pre-existing toolchain issue.)

## Fix (minimal, self-contained)

Give FFI closures a **stable, type-level identity** instead of a truncated
high bit, and add a bounds check so a stale/wrong cid can never reach
`func_at`.

### 1. Add a dedicated RefType for FFI closures

`src/compiler/value_tags.h` — the 4-bit field has slots 12–15 free
(`RefKeyword == 11` is the current top). Add:

```cpp
inline constexpr RefType RefFFI = 12;   // c-func foreign function closure
```

Keep the `static_assert(RefKeyword < 16, ...)` guard; add:

```cpp
static_assert(RefFFI == 12, "RefType drift: update value_tags.h + lib/runtime.c");
```

### 2. Add `make_ffi_closure` / `is_ffi_closure` / `as_ffi_index`

`src/compiler/value.ixx`:

```cpp
export inline EvalValue make_ffi_closure(std::uint64_t fidx) noexcept {
    return EvalValue(make_ref(RefFFI, fidx));     // no high-bit hack
}
export inline bool is_ffi_closure(const EvalValue& v) noexcept {
    return is_ref(v.val) && ref_type(v.val) == RefFFI;
}
export inline std::uint64_t as_ffi_index(const EvalValue& v) noexcept {
    contract_assert(is_ffi_closure(v));
    return ref_index(v.val);
}
```

### 3. `c-func` returns the typed closure

`src/compiler/ffi_primitives_impl.cpp:108-111`:

```cpp
auto fidx = funcs_.size();
funcs_.push_back({fn_ptr, name, ret_type, std::move(arg_types)});
return types::make_ffi_closure(fidx);     // was: make_closure(fidx | (1<<63))
```

### 4. Dispatch on the type, not on a numeric range

`src/compiler/evaluator_eval_flat.cpp` — **both** sites. At `:2495` (inline
call path):

```cpp
if (is_closure(*fn)) {
    if (is_ffi_closure(*fn)) {                     // was: cid < func_count()
        std::vector<EvalValue> cargs;
        for (std::size_t i = 0; i + 1 < v.children.size(); ++i) {
            auto ar = eval_flat(*f, *p, v.child(i + 1), eval_env);
            if (!ar) return ar;
            cargs.push_back(*ar);
        }
        auto result = apply_ffi(*fn, cargs);       // direct, see step 5
        if (result) return *result;
        return std::unexpected(Diagnostic{ErrorKind::InvalidClosure,
                                          "eval_flat: foreign call failed"});
    }
    auto cid = as_closure_id(*fn);                 // TW closure path unchanged
    ...
```

At `:132` (`apply_closure`) — same `is_ffi_closure` check, calling
`apply_ffi` instead of recursing into the TW path.

### 5. Add a single, bounds-checked `apply_ffi`

`src/compiler/evaluator_eval_flat.cpp` (or a new helper):

```cpp
std::optional<EvalValue> Evaluator::apply_ffi(const types::EvalValue& fn,
                                              std::span<const EvalValue> args) {
    auto fidx = types::as_ffi_index(fn);
    if (fidx >= ffi_runtime_.func_count())        // ← the safety net
        return std::nullopt;
    auto& ff = ffi_runtime_.func_at(fidx);
    // ... existing marshal + i_fn/d_fn dispatch from apply_closure's FFI branch ...
}
```

### 6. Add bounds checks to `func_at` (defense in depth)

`src/compiler/ffi_primitives.ixx:144`:

```cpp
const FFIFunc& func_at(std::size_t i) const {
    static const FFIFunc empty{};
    return i < funcs_.size() ? funcs_[i] : empty;   // was: funcs_[i]  (UB if i >= size)
}
```

### 7. Sync the display helper

`lib/runtime.c` — add `case 12: /* RefFFI */` to the ref-type display switch so
`#<ffi ...>` prints instead of mis-typing as something else. (Low priority; only
affects pretty-printing.)

## Why this fixes both the marker bug and the segfault

- The marker bit no longer exists to be truncated — `is_ffi_closure` is a
  **type tag** that survives any value copying through env frames, materialize
  paths, and the two dispatch sites. Both call paths now agree on "is this
  foreign" by inspecting the same type bit, not by a numeric range that only
  happens to work when the cid is small.
- Even if a future bug hands a wrong index to `func_at`, the bounds check
  returns `empty`/`nullopt` instead of dereferencing a garbage `fn_ptr`, so the
  worst case is a clean "foreign call failed" diagnostic, not SIGSEGV.

## Verification plan (once the build is unblocked)

1. Rebuild `build-gcc16/aura`.
2. Run the reproducer at the top — `(f)` must print `42`, exit 0.
3. Rewrite `tests/bench/pidigits.aura` with a real `while`/named-let loop (no
   unrolling) and confirm N=27 still diffs clean against the reference, and
   N=100 completes (bignum path).
4. Run the existing EDSL/FFI test suites (`tests/suite/`, anything touching
   `c-load`/`c-func`) to ensure no regression in the top-level FFI path.
5. Optional: `./build.py check` for the full CI gate.

## Related

- `tests/bench/pidigits.aura` + `scripts/gen_pidigits.py` — the unrolled
  workaround that this fix would let us replace with a real loop.
- `memory/ffi-closure-nesting-bug.md` — the user-facing note.
