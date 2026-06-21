// evaluator_primitives_mutation.cpp — P0 step 31: mutation-count / rollback primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <string>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.ast;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using namespace types;

void register_mutation_primitives(PrimRegistrar add, Evaluator& ev) {

    add("mutation-count", [&ev](const auto&) {
        if (!ev.workspace_flat_)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->mutation_count()));
    });

    add("mutation-history", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_)
            return make_int(0);
        auto node = static_cast<aura::ast::NodeId>(as_int(a[0]));
        auto hist = ev.workspace_flat_->mutation_history(node);
        EvalValue result = make_void();
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            auto& rec = *it;
            auto sid = ev.string_heap_.size();
            ev.string_heap_.push_back(std::format(
                "[{}] {}: {}{}", rec.mutation_id, rec.operator_name, rec.summary,
                rec.status == aura::ast::MutationStatus::RolledBack ? " [rolled-back]" : ""));
            auto pair_id = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sid), result});
            result = make_pair(pair_id);
        }
        return result;
    });

    add("rollback", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_)
            return make_bool(false);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_bool(ev.workspace_flat_->rollback(mid));
    });

    add("rollback-since", [&ev](std::span<const EvalValue> a) {
        if (a.empty() || !is_int(a[0]) || !ev.workspace_flat_)
            return make_int(0);
        auto mid = static_cast<std::uint64_t>(as_int(a[0]));
        return make_int(static_cast<std::int64_t>(ev.workspace_flat_->rollback_since(mid)));
    });
}

} // namespace aura::compiler::primitives_detail
