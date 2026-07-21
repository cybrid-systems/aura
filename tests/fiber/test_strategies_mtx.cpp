// @category: unit
// @reason: Issue #1722 — ev.strategies_ access must use strategies_mtx_
// Issue #1722 (#1978 renamed): issue# moved from filename to header.
// (sibling of #1720). Locks land with #1720; this locks AC coverage.
//
//   AC1: strategies_mtx_ declared; sources cite #1722
//   AC2: define-strategy / strategy-field / strategy-set-field! /
//        strategy-inspect / register-strategy! / evolve-strategy hold lock
//   AC3: concurrent define-strategy + strategy-field do not crash

#include "test_harness.hpp"

#include <atomic>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

// Extract body of add("name" ... until next top-level add(
std::string prim_window(const std::string& src, const char* name) {
    auto needle = std::string("add(\"") + name + "\"";
    auto pos = src.find(needle);
    if (pos == std::string::npos)
        return {};
    auto end = src.find("\n    add(\"", pos + 10);
    return src.substr(pos, end == std::string::npos ? 4000 : end - pos);
}

bool window_locks(const std::string& win) {
    return win.find("strategies_mtx_") != std::string::npos;
}

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: strategies_mtx_ on all 6 prims ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read ixx");
        CHECK(ixx.find("strategies_mtx_") != std::string::npos, "strategies_mtx_ declared");
        CHECK(ixx.find("Issue #1722") != std::string::npos ||
                  ixx.find("#1722") != std::string::npos,
              "cites #1722 on strategies_mtx_");

        std::string agent;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            agent = read_file(p);
            if (!agent.empty())
                break;
        }
        CHECK(!agent.empty(), "read agent");
        CHECK(agent.find("#1722") != std::string::npos, "agent cites #1722");

        const char* prims[] = {
            "define-strategy",     "register-strategy!", "strategy-field",
            "strategy-set-field!", "strategy-inspect",   "evolve-strategy",
        };
        for (const char* name : prims) {
            auto win = prim_window(agent, name);
            CHECK(!win.empty(), std::string("found ") + name);
            CHECK(window_locks(win), std::string(name) + " holds strategies_mtx_");
        }
    }

    // ── AC3: concurrent define + field ──
    {
        std::println("\n--- AC3: concurrent define-strategy / strategy-field ---");
        CompilerService cs;
        (void)cs.eval(R"AURA((define-strategy "s1722" "(lambda (x) x)"))AURA");
        std::atomic<int> errors{0};
        auto writer = [&]() {
            for (int i = 0; i < 40; ++i) {
                auto r = cs.eval(
                    R"AURA((define-strategy "s1722" "(lambda (x) (+ x 1))" :max-attempts 3))AURA");
                if (!r)
                    errors.fetch_add(1);
            }
        };
        auto reader = [&]() {
            for (int i = 0; i < 40; ++i) {
                auto r = cs.eval(R"AURA((strategy-field "s1722" "body"))AURA");
                if (!r)
                    errors.fetch_add(1);
                r = cs.eval(R"AURA((strategy-inspect "s1722"))AURA");
                if (!r)
                    errors.fetch_add(1);
            }
        };
        std::vector<std::thread> thr;
        thr.emplace_back(writer);
        thr.emplace_back(reader);
        thr.emplace_back(writer);
        thr.emplace_back(reader);
        for (auto& t : thr)
            t.join();
        CHECK(errors.load() == 0, "no eval errors under concurrent strategy R/W");
    }

    std::println("\n=== test_strategies_mtx_1722: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
