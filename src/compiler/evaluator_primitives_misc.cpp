// evaluator_primitives_misc.cpp — P0 step 28: current-time / arena-offset primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <ctime>
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

void register_misc_primitives(PrimRegistrar add, Evaluator& ev) {

    add("current-time", [](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(std::time(nullptr)));
    });

    add("arena-offset", [](const auto&) -> EvalValue {
        // g_tl_arena is thread-local TLarena declared in runtime_shared.h
        return make_int(static_cast<int64_t>(g_tl_arena.offset));
    });
}

} // namespace aura::compiler::primitives_detail
