// service_module_source.hh - read the whole aura.compiler.service module source.
//
// #5242: the module interface keeps declarations; the member bodies live in
// implementation units (service_*.cpp). Source-text checks that pin a sentence
// from a body read the interface plus every implementation unit, so the pin
// keeps matching wherever the definition currently sits.
#pragma once

#include <fstream>
#include <iterator>
#include <string>

namespace aura_test {

inline std::string read_service_text(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::string service_module_source() {
    static constexpr const char* kUnits[] = {
        "src/compiler/service.ixx",          "src/compiler/service_ctor.cpp",
        "src/compiler/service_dirty.cpp",    "src/compiler/service_eval.cpp",
        "src/compiler/service_eval_ir.cpp",  "src/compiler/service_exec_jit.cpp",
        "src/compiler/service_jit.cpp",      "src/compiler/service_relower.cpp",
        "src/compiler/service_snapshot.cpp", "src/compiler/service_typed_mutate.cpp",
    };
    std::string all;
    for (const char* unit : kUnits) {
        std::string text = read_service_text(unit);
        if (text.empty())
            text = read_service_text(std::string("../") + unit);
        all += text;
        if (!all.empty() && all.back() != '\n')
            all += '\n';
    }
    return all;
}

} // namespace aura_test
