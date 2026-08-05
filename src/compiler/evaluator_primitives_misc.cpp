// evaluator_primitives_misc.cpp — P0 step 28: current-time / arena-offset
// primitives; Issue #2655: current-time-ms + monotonic-ms for sub-second
// denseness metrology.
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include "runtime_shared.h"

#include <chrono>
#include <ctime>

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

void register_misc_primitives(PrimRegistrar add, Evaluator& /*ev*/) {

    // Wall-clock seconds since Unix epoch (legacy; coarse for denseness).
    add("current-time", [](const auto&) -> EvalValue {
        return make_int(static_cast<std::int64_t>(std::time(nullptr)));
    });

    // Issue #2655: wall-clock milliseconds since Unix epoch (system_clock).
    // Suitable for timestamps; for elapsed prefer monotonic-ms (steady_clock
    // is not subject to wall-clock steps).
    add("current-time-ms", [](const auto&) -> EvalValue {
        using clock = std::chrono::system_clock;
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
                .count();
        return make_int(static_cast<std::int64_t>(ms));
    });

    // Issue #2655: steady_clock milliseconds (process-relative origin).
    // Preferred for denseness elapsed / ops-s metrology on short pure loops.
    add("monotonic-ms", [](const auto&) -> EvalValue {
        using clock = std::chrono::steady_clock;
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch())
                .count();
        return make_int(static_cast<std::int64_t>(ms));
    });

    add("arena-offset", [](const auto&) -> EvalValue {
        // g_tl_arena is thread-local TLarena declared in runtime_shared.h
        return make_int(static_cast<int64_t>(g_tl_arena.offset));
    });
}

} // namespace aura::compiler::primitives_detail
