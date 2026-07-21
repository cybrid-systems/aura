// @category: unit
// @reason: Issue #1720 — intend/strategy vectors need shared_mutex guards
// under concurrent fiber access.
//
//   AC1: Evaluator declares strategies_mtx_/intend_history_mtx_/timeline_mtx_
//   AC2: agent helpers timeline_push / intend_history_push present
//   AC3: strategy + intend primitives use locks (source audit)
//   AC4: concurrent set-strategy + active reads do not crash
//   AC5: concurrent intend-history reads while intend runs do not crash

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

} // namespace

int main() {
    // ── AC1–AC3: source audit ──
    {
        std::println("\n--- AC1–AC3: mutex fields + helpers ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("strategies_mtx_") != std::string::npos, "strategies_mtx_");
        CHECK(ixx.find("intend_history_mtx_") != std::string::npos, "intend_history_mtx_");
        CHECK(ixx.find("timeline_mtx_") != std::string::npos, "timeline_mtx_");
        CHECK(ixx.find("Issue #1720") != std::string::npos, "cites #1720 in ixx");

        std::string agent;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            agent = read_file(p);
            if (!agent.empty())
                break;
        }
        CHECK(!agent.empty(), "read agent");
        CHECK(agent.find("timeline_push") != std::string::npos, "timeline_push helper");
        CHECK(agent.find("intend_history_push") != std::string::npos, "intend_history_push");
        CHECK(agent.find("strategies_mtx_") != std::string::npos, "agent locks strategies");
        CHECK(agent.find("Issue #1720") != std::string::npos, "cites #1720 in agent");
    }

    // ── AC4: concurrent strategy set/get ──
    {
        std::println("\n--- AC4: concurrent strategy set/active ---");
        CompilerService cs;
        std::atomic<int> errors{0};
        auto writer = [&]() {
            for (int i = 0; i < 50; ++i) {
                auto r = cs.eval("(strategy:set-strategy \"coverage-greedy\")");
                if (!r)
                    errors.fetch_add(1);
                r = cs.eval("(strategy:set-strategy \"minimal-mutation\")");
                if (!r)
                    errors.fetch_add(1);
            }
        };
        auto reader = [&]() {
            for (int i = 0; i < 50; ++i) {
                auto r = cs.eval("(strategy:active)");
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
        CHECK(errors.load() == 0, "no eval errors under concurrent strategy access");
    }

    // ── AC5: concurrent intend-history while intend ──
    {
        std::println("\n--- AC5: concurrent intend-history ---");
        CompilerService cs;
        std::atomic<int> errors{0};
        auto intender = [&]() {
            for (int i = 0; i < 20; ++i) {
                auto r = cs.eval(
                    R"AURA((begin
                       (define gen (lambda (g) "(define (f x) x)"))
                       (define ver (lambda (c) "#t"))
                       (intend "g" gen ver 1)))AURA");
                if (!r)
                    errors.fetch_add(1);
            }
        };
        auto hist = [&]() {
            for (int i = 0; i < 40; ++i) {
                auto r = cs.eval("(intend-history)");
                if (!r)
                    errors.fetch_add(1);
            }
        };
        std::thread t1(intender);
        std::thread t2(hist);
        t1.join();
        t2.join();
        CHECK(errors.load() == 0, "no eval errors under concurrent intend-history");
    }

    std::println("\n=== test_strategy_intend_mutex_1720: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
