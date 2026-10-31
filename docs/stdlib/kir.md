# std/kir — typed kernel IR, schedules and C / CUDA / PTX / Triton backends and build cache (#4802, #4803)

A kernel is a plain Aura datum: it can be stored in the workspace, diffed,
mutated by `set-code` / `mutate`, proposed by an agent, and lowered by a
backend. Everything is checked before it is printed, so an ill-typed or racy
proposal fails in the checker instead of in a compiler or on a device.

| Module | Role |
|--------|------|
| `std/kir` | IR, type/contract checker (`kir-check`), C lowering, call graph |
| `std/kir-sched` | schedule rewrites: split / reorder / tile / unroll / vectorize |
| `std/kir-gpu` | CUDA C, PTX and Triton lowering plus GPU contracts |
| `std/cgen` | the C-subset printer used by the C / CUDA backends |

## IR

```aura
(kernel NAME (params P ...) STMT ...)        ; void entry point
(func NAME RET (params P ...) STMT ...)      ; helper, called with (call NAME arg ...)
(module NAME ITEM ...)                       ; kernels + funcs

P     = (name TYPE [:in | :out | :inout])     ; pointer access mode, default :inout
TYPE  = i32 | i64 | f32 | f64 | bool | (ptr TYPE)
STMT  = (let v TYPE [e]) | (set lv e) | (+= lv e) (-= ..) (*= ..) (/= ..)
      | (for (i lo hi [step]) STMT ...) | (simd-for (i lo hi [step]) STMT ...)
      | (pfor (i lo hi) STMT ...)            ; data-parallel axis, kernel top level only
      | (if c STMT [STMT]) | (when c STMT ...) | (begin STMT ...)
      | (return [e]) | (expr e)
EXPR  = integer | float (f64) | (f32 x) | symbol | (ref p i) | (cast TYPE e) | (? c a b)
      | (+ - * / % min max a b ...) | (< <= > >= == != a b) | (and or a b ...) | (not a) | (neg a)
      | (sqrt e) (exp e) (log e) (fabs e) (fmax a b) (fmin a b) | (call f arg ...)
```

