#define _GNU_SOURCE
// Aura standalone runtime
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

// Pointer tagging:
// bit 0 = 0: Fixnum (signed int, val >> 1)
// bits 1-0 = 01: Pair/indexed (index = val >> 2) — pairs, strings, closures
// bits 1-0 = 11: Special (tag = (val >> 2) & 3)  tag 0=#f(3), 1=#t(7), 2=void(11)
// Strings use pair encoding: (string_id << 2) | 1, with string_id >= STRING_BASE

#define IS_PAIR(v)     (((v) & 3) == 1)
#define IS_SPECIAL(v)  (((v) & 3) == 3)
#define IS_FIXNUM(v)   (((v) & 1) == 0)
#define PAIR_INDEX(v)  ((v) >> 2)
#define SPECIAL_TAG(v) (((v) >> 2) & 3)
#define STRING_BASE    1024

static const int KWD_FALSE = 0;
static const int KWD_TRUE = 1;
static const int KWD_VOID = 2;
