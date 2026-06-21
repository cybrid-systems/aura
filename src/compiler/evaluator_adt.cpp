// evaluator_adt.cpp — P1-q: ADT ctor registration + make_merr helper
// extracted from evaluator_impl.cpp.

module;

#include <string>

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler {

using types::EvalValue;
using namespace types;

EvalValue Evaluator::make_merr(const std::string& k, const std::string& m) {
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

void Evaluator::register_adt_ctor(const std::string& ctor_name, types::EvalValue tag_str,
                                  int field_count) {
    const auto slot = primitives_.slot_count();
    auto body = [this, tag_str](const auto& args) -> EvalValue {
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
    types::EvalValue rest = make_void();
    auto cid = static_cast<std::uint64_t>(pairs_.size());
    pairs_.push_back({tag_str, rest});
    return make_pair(cid);
}

} // namespace aura::compiler