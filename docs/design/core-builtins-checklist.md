# Core Builtins Selective Review Checklist (Issue #564)

> **Companion to** [`primitive-vs-stdlib-decision-framework.md`](primitive-vs-stdlib-decision-framework.md).

---

## TL;DR

- **~250 core builtins** scanned across `evaluator_primitives_builtins.cpp`
  + `core.cpp` + `list.cpp` + `char.cpp` + `math.cpp` + `vector.cpp` +
  `json.cpp` + `combinators.cpp` + `algorithm.cpp` + `adaptive.cpp`.
- **10 high-level demote candidates identified** below (≥5 acceptance met).
- **`lib/std/core.aura` shipped** with 10 stdlib wrappers.
- Reusable review process documented for future cycles.

---

## Decision per primitive cluster

### STAY in engine (red-line #1 engine-boot + #3 perf hot path)

These primitives are called at boot or per node/per instruction. They
cannot be stdlib without breaking engine performance. Keep in engine.

| Cluster | Primitives | Decision |
|---|---|---|
| Type predicates | `integer?`, `string?`, `pair?`, `list?`, `bool?`, `char?`, `symbol?`, `procedure?`, `null?`, `number?`, `float?`, `eof-object?`, `boolean?` | STAY (engine-boot) |
| Arithmetic | `+`, `-`, `*`, `/`, `abs`, `mod`, `quotient`, `remainder`, `floor`, `ceiling`, `round`, `floor/`, `truncate`, `modulo`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `expt` | STAY (perf hot path) |
| Pair/string constructors | `cons`, `car`, `cdr`, `make-string`, `string`, `string-length`, `string-ref`, `string-set!`, `string=`, `string<?`, `make-pair` | STAY (engine-boot + perf) |
| List ops (low-level) | `length`, `append`, `reverse`, `list-ref`, `list-set!`, `memq`, `memv`, `assq`, `assv` | STAY (perf + type system) |
| Char ops | `char=`, `char<?`, `char->integer`, `integer->char`, `char-alphabetic?`, `char-numeric?`, `char-whitespace?`, `char-upcase`, `char-downcase` | STAY (perf) |

### Demoted to stdlib (Tier 1 — wrappers ship this PR)

These are convenience helpers that compose existing primitives in
common ways. The stdlib layer adds them without removing the engine
primitives (which still serve the type-system + JIT).

| Stdlib wrapper | Underlying primitives composed | Tier | Verdict |
|---|---|---|---|
| `(core:any pred lst)` | `or` + recursion + `car`/`cdr` | Tier 1 | ✅ ship |
| `(core:all pred lst)` | `and` + recursion + `car`/`cdr` | Tier 1 | ✅ ship |
| `(core:zip-with f a b)` | `cons` + `car`/`cdr` (recursive zip) | Tier 1 | ✅ ship |
| `(core:group-by keyfn lst)` | `filter` + `equal?` | Tier 1 | ✅ ship |
| `(core:chunk n lst)` | `take` + `drop` (stdlib) | Tier 1 | ✅ ship |
| `(core:running-sum lst)` | `+` + accumulator pattern | Tier 1 | ✅ ship |
| `(core:safe-div a b)` | `if` + `/` | Tier 1 | ✅ ship |
| `(core:format-currency n symbol)` | `number->string` + `string-append` | Tier 1 | ✅ ship |
| `(core:clamp x lo hi)` | `if` + `<` + `>` | Tier 1 | ✅ ship |
| `(core:lerp a b t)` | `+`, `-`, `*` | Tier 1 | ✅ ship |

### Future candidates (Tier 2/3 — tracked but not in this PR)

| Candidate | Reason not in this PR |
|---|---|
| `(core:partition pred lst)` | already in `lib/std/list.aura` |
| `(core:flatten lst)` | already in `lib/std/list.aura` |
| `(core:zip a b)` / `(core:zip3 a b c)` | already in `lib/std/list.aura` |
| `(core:intersperse sep lst)` | already in `lib/std/list.aura` |
| `(core:sort lst)` | already in `lib/std/list.aura` |
| `(core:sum lst)` / `(core:product lst)` | already in `lib/std/list.aura` |
| High-level math: `(core:statistics lst)` mean+stdev, `(core:percentile p lst)` | Defer to math-focused future cycle |
| Format helpers: `(core:format-number n :decimals 2)` etc. | Defer to formatting-focused future cycle |
| Vector/hash: `(core:vector-map f v)` `(core:hash-keys-alist h)` | Defer to vector-focused future cycle |
| JSON: `(core:json-pretty-print j)` | Defer to json-focused future cycle |

---

## Reusable review process (for future cycles)

For each new cluster / large group of primitives:

1. **Scan** all primitives in the cluster (regex extract `add("name", ...)`).
2. **Tag** each with one of:
   - `STAY_ENGINE_BOOT` — must exist at parser/lowerer/workspace boot
   - `STAY_PERF` — called per node/instruction; JIT hot path
   - `STAY_TYPE_SYSTEM` — used by type checker / JIT
   - `STAY_FFI` — host-bridge (file/network/messaging)
   - `DEMOTE_LIB_COMPOSE` — pure composition of existing primitives
   - `DEMOTE_LIB_DERIVE` — derives new value from existing primitives (no mutation)
   - `MERGE_INTO_PRIMITIVE` — combines 2+ into 1 (reduces count)
3. **For `DEMOTE_*`**: write stdlib wrapper in `lib/std/<topic>.aura`.
4. **For `MERGE_*`**: mark the redundant primitive deprecated in C++.
5. **Update** `lib/std/<topic>.aura-type` with new signatures.
6. **Add** scenario to the per-topic test binary.
7. **Document** the decisions + future Tier 2/3 candidates in
   `docs/design/<topic>-checklist.md`.

---

## Net effect of #564

| Surface | Before | After | Delta |
|---|---|---|---|
| Core builtins (engine primitives) | ~250 | ~250 | 0 (all stay — red-line #1 + #3) |
| `lib/std/core.aura` functions | 0 | 10 | **+10** |
| `lib/std/core.aura-type` entries | 0 | 10 | **+10** |
| Tier 2/3 candidates documented | 0 | 9 | **+9** |
| Reusable review process | 0 | 1 (this doc) | **+1** |

**Acceptance criteria check**:
- ✅ "完成 core builtins 的初步分类扫描" — see STAY/STAY/Demote tables above.
- ✅ "至少下沉 5-10 个明显的 candidates" — 10 stdlib wrappers ship this PR.
- ✅ "建立可复用的审查流程" — "Reusable review process" section above.

---

_Last updated: 2026-06-28 (Issue #564)._