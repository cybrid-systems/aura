// evaluator_primitives_core.cpp — P0 step 1: stateless R5RS-style primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.
//
// aura.compiler.evaluator module partition.
// Registration uses a callback to avoid friending or exposing Evaluator
// internals — mirrors ffi_primitives_impl.cpp / adt_runtime_impl.cpp.

module;

#include <bit>
#include <cmath>
#include <cstdint>

#include "prim_registrar_scaffold.hh" // Issue #5003: bitwise prims want PrimSpec

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function_ref<void(std::string, PrimFn)>;

void register_type_and_char_primitives(PrimRegistrar add, Evaluator& ev) {
    using aura::compiler::pure_general;
    using aura::compiler::register_prim;
    // ── Type predicates ──────────────────────────────────────────
    // Issue #4469: an integral float (2.0, (floor 2.7)) is an integer.
    // Non-finite floats are not. exact? stays on the int tag only.
    add("integer?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        if (types::is_int(a[0]))
            return types::make_bool(true);
        if (types::is_float(a[0])) {
            const double d = types::as_float(a[0]);
            return types::make_bool(std::isfinite(d) && std::trunc(d) == d);
        }
        return types::make_bool(false);
    });
    add("exact?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_int(a[0]));
    });
    add("inexact?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_float(a[0]));
    });
    add("float?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_float(a[0]));
    });
    add("boolean?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_bool(a[0]));
    });
    add("number?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_int(a[0]) || types::is_float(a[0]));
    });
    add("symbol?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        // Issue #4727: quoted identifier atoms are first-class interned
        // symbols (ImmKind 10, evaluator-global symbol table). Everything
        // else — including the string a symbol-typed atom used to decay to —
        // is not a symbol.
        return types::make_bool(types::is_symbol(a[0]));
    });
    add("procedure?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_closure(a[0]) || types::is_primitive(a[0]));
    });
    add("void?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        // #4502 step 4: the unspecified value is also void-like.
        return types::make_bool(types::is_void(a[0]) || types::is_unspecified(a[0]));
    });
    add("void", [](const auto&) { return types::make_void(); });

    // ── Character operations (chars are integers in Aura) ──────────
    add("char=?", [](const auto& a) {
        if (a.size() < 2)
            return types::make_bool(false);
        return types::make_bool(types::is_int(a[0]) && types::is_int(a[1]) &&
                                types::as_int(a[0]) == types::as_int(a[1]));
    });
    add("char<?", [](const auto& a) {
        if (a.size() < 2)
            return types::make_bool(false);
        return types::make_bool(types::is_int(a[0]) && types::is_int(a[1]) &&
                                types::as_int(a[0]) < types::as_int(a[1]));
    });
    add("char->integer", [](const auto& a) {
        if (a.empty() || !types::is_int(a[0]))
            return types::make_bool(false);
        return a[0];
    });
    add("integer->char", [](const auto& a) {
        if (a.empty() || !types::is_int(a[0]))
            return types::make_bool(false);
        return a[0];
    });

    // ── Character predicates ──────────────────────────────────────
    // Issue #5137 (S-CHAR): char-alphabetic?, char-numeric?, char-whitespace?,
    // char-upcase and char-downcase are Aura definitions in lib/std/char now.
    // They stayed kernel only because nothing shadowed a primitive; the
    // definitions reproduce the same ASCII-only logic and the same #f for a
    // non-integer argument.

    // ── SRFI-151 bitwise primitives (Issue #5003) ──────────────────
    // Aura has no exact bignums (fixnum range [-2^62, 2^62-1]), so these
    // are int64 operations on the fixnum payload. Results outside the
    // fixnum range promote to an integral float via make_int_or_promote;
    // a Float argument is a type error (void), not a silent coercion.
    auto all_ints = [](std::span<const EvalValue> a) {
        for (const auto& v : a)
            if (!types::is_int(v))
                return false;
        return true;
    };

    register_prim(
        add, ev, "bitwise-not",
        [](const auto& a) -> EvalValue {
            if (a.empty() || !types::is_int(a[0]))
                return types::make_void();
            return types::make_int_or_promote(~types::as_int(a[0]));
        },
        pure_general(1, "(int) -> int",
                     "SRFI-151 bitwise-not: one's complement of an exact integer."));

    register_prim(
        add, ev, "bitwise-and",
        [all_ints](const auto& a) -> EvalValue {
            if (!all_ints(a))
                return types::make_void();
            std::int64_t r = -1;
            for (const auto& v : a)
                r &= types::as_int(v);
            return types::make_int_or_promote(r);
        },
        pure_general(255, "(int ...) -> int",
                     "SRFI-151 bitwise-and: fold a variadic and; no args is -1."));

    register_prim(
        add, ev, "bitwise-ior",
        [all_ints](const auto& a) -> EvalValue {
            if (!all_ints(a))
                return types::make_void();
            std::int64_t r = 0;
            for (const auto& v : a)
                r |= types::as_int(v);
            return types::make_int_or_promote(r);
        },
        pure_general(255, "(int ...) -> int",
                     "SRFI-151 bitwise-ior: fold a variadic inclusive or; no args is 0."));

    register_prim(
        add, ev, "bitwise-xor",
        [all_ints](const auto& a) -> EvalValue {
            if (!all_ints(a))
                return types::make_void();
            std::int64_t r = 0;
            for (const auto& v : a)
                r ^= types::as_int(v);
            return types::make_int_or_promote(r);
        },
        pure_general(255, "(int ...) -> int",
                     "SRFI-151 bitwise-xor: fold a variadic exclusive or; no args is 0."));

    register_prim(
        add, ev, "arithmetic-shift",
        [](const auto& a) -> EvalValue {
            if (a.size() < 2 || !types::is_int(a[0]) || !types::is_int(a[1]))
                return types::make_void();
            const std::int64_t n = types::as_int(a[0]);
            const std::int64_t k = types::as_int(a[1]);
            if (k >= 63)
                return types::make_int_or_promote(n == 0 ? 0
                                                         : (n > 0 ? (1LL << 62) : -(1LL << 62)));
            if (k > 0)
                return types::make_int_or_promote(n << k);
            if (k <= -63)
                return types::make_int(n < 0 ? -1 : 0);
            // Arithmetic right shift on a negative value keeps the sign.
            const std::int64_t s = -k;
            if (n >= 0)
                return types::make_int(n >> s);
            return types::make_int(~(~n >> s));
        },
        pure_general(2, "(int int) -> int",
                     "SRFI-151 arithmetic-shift: shift left by n>0, arithmetic right by n<0."));

    register_prim(
        add, ev, "bit-count",
        [](const auto& a) -> EvalValue {
            if (a.empty() || !types::is_int(a[0]))
                return types::make_void();
            const std::int64_t n = types::as_int(a[0]);
            const auto u = static_cast<std::uint64_t>(n);
            // SRFI-151 / R6RS: a negative count is -1 minus the zero-bit
            // count of the complement, so (bit-count -1) is -1.
            return types::make_int(static_cast<std::int64_t>(
                n >= 0 ? std::popcount(u) : -1 - static_cast<std::int64_t>(std::popcount(~u))));
        },
        pure_general(1, "(int) -> int",
                     "SRFI-151 bit-count: number of set bits (negative: two's complement)."));

    register_prim(
        add, ev, "integer-length",
        [](const auto& a) -> EvalValue {
            if (a.empty() || !types::is_int(a[0]))
                return types::make_void();
            const std::int64_t n = types::as_int(a[0]);
            const auto u = static_cast<std::uint64_t>(n);
            if (n >= 0)
                return types::make_int(static_cast<std::int64_t>(64 - std::countl_zero(u)));
            return types::make_int(static_cast<std::int64_t>(64 - std::countl_zero(~u)));
        },
        pure_general(1, "(int) -> int",
                     "SRFI-151 integer-length: bits needed for the magnitude (0 for 0)."));
}

} // namespace aura::compiler::primitives_detail
