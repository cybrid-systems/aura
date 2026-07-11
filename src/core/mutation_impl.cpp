// mutation_impl.cpp — implementation of exports from aura.core.mutation.
//
// Issue #279 follow-up #4: custom-predicate registry
// (register_custom_predicate / lookup_custom_predicate_type).
// Lives in aura.core.mutation so both the evaluator module
// (Aura primitive) and the type_checker module (analyzer)
// can see the same definition. No cross-module symbol
// resolution problems.

module;

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

module aura.core.mutation;

import std;

namespace aura::ast {
namespace mutation {

    // Process-global registry. Same shape as the primitive-detail
    // registry I had before; lives here so it's in the aura.core
    // module's purview.
    // Issue #1296 (P0): concurrent register + lookup from JIT workers /
    // multi-fiber is a data race on bare unordered_map. Guard both paths.
    std::unordered_map<std::string, std::string>& custom_predicate_registry() {
        static std::unordered_map<std::string, std::string> m;
        return m;
    }

    std::mutex& custom_predicate_mtx() {
        static std::mutex mtx;
        return mtx;
    }

    void register_custom_predicate(const std::string& pred_name, const std::string& type_name) {
        std::lock_guard<std::mutex> lock(custom_predicate_mtx());
        custom_predicate_registry()[pred_name] = type_name;
    }

    std::optional<std::string> lookup_custom_predicate_type(const std::string& pred_name) {
        std::lock_guard<std::mutex> lock(custom_predicate_mtx());
        auto& m = custom_predicate_registry();
        auto it = m.find(pred_name);
        if (it == m.end())
            return std::nullopt;
        return it->second;
    }

} // namespace mutation
} // namespace aura::ast
