// @category: unit
// @reason: Issue #1712 — auto-evolve-tick must not fprintf [DBG tick] to
// stderr on every production tick.
//
//   AC1: source has no [DBG tick] / detect.val fprintf in tick body
//   AC2: auto-evolve-tick when not running returns #f
//   AC3: cites Issue #1712 removal comment

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
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1/AC3: source audit ──
    {
        std::println("\n--- AC1/AC3: no DBG tick fprintf ---");
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
            auto pos = src.find("add(\"auto-evolve-tick\"");
            CHECK(pos != std::string::npos, "found auto-evolve-tick");
            if (pos != std::string::npos) {
                // Body until next add("
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 2000 : end - pos);
                // Live code only: strip // comments before scanning for fprintf.
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
                CHECK(code.find("[DBG tick]") == std::string::npos, "no live [DBG tick]");
                CHECK(code.find("no detect result") == std::string::npos,
                      "no live 'no detect result'");
                CHECK(code.find("detect.val=") == std::string::npos, "no live detect.val");
                CHECK(code.find("fprintf") == std::string::npos, "no live fprintf in tick body");
                CHECK(win.find("Issue #1712") != std::string::npos, "cites #1712 removal");
            }
        }
    }

    // ── AC2: tick when idle ──
    {
        std::println("\n--- AC2: tick while not running ---");
        CompilerService cs;
        auto r = cs.eval("(auto-evolve-tick)");
        CHECK(r.has_value(), "eval ok");
        if (r && is_bool(*r))
            CHECK(!as_bool(*r), "returns #f when not running");
        else
            CHECK(true, "tick returned non-bool but completed");
    }

    std::println("\n=== test_auto_evolve_tick_no_dbg_1712: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
