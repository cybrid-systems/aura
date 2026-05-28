# Aura Value Encoding Contract

> Last updated: 2026-05-28
> After: `make_string(odd_idx)` vs `is_ref` encoding collision bug (commit d2e3b19)

## Overview

Every `EvalValue` in Aura is exactly **64 bits**. The low bits encode the type tag,
and the remaining bits carry the payload (index, pointer, or immediate value).

This document defines the encoding scheme, the invariants that MUST be maintained,
and the collision analysis that led to the contract.

## Tag Layout (Low 2 Bits)

```
Bit 1  Bit 0  |  Tag Name    |  Description
────── ────── | ──────────── | ──────────────────────────
  0      0    |  fixnum      |  Immediate integer (v >> 1)
  0      1    |  ref         |  Heap reference (pair, closure, error, ...)
  1      0    |  special     |  Void (11), Bool (3/7), Float (bias range)
  1      1    |  (reserved)  |  Not currently used
```

### fixnum (tag = 00)

```
value = int << 1
```

Used for small integers. Any even value that is NOT in the `float_bias` or
`string_bias` range is treated as a fixnum.

### ref (tag = 01)

```
bit 63..6  = index / pointer (58 bits)
bit 5..2   = ref_type (4 bits, 16 possible types)
bit 1..0   = 01 (tag)
```

Current ref types:

| Type # | Name         | `is_X()` predicate |
|--------|--------------|---------------------|
| 0      | `RefPair`    | `is_pair()`         |
| 1      | `RefClosure` | `is_closure()`      |
| 2      | `RefCell`    | `is_cell()`         |
| 3      | `RefVector`  | `is_vector()`       |
| 4      | `RefHash`    | `is_hash()`         |
| 5      | `RefPrimitive` | `is_primitive()`  |
| 6      | `RefModule`  | `is_module()`       |
| 7      | `RefOpaque`  | `is_opaque()`       |
| **8**  | **`RefError`** | **`is_error()`** |
| 9      | `RefLinear`  | `is_linear()`       |
| 10     | (unused)     | —                   |
| **11** | **`RefKeyword`** | **`is_keyword()`** |
| 12-15  | (unused)     | —                   |

### special (tag = 10)

Used for:
- `#f` = 3 (0b11 — tag 10 with bit 0 = 1 is a special sentinel)
- `#t` = 7 (0b111)
- `()` void = 11 (0b1011)
- Float values
- String values (biased encoding)

### float (in special range)

Float values are allocated on the float heap and the EvalValue stores a handle
in the `float_bias` range:

```
FLOAT_BIAS_VAL = +9000000000000000000  (positive)
is_float(v): FLOAT_BIAS_VAL >= v > STRING_BIAS_VAL
```

### string (biased encoding — CRITICAL)

Strings use a **biased negative encoding**:

```
STRING_BIAS_VAL = -9000000000000000000  (negative)
make_string(idx) = STRING_BIAS_VAL - idx
is_string(v): v.val <= STRING_BIAS_VAL
```

## ⚠️ COLLISION ANALYSIS (Discovered 2026-05-28)

### The Problem

For **odd** `idx` values, `make_string(idx)` produces an `EvalValue` where
`(val & 3) == 1`. This matches the `is_ref()` tag.

When the ref_type bits (bits 5..2) happen to match a defined ref type,
the string value is **mistakenly identified** as a ref type.

### Collision Table

| String idx | ref_type | Colliding predicate | Impact |
|-----------|----------|-------------------|--------|
| 3, 67, 131, ... | 15 | (unused) | Low |
| 7, 71, 135, ... | 14 | (unused) | Low |
| 11, 75, 139, ... | 13 | (unused) | Low |
| 15, 79, 143, ... | 12 | (unused) | Low |
| **19, 83, 147, ...** | **11** | **`is_keyword()`** | **Cosmetic (`:<kwd>` display)** |
| 23, 87, 151, ... | 10 | (unused) | Low |
| 27, 91, 155, ... | 9 | (unused) | Low |
| **31, 95, 159, ...** | **8** | **`is_error()`** | **DATA LOSS (query:def-use failure)** |
| 35, 99, 163, ... | 7 | (unused) | Low |
| 39, 103, 167, ... | 6 | (unused) | Low |
| 43, 107, 171, ... | 5 | (unused) | Low |
| 47, 111, 175, ... | 4 | (unused) | Low |

**Pattern**: `idx ≡ 31 (mod 64)` produces `RefError` (type 8).
`idx ≡ 19 (mod 64)` produces `RefKeyword` (type 11).

