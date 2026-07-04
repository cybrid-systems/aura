// evaluator_primitives_core.cpp — P0 step 1: stateless R5RS-style primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.
//
// aura.compiler.evaluator module partition.
// Registration uses a callback to avoid friending or exposing Evaluator
// internals — mirrors ffi_primitives_impl.cpp / adt_runtime_impl.cpp.

module;


module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

void register_type_and_char_primitives(PrimRegistrar add) {
    // ── Type predicates ──────────────────────────────────────────
    add("integer?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_int(a[0]));
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
        // Symbols are interned during parsing and not represented as
        // first-class EvalValue values; always return false.
        return types::make_bool(false);
    });
    add("procedure?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_closure(a[0]) || types::is_primitive(a[0]));
    });
    add("void?", [](const auto& a) {
        if (a.empty())
            return types::make_bool(false);
        return types::make_bool(types::is_void(a[0]));
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
