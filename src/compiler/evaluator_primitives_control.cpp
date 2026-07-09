// evaluator_primitives_control.cpp — P0 step 29: while primitive
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

void register_control_primitives(PrimRegistrar add, Evaluator& ev) {

    add("while", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_closure(a[0]) || !types::is_closure(a[1]))
            return make_void();
        auto pred_cid = types::as_closure_id(a[0]);
        auto body_cid = types::as_closure_id(a[1]);
        for (;;) {
            auto pred_result = ev.apply_closure(pred_cid, {});
            if (!pred_result)
                break;
            auto& val = *pred_result;
            bool cont = types::is_bool(val)  ? types::as_bool(val)
                        : types::is_int(val) ? types::as_int(val) != 0
                                             : false;
            if (!cont)
                break;
            (void)ev.apply_closure(body_cid, {});
        }
        return make_void();
    });
}

} // namespace aura::compiler::primitives_detail
