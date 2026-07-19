// @category: unit
// @reason: Issue #1857 — compile_02.cpp must not re-import
// hardware_backend / sv_ir (copy-paste duplicate imports).
//
//   AC1: source cites #1857; each import line appears once
//   AC2: all compile_0*.cpp module blocks free of duplicate imports

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

import std;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> import_lines(const std::string& src) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < src.size()) {
        auto nl = src.find('\n', i);
        auto line = src.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        if (line.starts_with("import "))
            out.push_back(line);
        if (nl == std::string::npos)
            break;
        i = nl + 1;
    }
    return out;
}

} // namespace

int main() {
    // ── AC1: compile_02 ──
    {
        std::println("\n--- AC1: compile_02 no duplicate imports ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile_02.cpp",
                              "../src/compiler/evaluator_primitives_compile_02.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_02.cpp");
        CHECK(src.find("#1857") != std::string::npos, "cites #1857");
        auto imps = import_lines(src);
        CHECK(!imps.empty(), "has import lines");
        std::unordered_map<std::string, int> counts;
        for (const auto& l : imps)
            counts[l]++;
        bool any_dup = false;
        for (const auto& [line, n] : counts) {
            if (n > 1) {
                any_dup = true;
                std::println("  FAIL: {} appears {} times", line, n);
            }
        }
        CHECK(!any_dup, "no duplicate import lines in compile_02");
        CHECK(src.find("import aura.compiler.hardware_backend;") != std::string::npos,
              "still imports hardware_backend once");
        CHECK(src.find("import aura.compiler.sv_ir;") != std::string::npos,
              "still imports sv_ir once");
    }

    // ── AC2: audit all compile_0*.cpp ──
    {
        std::println("\n--- AC2: all compile_0*.cpp free of dup imports ---");
        const char* files[] = {
            "src/compiler/evaluator_primitives_compile_00.cpp",
            "src/compiler/evaluator_primitives_compile_01.cpp",
            "src/compiler/evaluator_primitives_compile_02.cpp",
            "src/compiler/evaluator_primitives_compile_03.cpp",
            "src/compiler/evaluator_primitives_compile_04.cpp",
            "src/compiler/evaluator_primitives_compile_05.cpp",
            "src/compiler/evaluator_primitives_compile_06.cpp",
            "src/compiler/evaluator_primitives_compile_07.cpp",
        };
        for (const char* rel : files) {
            std::string src = read_file(rel);
            if (src.empty())
                src = read_file((std::string("../") + rel).c_str());
            CHECK(!src.empty(), std::format("read {}", rel));
            auto imps = import_lines(src);
            std::unordered_map<std::string, int> counts;
            for (const auto& l : imps)
                counts[l]++;
            bool ok = true;
            for (const auto& [line, n] : counts) {
                if (n > 1) {
                    ok = false;
                    std::println("  FAIL: {} : {} x{}", rel, line, n);
                }
            }
            CHECK(ok, std::format("{} no duplicate imports", rel));
        }
    }

    std::println("\n=== test_compile02_no_dup_imports_1857: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
