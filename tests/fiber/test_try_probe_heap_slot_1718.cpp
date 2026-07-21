// @category: unit
// @reason: Issue #1718 — synthesize:optimize try_probe must reuse a fixed
// string_heap slot, not push_back every probe (unbounded heap growth).
//
//   AC1: source cites #1718 and uses probe_slot reuse
//   AC2: try_probe assigns string_heap_[probe_slot] (not only push_back)
//   AC3: no unbounded push_back(call_src) without slot reuse in fitness
//   AC4: synthesize:optimize still registered

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string strip_line_comments(std::string_view win) {
    std::string code;
    code.reserve(win.size());
    for (size_t i = 0; i < win.size();) {
        if (i + 1 < win.size() && win[i] == '/' && win[i + 1] == '/') {
            while (i < win.size() && win[i] != '\n')
                ++i;
            continue;
        }
        code.push_back(win[i++]);
    }
    return code;
}

} // namespace

int main() {
    // ── AC1–AC3: source audit ──
    {
        std::println("\n--- AC1–AC3: probe_slot reuse ---");
        const char* candidates[] = {
            "src/compiler/evaluator_primitives_agent.cpp",
            "../src/compiler/evaluator_primitives_agent.cpp",
        };
        std::string src;
        for (const char* p : candidates) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent primitives");
        if (!src.empty()) {
            CHECK(src.find("Issue #1718") != std::string::npos, "cites #1718");
            CHECK(src.find("probe_slot") != std::string::npos, "probe_slot present");

            auto pos = src.find("add(\"synthesize:optimize\"");
            CHECK(pos != std::string::npos, "found synthesize:optimize");
            if (pos != std::string::npos) {
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 25000 : end - pos);
                auto code = strip_line_comments(win);
                // try_probe region
                auto tp = code.find("try_probe");
                CHECK(tp != std::string::npos, "try_probe present");
                if (tp != std::string::npos) {
                    auto region = code.substr(tp, 900);
                    CHECK(region.find("probe_slot") != std::string::npos,
                          "try_probe uses probe_slot");
                    CHECK(region.find("push_back(call_src)") != std::string::npos,
                          "first-time push still present");
                    CHECK(region.find("string_heap_[probe_slot]") != std::string::npos,
                          "in-place assign to slot");
                    CHECK(region.find("make_string(probe_slot)") != std::string::npos,
                          "eval uses probe_slot");
                }
            }
        }
    }

    // ── AC4: primitive registered ──
    {
        std::println("\n--- AC4: synthesize:optimize registered ---");
        CompilerService cs;
        auto r = cs.eval("(procedure? synthesize:optimize)");
        CHECK(r.has_value(), "eval ok");
        if (r) {
            using aura::compiler::types::as_bool;
            using aura::compiler::types::is_bool;
            CHECK(is_bool(*r) && as_bool(*r), "synthesize:optimize is a procedure");
        }
    }

    std::println("\n=== test_try_probe_heap_slot_1718: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
