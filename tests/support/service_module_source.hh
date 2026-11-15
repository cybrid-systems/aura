// service_module_source.hh - read the whole aura.compiler.service module source.
//
// #5242: the module interface keeps declarations; the member bodies live in
// implementation units (service_*.cpp). Source-text checks that pin a sentence
// from a body read the interface plus every implementation unit, so the pin
// keeps matching wherever the definition currently sits.
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace aura_test {

inline std::string read_service_text(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

inline std::string service_module_source() {
    // Interface first, then every service_*.cpp unit (globbing keeps this
    // helper correct as #5242 moves more bodies out).
    // Units first, interface last: a source-cite row that finds a function by
    // its bare name must land on the out-of-class definition, not the
    // in-class declaration.
    std::vector<std::string> units;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("src/compiler", ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.rfind("service_", 0) == 0 && it->path().extension() == ".cpp")
            units.push_back("src/compiler/" + name);
    }
    std::sort(units.begin(), units.end());
    units.push_back("src/compiler/service.ixx");
    std::string all;
    for (const auto& unit : units) {
        std::string text = read_service_text(unit);
        if (text.empty())
            text = read_service_text(std::string("../") + unit);
        all += text;
        if (!all.empty() && all.back() != '\n')
            all += '\n';
    }
    // #5242: out-of-class definitions are spelled "Ret CompilerService::fn(";
    // rows written for the old in-class body pin "Ret fn(". Drop the
    // qualifier (except in &CompilerService:: / ::CompilerService:: uses,
    // which are real references) so the same sentence keeps matching.
    std::string normalized;
    normalized.reserve(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all.compare(i, 17, "CompilerService::") == 0 &&
            (i == 0 || (all[i - 1] != '&' && all[i - 1] != ':'))) {
            i += 16;
            continue;
        }
        normalized.push_back(all[i]);
    }
    return normalized;
}

} // namespace aura_test
