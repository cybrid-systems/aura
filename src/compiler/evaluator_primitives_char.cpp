// evaluator_primitives_char.cpp — P0 step 30: char? / string->list / read-line primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

void register_char_primitives(PrimRegistrar add, Evaluator& ev) {

    add("char?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        return make_bool(is_int(a[0]));
    });

    add("char->integer", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });

    add("integer->char", [](const auto& a) {
        if (a.empty() || !is_int(a[0]))
            return make_int(0);
        return a[0];
    });

    add("string->list", [&ev](std::span<const EvalValue> a) {
        if (a.empty())
            return make_void();
        std::string s;
        if (is_string(a[0])) {
            auto idx = as_string_idx(a[0]);
            if (idx < ev.string_heap_.size())
                s = ev.string_heap_[idx];
        } else if (is_int(a[0])) {
            s = std::to_string(as_int(a[0]));
        }
        EvalValue result = make_void();
        for (auto it = s.rbegin(); it != s.rend(); ++it) {
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back(
                {make_int(static_cast<std::int64_t>(static_cast<unsigned char>(*it))), result});
            result = make_pair(pid);
        }
        return result;
    });

    add("list->string", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || (!is_pair(a[0]) && !is_void(a[0])))
            return make_int(0);
        std::string result;
        auto v = a[0];
        while (is_pair(v)) {
            auto idx = as_pair_idx(v);
            if (idx >= ev.pairs_.size())
                break;
            auto car = ev.pairs_[idx].car;
            if (is_int(car))
                result.push_back(static_cast<char>(as_int(car)));
            v = ev.pairs_[idx].cdr;
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    add("read-line", [&ev](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    add("eof-object?", [](const auto& a) {
        if (a.empty())
            return make_bool(false);
        // EOF is represented as void (the same as when read-line returns empty)
        return make_bool(is_void(a[0]));
    });
}

} // namespace aura::compiler::primitives_detail