Logical operators are spelled `and` / `or`. They are lowered to `&&` / `||`.
The IR does not accept the C spellings: `&&` reads as an ordinary symbol, but
`||` is the R7RS empty symbol `||` (#4805).

### Contracts (`kir-check`)

`(kir-check item [module])` returns a list of error strings (`'()` = ok);
`kir-check!` raises. Enforced before any lowering:

- declare before use, no shadowing, no local pointers;
- operand types agree — no implicit promotion (integer literals adapt, float
  literals are f64, write `(f32 1.5)`); `(cast T e)` converts;
- indices are integers; `:in` pointers and loop variables are read-only;
- at most one top-level `pfor` per kernel; inside it every pointer store is
  indexed by the parallel variable and no outer scalar is written (no races);
- `simd-for` may only write outer scalars with `+=` (lowered as a reduction);
- calls resolve in the module and match the callee signature.

### C lowering

```aura
(require "std/kir" all:)
(kir->c kernel)            ; one C function (restrict pointers, const for :in)
(kir->c-unit module)       ; prototypes + all items as one translation unit
(kir-callees item)         ; direct callees
(kir-dependents m 'sq)     ; transitive callers — the units to rebuild after a signature change
```

`pfor` lowers to a plain loop in C; `simd-for` lowers to
`#pragma omp simd [reduction(+:acc,...)]`.

## Schedules (`std/kir-sched`)

Each transform is a pure kernel → kernel rewrite addressed by loop variable,
with legality checked first (errors start with `kir-sched:`):

```aura
(kir-split k 'j 8)          ; j -> j_o (step 8) and j in [j_o, min(j_o+8, hi))
(kir-reorder k 'i 'j)       ; swap a perfect, rectangular nest
(kir-tile k 'i 'j 32 32)    ; split, split, reorder -> i_o j_o i j
(kir-unroll k 'k 4)         ; step-4 main loop with 4 body copies + remainder loop
(kir-vectorize k 'k)        ; innermost for -> simd-for (reductions only)
(kir-sched-apply k '((tile i j 32 32) (unroll k 4)))   ; -> (ok . k2) | (error . msg)
```

A `pfor` is never split or moved; `kir-sched-apply` re-runs `kir-check` on
the result.

## GPU backends (`std/kir-gpu`)

```aura
(require "std/kir-gpu" all:)
(kir-gpu-check 'cuda|'ptx|'triton item [module])  ; -> list of errors
(kir->cuda-unit module)      ; CUDA C: __device__ funcs, extern "C" __global__ kernels
(kir->ptx module ['sm_80])   ; PTX 7.0, one .entry per kernel, .func per func
(kir->triton module)         ; Python: @triton.jit funcs/kernels + launch_<kernel>
```

The top-level `(pfor (i lo hi) ...)` is the 1-D launch axis:

| Backend | Index | Bounds |
|---------|-------|--------|
| CUDA C | `i = lo + blockIdx.x*blockDim.x + threadIdx.x` | `if (i < hi)` |
| PTX | `%ctaid.x * %ntid.x + %tid.x` (`mul.wide` for i64) | `setp.ge` + `bra` |
| Triton | `i = lo + program_id(0)*KIR_BLOCK + tl.arange(0, KIR_BLOCK)` | `_mask = i < hi` on every load/store |

Launch with `grid = ceil((hi - lo) / block)` blocks of any size. Triton gets a
`launch_<kernel>(..., block=256)` helper when the bounds only use scalar params.

GPU contracts (on top of `kir-check`):

- a kernel has exactly one top-level `pfor`; only `let` and scalar `set` may
  precede it (they run in every thread / program); nothing may follow it;
- PTX: `exp` / `log` on f64 have no instruction — use the CUDA backend
  (f32 uses `ex2.approx` / `lg2.approx`);
- Triton: loop bounds inside a `pfor` must be uniform (params, kernel-level
  lets, uniform loop variables); no `return` inside a `pfor`; funcs take
  values, not pointers (no `ref`).

Triton if-conversion: an `if` / `when` whose condition depends on the element
evaluates both branches; stores are masked with the condition, locals are
updated with `tl.where`, and loads under `and` / `or` / `?` / a condition are
masked so no lane reads out of bounds.

Semantics notes: `&&`-style short-circuit holds on CUDA and PTX (branches);
Triton evaluates both operands but masks the loads. Integer `/` lowers to
`//` in Triton; inputs in the tests are non-negative.

### Validating without a GPU

`tests/suite/kir_gpu_4802.aura` runs whatever is installed and skips the rest:

- `ptxas` (or `$AURA_PTXAS`): assembles the emitted PTX for sm_80;
- `clang++` with the nvptx64 target compiles the CUDA C device-only. Pass
  `-DAURA_KIR_NOCUDAINC -nocudainc -nocudalib`; the unit carries a small shim
  for the CUDA builtins;
- `python3` parses the Triton module. With `triton` and `torch` importable,
  the kernels run under `TRITON_INTERPRET=1` and the values are compared.

Set `AURA_KIR_GPU_OUT=<dir>` to keep `k.cu`, `k.ptx`, `k.py` and
`run_triton.py`.

Out of scope: real-GPU timing, autotuning, 2-D/3-D grids, shared memory and
warp primitives.

## Build cache (`std/cbuild`, #4803)

`std/cbuild` stores build outputs on disk under a content key. Rebuilding an
unchanged input costs one file probe, and the cache is shared across processes
and sessions.

```scheme
(require "std/cbuild" all:)
(define o   (cbuild-object! "int add3(int a) { return a + 3; }\n"))  ; cached .o
(define o2  (cbuild-object! c-text "-O3 -march=native -fPIC"))       ; flags are in the key
(define exe (cbuild-link! (list main-o o) 'exe))                      ; or 'so
(cbuild-stats)   ; (("hits" . n) ("shared-hits" . n) ("built" . n) ("failed" . n))
```

- **Key.** The key is a digest of `cbuild-v1`, the kind, the key material and
  the input text. For objects and links, the key material is the toolchain id
  plus the flags. The toolchain id is a digest of `$AURA_CC` (or `cc`)
  `--version` and `-dumpmachine`, so a compiler upgrade or a flag change never
  reuses a stale object.
- **Root.** The cache lives in `$AURA_CGEN_CACHE`, falling back to
  `$XDG_CACHE_HOME/aura/cgen`, then `~/.cache/aura/cgen`. Call
  `cbuild-set-root!` to override it, for example in tests. Each entry is stored
  at `<root>/<k2>/<key>/` and contains the input, the outputs, `.meta` and
  `.atime`.
- **Concurrency.** Each build runs in a private staging directory while holding
  `flock(<root>/locks/<key>.lock)`. The finished directory is then published
  with a single rename. Other builders of the same key, in other fibers or
  other processes, wait on the lock and then reuse the published entry.
  Each key is built exactly once, and readers never see a partial entry.
- **Failures.** A failed build publishes nothing. It raises `cbuild: <kind>
  build failed: <log>`. A command that exits 0 but does not create a declared
  output also counts as a failure.
- **Generic artifacts.** `(cbuild-artifact! kind material input-name input-text
  cmd outputs)` writes the input to a file, runs `cmd` in `sh` inside the
  staging directory, and returns the entry directory. It works for any tool,
  for example `ptxas` or code generators.
- **Maintenance.**
  - `cbuild-lookup` returns the entry directory, or `#f` if there is none.
  - `cbuild-invalidate!` removes a single entry.
  - `(cbuild-gc! max-kbytes [max-age-s])` evicts least-recently-used entries
    until the cache fits in `max-kbytes`, and removes entries unused for longer
    than `max-age-s`. It also deletes staging directories older than one day.
  - `cbuild-clear!` empties the cache.
  - `cbuild-disk-stats` and `cbuild-entries` report the cache's size and
    contents.

### Per-function incremental builds (`std/kir-build`)

```scheme
(require "std/kir-build" all:)
(define r (kir-build! m))              ; [flags "-O2 -fPIC"] [link 'so | 'exe | #f]
(cdr (assoc "lib" r))                  ; linked shared library
(cdr (assoc "rebuilt" r))              ; items compiled in this call
(cdr (assoc "reused" r))               ; items served from the cache
(kir-rebuild-set old-m new-m)          ; what an edit will recompile
```

Each module item is compiled as its own translation unit, made of the
includes, the prototypes of the item's callees, and `kir->c` of the item. The
object goes through `std/cbuild`.

- Editing a function body changes only that function's unit, so only that
  function is recompiled.
- Changing a function's signature also changes the units of its callers, so
  `kir-rebuild-set` returns the item plus its `kir-dependents`.
- Lowering is memoised in-process on the item datum and its callee prototypes,
  so unchanged items are not re-checked or re-lowered.
- A new process starts with an empty memo, but its objects still come from the
  disk cache.
