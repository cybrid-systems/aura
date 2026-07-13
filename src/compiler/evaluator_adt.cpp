// evaluator_adt.cpp — P1-q: ADT ctor registration + make_merr helper
// aura.compiler.evaluator module partition.

module;


module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
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

EvalValue Evaluator::make_merr(const std::string& k, const std::string& m) {
    // Issue #1397: merr construction interleaves 2x string_heap_ + 2x
    // pairs_ push_backs; the two-pair construction (mp / kp) must
    // see consistent indices across fiber:spawn workers.
    std::lock_guard<std::mutex> lock(alloc_storage_lock_);
    auto mi = string_heap_.size();
    string_heap_.push_back(m);
    auto ki = string_heap_.size();
    string_heap_.push_back(k);
    auto mp = make_pair(pairs_.size());
    pairs_.push_back({make_string(mi), EvalValue(0)});
    auto kp = make_pair(pairs_.size());
    pairs_.push_back({make_string(ki), mp});
    return kp;
}

void Evaluator::sync_workspace_adt_registry() {
    if (workspace_adt_sync_fn_ && compiler_service_)
        workspace_adt_sync_fn_(compiler_service_);
}

void Evaluator::register_adt_ctor(const std::string& ctor_name, types::EvalValue tag_str,
                                  int field_count) {
    const auto slot = primitives_.slot_count();
    auto body = [this, tag_str](const auto& args) -> EvalValue {
        // Issue #1397: per-arg list construction (push_back loop + tail)
        // must be atomic so the resulting ADT instance has a stable
        // pair index across concurrent fiber:spawn workers.
        std::lock_guard<std::mutex> lock(alloc_storage_lock_);
        types::EvalValue rest = make_void();
        for (auto it = args.rbegin(); it != args.rend(); ++it) {
            auto pid = static_cast<std::uint64_t>(pairs_.size());
            pairs_.push_back({*it, rest});
            rest = make_pair(pid);
        }
        auto pid = static_cast<std::uint64_t>(pairs_.size());
        pairs_.push_back({tag_str, rest});
        return make_pair(pid);
    };
    adt_runtime_.register_dynamic_ctor(prim_registrar(), ctor_name, std::move(body), field_count,
                                       slot);
}

types::EvalValue Evaluator::make_adt_zero_arg_ctor(types::EvalValue tag_str) {
    // Issue #1397: zero-arg ADT ctor pairs_ push_back atomic for
    // stable pair index across fiber:spawn workers.
    std::lock_guard<std::mutex> lock(alloc_storage_lock_);
    types::EvalValue rest = make_void();
    auto cid = static_cast<std::uint64_t>(pairs_.size());
    pairs_.push_back({tag_str, rest});
    return make_pair(cid);
}

} // namespace aura::compiler
