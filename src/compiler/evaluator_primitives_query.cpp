// evaluator_primitives_query.cpp — P0 step 8: standalone query primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>
#include "runtime_shared.h"

module aura.compiler.evaluator;

import std;
import aura.core.type;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimFn = std::function<EvalValue(std::span<const EvalValue>)>;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;
using ModulePathResolver = std::function<std::string(const std::string&)>;

using namespace types;

void register_query_primitives(PrimRegistrar add, std::pmr::vector<Pair>& pairs,
                               std::pmr::vector<std::string>& string_heap, void*& type_registry,
                               ModulePathResolver resolve_module_path) {

    add("query:module-exports", [&pairs, &string_heap, resolve_module_path](
                                   std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_void();
        auto path = string_heap[idx];
        auto resolved = resolve_module_path(path);
        if (resolved.empty())
            return make_void();
        std::ifstream f(resolved);
        if (!f.is_open())
            return make_void();
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        std::vector<std::string> exports;
        std::size_t pos = 0;
        while (pos < content.size()) {
            auto export_pos = content.find("(export", pos);
            if (export_pos == std::string::npos)
                break;
            if (export_pos > 0) {
                char prev = content[export_pos - 1];
                if (prev != '\n' && prev != ' ' && prev != '\t' && prev != '(') {
                    pos = export_pos + 1;
                    continue;
                }
            }
            auto sym_start = export_pos + 7;
            while (sym_start < content.size() &&
                   (content[sym_start] == ' ' || content[sym_start] == '\t' ||
                    content[sym_start] == '\n' || content[sym_start] == '\r')) {
                ++sym_start;
            }
            std::size_t i = sym_start;
            while (i < content.size() && content[i] != ')') {
                if (content[i] == ' ' || content[i] == '\t' || content[i] == '\n' ||
                    content[i] == '\r') {
                    ++i;
                    continue;
                }
                if (content[i] == ';') {
                    while (i < content.size() && content[i] != '\n')
                        ++i;
                    continue;
                }
                std::size_t s = i;
                while (i < content.size()) {
                    char c = content[i];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '?' || c == '!' || c == '<' ||
                        c == '>' || c == '=' || c == '*' || c == '+' || c == '-' || c == '/' ||
                        c == '.' || c == '$') {
                        ++i;
                    } else {
                        break;
                    }
                }
                if (i > s) {
                    exports.push_back(content.substr(s, i - s));
                } else {
                    ++i;
                }
            }
            break;
        }
        EvalValue lst = make_void();
        for (auto it = exports.rbegin(); it != exports.rend(); ++it) {
            auto sidx = string_heap.size();
            string_heap.push_back(*it);
            auto pid = pairs.size();
            pairs.push_back({make_string(sidx), lst});
            lst = make_pair(pid);
        }
        return lst;
    });

    add("query:schema", [&string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_bool(false);
        std::string name = string_heap[idx];
        if (!type_registry) {
            type_registry = new aura::core::TypeRegistry();
        }
        auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
        if (!treg)
            return make_bool(false);
        auto ty = treg->lookup_type(name);
        if (!ty.valid())
            return make_bool(false);
        std::string schema = "{\"title\": \"" + name + "\"";
        schema += ", \"type\": \"" +
                  std::string(treg->tag_of(ty) == aura::core::TypeTag::MODULE ? "object" : "any") +
                  "\"}";
        auto sidx = string_heap.size();
        string_heap.push_back(schema);
        return make_string(sidx);
    });
}

} // namespace aura::compiler::primitives_detail