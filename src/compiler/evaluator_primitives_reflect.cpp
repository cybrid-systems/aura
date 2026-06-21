// evaluator_primitives_reflect.cpp — P0 step 7: reflect/type/keyword primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <cstdint>
#include <functional>
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

using namespace types;

namespace {

const char* infer_type_name(const EvalValue& v) {
    if (is_float(v))
        return "Float";
    if (is_hash(v))
        return "Hash";
    if (is_vector(v))
        return "Vector";
    if (is_string(v))
        return "String";
    if (is_pair(v))
        return "Pair";
    if (is_cell(v))
        return "Cell";
    if (is_closure(v))
        return "Closure";
    if (is_bool(v))
        return "Bool";
    // Backward compat: int 0/1 was historically treated as Bool
    if (is_int(v) && (as_int(v) == 0 || as_int(v) == 1))
        return "Bool";
    if (is_int(v))
        return "Int";
    if (is_keyword(v))
        return "Keyword";
    if (is_void(v))
        return "Void";
    return "Unknown";
}

} // namespace

void register_reflect_and_type_primitives(
    PrimRegistrar add, std::pmr::vector<Pair>& pairs, std::pmr::vector<std::string>& string_heap,
    std::vector<std::string>& keyword_table, void*& type_registry) {

    add("type-of", [&string_heap](const auto& a) -> EvalValue {
        if (a.empty())
            return make_int(0);
        auto type_name = infer_type_name(a[0]);
        auto id = string_heap.size();
        string_heap.push_back(type_name);
        return make_string(id);
    });

    add("reflect-type", [&pairs, &string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_void();
        std::string name = string_heap[idx];
        auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
        if (!treg)
            return make_void();
        auto ty = treg->lookup_type(name);
        if (!ty.valid())
            return make_void();
        std::vector<EvalValue> out;
        auto kind_idx = string_heap.size();
        std::string kind;
        switch (treg->tag_of(ty)) {
            case aura::core::TypeTag::MODULE:
                kind = "module";
                break;
            case aura::core::TypeTag::RECORD:
                kind = "record";
                break;
            case aura::core::TypeTag::VARIANT:
                kind = "variant";
                break;
            case aura::core::TypeTag::LINEAR:
                kind = "linear";
                break;
            case aura::core::TypeTag::FORALL:
                kind = "forall";
                break;
            case aura::core::TypeTag::FUNC:
                kind = "function";
                break;
            default:
                kind = "scalar";
                break;
        }
        string_heap.push_back(kind);
        out.push_back(make_string(kind_idx));
        auto name_idx = string_heap.size();
        string_heap.push_back(name);
        out.push_back(make_string(name_idx));
        if (treg->tag_of(ty) == aura::core::TypeTag::MODULE) {
            auto* mt = treg->module_of(ty);
            if (mt) {
                for (auto& [mname, mtype] : mt->members) {
                    auto mname_idx = string_heap.size();
                    string_heap.push_back(mname);
                    auto mtype_name = std::string(treg->name_of(mtype));
                    auto mtype_idx = string_heap.size();
                    string_heap.push_back(mtype_name);
                    auto pid = pairs.size();
                    pairs.push_back({make_string(mname_idx), make_string(mtype_idx)});
                    out.push_back(make_pair(pid));
                }
            }
        } else if (treg->tag_of(ty) == aura::core::TypeTag::RECORD) {
            auto* rt = treg->record_of(ty);
            if (rt) {
                for (auto& [fname, ftype] : rt->fields) {
                    auto fname_idx = string_heap.size();
                    string_heap.push_back(fname);
                    auto ftype_name = std::string(treg->name_of(ftype));
                    auto ftype_idx = string_heap.size();
                    string_heap.push_back(ftype_name);
                    auto pid = pairs.size();
                    pairs.push_back({make_string(fname_idx), make_string(ftype_idx)});
                    out.push_back(make_pair(pid));
                }
            }
        }
        EvalValue lst = make_void();
        for (auto it = out.rbegin(); it != out.rend(); ++it) {
            auto pid = pairs.size();
            pairs.push_back({*it, lst});
            lst = make_pair(pid);
        }
        return lst;
    });

    add("reflect-members", [&pairs, &string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_void();
        std::string name = string_heap[idx];
        auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
        if (!treg)
            return make_void();
        auto ty = treg->lookup_type(name);
        if (!ty.valid())
            return make_void();
        std::vector<std::pair<std::string, std::string>> members;
        if (treg->tag_of(ty) == aura::core::TypeTag::MODULE) {
            auto* mt = treg->module_of(ty);
            if (mt) {
                for (auto& [mname, mtype] : mt->members) {
                    members.emplace_back(mname, std::string(treg->name_of(mtype)));
                }
            }
        } else if (treg->tag_of(ty) == aura::core::TypeTag::RECORD) {
            auto* rt = treg->record_of(ty);
            if (rt) {
                for (auto& [fname, ftype] : rt->fields) {
                    members.emplace_back(fname, std::string(treg->name_of(ftype)));
                }
            }
        } else if (treg->tag_of(ty) == aura::core::TypeTag::VARIANT) {
            auto* vt = treg->variant_of(ty);
            if (vt) {
                for (auto& [vname, ftypes] : vt->variants) {
                    std::string joined;
                    for (auto& ft : ftypes) {
                        if (!joined.empty())
                            joined += " ";
                        joined += std::string(treg->name_of(ft));
                    }
                    members.emplace_back(vname, joined);
                }
            }
        }
        EvalValue lst = make_void();
        for (auto it = members.rbegin(); it != members.rend(); ++it) {
            auto nidx = string_heap.size();
            string_heap.push_back(it->first);
            auto tidx = string_heap.size();
            string_heap.push_back(it->second);
            auto pid = pairs.size();
            pairs.push_back({make_string(nidx), make_string(tidx)});
            auto ppid = pairs.size();
            pairs.push_back({make_pair(pid), lst});
            lst = make_pair(ppid);
        }
        return lst;
    });

    add("reflect-module-exports", [&pairs, &string_heap, &type_registry](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= string_heap.size())
            return make_void();
        std::string name = string_heap[idx];
        auto* treg = static_cast<aura::core::TypeRegistry*>(type_registry);
        if (!treg)
            return make_void();
        auto ty = treg->lookup_type(name);
        if (!ty.valid())
            return make_void();
        if (treg->tag_of(ty) != aura::core::TypeTag::MODULE)
            return make_void();
        auto* mt = treg->module_of(ty);
        if (!mt)
            return make_void();
        EvalValue lst = make_void();
        for (auto it = mt->members.rbegin(); it != mt->members.rend(); ++it) {
            auto nidx = string_heap.size();
            string_heap.push_back(it->first);
            auto pid = pairs.size();
            auto pid2 = pairs.size();
            pairs.push_back({make_string(nidx), lst});
            lst = make_pair(pid2);
            lst = make_pair(pid);
        }
        return lst;
    });

    add("type?", [&string_heap](const auto& a) -> EvalValue {
        if (a.size() < 2 || !is_string(a[1]))
            return make_int(0);
        auto val_type = infer_type_name(a[0]);
        auto expected_idx = as_string_idx(a[1]);
        if (expected_idx >= string_heap.size())
            return make_int(0);
        auto& expected = string_heap[expected_idx];
        return make_int(val_type == expected ? 1 : 0);
    });

    add("keyword?", [](std::span<const EvalValue> a) {
        return make_bool(a.size() >= 1 && is_keyword(a[0]) && !is_string(a[0]));
    });

    add("keyword->string", [&string_heap, &keyword_table](std::span<const EvalValue> a) {
        if (a.size() < 1 || !is_keyword(a[0]))
            return make_void();
        auto kidx = as_keyword_idx(a[0]);
        if (kidx >= keyword_table.size())
            return make_void();
        auto kw = keyword_table[kidx];
        auto sname = kw.substr(1);
        auto sid = string_heap.size();
        string_heap.push_back(sname);
        return make_string(sid);
    });
}

} // namespace aura::compiler::primitives_detail