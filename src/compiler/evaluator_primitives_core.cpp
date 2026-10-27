// evaluator_primitives_core.cpp — P0 step 1: stateless R5RS-style primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.
//
// aura.compiler.evaluator module partition.
// Registration uses a callback to avoid friending or exposing Evaluator
// internals — mirrors ffi_primitives_impl.cpp / adt_runtime_impl.cpp.

module;

#include <cmath>

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

void register_type_and_char_primitives(PrimRegistrar add) {
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
    add("char-alphabetic?", [](const auto& a) {
        if (a.empty() || !types::is_int(a[0]))
            return types::make_bool(false);
        auto c = types::as_int(a[0]);
        return types::make_bool((c >= 65 && c <= 90) || (c >= 97 && c <= 122));
    });
    add("char-numeric?", [](const auto& a) {
        if (a.empty() || !types::is_int(a[0]))
            return types::make_bool(false);
        return types::make_bool(types::as_int(a[0]) >= 48 && types::as_int(a[0]) <= 57);
    });
    add("char-whitespace?", [](const auto& a) {
        if (a.empty() || !types::is_int(a[0]))
            return types::make_bool(false);
        auto c = types::as_int(a[0]);
        return types::make_bool(c == 32 || (c >= 9 && c <= 13));
    });
    add("char-upcase", [](const auto& a) {
        if (a.empty() || !types::is_int(a[0]))
            return types::make_bool(false);
        auto c = types::as_int(a[0]);
        if (c >= 97 && c <= 122)
            return types::make_int(c - 32);
        return types::make_int(c);
    });
    add("char-downcase", [](const auto& a) {
        if (a.empty() || !types::is_int(a[0]))
            return types::make_bool(false);
        auto c = types::as_int(a[0]);
        if (c >= 65 && c <= 90)
            return types::make_int(c + 32);
        return types::make_int(c);
    });
}

} // namespace aura::compiler::primitives_detail
