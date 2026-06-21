// evaluator_primitives_control.cpp — P0 step 29: while primitive
// extracted from init_pair_primitives().

module;

#include <functional>
#include <span>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

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