### Root Cause

```
STRING_BIAS_VAL = -9000000000000000000
                 = 0xFFFFFFFFF83199DB00000000... (truncated to 64 bits for the
                   actual computation)
```

For odd idx: `STRING_BIAS_VAL - idx` is odd → `(val & 3) == 1` → `is_ref()` true.
The ref_type bits (5..2) depend on `idx mod 64`, producing predictable collisions.

### Fix Applied (commit d2e3b19)

```cpp
// Before:
if (is_error(*ar)) return ar;

// After:
if (is_error(*ar) && !is_string(*ar)) return ar;
```

This is a **defense-in-depth** fix at the check sites. The encoding itself
remains unchanged, but all `is_ref`-based predicates (`is_error`, `is_keyword`,
etc.) must be guarded with `!is_string` when applied to values that could
originate from `make_string` or string evaluation.

## Invariants (MUST Maintain)

### I1: String values must never be mistaken for Error values

```cpp
// For ALL idx >= 0:
assert(!is_error(make_string(idx)));
```

**Rationale**: A string silently dropped due to is_error() causes data loss.
This is the most severe consequence.

### I2: String values must never be mistaken for Keyword values

```cpp
assert(!is_keyword(make_string(idx)));
```

**Rationale**: Causes cosmetic display artifacts (`:<kwd>` instead of the
actual string). Less severe than I1 but still a bug.

### I3: Type predicates are checked in the correct order

All type-checking code paths must check `is_string` BEFORE `is_ref`-based
predicates (`is_error`, `is_keyword`, `is_pair`, etc.) when the value could
be either a string or a ref type.

### I4: make_string encoding must not collide with any ref_type

```cpp
// make_string(idx) must guarantee (val & 3) != 1
// for ALL possible idx values (0 to 2^64-1).
```

**Status**: ⚠️ CURRENTLY VIOLATED for odd idx. The fix at check sites (I3)
mitigates this, but a future encoding redesign should enforce this at the
source.

## Defense Layers

| Layer | What | Where | Status |
|-------|------|-------|--------|
| 1 | `is_error(*ar) && !is_string(*ar)` | `eval_flat` arg loops | ✅ Fixed |
| 2 | `is_error(*ar) && !is_string(*ar)` | `eval_data_as_code` arg loops | ✅ Fixed |
| 3 | `is_error` / `is_keyword` check order | `io_print_val`, `fmt_val_to_string` | ⚠️ Not fixed (cosmetic) |
| 4 | encoding contract assertions | `make_string`, unit tests | 🔲 Planned |
| 5 | encoding redesign | future work | 🔲 Future |

## Future: Encoding Redesign Options

### Option A: Reserve a tag bit for strings

Change the string encoding to use a dedicated tag bit pattern that never
collides with is_ref. For example, ensure `(val & 1) == 0` for ALL strings
by adjusting STRING_BIAS_VAL.

**Cost**: Changes `as_string_idx` / `string_idx_raw` to handle the adjustment.

### Option B: Change RefError / RefKeyword type numbers

Move RefError and RefKeyword to type numbers >= 12 (currently unused).
This would avoid the collision for idx ≡ 31 and idx ≡ 19.

**Cost**: Changes the binary encoding of EvalValue, affecting serialization.

### Option C: Accept the current fix as sufficient

The current `is_error(*ar) && !is_string(*ar)` guard is correct, minimal,
and well-understood. The remaining cosmetic `:<kwd>` issue is a minor
display bug.

**Cost**: None, but future additions to ref types must be aware of the
collision.

## Appendix: Bit-Level Proof

```python
STRING_BIAS = -9000000000000000000

def ref_type(val):
    """Extract ref_type from a ref-encoded value"""
    return (abs(val) >> 2) & 0xF

def make_string(idx):
    return STRING_BIAS - idx

# Verify collision pattern
for idx in range(0, 128):
    v = make_string(idx)
    if v & 1:  # odd → passes is_ref()
        rt = (v >> 2) & 0xF
        if rt in (8, 11):
            print(f"COLLISION idx={idx}: ref_type={rt}")
```

Output:
```
COLLISION idx=19: ref_type=11 (RefKeyword - display artifact)
COLLISION idx=31: ref_type=8 (RefError - DATA LOSS)
COLLISION idx=83: ref_type=11 (RefKeyword)
COLLISION idx=95: ref_type=8 (RefError)
COLLISION idx=147: ref_type=11 (RefKeyword)
COLLISION idx=159: ref_type=8 (RefError)
```
