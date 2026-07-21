// @category: unit
// @reason: Issue #1721 — intend must not unbounded-push intermediate
// Issue #1721 (#1978 renamed): issue# moved from filename to header.
// strings onto string_heap each attempt; reuse fixed slots.
//
//   AC1: source cites #1721 and uses put_slot / slot_goal/code/err
//   AC2: no per-attempt push_back(goal)/push_back(code_str) in loop body
//   AC3: live intend completes; multi-attempt free-gen does not crash
//   AC4: heap growth bounded across many intend calls (slot reuse)

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
    // ── AC1/AC2: source audit ──
    {
        std::println("\n--- AC1/AC2: put_slot reuse ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_agent.cpp",
                              "../src/compiler/evaluator_primitives_agent.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read agent");
        CHECK(src.find("Issue #1721") != std::string::npos, "cites #1721");
        auto pos = src.find("add(\"intend\"");
        CHECK(pos != std::string::npos, "found intend");
        if (pos != std::string::npos) {
            auto end = src.find("\n    add(\"", pos + 10);
            auto win = src.substr(pos, end == std::string::npos ? 12000 : end - pos);
            auto code = strip_line_comments(win);
            CHECK(code.find("put_slot") != std::string::npos, "put_slot present");
            CHECK(code.find("slot_goal") != std::string::npos, "slot_goal");
            CHECK(code.find("slot_code") != std::string::npos, "slot_code");
            CHECK(code.find("slot_err") != std::string::npos, "slot_err");
            CHECK(code.find("finish_result") != std::string::npos, "finish_result");
            // Old unbounded pattern: string_heap_.push_back of intermediates.
            CHECK(code.find("string_heap_.push_back(goal)") == std::string::npos,
                  "no string_heap push_back(goal)");
            CHECK(code.find("string_heap_.push_back(current_code_str)") == std::string::npos,
                  "no string_heap push_back(current_code_str)");
            CHECK(code.find("string_heap_.push_back(last_error)") == std::string::npos,
                  "no string_heap push_back(last_error)");
            CHECK(code.find("string_heap_.push_back(code_str)") == std::string::npos,
                  "no string_heap push_back(code_str)");
        }
    }

    // ── AC3: functional ──
    {
        std::println("\n--- AC3: intend completes ---");
        CompilerService cs;
        auto r = cs.eval(
            R"AURA((begin
               (define gen (lambda (g) "(define (f x) x)"))
               (define ver (lambda (c) "#t"))
               (intend "goal" gen ver 2)))AURA");
        CHECK(r.has_value(), "live intend ok");
    }

    // ── AC4: bounded heap growth ──
    {
        std::println("\n--- AC4: heap growth bounded across many intends ---");
        CompilerService cs;
        // Warm once so slots exist.
        (void)cs.eval(
            R"AURA((begin
               (define gen (lambda (g) "(define (f x) x)"))
               (define ver (lambda (c) "#t"))
               (intend "w" gen ver 1)))AURA");
        const auto heap0 = cs.evaluator().string_heap_mut().size();
        for (int i = 0; i < 30; ++i) {
            auto r = cs.eval(
                R"AURA((begin
                   (define gen (lambda (g) "(define (f x) x)"))
                   (define ver (lambda (c) "#t"))
                   (intend "g" gen ver 1)))AURA");
            CHECK(r.has_value(), "intend iter ok");
        }
        const auto heap1 = cs.evaluator().string_heap_mut().size();
        // Each intend still pushes: define names + final result (+ gen/ver bodies).
        // Intermediate goal/code/err must NOT grow with attempts×calls.
        // Allow generous headroom for defines/results but fail if linear with pollution.
        // 30 calls × ~15 polluted strings would be ~450; with slots expect much less.
        const auto growth = heap1 > heap0 ? heap1 - heap0 : 0;
        CHECK(growth < 200, "heap growth bounded (slot reuse, not O(attempts) pollution)");
        std::println("    heap growth over 30 intends: {} (slots {})", growth, heap0);
    }

    std::println("\n=== test_intend_heap_slots_1721: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
