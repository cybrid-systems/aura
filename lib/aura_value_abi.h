/* aura_value_abi.h -- single source of truth for the Aura value encoding
 * (issue #4502 step 1). Plain C99/C++ so it can be included by
 * lib/runtime.c, src/compiler/value_tags.h, the AOT preamble (main.cpp
 * emits it verbatim via AURA_VALUE_ABI_TEXT) and downstream C kernels
 * (aura-kernel-sink sink_abi.h checks AURA_VALUE_ABI).
 *
 * Layout (ABI 4, #4482 + #4502 steps 4/5):
 *   bit0 == 0     Fixnum, payload v >> 1, symmetric [-2^62, 2^62-1]
 *   low2 == 01    Ref (pairs in runtime.c: index = v >> 2)
 *   low2 == 11    Immediate: v = (payload << 8) | (kind << 2) | 3
 *                 kind = (v >> 2) & 0x3F, payload = (uint64)v >> 8
 *                 kind 0/1/2 = #f/#t/void (payload 0 => raw 3/7/11);
 *                 void (11) is also the empty list '(). kind 3 = the
 *                 unspecified value (raw 15) of (if #f #f) / when / unless,
 *                 distinct from '() (#4502 step 4),
 *                 kind 8 = boxed Float, kind 9 = boxed String (payload =
 *                 pool index, up to 2^56). No range comparisons.
 *                 kind 10 = interned Symbol (payload = symbol-table index,
 *                 ABI-additive: kind 10 was unassigned, so every existing
 *                 encoding above keeps its exact bits — Issue #4727).
 * Change anything here => bump AURA_VALUE_ABI; static_asserts in
 * value_tags.h and runtime.c keep the copies honest. */
#ifndef AURA_VALUE_ABI_H
#define AURA_VALUE_ABI_H

#define AURA_VALUE_ABI 4

#define AURA_IMM_KIND_SHIFT 2
#define AURA_IMM_KIND_MASK 0x3FLL
#define AURA_IMM_PAYLOAD_SHIFT 8
#define AURA_IMM_LOW8(kind) ((((long long)(kind)) << AURA_IMM_KIND_SHIFT) | 3)
#define AURA_IMM(kind, payload)                                                                    \
    ((long long)((((unsigned long long)(payload)) << AURA_IMM_PAYLOAD_SHIFT) |                     \
                 (unsigned long long)AURA_IMM_LOW8(kind)))
#define AURA_IMM_KIND(v) ((((long long)(v)) >> AURA_IMM_KIND_SHIFT) & AURA_IMM_KIND_MASK)
#define AURA_IMM_PAYLOAD(v) (((unsigned long long)(v)) >> AURA_IMM_PAYLOAD_SHIFT)

#define AURA_KIND_FALSE 0
#define AURA_KIND_TRUE 1
#define AURA_KIND_VOID 2
#define AURA_KIND_UNSPEC 3
#define AURA_KIND_FLOAT 8
#define AURA_KIND_STRING 9
/* Issue #4727: interned symbol atom. Same immediate shape as String; the
 * payload indexes the evaluator-global symbol table, so two symbols with
 * the same name share one payload and eq? is bit identity. */
#define AURA_KIND_SYMBOL 10

#define AURA_V_FALSE 3LL
#define AURA_V_TRUE 7LL
#define AURA_V_VOID 11LL
#define AURA_V_UNSPEC 15LL
#define AURA_FLOAT_LOW8 35LL  /* AURA_IMM_LOW8(AURA_KIND_FLOAT) */
#define AURA_STRING_LOW8 39LL /* AURA_IMM_LOW8(AURA_KIND_STRING) */
#define AURA_SYMBOL_LOW8 43LL /* AURA_IMM_LOW8(AURA_KIND_SYMBOL) */

#define AURA_FIXNUM_MAX ((long long)((1ULL << 62) - 1))
#define AURA_FIXNUM_MIN (-AURA_FIXNUM_MAX - 1)

#define AURA_CLOSURE_ID_HIGH_BIT (1ULL << 48)

#define AURA_IS_FIXNUM(v) ((((long long)(v)) & 1) == 0)
#define AURA_IS_PAIR(v) ((((long long)(v)) & 3) == 1)
#define AURA_IS_SPECIAL(v)                                                                         \
    (((long long)(v)) == AURA_V_FALSE || ((long long)(v)) == AURA_V_TRUE ||                        \
     ((long long)(v)) == AURA_V_VOID || ((long long)(v)) == AURA_V_UNSPEC)
#define AURA_IS_FLOAT(v) ((((long long)(v)) & 0xFF) == AURA_FLOAT_LOW8)
#define AURA_IS_STRING(v) ((((long long)(v)) & 0xFF) == AURA_STRING_LOW8)
#define AURA_IS_SYMBOL(v) ((((long long)(v)) & 0xFF) == AURA_SYMBOL_LOW8)
#define AURA_FLOAT_IDX(v) ((long long)AURA_IMM_PAYLOAD(v))
#define AURA_STRING_IDX(v) ((long long)AURA_IMM_PAYLOAD(v))
#define AURA_SYMBOL_IDX(v) ((long long)AURA_IMM_PAYLOAD(v))
#define AURA_MAKE_FLOAT(i) AURA_IMM(AURA_KIND_FLOAT, (i))
#define AURA_MAKE_STRING(i) AURA_IMM(AURA_KIND_STRING, (i))
#define AURA_MAKE_SYMBOL(i) AURA_IMM(AURA_KIND_SYMBOL, (i))

#endif /* AURA_VALUE_ABI_H */
