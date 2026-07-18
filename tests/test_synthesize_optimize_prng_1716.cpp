// @category: unit
// @reason: Issue #1716 — synthesize:optimize must not use std::rand()
// (non-thread-safe); use thread_local mt19937 instead.
//
//   AC1: source has no std::rand / RAND_MAX in synthesize:optimize
//   AC2: source uses thread_local mt19937 / agent_prng
//   AC3: cites Issue #1716
//   AC4: synthesize:optimize primitive still registered (callable)

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

// Live code only: strip // line comments before scanning for banned APIs.
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
    // ── AC1/AC2/AC3: source audit ──
    {
        std::println("\n--- AC1/AC2/AC3: thread_local PRNG, no std::rand ---");
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
            CHECK(src.find("Issue #1716") != std::string::npos, "cites #1716");
            CHECK(src.find("thread_local std::mt19937") != std::string::npos,
                  "thread_local mt19937");
            CHECK(src.find("agent_prng") != std::string::npos, "agent_prng helper");
            CHECK(src.find("agent_rand_below") != std::string::npos, "agent_rand_below");
            CHECK(src.find("agent_rand_unit") != std::string::npos, "agent_rand_unit");

            auto pos = src.find("add(\"synthesize:optimize\"");
            CHECK(pos != std::string::npos, "found synthesize:optimize");
            if (pos != std::string::npos) {
                // Body until next top-level add(
                auto end = src.find("\n    add(\"", pos + 10);
                auto win = src.substr(pos, end == std::string::npos ? 12000 : end - pos);
                auto code = strip_line_comments(win);
                CHECK(code.find("std::rand") == std::string::npos, "no live std::rand");
                CHECK(code.find("RAND_MAX") == std::string::npos, "no live RAND_MAX");
                CHECK(code.find("agent_rand") != std::string::npos,
                      "optimize body uses agent_rand*");
            }
        }
    }

    // ── AC4: primitive exists ──
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

    std::println("\n=== test_synthesize_optimize_prng_1716: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
